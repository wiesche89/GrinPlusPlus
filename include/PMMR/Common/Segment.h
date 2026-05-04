#pragma once

#include <Crypto/Models/Hash.h>
#include <Crypto/Hasher.h>
#include <Core/Exceptions/DeserializationException.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Traits/Serializable.h>
#include <PMMR/Common/Index.h>
#include <PMMR/Common/BitmapAccumulator.h>
#include <PMMR/Common/SegmentId.h>
#include <PMMR/Common/SegmentProof.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

template<size_t DATA_SIZE, class DATA_TYPE>
class Segment : public Traits::ISerializable
{
    SegmentIdentifier m_id;
    std::vector<uint64_t> m_hash_pos;
    std::vector<Hash> m_hashes;
    std::vector<uint64_t> m_leaf_pos;
    std::vector<DATA_TYPE> m_leaves;
    SegmentProof m_proof;

    static Hash HashLeafWithIndex(const DATA_TYPE& leaf, const uint64_t mmrIndex)
    {
        Serializer leafSerializer;
        leaf.Serialize(leafSerializer);

        Serializer hashSerializer;
        hashSerializer.Append<uint64_t>(mmrIndex);
        hashSerializer.AppendByteVector(leafSerializer.GetBytes());
        return Hasher::Blake2b(hashSerializer.GetBytes());
    }

    static Hash HashParentWithIndex(const Hash& leftChild, const Hash& rightChild, const uint64_t parentIndex)
    {
        Serializer serializer;
        serializer.Append<uint64_t>(parentIndex);
        serializer.AppendBigInteger<32>(leftChild);
        serializer.AppendBigInteger<32>(rightChild);
        return Hasher::Blake2b(serializer.GetBytes());
    }

    std::optional<Hash> GetProvidedHash(const uint64_t position) const
    {
        const auto hashIter = std::find(m_hash_pos.begin(), m_hash_pos.end(), position);
        if (hashIter != m_hash_pos.end()) {
            return m_hashes[std::distance(m_hash_pos.begin(), hashIter)];
        }

        const auto leafIter = std::find(m_leaf_pos.begin(), m_leaf_pos.end(), position);
        if (leafIter != m_leaf_pos.end()) {
            return HashLeafWithIndex(m_leaves[std::distance(m_leaf_pos.begin(), leafIter)], position);
        }

        return std::nullopt;
    }

    std::optional<Hash> GetProvidedStoredHash(const uint64_t position) const
    {
        const auto hashIter = std::find(m_hash_pos.begin(), m_hash_pos.end(), position);
        if (hashIter == m_hash_pos.end()) {
            return std::nullopt;
        }

        return m_hashes[std::distance(m_hash_pos.begin(), hashIter)];
    }

    static bool BitmapContains(const BitmapAccumulator& bitmap, const uint64_t bitIndex)
    {
        const uint64_t chunkIndex = BitmapAccumulator::ChunkIndex(bitIndex);
        const std::vector<BitmapChunk>& chunks = bitmap.GetChunks();
        if (chunkIndex >= chunks.size()) {
            return false;
        }

        return chunks[chunkIndex].IsSet(bitIndex % BitmapChunk::LEN_BITS);
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

public:
    //
    // Constructors
    //
    Segment() = default;
    Segment(SegmentIdentifier id) : m_id(std::move(id)) { }
    Segment(
        SegmentIdentifier id,
        std::vector<uint64_t> hashPos,
        std::vector<Hash> hashes,
        std::vector<uint64_t> leafPos,
        std::vector<DATA_TYPE> leaves,
        SegmentProof proof)
        : m_id(std::move(id)),
        m_hash_pos(std::move(hashPos)),
        m_hashes(std::move(hashes)),
        m_leaf_pos(std::move(leafPos)),
        m_leaves(std::move(leaves)),
        m_proof(std::move(proof))
    {
    }
    Segment(const Segment& other) = default;
    Segment(Segment&& other) noexcept = default;

    //
    // Destructor
    //
    virtual ~Segment() = default;

    //
    // Operators
    //
    Segment& operator=(const Segment& other) = default;
    Segment& operator=(Segment&& other) noexcept = default;

    //
    // Getters
    //
    const SegmentIdentifier& GetIdentifier() const noexcept { return m_id; }
    uint8_t GetHeight() const noexcept { return m_id.GetHeight(); }
    uint64_t GetIndex() const noexcept { return m_id.GetIndex(); }
    const std::vector<uint64_t>& GetHashPositions() const noexcept { return m_hash_pos; }
    const std::vector<Hash>& GetHashes() const noexcept { return m_hashes; }
    const std::vector<uint64_t>& GetLeafPositions() const noexcept { return m_leaf_pos; }
    const std::vector<DATA_TYPE>& GetLeaves() const noexcept { return m_leaves; }
    const SegmentProof& GetProof() const noexcept { return m_proof; }

    std::pair<uint64_t, uint64_t> GetPositionRange(const uint64_t mmrSize) const noexcept
    {
        return m_id.GetPositionRange(mmrSize);
    }

    std::optional<Hash> Root(const uint64_t mmrSize, const BitmapAccumulator* pBitmap = nullptr) const
    {
        if (m_hash_pos.size() != m_hashes.size() || m_leaf_pos.size() != m_leaves.size()) {
            return std::nullopt;
        }

        const std::pair<uint64_t, uint64_t> posRange = GetPositionRange(mmrSize);
        if (posRange.first > posRange.second || mmrSize == 0) {
            return std::nullopt;
        }

        std::vector<std::optional<Hash>> hashes;
        hashes.reserve((size_t)(posRange.second - posRange.first + 1));

        for (uint64_t pos = posRange.first; pos <= posRange.second; ++pos) {
            const Index index = Index::At(pos);
            if (index.IsLeaf()) {
                if (pBitmap == nullptr || IsLeafRequiredByBitmap(*pBitmap, index, mmrSize)) {
                    const auto leafIter = std::find(m_leaf_pos.begin(), m_leaf_pos.end(), pos);
                    if (leafIter == m_leaf_pos.end()) {
                        return std::nullopt;
                    }

                    hashes.emplace_back(HashLeafWithIndex(m_leaves[std::distance(m_leaf_pos.begin(), leafIter)], pos));
                } else {
                    hashes.emplace_back(std::nullopt);
                }

                continue;
            }

            if (hashes.size() < 2) {
                return std::nullopt;
            }

            std::optional<Hash> rightHash = hashes.back();
            hashes.pop_back();
            std::optional<Hash> leftHash = hashes.back();
            hashes.pop_back();

            if (pBitmap != nullptr) {
                if (!leftHash.has_value() && !rightHash.has_value()) {
                    hashes.emplace_back(std::nullopt);
                    continue;
                }

                if (!leftHash.has_value()) {
                    leftHash = GetProvidedStoredHash(index.GetLeftChild().GetPosition());
                }
                if (!rightHash.has_value()) {
                    rightHash = GetProvidedStoredHash(index.GetRightChild().GetPosition());
                }
            }

            if (!leftHash.has_value() || !rightHash.has_value()) {
                return std::nullopt;
            }

            hashes.emplace_back(HashParentWithIndex(leftHash.value(), rightHash.value(), pos));
        }

        const uint64_t segmentSize = m_id.GetSegmentUnprunedSize(mmrSize);
        if (segmentSize == m_id.GetSegmentCapacity()) {
            return hashes.empty() ? std::nullopt : hashes.back();
        }

        std::optional<Hash> root;
        const std::vector<uint64_t> peaks = MMRUtil::GetPeakIndices(mmrSize);
        for (auto iter = peaks.crbegin(); iter != peaks.crend(); ++iter) {
            const uint64_t peakPos = *iter;
            if (peakPos < posRange.first || peakPos > posRange.second) {
                continue;
            }

            if (hashes.empty()) {
                return std::nullopt;
            }

            std::optional<Hash> peakHash = hashes.back();
            hashes.pop_back();
            if (!peakHash.has_value() && pBitmap != nullptr) {
                peakHash = GetProvidedStoredHash(peakPos);
            }
            if (!peakHash.has_value()) {
                return std::nullopt;
            }

            root = root.has_value()
                ? std::optional<Hash>(HashParentWithIndex(peakHash.value(), root.value(), mmrSize))
                : peakHash;
        }

        return root;
    }

    std::optional<std::pair<Hash, uint64_t>> FirstUnprunedParent(
        const uint64_t mmrSize,
        const BitmapAccumulator* pBitmap = nullptr) const
    {
        const std::optional<Hash> root = Root(mmrSize, pBitmap);
        const std::pair<uint64_t, uint64_t> posRange = GetPositionRange(mmrSize);
        if (root.has_value()) {
            return std::make_pair(root.value(), posRange.second + 1);
        }

        if (pBitmap == nullptr) {
            return std::nullopt;
        }

        const uint64_t segmentSize = m_id.GetSegmentUnprunedSize(mmrSize);
        if (segmentSize != m_id.GetSegmentCapacity()) {
            return std::nullopt;
        }

        if (IsRequiredByBitmap(*pBitmap, Index::At(posRange.second), mmrSize)) {
            return std::nullopt;
        }

        if (const std::optional<Hash> hash = GetProvidedStoredHash(posRange.second)) {
            return std::make_pair(hash.value(), posRange.second + 1);
        }

        for (const std::pair<uint64_t, uint64_t>& entry : MMRUtil::FamilyBranch(posRange.second, mmrSize)) {
            const uint64_t parentPos = entry.first;
            if (IsRequiredByBitmap(*pBitmap, Index::At(parentPos), mmrSize)) {
                break;
            }

            if (const std::optional<Hash> hash = GetProvidedStoredHash(parentPos)) {
                return std::make_pair(hash.value(), parentPos + 1);
            }
        }

        return std::nullopt;
    }

    bool Validate(const uint64_t mmrSize, const Hash& mmrRoot, const BitmapAccumulator* pBitmap = nullptr) const
    {
        const std::optional<std::pair<Hash, uint64_t>> firstUnprunedParent = FirstUnprunedParent(mmrSize, pBitmap);
        if (!firstUnprunedParent.has_value()) {
            return false;
		}

		const std::pair<uint64_t, uint64_t> posRange = GetPositionRange(mmrSize);
		return m_proof.Validate(mmrSize, posRange.first, posRange.second, firstUnprunedParent->first, firstUnprunedParent->second, mmrRoot);
	}

    bool ValidateWith(
        const uint64_t mmrSize,
        const Hash& mmrRoot,
        const uint64_t hashLastPos,
        const Hash& otherRoot,
        const bool otherIsLeft,
        const BitmapAccumulator* pBitmap = nullptr) const
    {
        const std::optional<std::pair<Hash, uint64_t>> firstUnprunedParent = FirstUnprunedParent(mmrSize, pBitmap);
        if (!firstUnprunedParent.has_value()) {
            return false;
        }

        const std::pair<uint64_t, uint64_t> posRange = GetPositionRange(mmrSize);
        return m_proof.ValidateWith(
            mmrSize,
            posRange.first,
            posRange.second,
            firstUnprunedParent->first,
            firstUnprunedParent->second,
            hashLastPos,
            otherRoot,
            otherIsLeft,
            mmrRoot);
    }

    //
    // Serialization/Deserialization
    //
    void Serialize(Serializer& serializer) const final
    {
        serializer.Append(m_id);
        serializer.Append<uint64_t>(m_hash_pos.size());
        for (const uint64_t pos : m_hash_pos) {
            serializer.Append<uint64_t>(pos + 1);
        }
        for (const Hash& hash : m_hashes) {
            serializer.AppendBigInteger<32>(hash);
        }
        serializer.Append<uint64_t>(m_leaf_pos.size());
        for (const uint64_t pos : m_leaf_pos) {
            serializer.Append<uint64_t>(pos + 1);
        }
        for (const DATA_TYPE& leaf : m_leaves) {
            leaf.Serialize(serializer);
        }
        m_proof.Serialize(serializer);
    }

    static Segment Deserialize(ByteBuffer& byteBuffer)
    {
        SegmentIdentifier segmentId = SegmentIdentifier::Deserialize(byteBuffer);
        const uint64_t numHashes = byteBuffer.Read<uint64_t>();
        std::vector<uint64_t> hashPos(numHashes);
        uint64_t lastWirePos = 0;
        for (uint64_t i = 0; i < numHashes; ++i) {
            const uint64_t wirePos = byteBuffer.Read<uint64_t>();
            if (wirePos <= lastWirePos) {
                throw DESERIALIZATION_EXCEPTION("Segment hash positions are not strictly sorted.");
            }
            lastWirePos = wirePos;
            hashPos[i] = wirePos - 1;
        }
        std::vector<Hash> hashes;
        hashes.reserve(numHashes);
        for (uint64_t i = 0; i < numHashes; ++i) {
            hashes.push_back(byteBuffer.ReadBigInteger<32>());
        }
        const uint64_t numLeaves = byteBuffer.Read<uint64_t>();
        std::vector<uint64_t> leafPos(numLeaves);
        lastWirePos = 0;
        for (uint64_t i = 0; i < numLeaves; ++i) {
            const uint64_t wirePos = byteBuffer.Read<uint64_t>();
            if (wirePos <= lastWirePos) {
                throw DESERIALIZATION_EXCEPTION("Segment leaf positions are not strictly sorted.");
            }
            lastWirePos = wirePos;
            leafPos[i] = wirePos - 1;
        }
        std::vector<DATA_TYPE> leaves;
        leaves.reserve(numLeaves);
        for (uint64_t i = 0; i < numLeaves; ++i) {
            leaves.push_back(DATA_TYPE::Deserialize(byteBuffer));
        }
        SegmentProof segmentProof = SegmentProof::Deserialize(byteBuffer);
        return Segment(
            std::move(segmentId),
            std::move(hashPos),
            std::move(hashes),
            std::move(leafPos),
            std::move(leaves),
            std::move(segmentProof)
        );
    }
};
