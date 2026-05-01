#include <catch.hpp>

#include <PMMR/SegmentRequestTracker.h>

TEST_CASE("SegmentRequestTracker tracks pending and timed out segments")
{
	SegmentRequestTracker tracker;
	const SegmentTypeIdentifier segment(SegmentType::Kernel, SegmentIdentifier(11, 42));

	REQUIRE_FALSE(tracker.IsPending(segment));

	tracker.AddOrRefresh(segment, "peer-a");
	REQUIRE(tracker.IsPending(segment));
	REQUIRE(tracker.GetPendingRequests().size() == 1);
	REQUIRE(tracker.GetPendingRequests()[0].attempts == 1);

	tracker.AddOrRefresh(segment, "peer-b");
	REQUIRE(tracker.GetPendingRequests().size() == 1);
	REQUIRE(tracker.GetPendingRequests()[0].attempts == 2);
	REQUIRE(tracker.GetPendingRequests()[0].peerId == "peer-b");

	REQUIRE(tracker.GetTimedOutRequests(std::chrono::seconds(0)).size() == 1);
	tracker.MarkReceived(segment);
	REQUIRE_FALSE(tracker.IsPending(segment));
}
