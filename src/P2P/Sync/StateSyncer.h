#pragma once

#include "../ConnectionManager.h"

#include <BlockChain/BlockChain.h>
#include <PMMR/PIBDParams.h>
#include <chrono>
#include <cstddef>
#include <limits>
#include <string>
#include <unordered_map>

// Forward Declarations
class SyncStatus;
class Pipeline;

class StateSyncer
{
public:
	StateSyncer(
		const std::weak_ptr<ConnectionManager>& pConnectionManager,
		const IBlockChain::Ptr& pBlockChain,
		std::shared_ptr<Pipeline> pPipeline)
		: m_pConnectionManager(pConnectionManager), m_pBlockChain(pBlockChain), m_pPipeline(std::move(pPipeline))
	{
		m_timeRequested = std::chrono::system_clock::now();
		m_requestedHeight = 0;
		m_pPeer = nullptr;
		m_lastPIBDProgressTime = std::chrono::steady_clock::now();
		m_lastPIBDProgressLeaves = std::numeric_limits<uint64_t>::max();
		m_pibdNoPeerSince = std::chrono::steady_clock::now();
		m_pibdNoPeerSinceSet = false;
		m_pibdFirstEligibleSince = std::chrono::steady_clock::now();
		m_pibdFirstEligibleSinceSet = false;
		m_lastLoggedPIBDPeerCount = 0;
		m_lastLoggedPIBDPeerLimit = 0;
	}

	bool SyncState(SyncStatus& syncStatus);

private:
	bool IsStateSyncDue(const SyncStatus& syncStatus) const;
	bool RequestState(SyncStatus& syncStatus);
	bool RequestPIBDState(SyncStatus& syncStatus);
	bool RequestZipState(SyncStatus& syncStatus);

	std::chrono::time_point<std::chrono::system_clock> m_timeRequested;
	uint64_t m_requestedHeight;
	PeerPtr m_pPeer;

	std::chrono::steady_clock::time_point m_lastPIBDProgressTime;
	uint64_t m_lastPIBDProgressLeaves;
	std::chrono::steady_clock::time_point m_pibdNoPeerSince;
	bool m_pibdNoPeerSinceSet;
	std::chrono::steady_clock::time_point m_pibdFirstEligibleSince;
	bool m_pibdFirstEligibleSinceSet;
	size_t m_lastLoggedPIBDPeerCount;
	size_t m_lastLoggedPIBDPeerLimit;
	std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_blockedPIBDPeers;

	std::weak_ptr<ConnectionManager> m_pConnectionManager;
	IBlockChain::Ptr m_pBlockChain;
	std::shared_ptr<Pipeline> m_pPipeline;
};
