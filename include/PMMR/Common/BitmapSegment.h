#pragma once

#include <PMMR/Common/BitmapAccumulator.h>
#include <PMMR/Common/LeafIndex.h>
#include <PMMR/Common/Segment.h>
#include <PMMR/Common/SegmentId.h>
#include <PMMR/Common/SegmentProof.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Traits/Serializable.h>
#include <Core/Exceptions/DeserializationException.h>
#include <Common/Util/StringUtil.h>

#include <cstdint>
#include <array>
#include <algorithm>
#include <vector>
#include <utility>

class BitmapSegmentBlock : public Traits::ISerializable
{
public:
    enum class ESerializationMode : uint8_t
    {
        Raw = 0,
        SetPositions = 1,
        UnsetPositions = 2
    };

    BitmapSegmentBlock() = default;
    BitmapSegmentBlock(
        uint8_t chunkCount,
        ESerializationMode mode,
        std::vector<uint16_t> positions,
        std::vector<uint8_t> bitmapBytes)
        : m_chunkCount(chunkCount),
        m_mode(mode),
        m_positions(std::move(positions)),
        m_bitmapBytes(std::move(bitmapBytes))
    {
    }

    BitmapSegmentBlock(const BitmapSegmentBlock& other) = default;
    BitmapSegmentBlock(BitmapSegmentBlock&& other) noexcept = default;
    virtual ~BitmapSegmentBlock() = default;

    BitmapSegmentBlock& operator=(const BitmapSegmentBlock& other) = default;
    BitmapSegmentBlock& operator=(BitmapSegmentBlock&& other) noexcept = default;

    uint8_t GetChunkCount() const noexcept { return m_chunkCount; }
    ESerializationMode GetMode() const noexcept { return m_mode; }
    const std::vector<uint16_t>& GetPositions() const noexcept { return m_positions; }
    const std::vector<uint8_t>& GetBitmapBytes() const noexcept { return m_bitmapBytes; }

    static BitmapSegmentBlock FromChunks(const std::vector<BitmapChunk>& chunks)
    {
        const uint8_t chunkCount = static_cast<uint8_t>(chunks.size());
        const size_t nBits = chunks.size() * BitmapChunk::LEN_BITS;
        const uint32_t threshold = (1 << 16) / 16;

        std::vector<uint16_t> setPositions;
        std::vector<uint16_t> unsetPositions;
        setPositions.reserve(threshold);
        unsetPositions.reserve(threshold);

        for (size_t bitIdx = 0; bitIdx < nBits; ++bitIdx) {
            const bool set = chunks[bitIdx / BitmapChunk::LEN_BITS].IsSet(bitIdx % BitmapChunk::LEN_BITS);
            if (set && setPositions.size() < threshold) {
                setPositions.push_back(static_cast<uint16_t>(bitIdx));
            } else if (!set && unsetPositions.size() < threshold) {
                unsetPositions.push_back(static_cast<uint16_t>(bitIdx));
            }
        }

        if (setPositions.size() < threshold) {
            return BitmapSegmentBlock(chunkCount, ESerializationMode::SetPositions, std::move(setPositions), {});
        }

        if (unsetPositions.size() < threshold) {
            return BitmapSegmentBlock(chunkCount, ESerializationMode::UnsetPositions, std::move(unsetPositions), {});
        }

        std::vector<uint8_t> bitmapBytes;
        bitmapBytes.reserve(chunks.size() * BitmapChunk::LEN_BYTES);
        for (const BitmapChunk& chunk : chunks) {
            const std::array<uint8_t, BitmapChunk::LEN_BYTES>& bytes = chunk.GetBytes();
            bitmapBytes.insert(bitmapBytes.end(), bytes.begin(), bytes.end());
        }

        return BitmapSegmentBlock(chunkCount, ESerializationMode::Raw, {}, std::move(bitmapBytes));
    }

    std::vector<BitmapChunk> ToChunks() const
    {
        std::vector<BitmapChunk> chunks(m_chunkCount);
        switch (m_mode)
        {
            case ESerializationMode::Raw:
            {
                const size_t expectedSize = static_cast<size_t>(m_chunkCount) * BitmapChunk::LEN_BYTES;
                if (m_bitmapBytes.size() != expectedSize) {
                    throw DESERIALIZATION_EXCEPTION_F(
                        "Invalid raw bitmap block size. Expected {}, got {}.",
                        expectedSize,
                        m_bitmapBytes.size());
                }

                for (size_t chunkIdx = 0; chunkIdx < m_chunkCount; ++chunkIdx) {
                    std::array<uint8_t, BitmapChunk::LEN_BYTES> bytes{};
                    std::copy_n(
                        m_bitmapBytes.begin() + (chunkIdx * BitmapChunk::LEN_BYTES),
                        BitmapChunk::LEN_BYTES,
                        bytes.begin());
                    chunks[chunkIdx] = BitmapChunk(std::move(bytes));
                }
                break;
            }
            case ESerializationMode::SetPositions:
            {
                for (const uint16_t position : m_positions) {
                    if (static_cast<size_t>(position) >= static_cast<size_t>(m_chunkCount) * BitmapChunk::LEN_BITS) {
                        throw DESERIALIZATION_EXCEPTION_F("Bitmap position {} exceeds block size.", position);
                    }
                    chunks[position / BitmapChunk::LEN_BITS].Set(position % BitmapChunk::LEN_BITS, true);
                }
                break;
            }
            case ESerializationMode::UnsetPositions:
            {
                for (BitmapChunk& chunk : chunks) {
                    for (uint64_t bitIdx = 0; bitIdx < BitmapChunk::LEN_BITS; ++bitIdx) {
                        chunk.Set(bitIdx, true);
                    }
                }

                for (const uint16_t position : m_positions) {
                    if (static_cast<size_t>(position) >= static_cast<size_t>(m_chunkCount) * BitmapChunk::LEN_BITS) {
                        throw DESERIALIZATION_EXCEPTION_F("Bitmap position {} exceeds block size.", position);
                    }
                    chunks[position / BitmapChunk::LEN_BITS].Set(position % BitmapChunk::LEN_BITS, false);
                }
                break;
            }
            default:
            {
                throw DESERIALIZATION_EXCEPTION("Invalid bitmap serialization mode");
            }
        }

        return chunks;
    }

    void Serialize(Serializer& serializer) const final
    {
        serializer.Append<uint8_t>(m_chunkCount);
        serializer.Append<uint8_t>(static_cast<uint8_t>(m_mode));

        switch (m_mode)
        {
        case ESerializationMode::Raw:
        {
            serializer.AppendByteVector(m_bitmapBytes);
            break;
        }
        case ESerializationMode::SetPositions:
        case ESerializationMode::UnsetPositions:
        {
            serializer.Append<uint16_t>(static_cast<uint16_t>(m_positions.size()));
            for (uint16_t position : m_positions)
            {
                serializer.Append<uint16_t>(position);
            }
            break;
        }
        default:
        {
            throw DESERIALIZATION_EXCEPTION_F(
                "Invalid bitmap serialization mode: {}",
                static_cast<uint8_t>(m_mode));
        }
        }
    }

    static BitmapSegmentBlock Deserialize(ByteBuffer& byteBuffer)
    {
        const uint8_t chunkCount = byteBuffer.Read<uint8_t>();
        const uint8_t modeValue = byteBuffer.Read<uint8_t>();

        if (modeValue > static_cast<uint8_t>(ESerializationMode::UnsetPositions)) {
            throw DESERIALIZATION_EXCEPTION_F("Invalid bitmap serialization mode: {}", modeValue);
        }

        const ESerializationMode mode = static_cast<ESerializationMode>(modeValue);

        switch (mode)
        {
            case ESerializationMode::Raw:
            {
                const size_t expectedSize = static_cast<size_t>(chunkCount) * 128;
                std::vector<uint8_t> bitmapBytes;
                bitmapBytes.reserve(expectedSize);
                for (size_t i = 0; i < expectedSize; ++i) {
                    bitmapBytes.push_back(byteBuffer.Read<uint8_t>());
                }

                return BitmapSegmentBlock(chunkCount, mode, {}, std::move(bitmapBytes));
            }
            case ESerializationMode::SetPositions:
            case ESerializationMode::UnsetPositions:
            {
                const uint16_t count = byteBuffer.Read<uint16_t>();
                std::vector<uint16_t> positions;
                positions.reserve(count);
                for (uint16_t i = 0; i < count; ++i) {
                    positions.push_back(byteBuffer.Read<uint16_t>());
                }

                return BitmapSegmentBlock(chunkCount, mode, std::move(positions), {});
            }
            default:
            {
                throw DESERIALIZATION_EXCEPTION("Invalid bitmap serialization mode");
            }
        }
    }

private:
    uint8_t m_chunkCount{ 0 };
    ESerializationMode m_mode{ ESerializationMode::Raw };
    std::vector<uint16_t> m_positions;
    std::vector<uint8_t> m_bitmapBytes;
};

class BitmapSegment : public Traits::ISerializable
{
public:
    BitmapSegment() = default;
    BitmapSegment(
        SegmentIdentifier identifier,
        std::vector<BitmapSegmentBlock> blocks,
        SegmentProof proof)
        : m_identifier(std::move(identifier)),
        m_blocks(std::move(blocks)),
        m_proof(std::move(proof))
    {
    }
    BitmapSegment(const BitmapSegment& other) = default;
    BitmapSegment(BitmapSegment&& other) noexcept = default;
    virtual ~BitmapSegment() = default;

    BitmapSegment& operator=(const BitmapSegment& other) = default;
    BitmapSegment& operator=(BitmapSegment&& other) noexcept = default;

    const SegmentIdentifier& GetIdentifier() const noexcept { return m_identifier; }
    const std::vector<BitmapSegmentBlock>& GetBlocks() const noexcept { return m_blocks; }
    const SegmentProof& GetProof() const noexcept { return m_proof; }

    static BitmapSegment FromSegment(const Segment<BitmapChunk::LEN_BYTES, BitmapChunk>& segment)
    {
        std::vector<BitmapSegmentBlock> blocks;
        const std::vector<BitmapChunk>& chunks = segment.GetLeaves();
        blocks.reserve((chunks.size() + 63) / 64);

        for (size_t offset = 0; offset < chunks.size(); offset += 64) {
            const size_t count = (std::min)(static_cast<size_t>(64), chunks.size() - offset);
            std::vector<BitmapChunk> blockChunks(chunks.begin() + offset, chunks.begin() + offset + count);
            blocks.push_back(BitmapSegmentBlock::FromChunks(blockChunks));
        }

        return BitmapSegment(segment.GetIdentifier(), std::move(blocks), segment.GetProof());
    }

    Segment<BitmapChunk::LEN_BYTES, BitmapChunk> ToSegment() const
    {
        std::vector<BitmapChunk> chunks;
        for (const BitmapSegmentBlock& block : m_blocks) {
            std::vector<BitmapChunk> blockChunks = block.ToChunks();
            chunks.insert(chunks.end(), blockChunks.begin(), blockChunks.end());
        }

        std::vector<uint64_t> leafPositions;
        leafPositions.reserve(chunks.size());
        const uint64_t leafOffset = m_identifier.GetLeafOffset();
        for (uint64_t i = 0; i < chunks.size(); ++i) {
            leafPositions.push_back(LeafIndex::At(leafOffset + i).GetPosition());
        }

        return Segment<BitmapChunk::LEN_BYTES, BitmapChunk>(
            m_identifier,
            {},
            {},
            std::move(leafPositions),
            std::move(chunks),
            m_proof);
    }

    void Serialize(Serializer& serializer) const final
    {
        serializer.Append(m_identifier);
        serializer.Append<uint16_t>(static_cast<uint16_t>(m_blocks.size()));
        for (const BitmapSegmentBlock& block : m_blocks) {
            block.Serialize(serializer);
        }
        m_proof.Serialize(serializer);
    }

    static BitmapSegment Deserialize(ByteBuffer& byteBuffer)
    {
        SegmentIdentifier identifier = SegmentIdentifier::Deserialize(byteBuffer);
        const uint16_t numBlocks = byteBuffer.Read<uint16_t>();

        std::vector<BitmapSegmentBlock> blocks;
        blocks.reserve(numBlocks);
        for (uint16_t i = 0; i < numBlocks; ++i) {
            blocks.emplace_back(BitmapSegmentBlock::Deserialize(byteBuffer));
        }

        SegmentProof proof = SegmentProof::Deserialize(byteBuffer);

        return BitmapSegment(
            std::move(identifier),
            std::move(blocks),
            std::move(proof));
    }

private:
    SegmentIdentifier m_identifier;
    std::vector<BitmapSegmentBlock> m_blocks;
    SegmentProof m_proof;
};
