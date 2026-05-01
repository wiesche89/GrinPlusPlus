#pragma once

#include <cstddef>
#include <cstdint>

namespace PIBD
{
	static constexpr uint8_t BITMAP_SEGMENT_HEIGHT = 9;
	static constexpr uint8_t OUTPUT_SEGMENT_HEIGHT = 11;
	static constexpr uint8_t RANGEPROOF_SEGMENT_HEIGHT = 11;
	static constexpr uint8_t KERNEL_SEGMENT_HEIGHT = 11;

	static constexpr size_t OUTPUT_DATA_SIZE = 34;
	static constexpr size_t RANGE_PROOF_DATA_SIZE = 683;
	static constexpr size_t KERNEL_DATA_SIZE = 114;

	static constexpr uint8_t MAX_CACHED_SEGMENTS = 30;
	static constexpr uint8_t SEGMENT_APPLY_BATCH_SIZE = 12;
	// Keep the in-flight window conservative so a single PIBD peer is not
	// overloaded and slow responses are not retried prematurely.
	static constexpr uint8_t SEGMENT_REQUEST_COUNT = 15;

	static constexpr uint16_t SEGMENT_REQUEST_TIMEOUT_SECS = 60;
	static constexpr uint16_t BLOCKING_SEGMENT_RETRY_SECS = 10;
	static constexpr uint8_t BLOCKING_SEGMENT_RETRY_COUNT = 0;
	static constexpr uint16_t BITMAP_BLOCKING_SEGMENT_HEDGE_SECS = 4;
	static constexpr uint16_t BLOCKING_SEGMENT_HEDGE_SECS = 20;
	static constexpr uint8_t BLOCKING_SEGMENT_HEDGE_COUNT = 2;
	static constexpr uint8_t PIBD_COMMIT_SEGMENT_THRESHOLD = 96;
	static constexpr uint8_t PIBD_MAX_PARALLEL_PEERS = 3;
	static constexpr uint8_t PIBD_START_MIN_PEERS = 2;
	static constexpr uint8_t PIBD_START_GRACE_SECS = 5;
	static constexpr uint16_t PIBD_STATUS_UPDATE_INTERVAL_SECS = 2;
	static constexpr uint32_t PIBD_STATUS_UPDATE_LEAF_DELTA = 65536;
	static constexpr uint16_t PIBD_PEER_HEIGHT_SLACK_BLOCKS = 2;
	static constexpr uint16_t TXHASHSET_ZIP_FALLBACK_TIME_SECS = 60;

	static constexpr uint8_t KERNEL_SEGMENT_MIN_HEIGHT = 9;
	static constexpr uint8_t KERNEL_SEGMENT_MAX_HEIGHT = 14;
	static constexpr uint8_t BITMAP_SEGMENT_MIN_HEIGHT = 9;
	static constexpr uint8_t BITMAP_SEGMENT_MAX_HEIGHT = 14;
	static constexpr uint8_t OUTPUT_SEGMENT_MIN_HEIGHT = 11;
	static constexpr uint8_t OUTPUT_SEGMENT_MAX_HEIGHT = 16;
	static constexpr uint8_t RANGEPROOF_SEGMENT_MIN_HEIGHT = 7;
	static constexpr uint8_t RANGEPROOF_SEGMENT_MAX_HEIGHT = 12;
}
