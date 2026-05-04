#pragma once

#include "MMR.h"
#include "HashFile.h"
#include "LeafSet.h"
#include "PruneList.h"

#include "MMRUtil.h"
#include "MMRHashUtil.h"

#include <Core/File/DataFile.h>
#include <Roaring.h>
#include <Core/Exceptions/TxHashSetException.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Traits/Lockable.h>
#include <Common/Logger.h>
#include <PMMR/Common/BitmapAccumulator.h>
#include <PMMR/Common/Segment.h>
#include <algorithm>
#include <optional>

template<size_t DATA_SIZE, class DATA_TYPE>
class PruneableMMR : public MMR, public Traits::IBatchable
{
public:
	PruneableMMR(
		std::shared_ptr<HashFile> pHashFile,
		std::shared_ptr<LeafSet> pLeafSet,
		std::shared_ptr<PruneList> pPruneList,
		std::shared_ptr<DataFile<DATA_SIZE>> pDataFile)
		: m_pHashFile(pHashFile),
		m_pLeafSet(pLeafSet),
		m_pPruneList(pPruneList),
		m_pDataFile(pDataFile)
	{

	}

	virtual ~PruneableMMR() = default;

	void Append(const DATA_TYPE& object)
	{
		SetDirty(true);

		// Add to LeafSet
		Index mmr_idx = Index::At(m_pHashFile->GetSize() + m_pPruneList->GetTotalShift());
		m_pLeafSet->Add(LeafIndex::From(mmr_idx));

		// Add to data file
		std::vector<uint8_t> serialized = object.Serialized();
		m_pDataFile->AddData(serialized);

		// Add hashes
		MMRHashUtil::AddHashes(m_pHashFile, serialized, m_pPruneList);
	}

	bool ApplySegment(
		const Segment<DATA_SIZE, DATA_TYPE>& segment,
		const uint64_t targetMMRSize,
		const BitmapAccumulator* pBitmap = nullptr)
	{
		if (segment.GetLeafPositions().size() != segment.GetLeaves().size()) {
			LOG_WARNING_F("Pruneable PIBD segment {}:{} has {} leaf positions but {} leaves.",
				segment.GetIdentifier().GetHeight(),
				segment.GetIdentifier().GetIndex(),
				segment.GetLeafPositions().size(),
				segment.GetLeaves().size());
			return false;
		}

		if (segment.GetHashPositions().size() != segment.GetHashes().size()) {
			LOG_WARNING_F("Pruneable PIBD segment {}:{} has {} hash positions but {} hashes.",
				segment.GetIdentifier().GetHeight(),
				segment.GetIdentifier().GetIndex(),
				segment.GetHashPositions().size(),
				segment.GetHashes().size());
			return false;
		}

		SetDirty(true);
		std::vector<SegmentNode> orderedNodes;
		orderedNodes.reserve(segment.GetHashPositions().size() + segment.GetLeafPositions().size());
		for (size_t i = 0; i < segment.GetHashPositions().size(); ++i) {
			orderedNodes.emplace_back(SegmentNode::Hash(i, segment.GetHashPositions()[i]));
		}
		for (size_t i = 0; i < segment.GetLeafPositions().size(); ++i) {
			orderedNodes.emplace_back(SegmentNode::Leaf(i, segment.GetLeafPositions()[i]));
		}
		std::sort(orderedNodes.begin(), orderedNodes.end());

		for (const SegmentNode& node : orderedNodes) {
			const uint64_t localSize = GetSize();
			if (node.position < localSize) {
				continue;
			}

			const Index index = Index::At(node.position);
			if (pBitmap != nullptr) {
				if (node.isHash) {
					if (IsRequiredByBitmap(*pBitmap, index, targetMMRSize)) {
						continue;
					}

					const uint64_t subtreeStart = GetSubtreeFirstPosition(index);
					if (subtreeStart < localSize) {
						if (subtreeStart == 0 && localSize == 1) {
							RewindToEmpty();
						} else {
							LOG_WARNING(StringUtil::Format(
								"Pruneable PIBD segment {}:{} pruned root at {} starts at {}. Expected local PMMR size {}.",
								segment.GetIdentifier().GetHeight(),
								segment.GetIdentifier().GetIndex(),
								node.position,
								subtreeStart,
								localSize));
							return false;
						}
					} else if (subtreeStart != localSize) {
						LOG_WARNING(StringUtil::Format(
							"Pruneable PIBD segment {}:{} pruned root at {} starts at {}. Expected local PMMR size {}.",
							segment.GetIdentifier().GetHeight(),
							segment.GetIdentifier().GetIndex(),
							node.position,
							subtreeStart,
							localSize));
						return false;
					}

					AppendPrunedSubtree(segment.GetHashes()[node.index], node.position);
					continue;
				}

				if (!IsLeafRequiredByBitmap(*pBitmap, index, targetMMRSize)) {
					continue;
				}
			}

			if (node.isHash) {
				AppendPrunedSubtree(segment.GetHashes()[node.index], node.position);
			} else if (node.position == localSize) {
				AppendLeaf(segment.GetLeaves()[node.index]);
			} else {
				LOG_WARNING_F("Pruneable PIBD segment {}:{} leaf starts at {}. Expected local PMMR size {}.",
					segment.GetIdentifier().GetHeight(),
					segment.GetIdentifier().GetIndex(),
					node.position,
					localSize);
				return false;
			}
		}

		return true;
	}

	void UpdateLeafSet(const BitmapAccumulator& bitmapAccumulator, const uint64_t numLeaves)
	{
		SetDirty(true);

		const std::vector<BitmapChunk>& chunks = bitmapAccumulator.GetChunks();
		for (uint64_t leafIdx = 0; leafIdx < numLeaves; ++leafIdx) {
			const uint64_t chunkIdx = leafIdx / BitmapChunk::LEN_BITS;
			const bool shouldBeUnspent = chunkIdx < chunks.size()
				&& chunks[chunkIdx].IsSet(leafIdx % BitmapChunk::LEN_BITS);
			const LeafIndex leafIndex = LeafIndex::At(leafIdx);

			if (shouldBeUnspent) {
				m_pLeafSet->Add(leafIndex);
			} else if (m_pLeafSet->Contains(leafIndex)) {
				m_pLeafSet->Remove(leafIndex);
			}
		}

		m_pLeafSet->Rewind(numLeaves, {});
	}

	void Remove(const LeafIndex& leaf_idx)
	{
		LOG_TRACE_F("Spending output at {}", leaf_idx);

		if (!m_pLeafSet->Contains(leaf_idx)) {
			LOG_WARNING_F("LeafSet does not contain output: {}", leaf_idx);
			throw TXHASHSET_EXCEPTION(StringUtil::Format("LeafSet does not contain output: {}", leaf_idx));
		}

		SetDirty(true);
		m_pLeafSet->Remove(leaf_idx);
	}

	void Rewind(const uint64_t num_leaves, const std::vector<uint64_t>& leavesToAdd)
	{
		SetDirty(true);

		LeafIndex next_leaf = LeafIndex::At(num_leaves);
		m_pHashFile->Rewind(next_leaf.GetPosition() - m_pPruneList->GetShift(next_leaf.GetIndex() - 1));
		m_pDataFile->Rewind(num_leaves - m_pPruneList->GetLeafShift(next_leaf.GetIndex() - 1));
		m_pLeafSet->Rewind(num_leaves, leavesToAdd);
	}

	void ResetToEmpty()
	{
		SetDirty(true);
		m_pHashFile->Rewind(0);
		m_pDataFile->Rewind(0);
		m_pLeafSet->Rewind(0, {});
	}

	Hash Root(const uint64_t size) const final
	{
		return MMRHashUtil::Root(m_pHashFile, size, m_pPruneList);
	}

	Hash UBMTRoot(const uint64_t size) const
	{
		return m_pLeafSet->Root(size);
	}

	uint64_t GetSize() const final
	{
		return m_pPruneList->GetTotalShift() + m_pHashFile->GetSize();
	}

	std::unique_ptr<Hash> GetHashAt(const Index& mmrIndex) const final
	{
		if (m_pPruneList->IsCompacted(mmrIndex)) {
			return nullptr;
		}

		Hash hash = MMRHashUtil::GetHashAt(m_pHashFile, mmrIndex, m_pPruneList);
		return std::make_unique<Hash>(std::move(hash));
	}

	std::unique_ptr<Hash> GetSegmentHashAt(const Index& mmrIndex) const
	{
		return GetHashAt(mmrIndex);
	}

	std::vector<Hash> GetLastLeafHashes(const uint64_t numHashes) const final
	{
		return MMRHashUtil::GetLastLeafHashes(m_pHashFile, m_pLeafSet, m_pPruneList, numHashes);
	}

	bool IsUnpruned(const LeafIndex& leaf_idx) const
	{
		return leaf_idx.GetPosition() < GetSize() && m_pLeafSet->Contains(leaf_idx);
	}

	bool IsCompacted(const Index& index) const
	{
		return m_pPruneList->IsCompacted(index);
	}

	std::unique_ptr<DATA_TYPE> GetAt(const LeafIndex& leaf_idx) const
	{
		if (IsUnpruned(leaf_idx)) {
			return GetDataAt(leaf_idx);
		}

		return std::unique_ptr<DATA_TYPE>(nullptr);
	}

	std::unique_ptr<DATA_TYPE> GetDataAt(const LeafIndex& leaf_idx) const
	{
		if (leaf_idx.GetPosition() >= GetSize() || m_pPruneList->IsCompacted(leaf_idx.GetIndex())) {
			return std::unique_ptr<DATA_TYPE>(nullptr);
		}

		// Match grin's PMMRBackend::get_data_from_file(): leaf data is read
		// with get_leaf_shift(1 + pos0), not get_leaf_shift(pos0).
		uint64_t shift = m_pPruneList->GetLeafShift(Index::At(leaf_idx.GetPosition() + 1));
		uint64_t shifted_idx = leaf_idx.Get() - shift;

		try {
			std::vector<unsigned char> data = m_pDataFile->GetDataAt(shifted_idx);
			if (data.size() == DATA_SIZE) {
				ByteBuffer byteBuffer(std::move(data));
				return std::make_unique<DATA_TYPE>(DATA_TYPE::Deserialize(byteBuffer));
			}
		}
		catch (FileException&) {
			return std::unique_ptr<DATA_TYPE>(nullptr);
		}

		return std::unique_ptr<DATA_TYPE>(nullptr);
	}

	void Commit() final
	{
		if (IsDirty())
		{
			LOG_TRACE_F("Flushing with size ({})", GetSize());
			m_pHashFile->Commit();
			m_pDataFile->Commit();
			m_pLeafSet->Commit();
			m_pPruneList->Flush();
			SetDirty(false);
		}
	}

	void Rollback() noexcept final
	{
		if (IsDirty())
		{
			LOG_INFO("Discarding changes since last flush");
			m_pHashFile->Rollback();
			m_pDataFile->Rollback();
			m_pLeafSet->Rollback();
			m_pPruneList->Rollback();
			SetDirty(false);
		}
	}

	void FlushPruneList()
	{
		m_pPruneList->Flush();
	}

private:
	struct SegmentNode
	{
		size_t index;
		uint64_t position;
		bool isHash;

		static SegmentNode Hash(const size_t index, const uint64_t position) noexcept
		{
			return SegmentNode{ index, position, true };
		}

		static SegmentNode Leaf(const size_t index, const uint64_t position) noexcept
		{
			return SegmentNode{ index, position, false };
		}

		bool operator<(const SegmentNode& other) const noexcept
		{
			if (position != other.position) {
				return position < other.position;
			}

			return isHash && !other.isHash;
		}
	};

	static bool BitmapContains(const BitmapAccumulator& bitmap, const uint64_t bitIndex)
	{
		const uint64_t chunkIndex = BitmapAccumulator::ChunkIndex(bitIndex);
		const std::vector<BitmapChunk>& chunks = bitmap.GetChunks();
		if (chunkIndex >= chunks.size()) {
			return false;
		}

		return chunks[chunkIndex].IsSet(bitIndex % BitmapChunk::LEN_BITS);
	}

	static uint64_t GetSubtreeFirstPosition(const Index& index) noexcept
	{
		const uint64_t nodeCount = (1ULL << (index.GetHeight() + 1)) - 1;
		return index.GetPosition() + 1 - nodeCount;
	}

	static uint64_t GetFirstLeafIndex(Index index) noexcept
	{
		while (!index.IsLeaf()) {
			index = index.GetLeftChild();
		}

		return index.GetLeafIndex();
	}

	static bool IsRequiredByBitmap(const BitmapAccumulator& bitmap, const Index& index, const uint64_t mmrSize)
	{
		const uint64_t firstLeaf = GetFirstLeafIndex(index);
		const uint64_t subtreeLeafCount = 1ULL << index.GetHeight();
		const uint64_t maxLeafCount = MMRUtil::CountLeaves(mmrSize);
		const uint64_t lastLeaf = (std::min)(maxLeafCount, firstLeaf + subtreeLeafCount);

		for (uint64_t leafIndex = firstLeaf; leafIndex < lastLeaf; ++leafIndex) {
			if (BitmapContains(bitmap, leafIndex)) {
				return true;
			}
		}

		return false;
	}

	static bool IsLeafRequiredByBitmap(const BitmapAccumulator& bitmap, const Index& index, const uint64_t mmrSize)
	{
		const uint64_t leafIndex = index.GetLeafIndex();
		if (BitmapContains(bitmap, leafIndex) || index.GetPosition() == mmrSize - 1) {
			return true;
		}

		const uint64_t siblingLeafIndex = MMRUtil::IsLeftSibling(index.GetPosition())
			? leafIndex + 1
			: (leafIndex == 0 ? leafIndex : leafIndex - 1);
		return siblingLeafIndex < MMRUtil::CountLeaves(mmrSize) && BitmapContains(bitmap, siblingLeafIndex);
	}

	void RewindToEmpty()
	{
		m_pHashFile->Rewind(0);
		m_pDataFile->Rewind(0);
		m_pLeafSet->Rewind(0, {});
	}

	void AppendLeaf(const DATA_TYPE& leaf)
	{
		const uint64_t position = GetSize();
		const Index index = Index::At(position);
		m_pLeafSet->Add(LeafIndex::From(index));

		std::vector<uint8_t> serialized = leaf.Serialized();
		m_pDataFile->AddData(serialized);
		MMRHashUtil::AddHashes(m_pHashFile, serialized, m_pPruneList);
	}

	void AppendPrunedSubtree(const Hash& hash, const uint64_t position)
	{
		m_pHashFile->AddData(hash);
		m_pPruneList->AddPrunedRoot(Index::At(position));

		uint64_t currentPosition = position;
		Hash currentHash = hash;
		for (Index parent = Index::At(currentPosition + 1); !parent.IsLeaf(); parent++) {
			if (parent.GetRightChild().GetPosition() != currentPosition) {
				break;
			}

			const Hash leftHash = MMRHashUtil::GetHashAt(m_pHashFile, parent.GetLeftChild(), m_pPruneList);
			currentHash = MMRHashUtil::HashParentWithIndex(leftHash, currentHash, parent.GetPosition());
			m_pHashFile->AddData(currentHash);
			currentPosition = parent.GetPosition();
		}
	}

	std::shared_ptr<HashFile> m_pHashFile;
	std::shared_ptr<LeafSet> m_pLeafSet;
	std::shared_ptr<PruneList> m_pPruneList;
	std::shared_ptr<DataFile<DATA_SIZE>> m_pDataFile;
};
