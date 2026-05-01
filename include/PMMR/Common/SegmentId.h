#pragma once

#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Traits/Serializable.h>
#include <PMMR/Common/LeafIndex.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

class SegmentIdentifier : public Traits::ISerializable
{
	uint8_t m_height;
	uint64_t m_index;

	static uint64_t CountLeaves(const uint64_t mmrSize) noexcept
	{
		return mmrSize == 0 ? 0 : LeafIndex::AtPos(mmrSize).Get();
	}

public:
	//
	// Constructors
	//
	SegmentIdentifier() : m_height(0), m_index(0) {}
	SegmentIdentifier(uint8_t height, uint64_t index) : m_height(height), m_index(index) {}
	SegmentIdentifier(const SegmentIdentifier& other) = default;
	SegmentIdentifier(SegmentIdentifier&& other) noexcept = default;

	//
	// Destructor
	//
	virtual ~SegmentIdentifier() = default;

	//
	// Operators
	//
	SegmentIdentifier& operator=(const SegmentIdentifier& other) = default;
	SegmentIdentifier& operator=(SegmentIdentifier&& other) noexcept = default;
	bool operator==(const SegmentIdentifier& other) const noexcept { return m_height == other.m_height && m_index == other.m_index; }
	bool operator!=(const SegmentIdentifier& other) const noexcept { return !(*this == other); }
	bool operator<(const SegmentIdentifier& other) const noexcept
	{
		return m_height == other.m_height ? m_index < other.m_index : m_height < other.m_height;
	}

	//
	// Getters
	//
	uint8_t GetHeight() const noexcept { return m_height; }
	uint64_t GetIndex() const noexcept { return m_index; }

	uint64_t GetSegmentCapacity() const noexcept { return 1ULL << m_height; }
	uint64_t GetLeafOffset() const noexcept { return m_index * GetSegmentCapacity(); }

	uint64_t GetSegmentUnprunedSize(const uint64_t mmrSize) const noexcept
	{
		const uint64_t numLeaves = CountLeaves(mmrSize);
		const uint64_t leafOffset = GetLeafOffset();
		if (leafOffset >= numLeaves) {
			return 0;
		}

		return std::min(GetSegmentCapacity(), numLeaves - leafOffset);
	}

	std::pair<uint64_t, uint64_t> GetPositionRange(const uint64_t mmrSize) const noexcept
	{
		const uint64_t segmentSize = GetSegmentUnprunedSize(mmrSize);
		if (segmentSize == 0) {
			return { 0, 0 };
		}

		const uint64_t first = LeafIndex::At(GetLeafOffset()).GetPosition();
		if (segmentSize == GetSegmentCapacity()) {
			const uint64_t lastLeafPos = LeafIndex::At(GetLeafOffset() + segmentSize - 1).GetPosition();
			return { first, lastLeafPos + m_height };
		}

		return { first, mmrSize - 1 };
	}

	static size_t CountSegmentsRequired(const uint64_t targetMMRSize, const uint8_t height) noexcept
	{
		const uint64_t numLeaves = CountLeaves(targetMMRSize);
		const uint64_t segmentCapacity = 1ULL << height;
		return (numLeaves + segmentCapacity - 1) / segmentCapacity;
	}

	static uint64_t GetPMMRSize(const uint64_t numSegments, const uint8_t height) noexcept
	{
		if (numSegments == 0) {
			return 0;
		}

		const uint64_t numLeaves = numSegments * (1ULL << height);
		return LeafIndex::At(numLeaves).GetPosition();
	}

	//
	// Serialization/Deserialization
	//
	void Serialize(Serializer& serializer) const final
	{
		serializer.Append(m_height);
		serializer.Append(m_index);
	}

	static SegmentIdentifier Deserialize(ByteBuffer& byteBuffer)
	{
		const uint8_t height = byteBuffer.Read<uint8_t>();
		const uint64_t index = byteBuffer.Read<uint64_t>();
		return SegmentIdentifier(height, index);
	}
};
