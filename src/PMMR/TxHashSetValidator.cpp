#include "TxHashSetValidator.h"
#include "TxHashSetImpl.h"
#include "KernelMMR.h"
#include "Common/MMR.h"
#include "Common/MMRUtil.h"
#include "Common/MMRHashUtil.h"

#include <Consensus.h>
#include <Crypto/Crypto.h>
#include <Core/Validation/KernelSignatureValidator.h>
#include <Core/Validation/KernelSumValidator.h>
#include <Core/Global.h>
#include <Common/Util/HexUtil.h>
#include <Common/Logger.h>
#include <BlockChain/BlockChain.h>
#include <thread>
#include <atomic>
#include <map>
#include <stdexcept>

namespace
{
	static constexpr size_t COMMITMENT_SUM_CHUNK_SIZE = 65536;

	struct NRDKernelPos
	{
		uint64_t height;
		uint64_t kernelIndex;
	};

	static void FlushCommitmentChunk(
		std::vector<Commitment>& chunk,
		std::vector<Commitment>& partialSums)
	{
		if (!chunk.empty()) {
			partialSums.push_back(Crypto::AddCommitments(chunk, std::vector<Commitment>()));
			chunk.clear();
		}
	}
}

std::unique_ptr<BlockSums> TxHashSetValidator::Validate(TxHashSet& txHashSet, const BlockHeader& blockHeader, SyncStatus& syncStatus) const
{
	std::shared_ptr<const KernelMMR> pKernelMMR = txHashSet.GetKernelMMR();
	std::shared_ptr<const OutputPMMR> pOutputPMMR = txHashSet.GetOutputPMMR();
	std::shared_ptr<const RangeProofPMMR> pRangeProofPMMR = txHashSet.GetRangeProofPMMR();

	syncStatus.UpdateStatus(ESyncStatus::TXHASHSET_SETUP);

	// Validate size of each MMR matches blockHeader
	if (!ValidateSizes(txHashSet, blockHeader))
	{
		LOG_ERROR("Invalid MMR size");
		return std::unique_ptr<BlockSums>(nullptr);
	}

	syncStatus.UpdateProcessingStatus(5);

	// Validate MMR hashes in parallel
	std::vector<std::thread> threads;
	std::atomic_bool mmrHashesValidated = true;
	threads.emplace_back(std::thread([this, pKernelMMR, &mmrHashesValidated] { if (!this->ValidateMMRHashes(pKernelMMR)) { mmrHashesValidated = false; }}));
	threads.emplace_back(std::thread([this, pOutputPMMR, &mmrHashesValidated] { if (!this->ValidateMMRHashes(pOutputPMMR)) { mmrHashesValidated = false; }}));
	threads.emplace_back(std::thread([this, pRangeProofPMMR, &mmrHashesValidated] { if (!this->ValidateMMRHashes(pRangeProofPMMR)) { mmrHashesValidated = false; }}));

	for (auto& thread : threads)
	{
		if (thread.joinable())
		{
			thread.join();
		}
	}

	if (!mmrHashesValidated)
	{
		LOG_ERROR("Invalid MMR hashes");
		return std::unique_ptr<BlockSums>(nullptr);
	}

	syncStatus.UpdateProcessingStatus(10);

	// Validate root for each MMR matches blockHeader
	if (!txHashSet.ValidateRoots(blockHeader))
	{
		LOG_ERROR("Invalid MMR roots");
		return std::unique_ptr<BlockSums>(nullptr);
	}

	syncStatus.UpdateProcessingStatus(15);
	syncStatus.UpdateStatus(ESyncStatus::TXHASHSET_KERNEL_HISTORY_VALIDATION);

	// Validate the full kernel history (kernel MMR root for every block header).
	LOG_DEBUG("Validating kernel history");
	if (!ValidateKernelHistory(*txHashSet.GetKernelMMR(), blockHeader, syncStatus))
	{
		LOG_ERROR("Invalid kernel history");
		return std::unique_ptr<BlockSums>(nullptr);
	}

	syncStatus.UpdateProcessingStatus(25);
	syncStatus.UpdateStatus(ESyncStatus::TXHASHSET_NRD_KERNELS_VALIDATION);

	LOG_DEBUG("Validating NRD kernel history");
	LoggerAPI::Flush();
	if (!ValidateNRDKernelHistory(*txHashSet.GetKernelMMR(), blockHeader, syncStatus))
	{
		LOG_ERROR("Invalid NRD kernel history");
		return std::unique_ptr<BlockSums>(nullptr);
	}

	syncStatus.UpdateProcessingStatus(30);
	syncStatus.UpdateStatus(ESyncStatus::TXHASHSET_KERNEL_SUMS_VALIDATION);

	// Validate kernel sums
	LOG_DEBUG("Validating kernel sums");
	LoggerAPI::Flush();

	std::unique_ptr<BlockSums> pBlockSums = nullptr;
	try
	{
		pBlockSums = std::make_unique<BlockSums>(ValidateKernelSums(txHashSet, blockHeader, syncStatus));
	}
	catch (...)
	{
		LOG_ERROR("Invalid kernel sums");
		return std::unique_ptr<BlockSums>(nullptr);
	}

	syncStatus.UpdateProcessingStatus(40);
	syncStatus.UpdateStatus(ESyncStatus::TXHASHSET_RANGE_PROOFS_VALIDATION);

	// Validate the rangeproof associated with each unspent output.
	LOG_DEBUG("Validating range proofs");
	LoggerAPI::Flush();
	if (!ValidateRangeProofs(txHashSet, syncStatus))
	{
		LOG_ERROR("Failed to verify rangeproofs");
		return std::unique_ptr<BlockSums>(nullptr);
	}

	syncStatus.UpdateProcessingStatus(70);
	syncStatus.UpdateStatus(ESyncStatus::TXHASHSET_KERNEL_SIGNATURES_VALIDATION);

	// Validate kernel signatures
	LOG_DEBUG("Validating kernel signatures");
	LoggerAPI::Flush();
	if (!ValidateKernelSignatures(*txHashSet.GetKernelMMR(), syncStatus))
	{
		LOG_ERROR("Failed to verify kernel signatures");
		return std::unique_ptr<BlockSums>(nullptr);
	}

	LOG_DEBUG("Success");
	LoggerAPI::Flush();

	syncStatus.UpdateProcessingStatus(100);
	syncStatus.UpdateStatus(ESyncStatus::TXHASHSET_SAVE);

	return pBlockSums;
}

bool TxHashSetValidator::ValidateSizes(TxHashSet& txHashSet, const BlockHeader& blockHeader) const
{
	if (txHashSet.GetKernelMMR()->GetSize() != blockHeader.GetKernelMMRSize())
	{
		LOG_ERROR_F("Kernel size not matching for header ({})", blockHeader);
		return false;
	}

	if (txHashSet.GetOutputPMMR()->GetSize() != blockHeader.GetOutputMMRSize())
	{
		LOG_ERROR_F("Output size not matching for header ({})", blockHeader);
		return false;
	}

	if (txHashSet.GetRangeProofPMMR()->GetSize() != blockHeader.GetOutputMMRSize())
	{
		LOG_ERROR_F("RangeProof size not matching for header ({})", blockHeader);
		return false;
	}

	return true;
}

// TODO: This probably belongs in MMRHashUtil.
bool TxHashSetValidator::ValidateMMRHashes(std::shared_ptr<const MMR> pMMR) const
{
	try
    {
        const uint64_t size = pMMR->GetSize();
		for (Index mmr_idx = Index::At(0); mmr_idx < size; mmr_idx++) {
			if (!Global::IsRunning()) {
				return false;
			}

            if (!mmr_idx.IsLeaf()) {
                const std::unique_ptr<Hash> pParentHash = pMMR->GetHashAt(mmr_idx);
                if (pParentHash != nullptr) {
                    const std::unique_ptr<Hash> pLeftHash = pMMR->GetHashAt(mmr_idx.GetLeftChild());
                    const std::unique_ptr<Hash> pRightHash = pMMR->GetHashAt(mmr_idx.GetRightChild());
                    if (pLeftHash != nullptr && pRightHash != nullptr) {
                        const Hash expectedHash = MMRHashUtil::HashParentWithIndex(*pLeftHash, *pRightHash, mmr_idx.GetPosition());
                        if (*pParentHash != expectedHash) {
                            LOG_ERROR_F("Invalid parent hash at {}", mmr_idx);
                            return false;
                        }
                    }
                }
            }
        }
	}
	catch (std::exception& e)
	{
		LOG_ERROR_F("Exception thrown while validating hashes: {}", e.what());
		return false;
	}

	return true;
}

bool TxHashSetValidator::ValidateKernelHistory(const KernelMMR& kernelMMR, const BlockHeader& blockHeader, SyncStatus& syncStatus) const
{
	const uint64_t totalHeight = blockHeader.GetHeight();
	const uint64_t totalHeaders = totalHeight + 1;
	for (uint64_t height = 0; height <= totalHeight; height++)
	{
		if (!Global::IsRunning()) {
			return false;
		}

		auto pHeader = m_blockChain.GetBlockHeaderByHeight(height, EChainType::CANDIDATE);
		if (pHeader == nullptr)
		{
			LOG_ERROR_F("No header found at height ({})", height);
			return false;
		}
		
		if (kernelMMR.Root(pHeader->GetKernelMMRSize()) != pHeader->GetKernelRoot())
		{
			LOG_ERROR_F("Kernel root not matching for header at height ({})", height);
			return false;
		}

		if (height % 1000 == 0 || height == totalHeight)
		{
			const uint64_t checkedHeaders = height + 1;
			const uint8_t processingStatus = (uint8_t)(15 + ((10.0 * checkedHeaders) / totalHeaders));
			syncStatus.UpdateProcessingStatus(processingStatus);
			syncStatus.UpdateProcessingProgress(checkedHeaders, totalHeaders);

			if (height % 100000 == 0 || height == totalHeight)
			{
				LOG_DEBUG_F("Validated kernel history through header {}/{}", height, totalHeight);
			}
		}
	}

	syncStatus.UpdateProcessingProgress(totalHeaders, totalHeaders);
	return true;
}

bool TxHashSetValidator::ValidateNRDKernelHistory(const KernelMMR& kernelMMR, const BlockHeader& blockHeader, SyncStatus& syncStatus) const
{
	std::map<Commitment, NRDKernelPos> nrdKernelPositions;
	uint64_t previousNumKernels = 0;
	uint64_t processedKernels = 0;
	uint64_t nrdKernels = 0;
	const uint64_t totalKernels = blockHeader.GetNumKernels();

	auto updateProgress = [&syncStatus, totalKernels](const uint64_t processed) {
		if (totalKernels == 0) {
			syncStatus.UpdateProcessingStatus(30);
			syncStatus.UpdateProcessingProgress(0, 0);
			return;
		}

		const uint8_t processingStatus = (uint8_t)(25 + ((5.0 * processed) / totalKernels));
		syncStatus.UpdateProcessingStatus(processingStatus);
		syncStatus.UpdateProcessingProgress(processed, totalKernels);
	};

	updateProgress(0);

	for (uint64_t height = 0; height <= blockHeader.GetHeight(); ++height)
	{
		if (!Global::IsRunning()) {
			return false;
		}

		auto pHeader = m_blockChain.GetBlockHeaderByHeight(height, EChainType::CANDIDATE);
		if (pHeader == nullptr)
		{
			LOG_ERROR_F("No header found at height ({}) while validating NRD kernels", height);
			return false;
		}

		const uint64_t currentNumKernels = pHeader->GetNumKernels();
		if (currentNumKernels < previousNumKernels || currentNumKernels > totalKernels)
		{
			LOG_ERROR_F("Invalid kernel count at height {} while validating NRD kernels: previous={}, current={}, total={}",
				height, previousNumKernels, currentNumKernels, totalKernels);
			return false;
		}

		for (uint64_t kernelIndex = previousNumKernels; kernelIndex < currentNumKernels; ++kernelIndex)
		{
			if (!Global::IsRunning()) {
				return false;
			}

			std::unique_ptr<TransactionKernel> pKernel = kernelMMR.GetKernelAt(LeafIndex::At(kernelIndex));
			if (pKernel == nullptr)
			{
				LOG_ERROR_F("No kernel found at index {} while validating NRD kernels", kernelIndex);
				return false;
			}

			if (pKernel->GetFeatures() == EKernelFeatures::NO_RECENT_DUPLICATE)
			{
				++nrdKernels;
				const uint64_t relativeHeight = pKernel->GetLockHeight();
				if (relativeHeight == 0 || relativeHeight > Consensus::WEEK_HEIGHT)
				{
					LOG_ERROR_F("Invalid NRD relative height {} for kernel {} at height {}",
						relativeHeight, pKernel->GetExcessCommitment(), height);
					return false;
				}

				const Commitment& excess = pKernel->GetExcessCommitment();
				const auto iter = nrdKernelPositions.find(excess);
				if (iter != nrdKernelPositions.cend())
				{
					const uint64_t heightDiff = height - iter->second.height;
					if (heightDiff < relativeHeight)
					{
						LOG_ERROR_F("NRD duplicate kernel {} at height {} violates relative height {}. Previous height={}, kernel_index={}",
							excess, height, relativeHeight, iter->second.height, iter->second.kernelIndex);
						return false;
					}
				}

				nrdKernelPositions[excess] = NRDKernelPos{ height, kernelIndex };
			}

			++processedKernels;
			if (processedKernels % 100000 == 0 || processedKernels == totalKernels)
			{
				updateProgress(processedKernels);
				LOG_DEBUG_F("Validated NRD kernel history through kernel {}/{} (nrd={})",
					processedKernels, totalKernels, nrdKernels);
			}
		}

		previousNumKernels = currentNumKernels;
	}

	updateProgress(totalKernels);
	LOG_DEBUG_F("Validated NRD kernel history for {} kernels (nrd={})", totalKernels, nrdKernels);
	return true;
}

BlockSums TxHashSetValidator::ValidateKernelSums(TxHashSet& txHashSet, const BlockHeader& blockHeader, SyncStatus& syncStatus) const
{
	const int64_t overage = 0 - (Consensus::REWARD * (1 + blockHeader.GetHeight()));
	const uint64_t numOutputs = blockHeader.GetNumOutputs();
	const uint64_t numKernels = blockHeader.GetNumKernels();
	const uint64_t totalItems = numOutputs + numKernels;
	uint64_t processedItems = 0;

	auto updateProgress = [&syncStatus, totalItems](const uint64_t processed) {
		if (totalItems == 0) {
			syncStatus.UpdateProcessingStatus(40);
			syncStatus.UpdateProcessingProgress(0, 0);
			return;
		}

		const uint8_t processingStatus = (uint8_t)(30 + ((10.0 * processed) / totalItems));
		syncStatus.UpdateProcessingStatus(processingStatus);
		syncStatus.UpdateProcessingProgress(processed, totalItems);
	};

	updateProgress(0);

	std::vector<Commitment> outputCommitmentSums;
	std::vector<Commitment> outputCommitmentChunk;
	outputCommitmentChunk.reserve(COMMITMENT_SUM_CHUNK_SIZE);
	std::shared_ptr<const OutputPMMR> pOutputPMMR = txHashSet.GetOutputPMMR();
	for (LeafIndex output_idx = LeafIndex::At(0); output_idx < numOutputs; output_idx++) {
		if (!Global::IsRunning()) {
			throw std::runtime_error("Interrupted by shutdown");
		}

		std::unique_ptr<OutputIdentifier> pOutput = pOutputPMMR->GetAt(output_idx);
		if (pOutput != nullptr) {
			outputCommitmentChunk.push_back(pOutput->GetCommitment());
			if (outputCommitmentChunk.size() >= COMMITMENT_SUM_CHUNK_SIZE) {
				FlushCommitmentChunk(outputCommitmentChunk, outputCommitmentSums);
			}
		}

		++processedItems;
		if (processedItems % 100000 == 0 || processedItems == totalItems) {
			updateProgress(processedItems);
			LOG_TRACE(StringUtil::Format(
				"Collected output sum commitments {}/{} partials={}",
				processedItems,
				totalItems,
				outputCommitmentSums.size()));
		}
	}
	FlushCommitmentChunk(outputCommitmentChunk, outputCommitmentSums);

	std::vector<Commitment> excessCommitmentSums;
	std::vector<Commitment> excessCommitmentChunk;
	excessCommitmentChunk.reserve(COMMITMENT_SUM_CHUNK_SIZE);
	std::shared_ptr<const KernelMMR> pKernelMMR = txHashSet.GetKernelMMR();
	for (LeafIndex kernel_idx = LeafIndex::At(0); kernel_idx < numKernels; kernel_idx++) {
		if (!Global::IsRunning()) {
			throw std::runtime_error("Interrupted by shutdown");
		}

		std::unique_ptr<TransactionKernel> pKernel = pKernelMMR->GetKernelAt(kernel_idx);
		if (pKernel != nullptr) {
			excessCommitmentChunk.push_back(pKernel->GetExcessCommitment());
			if (excessCommitmentChunk.size() >= COMMITMENT_SUM_CHUNK_SIZE) {
				FlushCommitmentChunk(excessCommitmentChunk, excessCommitmentSums);
			}
		}

		++processedItems;
		if (processedItems % 100000 == 0 || processedItems == totalItems) {
			updateProgress(processedItems);
			LOG_TRACE(StringUtil::Format(
				"Collected kernel sum commitments {}/{} partials={}",
				processedItems,
				totalItems,
				excessCommitmentSums.size()));
		}
	}
	FlushCommitmentChunk(excessCommitmentChunk, excessCommitmentSums);

	updateProgress(totalItems);
	return KernelSumValidator::ValidateKernelSums(
		std::vector<Commitment>(),
		outputCommitmentSums,
		excessCommitmentSums,
		overage,
		blockHeader.GetOffset(),
		std::nullopt
	);
}

bool TxHashSetValidator::ValidateRangeProofs(TxHashSet& txHashSet, SyncStatus& syncStatus) const
{
	std::vector<std::pair<Commitment, RangeProof>> rangeProofs;

	const uint64_t outputMMRSize = txHashSet.GetOutputPMMR()->GetSize();
	for (LeafIndex leaf_idx = LeafIndex::At(0); leaf_idx.GetPosition() < outputMMRSize; leaf_idx++)
	{
		if (!Global::IsRunning()) {
			return false;
		}

		std::unique_ptr<OutputIdentifier> pOutput = txHashSet.GetOutputPMMR()->GetAt(leaf_idx);
		if (pOutput != nullptr)
		{
			std::unique_ptr<RangeProof> pRangeProof = txHashSet.GetRangeProofPMMR()->GetAt(leaf_idx);
			if (pRangeProof == nullptr)
			{
				LOG_ERROR_F("No rangeproof found at leaf index ({})", leaf_idx);
				return false;
			}

			rangeProofs.emplace_back(std::make_pair(pOutput->GetCommitment(), *pRangeProof));

			if (rangeProofs.size() >= 1000)
			{
				if (!Crypto::VerifyRangeProofs(rangeProofs))
				{
					return false;
				}

				rangeProofs.clear();

				syncStatus.UpdateProcessingStatus((uint8_t)(40 + ((30.0 * leaf_idx.GetPosition()) / outputMMRSize)));
				syncStatus.UpdateProcessingProgress(leaf_idx.GetPosition(), outputMMRSize);
			}
		}
	}

	if (!rangeProofs.empty() && !Crypto::VerifyRangeProofs(rangeProofs))
	{
		return false;
	}

	syncStatus.UpdateProcessingProgress(outputMMRSize, outputMMRSize);
	return true;
}

bool TxHashSetValidator::ValidateKernelSignatures(const KernelMMR& kernelMMR, SyncStatus& syncStatus) const
{
	std::vector<TransactionKernel> kernels;

	const uint64_t num_kernels = kernelMMR.GetNumKernels();
	for (LeafIndex leaf_idx = LeafIndex::At(0); leaf_idx < num_kernels; leaf_idx++) {
		if (!Global::IsRunning()) {
			return false;
		}

		std::unique_ptr<TransactionKernel> pKernel = kernelMMR.GetKernelAt(leaf_idx);
		if (pKernel == nullptr) {
			return false;
		}

		kernels.push_back(*pKernel);

		if (kernels.size() >= 5000) {
			if (!KernelSignatureValidator::BatchVerify(kernels)) {
				return false;
			}

			kernels.clear();

			syncStatus.UpdateProcessingStatus((uint8_t)(70 + ((30.0 * leaf_idx.Get()) / num_kernels)));
			syncStatus.UpdateProcessingProgress(leaf_idx.Get(), num_kernels);
		}
	}

	syncStatus.UpdateProcessingProgress(num_kernels, num_kernels);
	return KernelSignatureValidator::BatchVerify(kernels);
}
