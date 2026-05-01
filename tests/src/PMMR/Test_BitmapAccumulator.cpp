#include <catch.hpp>

#include <Crypto/Hasher.h>
#include <PMMR/Common/BitmapAccumulator.h>
#include <PMMR/Common/BitmapSegment.h>

static Hash BitmapTestHashLeaf(const BitmapChunk& chunk, const uint64_t mmrIndex)
{
	Serializer leafSerializer;
	chunk.Serialize(leafSerializer);

	Serializer hashSerializer;
	hashSerializer.Append<uint64_t>(mmrIndex);
	hashSerializer.AppendByteVector(leafSerializer.GetBytes());
	return Hasher::Blake2b(hashSerializer.GetBytes());
}

static Hash BitmapTestHashParent(const Hash& leftChild, const Hash& rightChild, const uint64_t parentIndex)
{
	Serializer serializer;
	serializer.Append<uint64_t>(parentIndex);
	serializer.AppendBigInteger<32>(leftChild);
	serializer.AppendBigInteger<32>(rightChild);
	return Hasher::Blake2b(serializer.GetBytes());
}

TEST_CASE("BitmapChunk serializes 1024 bits with rust bit order")
{
	BitmapChunk chunk;
	chunk.Set(0, true);
	chunk.Set(9, true);

	Serializer serializer;
	chunk.Serialize(serializer);
	const std::vector<uint8_t>& bytes = serializer.GetBytes();

	REQUIRE(bytes.size() == BitmapChunk::LEN_BYTES);
	REQUIRE(bytes[0] == 0x80);
	REQUIRE(bytes[1] == 0x40);
}

TEST_CASE("BitmapSegmentBlock roundtrips sparse and dense blocks")
{
	BitmapChunk sparse;
	sparse.Set(0, true);
	sparse.Set(1023, true);

	BitmapSegmentBlock sparseBlock = BitmapSegmentBlock::FromChunks({ sparse });
	REQUIRE(sparseBlock.GetMode() == BitmapSegmentBlock::ESerializationMode::SetPositions);
	std::vector<BitmapChunk> sparseChunks = sparseBlock.ToChunks();
	REQUIRE(sparseChunks.size() == 1);
	REQUIRE(sparseChunks[0].IsSet(0));
	REQUIRE(sparseChunks[0].IsSet(1023));
	REQUIRE_FALSE(sparseChunks[0].IsSet(1));

	BitmapChunk dense;
	for (uint64_t bitIdx = 0; bitIdx < BitmapChunk::LEN_BITS; ++bitIdx) {
		dense.Set(bitIdx, true);
	}
	dense.Set(17, false);

	BitmapSegmentBlock denseBlock = BitmapSegmentBlock::FromChunks({ dense });
	REQUIRE(denseBlock.GetMode() == BitmapSegmentBlock::ESerializationMode::UnsetPositions);
	std::vector<BitmapChunk> denseChunks = denseBlock.ToChunks();
	REQUIRE(denseChunks.size() == 1);
	REQUIRE_FALSE(denseChunks[0].IsSet(17));
	REQUIRE(denseChunks[0].IsSet(18));
}

TEST_CASE("BitmapAccumulator root matches manual MMR root")
{
	BitmapChunk left;
	left.Set(0, true);
	BitmapChunk right;
	right.Set(1023, true);

	BitmapAccumulator accumulator;
	accumulator.AppendChunk(left);
	accumulator.AppendChunk(right);

	const Hash leftHash = BitmapTestHashLeaf(left, 0);
	const Hash rightHash = BitmapTestHashLeaf(right, 1);
	const Hash expectedRoot = BitmapTestHashParent(leftHash, rightHash, 2);

	REQUIRE(accumulator.GetMMRSize() == 3);
	REQUIRE(accumulator.Root() == expectedRoot);
}

TEST_CASE("BitmapSegment converts to PMMR segment")
{
	BitmapChunk first;
	first.Set(7, true);
	BitmapChunk second;
	second.Set(8, true);

	Segment<BitmapChunk::LEN_BYTES, BitmapChunk> segment(
		SegmentIdentifier(1, 0),
		{},
		{},
		std::vector<uint64_t>{ 0, 1 },
		std::vector<BitmapChunk>{ first, second },
		SegmentProof());

	BitmapSegment bitmapSegment = BitmapSegment::FromSegment(segment);
	Segment<BitmapChunk::LEN_BYTES, BitmapChunk> roundtrip = bitmapSegment.ToSegment();

	REQUIRE(roundtrip.GetLeafPositions() == std::vector<uint64_t>{ 0, 1 });
	REQUIRE(roundtrip.GetLeaves().size() == 2);
	REQUIRE(roundtrip.GetLeaves()[0].IsSet(7));
	REQUIRE(roundtrip.GetLeaves()[1].IsSet(8));
}
