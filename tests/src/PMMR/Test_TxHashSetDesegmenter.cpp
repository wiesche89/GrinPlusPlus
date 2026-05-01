#include <catch.hpp>

#include <Core/Models/BlockHeader.h>
#include <PMMR/Common/Index.h>
#include <PMMR/TxHashSetDesegmenter.h>

TEST_CASE("TxHashSetDesegmenter counts bitmap leaves in PIBD progress")
{
	const uint64_t outputLeaves = BitmapChunk::LEN_BITS + 1;
	const uint64_t kernelLeaves = 3;
	const uint64_t outputMMRSize = LeafIndex::At(outputLeaves).GetPosition();
	const uint64_t kernelMMRSize = LeafIndex::At(kernelLeaves).GetPosition();

	BlockHeader header(
		1,
		100,
		0,
		Hash(),
		Hash(),
		Hash(),
		Hash(),
		Hash(),
		BlindingFactor(),
		outputMMRSize,
		kernelMMRSize,
		1,
		1,
		0,
		ProofOfWork((uint8_t)10, std::vector<uint64_t>{})
	);

	TxHashSetDesegmenter desegmenter(std::move(header));

	const uint64_t bitmapLeaves = 2;
	REQUIRE(desegmenter.GetCompletedLeaves() == 0);
	REQUIRE(desegmenter.GetLeavesRequired() == bitmapLeaves + (outputLeaves * 2) + kernelLeaves);
	REQUIRE_FALSE(desegmenter.IsApplied(SegmentTypeIdentifier(SegmentType::Output, SegmentIdentifier(11, 0))));
}
