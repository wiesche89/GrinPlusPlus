#pragma once

#include "../ConnectionManager.h"

#include <BlockChain/BlockChain.h>
#include <PMMR/Common/SegmentId.h>
#include <chrono>
#include <vector>

// Forward Declarations
class SyncStatus;

class HeaderSyncer
{
public:
	HeaderSyncer(const std::weak_ptr<ConnectionManager>& pConnectionManager, const IBlockChain::Ptr& pBlockChain)
		: m_pConnectionManager(pConnectionManager), m_pBlockChain(pBlockChain)
	{
		m_lastHeight = 0;
	}

	bool SyncHeaders(SyncStatus& syncStatus, const bool startup);

private:
	struct PendingHeaderRequest
	{
		PeerPtr pPeer;
		SegmentIdentifier identifier;
		std::chrono::time_point<std::chrono::system_clock> timeout;
	};

	bool IsHeaderSyncDue(const SyncStatus& syncStatus);
	bool RequestHeaders(SyncStatus& syncStatus);

	std::weak_ptr<ConnectionManager> m_pConnectionManager;
	IBlockChain::Ptr m_pBlockChain;

	uint64_t m_lastHeight;
	std::vector<PendingHeaderRequest> m_pendingRequests;
};
