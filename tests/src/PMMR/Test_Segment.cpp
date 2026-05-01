#include <catch.hpp>

#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Crypto/Hasher.h>
#include <PMMR/Common/Segment.h>
#include <PMMR/Common/SegmentId.h>

class TestSegmentLeaf : public Traits::ISerializable
{
	uint8_t m_value;

public:
	TestSegmentLeaf() : m_value(0) {}
	explicit TestSegmentLeaf(const uint8_t value) : m_value(value) {}

	uint8_t GetValue() const noexcept { return m_value; }

	void Serialize(Serializer& serializer) const final
	{
		serializer.Append<uint8_t>(m_value);
	}

	static TestSegmentLeaf Deserialize(ByteBuffer& byteBuffer)
	{
		return TestSegmentLeaf(byteBuffer.Read<uint8_t>());
	}
};

static Hash TestHashLeaf(const TestSegmentLeaf& leaf, const uint64_t mmrIndex)
{
	Serializer leafSerializer;
	leaf.Serialize(leafSerializer);

	Serializer hashSerializer;
	hashSerializer.Append<uint64_t>(mmrIndex);
	hashSerializer.AppendByteVector(leafSerializer.GetBytes());
	return Hasher::Blake2b(hashSerializer.GetBytes());
}

static Hash TestHashParent(const Hash& leftChild, const Hash& rightChild, const uint64_t parentIndex)
{
	Serializer serializer;
	serializer.Append<uint64_t>(parentIndex);
	serializer.AppendBigInteger<32>(leftChild);
	serializer.AppendBigInteger<32>(rightChild);
	return Hasher::Blake2b(serializer.GetBytes());
}

TEST_CASE("SegmentIdentifier calculates rust-compatible ranges")
{
	const SegmentIdentifier firstSegment(1, 0);
	REQUIRE(firstSegment.GetSegmentCapacity() == 2);
	REQUIRE(firstSegment.GetLeafOffset() == 0);
	REQUIRE(firstSegment.GetSegmentUnprunedSize(7) == 2);
	REQUIRE(firstSegment.GetPositionRange(7) == std::make_pair<uint64_t, uint64_t>(0, 2));

	const SegmentIdentifier secondSegment(1, 1);
	REQUIRE(secondSegment.GetLeafOffset() == 2);
	REQUIRE(secondSegment.GetSegmentUnprunedSize(7) == 2);
	REQUIRE(secondSegment.GetPositionRange(7) == std::make_pair<uint64_t, uint64_t>(3, 5));

	const SegmentIdentifier partialSegment(1, 2);
	REQUIRE(partialSegment.GetSegmentUnprunedSize(8) == 1);
	REQUIRE(partialSegment.GetPositionRange(8) == std::make_pair<uint64_t, uint64_t>(7, 7));
}

TEST_CASE("Segment serializes positions one-based on the wire")
{
	Segment<1, TestSegmentLeaf> segment(
		SegmentIdentifier(1, 0),
		std::vector<uint64_t>{ 0, 2 },
		std::vector<Hash>{ Hash::ValueOf(10), Hash::ValueOf(11) },
		std::vector<uint64_t>{ 1, 3 },
		std::vector<TestSegmentLeaf>{ TestSegmentLeaf(1), TestSegmentLeaf(2) },
		SegmentProof()
	);

	Serializer serializer;
	segment.Serialize(serializer);

	ByteBuffer byteBuffer(serializer.GetBytes());
	(void)byteBuffer.Read<uint8_t>();
	(void)byteBuffer.Read<uint64_t>();
	REQUIRE(byteBuffer.Read<uint64_t>() == 2);
	REQUIRE(byteBuffer.Read<uint64_t>() == 1);
	REQUIRE(byteBuffer.Read<uint64_t>() == 3);
}

TEST_CASE("Segment validates proof against MMR root")
{
	const TestSegmentLeaf leaf0(10);
	const TestSegmentLeaf leaf1(11);
	const TestSegmentLeaf leaf2(12);
	const TestSegmentLeaf leaf3(13);

	const Hash hash0 = TestHashLeaf(leaf0, 0);
	const Hash hash1 = TestHashLeaf(leaf1, 1);
	const Hash hash3 = TestHashLeaf(leaf2, 3);
	const Hash hash4 = TestHashLeaf(leaf3, 4);
	const Hash hash2 = TestHashParent(hash0, hash1, 2);
	const Hash hash5 = TestHashParent(hash3, hash4, 5);
	const Hash root = TestHashParent(hash2, hash5, 6);

	const Segment<1, TestSegmentLeaf> segment(
		SegmentIdentifier(1, 0),
		std::vector<uint64_t>{},
		std::vector<Hash>{},
		std::vector<uint64_t>{ 0, 1 },
		std::vector<TestSegmentLeaf>{ leaf0, leaf1 },
		SegmentProof(std::vector<Hash>{ hash5 })
	);

	REQUIRE(segment.Root(7).has_value());
	REQUIRE(segment.Root(7).value() == hash2);
	REQUIRE(segment.Validate(7, root));
}
