#pragma once

#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Traits/Serializable.h>
#include <Crypto/Hasher.h>
#include <Crypto/Models/Hash.h>
#include <PMMR/Common/Index.h>
#include <PMMR/Common/MMRUtil.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

class BitmapChunk : public Traits::ISerializable
{
public:
	static constexpr size_t LEN_BITS = 1024;
	static constexpr size_t LEN_BYTES = LEN_BITS / 8;

	BitmapChunk() = default;
	explicit BitmapChunk(std::array<uint8_t, LEN_BYTES> bytes)
		: m_bytes(std::move(bytes))
	{
	}

	bool Any() const noexcept
	{
		for (const uint8_t byte : m_bytes) {
			if (byte != 0) {
				return true;
			}
		}

		return false;
	}

	bool IsSet(const uint64_t bitIndex) const
	{
		const size_t byteIndex = static_cast<size_t>(bitIndex / 8);
		const uint8_t bitOffset = static_cast<uint8_t>(bitIndex % 8);
		return (m_bytes[byteIndex] & BitToByte(bitOffset)) != 0;
	}

	void Set(const uint64_t bitIndex, const bool value)
	{
		const size_t byteIndex = static_cast<size_t>(bitIndex / 8);
		const uint8_t bitOffset = static_cast<uint8_t>(bitIndex % 8);
		if (value) {
			m_bytes[byteIndex] |= BitToByte(bitOffset);
		} else {
			m_bytes[byteIndex] &= static_cast<uint8_t>(~BitToByte(bitOffset));
		}
	}

	const std::array<uint8_t, LEN_BYTES>& GetBytes() const noexcept { return m_bytes; }

	void Serialize(Serializer& serializer) const final
	{
		for (const uint8_t byte : m_bytes) {
			serializer.Append<uint8_t>(byte);
		}
	}

	static BitmapChunk Deserialize(ByteBuffer& byteBuffer)
	{
		std::array<uint8_t, LEN_BYTES> bytes{};
		for (size_t i = 0; i < LEN_BYTES; ++i) {
			bytes[i] = byteBuffer.Read<uint8_t>();
		}

		return BitmapChunk(std::move(bytes));
	}

private:
	static uint8_t BitToByte(const uint8_t bitOffset) noexcept
	{
		return static_cast<uint8_t>(1 << (7 - bitOffset));
	}

	std::array<uint8_t, LEN_BYTES> m_bytes{};
};

class BitmapAccumulator
{
public:
	static constexpr uint64_t NBITS = BitmapChunk::LEN_BITS;

	static uint64_t ChunkStartIndex(const uint64_t bitIndex) noexcept
	{
		return bitIndex & ~(NBITS - 1);
	}

	static uint64_t ChunkIndex(const uint64_t bitIndex) noexcept
	{
		return bitIndex / NBITS;
	}

	void Init(const std::vector<uint64_t>& setBitIndices, const uint64_t size)
	{
		m_chunks.clear();
		ApplyFrom(setBitIndices, 0, size);
	}

	void AppendChunk(BitmapChunk chunk)
	{
		m_chunks.push_back(chunk);
		AppendHashChunk(chunk);
	}

	const std::vector<BitmapChunk>& GetChunks() const noexcept { return m_chunks; }

	std::optional<Hash> GetHashAt(const uint64_t mmrPosition) const
	{
		if (mmrPosition >= m_nodes.size()) {
			return std::nullopt;
		}

		return m_nodes[mmrPosition];
	}

	uint64_t GetMMRSize() const noexcept
	{
		return m_nodes.size();
	}

	Hash Root() const
	{
		if (m_nodes.empty()) {
			return ZERO_HASH;
		}

		Hash root = ZERO_HASH;
		const std::vector<uint64_t> peakIndices = MMRUtil::GetPeakIndices(m_nodes.size());
		for (auto iter = peakIndices.crbegin(); iter != peakIndices.crend(); ++iter) {
			const Hash& peakHash = m_nodes[*iter];
			if (root == ZERO_HASH) {
				root = peakHash;
			} else {
				root = HashParentWithIndex(peakHash, root, m_nodes.size());
			}
		}

		return root;
	}

	void Build()
	{
		m_nodes.clear();
		for (const BitmapChunk& chunk : m_chunks) {
			AppendHashChunk(chunk);
		}
	}

private:
	static Hash HashLeafWithIndex(const BitmapChunk& chunk, const uint64_t mmrIndex)
	{
		Serializer leafSerializer;
		chunk.Serialize(leafSerializer);

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

	void ApplyFrom(const std::vector<uint64_t>& setBitIndices, const uint64_t fromIndex, const uint64_t size)
	{
		uint64_t chunkIndex = ChunkIndex(fromIndex);
		BitmapChunk chunk;

		for (const uint64_t bitIndex : setBitIndices) {
			if (bitIndex >= size) {
				break;
			}

			while (bitIndex >= (chunkIndex + 1) * NBITS) {
				m_chunks.push_back(chunk);
				++chunkIndex;
				chunk = BitmapChunk();
			}

			chunk.Set(bitIndex % NBITS, true);
		}

		if (chunk.Any() || !setBitIndices.empty()) {
			m_chunks.push_back(chunk);
		}

		Build();
	}

	void AppendHashChunk(const BitmapChunk& chunk)
	{
		const uint64_t leafPos = m_nodes.size();
		m_nodes.push_back(HashLeafWithIndex(chunk, leafPos));

		for (Index mmrIndex = Index::At(leafPos + 1); !mmrIndex.IsLeaf(); ++mmrIndex) {
			const Hash& leftHash = m_nodes[mmrIndex.GetLeftChild().GetPosition()];
			const Hash& rightHash = m_nodes[mmrIndex.GetRightChild().GetPosition()];
			m_nodes.push_back(HashParentWithIndex(leftHash, rightHash, mmrIndex.GetPosition()));
		}
	}

	std::vector<BitmapChunk> m_chunks;
	std::vector<Hash> m_nodes;
};
