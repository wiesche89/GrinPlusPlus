#pragma once

#include "KernelMMR.h"
#include "OutputPMMR.h"
#include "RangeProofPMMR.h"

#include <PMMR/TxHashSet.h>
#include <PMMR/Common/BitmapAccumulator.h>
#include <Core/Config.h>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>

class TxHashSet : public ITxHashSet
{
public:
	TxHashSet(
		std::shared_ptr<KernelMMR> pKernelMMR,
		std::shared_ptr<OutputPMMR> pOutputPMMR,
		std::shared_ptr<RangeProofPMMR> pRangeProofPMMR,
		BlockHeaderPtr pBlockHeader
	);
	virtual ~TxHashSet() = default;

	BlockHeaderPtr GetFlushedBlockHeader() const noexcept final { return m_pBlockHeaderBackup; }
	uint64_t GetOutputMMRSize() const final;
	uint64_t GetRangeProofMMRSize() const final;
	uint64_t GetKernelMMRSize() const final;

	bool IsValid(std::shared_ptr<const IBlockDB> pBlockDB, const Transaction& transaction) const final;
	std::unique_ptr<BlockSums> ValidateTxHashSet(const BlockHeader& header, const IBlockChain& blockChain, SyncStatus& syncStatus) final;
	bool ApplyBlock(std::shared_ptr<IBlockDB> pBlockDB, const FullBlock& block) final;
	bool ValidateNRDKernelRules(std::shared_ptr<const IBlockDB> pBlockDB, const FullBlock& block) const final;
	bool ValidateRoots(const BlockHeader& blockHeader) const final;
	TxHashSetRoots GetRoots(const std::shared_ptr<const IBlockDB>& pBlockDB, const TransactionBody& body) final;
	void SaveOutputPositions(const Chain::CPtr& pChain, std::shared_ptr<IBlockDB> pBlockDB) const final;

	std::vector<Hash> GetLastKernelHashes(const uint64_t numberOfKernels) const final;
	std::vector<Hash> GetLastOutputHashes(const uint64_t numberOfOutputs) const final;
	std::vector<Hash> GetLastRangeProofHashes(const uint64_t numberOfRangeProofs) const final;
	OutputRange GetOutputsByLeafIndex(std::shared_ptr<const IBlockDB> pBlockDB, const uint64_t startIndex, const uint64_t maxNumOutputs) const final;
	std::vector<OutputDTO> GetOutputsByMMRIndex(std::shared_ptr<const IBlockDB> pBlockDB, const uint64_t startIndex, const uint64_t lastIndex) const final;
	OutputDTO GetOutput(const OutputLocation& location) const final;

	void Rewind(std::shared_ptr<IBlockDB> pBlockDB, const BlockHeader& header) final;
	void Commit() final;
	void Rollback() noexcept final;
	void Compact() final;

	std::shared_ptr<KernelMMR> GetKernelMMR() { return m_pKernelMMR; }
	std::shared_ptr<OutputPMMR> GetOutputPMMR() { return m_pOutputPMMR; }
	std::shared_ptr<RangeProofPMMR> GetRangeProofPMMR() { return m_pRangeProofPMMR; }
	std::shared_ptr<const KernelMMR> GetKernelMMR() const { return m_pKernelMMR; }
	std::shared_ptr<const OutputPMMR> GetOutputPMMR() const { return m_pOutputPMMR; }
	std::shared_ptr<const RangeProofPMMR> GetRangeProofPMMR() const { return m_pRangeProofPMMR; }

	std::optional<BitmapSegment> GetOutputBitmapSegment(const SegmentIdentifier& identifier) const final;
	std::optional<Segment<PIBD::OUTPUT_DATA_SIZE, OutputIdentifier>> GetOutputSegment(const SegmentIdentifier& identifier) const final;
	Hash GetOutputBitmapRoot(const uint64_t numOutputs) const final;
	std::optional<Segment<PIBD::RANGE_PROOF_DATA_SIZE, RangeProof>> GetRangeProofSegment(const SegmentIdentifier& identifier) const final;
	std::optional<Segment<PIBD::KERNEL_DATA_SIZE, TransactionKernel>> GetKernelSegment(const SegmentIdentifier& identifier) const final;

	bool ApplyOutputSegment(const Segment<OUTPUT_SIZE, OutputIdentifier>& segment, const uint64_t targetMMRSize);
	bool ApplyRangeProofSegment(const Segment<RANGE_PROOF_SIZE, RangeProof>& segment, const uint64_t targetMMRSize);
	bool ApplyKernelSegment(const Segment<KERNEL_SIZE, TransactionKernel>& segment);
	bool ApplyKernelSegment(const Segment<KERNEL_SIZE, TransactionKernel>& segment, const uint64_t targetMMRSize) final;
	void UpdateLeafSets(const BitmapAccumulator& outputBitmap, const uint64_t numOutputs) final;

private:
	void BuildOutputBitmapCache() const;
	void InvalidateOutputBitmapCache() const;

	std::shared_ptr<KernelMMR> m_pKernelMMR;
	std::shared_ptr<OutputPMMR> m_pOutputPMMR;
	std::shared_ptr<RangeProofPMMR> m_pRangeProofPMMR;

	BlockHeaderPtr m_pBlockHeader;
	BlockHeaderPtr m_pBlockHeaderBackup;

	mutable std::mutex m_outputBitmapCacheMutex;
	mutable std::optional<BitmapAccumulator> m_outputBitmapCache;
	mutable uint64_t m_outputBitmapCacheOutputMMRSize{ 0 };
};
