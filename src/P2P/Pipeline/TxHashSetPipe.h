#pragma once

#include "../Messages/SegmentResponseMessage.h"
#include <P2P/SyncStatus.h>
#include <Crypto/Models/Hash.h>
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

private:
	void UpdatePIBDStatus(const bool aborted = false, const bool errored = false);
	uint64_t GetPIBDCompletedToHeight() const;

	TxHashSetPipe(
		const std::shared_ptr<ConnectionManager>& pConnectionManager,
		const IBlockChain::Ptr& pBlockChain,
		std::shared_ptr<Locked<TxHashSetManager>> pTxHashSetManager,
		SyncStatusPtr pSyncStatus
	) : m_pConnectionManager(pConnectionManager),
		m_pBlockChain(pBlockChain),
		m_pTxHashSetManager(std::move(pTxHashSetManager)),
		m_pSyncStatus(pSyncStatus),
		m_processing(false) { }

	std::shared_ptr<ConnectionManager> m_pConnectionManager;
	IBlockChain::Ptr m_pBlockChain;
	std::shared_ptr<Locked<TxHashSetManager>> m_pTxHashSetManager;
	SyncStatusPtr m_pSyncStatus;

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
};
