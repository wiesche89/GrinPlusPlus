#pragma once

#include <cstdint>
#include <memory>
#include <atomic>

enum class ESyncStatus
{
	SYNCING_HEADERS,
	SYNCING_TXHASHSET,
	SYNCING_TXHASHSET_PIBD,
	PROCESSING_TXHASHSET,
	TXHASHSET_PIBD_LEAFSET_UPDATE,
	TXHASHSET_SETUP,
	TXHASHSET_KERNEL_HISTORY_VALIDATION,
	TXHASHSET_NRD_KERNELS_VALIDATION,
	TXHASHSET_KERNEL_SUMS_VALIDATION,
	TXHASHSET_RANGE_PROOFS_VALIDATION,
	TXHASHSET_KERNEL_SIGNATURES_VALIDATION,
	TXHASHSET_SAVE,
	TXHASHSET_DONE,
	TXHASHSET_SYNC_FAILED,
	SYNCING_BLOCKS,
	WAITING_FOR_PEERS,
	NOT_SYNCING
};

enum class EHeaderSyncType
{
	UNKNOWN,
	LEGACY,
	PIHD
};

class SyncStatus
{
public:
	SyncStatus()
		: m_syncStatus(ESyncStatus::SYNCING_HEADERS),
		m_headerSyncType(EHeaderSyncType::UNKNOWN),
		m_numActiveConnections(0),
		m_networkHeight(0),
		m_networkDifficulty(0),
		m_headerHeight(0),
		m_headerDifficulty(0),
		m_blockHeight(0),
		m_blockDifficulty(0),
		m_lastBlockTime(0),
		m_txHashSetDownloaded(0),
		m_txHashSetTotalSize(0),
		m_txHashSetProcessingStatus(0),
		m_txHashSetProcessingCurrent(0),
		m_txHashSetProcessingTotal(0),
		m_pibdAborted(false),
		m_pibdErrored(false),
		m_pibdCompletedLeaves(0),
		m_pibdLeavesRequired(0),
		m_pibdCompletedToHeight(0),
		m_pibdRequiredHeight(0)
	{

	}
	SyncStatus(const SyncStatus& other)
		: m_syncStatus(other.m_syncStatus.load()),
		m_headerSyncType(other.m_headerSyncType.load()),
		m_numActiveConnections(other.m_numActiveConnections.load()),
		m_networkHeight(other.m_networkHeight.load()),
		m_networkDifficulty(other.m_networkDifficulty.load()),
		m_headerHeight(other.m_headerHeight.load()),
		m_headerDifficulty(other.m_headerDifficulty.load()),
		m_blockHeight(other.m_blockHeight.load()),
		m_blockDifficulty(other.m_blockDifficulty.load()),
		m_lastBlockTime(other.m_lastBlockTime.load()),
		m_txHashSetDownloaded(other.m_txHashSetDownloaded.load()),
		m_txHashSetTotalSize(other.m_txHashSetTotalSize.load()),
		m_txHashSetProcessingStatus(other.m_txHashSetProcessingStatus.load()),
		m_txHashSetProcessingCurrent(other.m_txHashSetProcessingCurrent.load()),
		m_txHashSetProcessingTotal(other.m_txHashSetProcessingTotal.load()),
		m_pibdAborted(other.m_pibdAborted.load()),
		m_pibdErrored(other.m_pibdErrored.load()),
		m_pibdCompletedLeaves(other.m_pibdCompletedLeaves.load()),
		m_pibdLeavesRequired(other.m_pibdLeavesRequired.load()),
		m_pibdCompletedToHeight(other.m_pibdCompletedToHeight.load()),
		m_pibdRequiredHeight(other.m_pibdRequiredHeight.load())
	{

	}

	bool IsSyncing() const { return m_syncStatus != ESyncStatus::NOT_SYNCING; }
	ESyncStatus GetStatus() const { return m_syncStatus; }
	EHeaderSyncType GetHeaderSyncType() const { return m_headerSyncType; }
	uint64_t GetNumActiveConnections() const { return m_numActiveConnections; }
	uint64_t GetNetworkHeight() const { return m_networkHeight; }
	uint64_t GetNetworkDifficulty() const { return m_networkDifficulty; }
	uint64_t GetHeaderHeight() const { return m_headerHeight; }
	uint64_t GetHeaderDifficulty() const { return m_headerDifficulty; }
	uint64_t GetBlockHeight() const { return m_blockHeight; }
	uint64_t GetBlockDifficulty() const { return m_blockDifficulty; }
	time_t GetLastBlockTime() const noexcept { return m_lastBlockTime; }
	uint64_t GetDownloaded() const { return m_txHashSetDownloaded; }
	uint64_t GetDownloadSize() const { return m_txHashSetTotalSize; }
	uint8_t GetProcessingStatus() const { return m_txHashSetProcessingStatus; }
	uint64_t GetProcessingCurrent() const { return m_txHashSetProcessingCurrent; }
	uint64_t GetProcessingTotal() const { return m_txHashSetProcessingTotal; }
	bool GetPIBDAborted() const { return m_pibdAborted; }
	bool GetPIBDErrored() const { return m_pibdErrored; }
	uint64_t GetPIBDCompletedLeaves() const { return m_pibdCompletedLeaves; }
	uint64_t GetPIBDLeavesRequired() const { return m_pibdLeavesRequired; }
	uint64_t GetPIBDCompletedToHeight() const { return m_pibdCompletedToHeight; }
	uint64_t GetPIBDRequiredHeight() const { return m_pibdRequiredHeight; }

	bool IsTxHashSetSyncStatus() const
	{
		const ESyncStatus status = GetStatus();
		return status == ESyncStatus::SYNCING_TXHASHSET
			|| status == ESyncStatus::SYNCING_TXHASHSET_PIBD
			|| status == ESyncStatus::PROCESSING_TXHASHSET
			|| status == ESyncStatus::TXHASHSET_PIBD_LEAFSET_UPDATE
			|| status == ESyncStatus::TXHASHSET_SETUP
			|| status == ESyncStatus::TXHASHSET_KERNEL_HISTORY_VALIDATION
			|| status == ESyncStatus::TXHASHSET_NRD_KERNELS_VALIDATION
			|| status == ESyncStatus::TXHASHSET_KERNEL_SUMS_VALIDATION
			|| status == ESyncStatus::TXHASHSET_RANGE_PROOFS_VALIDATION
			|| status == ESyncStatus::TXHASHSET_KERNEL_SIGNATURES_VALIDATION
			|| status == ESyncStatus::TXHASHSET_SAVE
			|| status == ESyncStatus::TXHASHSET_DONE;
	}

	void UpdateStatus(const ESyncStatus syncStatus)
	{
		m_syncStatus = syncStatus;
		m_txHashSetProcessingCurrent = 0;
		m_txHashSetProcessingTotal = 0;
	}

	void UpdateHeaderSyncType(const EHeaderSyncType headerSyncType)
	{
		m_headerSyncType = headerSyncType;
	}

	void UpdateNetworkStatus(const uint64_t numActiveConnections, const uint64_t networkHeight, const uint64_t networkDifficulty)
	{
		m_numActiveConnections = numActiveConnections;
		if (networkHeight != 0) m_networkHeight = networkHeight;
		if (networkDifficulty != 0) m_networkDifficulty = networkDifficulty;
	}

	void UpdateHeaderStatus(const uint64_t headerHeight, const uint64_t headerDifficulty)
	{
		m_headerHeight = headerHeight;
		m_headerDifficulty = headerDifficulty;
	}

	void UpdateBlockStatus(const uint64_t blockHeight, const uint64_t blockDifficulty, const time_t lastBlockTime)
	{
		m_blockHeight = blockHeight;
		m_blockDifficulty = blockDifficulty;
		m_lastBlockTime = lastBlockTime;
	}

	void UpdateDownloaded(const uint64_t downloaded) { m_txHashSetDownloaded = downloaded; }
	void UpdateDownloadSize(const uint64_t downloadSize) { m_txHashSetTotalSize = downloadSize; }
	void UpdateProcessingStatus(const uint8_t processingStatus) { m_txHashSetProcessingStatus = processingStatus; }
	void UpdateProcessingProgress(const uint64_t current, const uint64_t total)
	{
		m_txHashSetProcessingCurrent = current;
		m_txHashSetProcessingTotal = total;
	}
	void UpdatePIBDStatus(
		const bool aborted,
		const bool errored,
		const uint64_t completedLeaves,
		const uint64_t leavesRequired,
		const uint64_t completedToHeight,
		const uint64_t requiredHeight)
	{
		m_syncStatus = ESyncStatus::SYNCING_TXHASHSET_PIBD;
		m_txHashSetProcessingCurrent = 0;
		m_txHashSetProcessingTotal = 0;
		m_pibdAborted = aborted;
		m_pibdErrored = errored;
		m_pibdCompletedLeaves = completedLeaves;
		m_pibdLeavesRequired = leavesRequired;
		m_pibdCompletedToHeight = completedToHeight;
		m_pibdRequiredHeight = requiredHeight;
		m_txHashSetDownloaded = completedLeaves;
		m_txHashSetTotalSize = leavesRequired;
	}

private:
	std::atomic<ESyncStatus> m_syncStatus;
	std::atomic<EHeaderSyncType> m_headerSyncType;
	std::atomic<uint64_t> m_numActiveConnections;
	std::atomic<uint64_t> m_networkHeight;
	std::atomic<uint64_t> m_networkDifficulty;
	std::atomic<uint64_t> m_headerHeight;
	std::atomic<uint64_t> m_headerDifficulty;
	std::atomic<uint64_t> m_blockHeight;
	std::atomic<uint64_t> m_blockDifficulty;
	std::atomic<time_t> m_lastBlockTime;
	std::atomic<uint64_t> m_txHashSetDownloaded;
	std::atomic<uint64_t> m_txHashSetTotalSize;
	std::atomic<uint8_t> m_txHashSetProcessingStatus;
	std::atomic<uint64_t> m_txHashSetProcessingCurrent;
	std::atomic<uint64_t> m_txHashSetProcessingTotal;
	std::atomic_bool m_pibdAborted;
	std::atomic_bool m_pibdErrored;
	std::atomic<uint64_t> m_pibdCompletedLeaves;
	std::atomic<uint64_t> m_pibdLeavesRequired;
	std::atomic<uint64_t> m_pibdCompletedToHeight;
	std::atomic<uint64_t> m_pibdRequiredHeight;
};

typedef std::shared_ptr<SyncStatus> SyncStatusPtr;
typedef std::shared_ptr<SyncStatus> SyncStatusConstPtr;
