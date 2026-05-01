#pragma once

#include "Message.h"

#include <Core/Models/BlockHeader.h>
#include <PMMR/Common/SegmentId.h>
#include <utility>
#include <vector>

class GetHeaderSegmentMessage : public IMessage
{
public:
	GetHeaderSegmentMessage(SegmentIdentifier identifier)
		: m_identifier(std::move(identifier))
	{
	}

	IMessagePtr Clone() const final { return IMessagePtr(new GetHeaderSegmentMessage(*this)); }

	MessageTypes::EMessageType GetMessageType() const final { return MessageTypes::GetHeaderSegment; }
	const SegmentIdentifier& GetIdentifier() const noexcept { return m_identifier; }

	static GetHeaderSegmentMessage Deserialize(ByteBuffer& byteBuffer)
	{
		return GetHeaderSegmentMessage(SegmentIdentifier::Deserialize(byteBuffer));
	}

protected:
	void SerializeBody(Serializer& serializer) const final
	{
		serializer.Append(m_identifier);
	}

private:
	SegmentIdentifier m_identifier;
};

class HeaderSegmentMessage : public IMessage
{
public:
	HeaderSegmentMessage(SegmentIdentifier identifier, std::vector<BlockHeaderPtr>&& headers)
		: m_identifier(std::move(identifier)), m_headers(std::move(headers))
	{
	}

	IMessagePtr Clone() const final { return IMessagePtr(new HeaderSegmentMessage(*this)); }

	MessageTypes::EMessageType GetMessageType() const final { return MessageTypes::HeaderSegment; }
	const SegmentIdentifier& GetIdentifier() const noexcept { return m_identifier; }
	const std::vector<BlockHeaderPtr>& GetHeaders() const noexcept { return m_headers; }

	static HeaderSegmentMessage Deserialize(ByteBuffer& byteBuffer)
	{
		SegmentIdentifier identifier = SegmentIdentifier::Deserialize(byteBuffer);
		std::vector<BlockHeaderPtr> headers;

		const uint16_t numHeaders = byteBuffer.ReadU16();
		for (uint16_t i = 0; i < numHeaders; ++i) {
			headers.emplace_back(std::make_shared<const BlockHeader>(BlockHeader::Deserialize(byteBuffer)));
		}

		return HeaderSegmentMessage(std::move(identifier), std::move(headers));
	}

protected:
	void SerializeBody(Serializer& serializer) const final
	{
		serializer.Append(m_identifier);
		serializer.Append<uint16_t>((uint16_t)m_headers.size());
		for (const BlockHeaderPtr& pHeader : m_headers) {
			pHeader->Serialize(serializer);
		}
	}

private:
	SegmentIdentifier m_identifier;
	std::vector<BlockHeaderPtr> m_headers;
};
