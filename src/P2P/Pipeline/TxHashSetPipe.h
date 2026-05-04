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
	void UpdatePIBDStatus(const bool aborted = false, const bool errored = false);
	uint64_t GetPIBDCompletedToHeight() const;
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
		m_processing(false) { }

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
