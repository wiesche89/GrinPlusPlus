#pragma once

#include "TxHashSetImpl.h"

#include <Core/Models/OutputIdentifier.h>
#include <Core/Models/TransactionKernel.h>
#include <Crypto/Models/RangeProof.h>
#include <PMMR/Common/BitmapSegment.h>
#include <PMMR/Common/Segment.h>
#include <PMMR/Common/SegmentId.h>
#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

class TxHashSetSegmenter
{
public:
	explicit TxHashSetSegmenter(std::shared_ptr<TxHashSet> pTxHashSet)
		: m_pTxHashSet(pTxHashSet.get())
	{
	}

	explicit TxHashSetSegmenter(const TxHashSet& txHashSet)
		: m_pTxHashSet(&txHashSet)
	{
	}

	std::optional<Segment<KERNEL_SIZE, TransactionKernel>> KernelSegment(const SegmentIdentifier& identifier) const
	{
		return BuildSegment<KERNEL_SIZE, TransactionKernel>(
			*m_pTxHashSet->GetKernelMMR(),
			identifier,
			false,
			[this](const LeafIndex& leafIdx) {
				return m_pTxHashSet->GetKernelMMR()->GetKernelAt(leafIdx);
			});
	}

	std::optional<Segment<OUTPUT_SIZE, OutputIdentifier>> OutputSegment(const SegmentIdentifier& identifier) const
	{
		return BuildSegment<OUTPUT_SIZE, OutputIdentifier>(
			*m_pTxHashSet->GetOutputPMMR(),
			identifier,
			true,
			[this](const LeafIndex& leafIdx) {
				return m_pTxHashSet->GetOutputPMMR()->GetAt(leafIdx);
			});
	}

	std::optional<Segment<RANGE_PROOF_SIZE, RangeProof>> RangeProofSegment(const SegmentIdentifier& identifier) const
	{
		return BuildSegment<RANGE_PROOF_SIZE, RangeProof>(
			*m_pTxHashSet->GetRangeProofPMMR(),
			identifier,
			true,
			[this](const LeafIndex& leafIdx) {
				return m_pTxHashSet->GetRangeProofPMMR()->GetAt(leafIdx);
			});
	}

	std::optional<BitmapSegment> OutputBitmapSegment(const SegmentIdentifier& identifier) const
	{
		BitmapAccumulator accumulator;
		const uint64_t outputMMRSize = m_pTxHashSet->GetOutputPMMR()->GetSize();
		const uint64_t outputLeaves = MMRUtil::CountLeaves(outputMMRSize);
		const uint64_t chunkCount = (outputLeaves + BitmapChunk::LEN_BITS - 1) / BitmapChunk::LEN_BITS;
		for (uint64_t chunkIdx = 0; chunkIdx < chunkCount; ++chunkIdx) {
			BitmapChunk chunk;
			const uint64_t firstLeaf = chunkIdx * BitmapChunk::LEN_BITS;
			const uint64_t lastLeaf = (std::min)(outputLeaves, firstLeaf + BitmapChunk::LEN_BITS);
			for (uint64_t leafIdx = firstLeaf; leafIdx < lastLeaf; ++leafIdx) {
				if (m_pTxHashSet->GetOutputPMMR()->IsUnpruned(LeafIndex::At(leafIdx))) {
					chunk.Set(leafIdx - firstLeaf, true);
				}
			}
			accumulator.AppendChunk(std::move(chunk));
		}

		return OutputBitmapSegment(identifier, accumulator);
	}

	static std::optional<BitmapSegment> OutputBitmapSegment(const SegmentIdentifier& identifier, const BitmapAccumulator& accumulator)
	{
		const uint64_t bitmapMMRSize = accumulator.GetMMRSize();
		if (identifier.GetSegmentUnprunedSize(bitmapMMRSize) == 0) {
			return std::nullopt;
		}

		const std::pair<uint64_t, uint64_t> posRange = identifier.GetPositionRange(bitmapMMRSize);
		std::vector<uint64_t> leafPositions;
		std::vector<BitmapChunk> chunks;
		for (uint64_t pos = posRange.first; pos <= posRange.second; ++pos) {
			const Index index = Index::At(pos);
			if (!index.IsLeaf()) {
				continue;
			}

			const uint64_t chunkIdx = index.GetLeafIndex();
			if (chunkIdx >= accumulator.GetChunks().size()) {
				return std::nullopt;
			}

			leafPositions.push_back(pos);
			chunks.push_back(accumulator.GetChunks()[chunkIdx]);
		}

		const std::optional<SegmentProof> proof = SegmentProof::Generate(
			bitmapMMRSize,
			posRange.first,
			posRange.second,
			std::nullopt,
			[&accumulator](const uint64_t position) {
				return accumulator.GetHashAt(position);
			});
		if (!proof.has_value()) {
			return std::nullopt;
		}

		Segment<BitmapChunk::LEN_BYTES, BitmapChunk> segment(
			identifier,
			{},
			{},
			std::move(leafPositions),
			std::move(chunks),
			proof.value());

		return BitmapSegment::FromSegment(segment);
	}

private:
	template<size_t DATA_SIZE, typename DATA_TYPE, typename MMR_TYPE, typename LEAF_READER>
	std::optional<Segment<DATA_SIZE, DATA_TYPE>> BuildSegment(
		const MMR_TYPE& mmr,
		const SegmentIdentifier& identifier,
		const bool prunable,
		LEAF_READER readLeaf) const
	{
		const uint64_t mmrSize = mmr.GetSize();
		if (identifier.GetSegmentUnprunedSize(mmrSize) == 0) {
			return std::nullopt;
		}

		const std::pair<uint64_t, uint64_t> posRange = identifier.GetPositionRange(mmrSize);
		std::vector<uint64_t> hashPositions;
		std::vector<Hash> hashes;
		std::vector<uint64_t> leafPositions;
		std::vector<DATA_TYPE> leaves;

		for (uint64_t pos = posRange.first; pos <= posRange.second; ++pos) {
			const Index index = Index::At(pos);
			if (index.IsLeaf()) {
				std::unique_ptr<DATA_TYPE> pLeaf = readLeaf(LeafIndex::From(index));
				if (pLeaf != nullptr) {
					leafPositions.push_back(pos);
					leaves.push_back(std::move(*pLeaf));
					continue;
				}

				if (!prunable) {
					return std::nullopt;
				}
			}

			if (prunable) {
				std::unique_ptr<Hash> pHash = mmr.GetHashAt(index);
				if (pHash != nullptr) {
					hashPositions.push_back(pos);
					hashes.push_back(*pHash);
				}
			}
		}

		std::optional<uint64_t> startPos;
		if (leaves.empty() && hashes.empty()) {
			const std::vector<std::pair<uint64_t, uint64_t>> branch = MMRUtil::FamilyBranch(posRange.second, mmrSize);
			for (const std::pair<uint64_t, uint64_t>& entry : branch) {
				std::unique_ptr<Hash> pHash = mmr.GetHashAt(Index::At(entry.first));
				if (pHash != nullptr) {
					hashPositions.push_back(entry.first);
					hashes.push_back(*pHash);
					startPos = entry.first + 1;
					break;
				}
			}
		}

		const std::optional<SegmentProof> proof = SegmentProof::Generate(
			mmrSize,
			posRange.first,
			posRange.second,
			startPos,
			[&mmr](const uint64_t position) -> std::optional<Hash> {
				std::unique_ptr<Hash> pHash = mmr.GetHashAt(Index::At(position));
				if (pHash == nullptr) {
					return std::nullopt;
				}
				return *pHash;
			});
		if (!proof.has_value()) {
			return std::nullopt;
		}

		return Segment<DATA_SIZE, DATA_TYPE>(
			identifier,
			std::move(hashPositions),
			std::move(hashes),
			std::move(leafPositions),
			std::move(leaves),
			proof.value());
	}

	const TxHashSet* m_pTxHashSet;
};
