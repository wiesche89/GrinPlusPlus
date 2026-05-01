#pragma once

#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Traits/Serializable.h>
#include <PMMR/Common/SegmentId.h>
#include <cstdint>
#include <utility>

enum class SegmentType : uint8_t
{
	OutputBitmap = 0,
	Output = 1,
	RangeProof = 2,
	Kernel = 3
};

class SegmentTypeIdentifier : public Traits::ISerializable
{
	SegmentType m_segmentType;
	SegmentIdentifier m_identifier;

public:
	SegmentTypeIdentifier()
		: m_segmentType(SegmentType::OutputBitmap), m_identifier()
	{
	}

	SegmentTypeIdentifier(SegmentType segmentType, SegmentIdentifier identifier)
		: m_segmentType(segmentType), m_identifier(std::move(identifier))
	{
	}

	SegmentType GetSegmentType() const noexcept { return m_segmentType; }
	const SegmentIdentifier& GetIdentifier() const noexcept { return m_identifier; }

	bool operator==(const SegmentTypeIdentifier& other) const noexcept
	{
		return m_segmentType == other.m_segmentType && m_identifier == other.m_identifier;
	}

	bool operator!=(const SegmentTypeIdentifier& other) const noexcept { return !(*this == other); }

	void Serialize(Serializer& serializer) const final
	{
		serializer.Append<uint8_t>(static_cast<uint8_t>(m_segmentType));
		m_identifier.Serialize(serializer);
	}

	static SegmentTypeIdentifier Deserialize(ByteBuffer& byteBuffer)
	{
		const SegmentType segmentType = static_cast<SegmentType>(byteBuffer.Read<uint8_t>());
		SegmentIdentifier identifier = SegmentIdentifier::Deserialize(byteBuffer);
		return SegmentTypeIdentifier(segmentType, std::move(identifier));
	}
};
