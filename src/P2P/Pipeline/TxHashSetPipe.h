#pragma once

#include "../Messages/SegmentResponseMessage.h"
#include "PIBDValidationJob.h"
#include <P2P/SyncStatus.h>
#include <Crypto/Models/Hash.h>
#include <Core/Global.h>
#include <Net/Socket.h>
#include <P2P/Peer.h>
#include <BlockChain/BlockChain.h>
#include <PMMR/TxHashSetManager.h>
#include <PMMR/Common/SegmentId.h>
#include <PMMR/SegmentRequestTracker.h>
#include <PMMR/TxHashSetDesegmenter.h>
#include <Common/Util/FileUtil.h>
#include <string>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <optional>
#include <limits>
#include <chrono>
#include <map>
#include <set>

// Forward Declarations
class ConnectionManager;
class Connection;
class TxHashSetArchiveMessage;
class OutputBitmapSegmentMessage;
class OutputSegmentMessage;

class TxHashSetPipe
{
public:
	static std::shared_ptr<TxHashSetPipe> Create(
		const std::shared_ptr<ConnectionManager>& pConnectionManager,
		const IBlockChain::Ptr& pBlockChain,
		std::shared_ptr<Locked<TxHashSetManager>> pTxHashSetManager,
		SyncStatusPtr pSyncStatus
	);

	~TxHashSetPipe();

	//
	// Downloads a TxHashSet and kicks off a new thread to process it.
	//
	void ReceiveTxHashSet(
		const std::shared_ptr<Connection>& pConnection,
		const TxHashSetArchiveMessage& archive_msg
	);

	void SendTxHashSet(
		const std::shared_ptr<Connection>& pConnection,
		const Hash& block_hash
	);

	void SendOutputBitmapSegment(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier);
	void SendOutputSegment(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier);
	void SendRangeProofSegment(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier);
	void SendKernelSegment(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier);

	void ReceiveOutputBitmapSegment(const std::shared_ptr<Connection>& pConnection, const OutputBitmapSegmentMessage& message);
	void ReceiveOutputSegment(const std::shared_ptr<Connection>& pConnection, const OutputSegmentMessage& message);
	void ReceiveRangeProofSegment(const std::shared_ptr<Connection>& pConnection, const RangeProofSegmentMessage& message);
	void ReceiveKernelSegment(const std::shared_ptr<Connection>& pConnection, const KernelSegmentMessage& message);

	bool StartPIBD(const BlockHeaderPtr& pArchiveHeader);
	bool RequestNextPIBDSegments(const std::shared_ptr<ConnectionManager>& pConnectionManager, const std::vector<PeerConstPtr>& peers);
	void ClearPIBDRequests();
	void AbortPIBD();
	bool IsPIBDComplete() const;
	bool IsPIBDValidationRunning() const;

private:
	struct PIBDSegmentServeRequest
	{
		SegmentType type;
		std::shared_ptr<Connection> pConnection;
		Hash blockHash;
		SegmentIdentifier identifier;
		std::chrono::steady_clock::time_point enqueuedAt;
		uint64_t sequence;
	};

	struct PIBDSegmentBitmapCache
	{
		std::optional<Hash> blockHash;
		uint64_t outputMMRSize{ 0 };
		uint64_t numOutputs{ 0 };
		std::shared_ptr<const BitmapAccumulator> pAccumulator;
		std::optional<Hash> outputBitmapRoot;
	};

	void UpdatePIBDStatus(const bool aborted = false, const bool errored = false);
	uint64_t GetPIBDCompletedToHeight() const;
	bool EnqueuePIBDSegmentServeRequest(
		const SegmentType type,
		const std::shared_ptr<Connection>& pConnection,
		const Hash& blockHash,
		const SegmentIdentifier& identifier);
	void Thread_ServePIBDSegments();
	void ProcessPIBDSegmentServeRequest(PIBDSegmentServeRequest&& request);
	void ProcessOutputBitmapSegmentRequest(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier);
	void ProcessOutputSegmentRequest(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier);
	void ProcessRangeProofSegmentRequest(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier);
	void ProcessKernelSegmentRequest(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier);
	std::deque<PIBDSegmentServeRequest>::iterator SelectNextPIBDSegmentServeRequest();
	std::shared_ptr<const BitmapAccumulator> GetCachedOutputBitmapAccumulator(
		const ITxHashSetConstPtr& pTxHashSet,
		const Hash& blockHash,
		const uint64_t outputMMRSize);
	Hash GetCachedOutputBitmapRoot(
		const ITxHashSetConstPtr& pTxHashSet,
		const Hash& blockHash,
		const uint64_t outputMMRSize,
		const uint64_t numOutputs);
	ITxHashSetConstPtr GetSegmentTxHashSet(const Hash& blockHash);
	void MarkOutputBitmapSegmentServed(
		const std::shared_ptr<Connection>& pConnection,
		const Hash& blockHash,
		const SegmentIdentifier& identifier,
		const uint64_t bitmapMMRSize);
	bool ShouldDeferTxHashSetDataSegment(
		const std::shared_ptr<Connection>& pConnection,
		const Hash& blockHash,
		const SegmentIdentifier& requestedIdentifier,
		const uint64_t bitmapMMRSize) const;

	TxHashSetPipe(
		const std::shared_ptr<ConnectionManager>& pConnectionManager,
		const IBlockChain::Ptr& pBlockChain,
		std::shared_ptr<Locked<TxHashSetManager>> pTxHashSetManager,
		SyncStatusPtr pSyncStatus
	) : m_pConnectionManager(pConnectionManager),
		m_pBlockChain(pBlockChain),
		m_pTxHashSetManager(std::move(pTxHashSetManager)),
		m_pSyncStatus(pSyncStatus),
		m_pPIBDValidationJob(std::make_unique<PIBDValidationJob>(
			pBlockChain,
			pSyncStatus,
			[this](EBlockChainStatus status) {
				m_processing = false;
				if (status != EBlockChainStatus::SUCCESS && Global::IsRunning()) {
					std::lock_guard<std::mutex> lock(m_pibdMutex);
					UpdatePIBDStatus(false, true);
				}
			})),
		m_processing(false)
	{
		for (size_t i = 0; i < PIBD_SEGMENT_SERVE_WORKERS; ++i) {
			m_pibdSegmentServeWorkers.emplace_back(&TxHashSetPipe::Thread_ServePIBDSegments, this);
		}
	}

	std::shared_ptr<ConnectionManager> m_pConnectionManager;
	IBlockChain::Ptr m_pBlockChain;
	std::shared_ptr<Locked<TxHashSetManager>> m_pTxHashSetManager;
	SyncStatusPtr m_pSyncStatus;
	std::unique_ptr<PIBDValidationJob> m_pPIBDValidationJob;

	static void Thread_ProcessTxHashSet(
		TxHashSetPipe& pipeline,
		std::shared_ptr<Connection> pConnection,
		const uint64_t zipped_size,
		const Hash blockHash
	);

	static void Thread_SendTxHashSet(
		IBlockChain::Ptr pBlockChain,
		std::shared_ptr<Connection> pConnection,
		Hash block_hash
	);

	std::thread m_txHashSetThread;
	std::vector<std::thread> m_sendThreads;

	std::atomic_bool m_processing;
	static constexpr size_t PIBD_SEGMENT_SERVE_QUEUE_CAPACITY = 64;
	static constexpr size_t PIBD_SEGMENT_SERVE_WORKERS = 4;
	std::mutex m_pibdSegmentServeMutex;
	std::condition_variable m_pibdSegmentServeCondition;
	std::deque<PIBDSegmentServeRequest> m_pibdSegmentServeQueue;
	std::vector<std::thread> m_pibdSegmentServeWorkers;
	bool m_stopPIBDSegmentServeWorkers{ false };
	std::atomic<uint64_t> m_pibdSegmentServeSequence{ 0 };
	std::mutex m_pibdSegmentBitmapCacheMutex;
	PIBDSegmentBitmapCache m_pibdSegmentBitmapCache;

	mutable std::mutex m_pibdMutex;
	std::optional<Hash> m_pibdBlockHash;
	std::unique_ptr<TxHashSetDesegmenter> m_pDesegmenter;
	SegmentRequestTracker m_segmentRequests;
	uint64_t m_lastLoggedPIBDCompletedLeaves{ std::numeric_limits<uint64_t>::max() };
	uint64_t m_cachedPIBDCompletedToHeight{ 0 };
	uint64_t m_lastPIBDStatusHeightCalcLeaves{ std::numeric_limits<uint64_t>::max() };
	std::chrono::steady_clock::time_point m_lastPIBDStatusHeightCalcTime{};
	size_t m_pibdNextPeerIndex{ 0 };
	size_t m_uncommittedPIBDSegments{ 0 };
	mutable std::mutex m_pibdSegmentTxHashSetMutex;
	std::condition_variable m_pibdSegmentTxHashSetCondition;
	std::optional<Hash> m_pibdSegmentTxHashSetCreatingHash;
	std::optional<Hash> m_pibdSegmentTxHashSetHash;
	ITxHashSetPtr m_pibdSegmentTxHashSet;
	struct ServedOutputBitmapState
	{
		std::set<uint64_t> segmentIndices;
		std::optional<std::chrono::steady_clock::time_point> completedAt;
	};
	mutable std::mutex m_servedBitmapSegmentsMutex;
	mutable std::map<std::string, ServedOutputBitmapState> m_servedBitmapSegments;
};
