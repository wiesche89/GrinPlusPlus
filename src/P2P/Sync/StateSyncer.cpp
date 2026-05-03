#include "StateSyncer.h"
#include "../Messages/TxHashSetRequestMessage.h"
#include "../Pipeline/Pipeline.h"

#include <Consensus.h>
#include <Common/Logger.h>
#include <Core/Global.h>
#include <Net/SocketAddress.h>
#include <PMMR/PIBDParams.h>

#include <algorithm>

static constexpr std::chrono::minutes PIBD_STALLED_PEER_BACKOFF(10);

static uint64_t GetPIBDMinPeerHeight(const uint64_t, const uint64_t archiveHeight)
{
	return archiveHeight;
}

static uint64_t GetConnectedPeerHeight(const std::vector<ConnectedPeer>& connectedPeers, const PeerConstPtr& pPeer)
{
	if (pPeer == nullptr) {
		return 0;
	}

	for (const ConnectedPeer& connectedPeer : connectedPeers) {
		if (connectedPeer.GetPeer() != nullptr
			&& connectedPeer.GetSocketAddress() == SocketAddress(pPeer->GetIPAddress(), pPeer->GetPort())) {
			return connectedPeer.GetHeight();
		}
	}

	return 0;
}

bool StateSyncer::SyncState(SyncStatus& syncStatus)
{
	if (syncStatus.GetStatus() == ESyncStatus::SYNCING_TXHASHSET_PIBD)
	{
		if (RequestPIBDState(syncStatus)) {
			return true;
		}

		if (syncStatus.GetPIBDErrored()) {
			LOG_WARNING(StringUtil::Format("PIBD failed with peer {}, restarting PIBD.", m_pPeer));
			m_pPipeline->AbortPIBD();
			m_pPeer = nullptr;
			return RequestPIBDState(syncStatus);
		} else if (m_pPeer != nullptr) {
			return true;
		}

		const uint64_t headerHeight = syncStatus.GetHeaderHeight();
		const uint64_t blockHeight = syncStatus.GetBlockHeight();
		return headerHeight >= Consensus::CUT_THROUGH_HORIZON
			&& blockHeight < (headerHeight - Consensus::CUT_THROUGH_HORIZON);
	}

	if (IsStateSyncDue(syncStatus))
	{
		syncStatus.UpdateStatus(ESyncStatus::SYNCING_TXHASHSET);
		RequestState(syncStatus);

		return true;
	}

	// If state sync is still in progress, return true to delay block sync.
	if (syncStatus.GetBlockHeight() < (syncStatus.GetHeaderHeight() - Consensus::CUT_THROUGH_HORIZON))
	{
		return true;
	}

	return false;
}

// NOTE: This doesn't handle re-orgs beyond the horizon.
bool StateSyncer::IsStateSyncDue(const SyncStatus& syncStatus) const
{
	const uint64_t headerHeight = syncStatus.GetHeaderHeight();
	const uint64_t blockHeight = syncStatus.GetBlockHeight();

	const ESyncStatus status = syncStatus.GetStatus();
	if (status == ESyncStatus::PROCESSING_TXHASHSET
		|| status == ESyncStatus::TXHASHSET_PIBD_LEAFSET_UPDATE
		|| status == ESyncStatus::TXHASHSET_SETUP
		|| status == ESyncStatus::TXHASHSET_KERNEL_HISTORY_VALIDATION
		|| status == ESyncStatus::TXHASHSET_NRD_KERNELS_VALIDATION
		|| status == ESyncStatus::TXHASHSET_KERNEL_SUMS_VALIDATION
		|| status == ESyncStatus::TXHASHSET_RANGE_PROOFS_VALIDATION
		|| status == ESyncStatus::TXHASHSET_KERNEL_SIGNATURES_VALIDATION
		|| status == ESyncStatus::TXHASHSET_SAVE)
	{
		return false;
	}

	if (status == ESyncStatus::TXHASHSET_SYNC_FAILED)
	{
		LOG_WARNING("TxHashSet sync failed.");
		return true;
	}

	// For the first week, there's no reason to request TxHashSet, since we can just download full blocks.
	if (headerHeight < Consensus::CUT_THROUGH_HORIZON)
	{
		return false;
	}

	// If block height is within threshold, just rely on block sync.
	if (blockHeight > (headerHeight - Consensus::CUT_THROUGH_HORIZON))
	{
		return false;
	}

	if (m_requestedHeight == 0)
	{
		LOG_INFO("Requesting TxHashSet for the first time.");
		return true;
	}

	// If TxHashSet download timed out, request it from another peer.
	if ((m_timeRequested + std::chrono::minutes(120)) < std::chrono::system_clock::now())
	{
		LOG_WARNING("Download timed out (120 minutes).");
		return true;
	}

	// If 60 seconds elapsed with no progress, try another peer.
	if ((m_timeRequested + std::chrono::seconds(PIBD::TXHASHSET_ZIP_FALLBACK_TIME_SECS)) < std::chrono::system_clock::now())
	{
		const uint64_t downloaded = syncStatus.GetDownloaded();
		if (downloaded == 0)
		{
			LOG_WARNING(StringUtil::Format("{} seconds elapsed and download still not started.", PIBD::TXHASHSET_ZIP_FALLBACK_TIME_SECS));
			return true;
		}
	}

	const auto pConnectionManager = m_pConnectionManager.lock();
	if (m_pPeer != nullptr && (pConnectionManager == nullptr || !pConnectionManager->IsConnected(SocketAddress(m_pPeer->GetIPAddress(), m_pPeer->GetPort()))))
	{
		LOG_WARNING("Sync peer no longer connected.");
		return true;
	}

	return false;
}

bool StateSyncer::RequestState(SyncStatus& syncStatus)
{
	if (RequestPIBDState(syncStatus)) {
		return true;
	}

	return RequestZipState(syncStatus);
}

bool StateSyncer::RequestPIBDState(SyncStatus& syncStatus)
{
	if (m_pPeer != nullptr)
	{
		const auto pConnectionManager = m_pConnectionManager.lock();
		if (pConnectionManager == nullptr) {
			return false;
		}

		if (!pConnectionManager->IsConnected(SocketAddress(m_pPeer->GetIPAddress(), m_pPeer->GetPort()))
			|| !m_pPeer->GetCapabilities().HasCapability(Capabilities::PIBD_HIST_1)) {
			LOG_WARNING(StringUtil::Format("PIBD peer {} is no longer eligible, selecting a new peer.", m_pPeer));
			m_pPipeline->ClearPIBDRequests();
			m_pPeer = nullptr;
		} else {
			const uint64_t headerHeight = syncStatus.GetHeaderHeight();
			const uint64_t archiveHeight = m_requestedHeight != 0
				? m_requestedHeight
				: Consensus::GetTxHashSetArchiveHeight(headerHeight);
			const uint64_t minPeerHeight = GetPIBDMinPeerHeight(headerHeight, archiveHeight);
			const uint64_t peerHeight = GetConnectedPeerHeight(pConnectionManager->GetConnectedPeers(), m_pPeer);
			if (peerHeight < minPeerHeight) {
				LOG_WARNING(StringUtil::Format(
					"PIBD peer {} is behind required height (peer_height={}, min_height={}, archive_height={}, header_height={}), selecting a new peer.",
					m_pPeer,
					peerHeight,
					minPeerHeight,
					archiveHeight,
					headerHeight));
				m_pPipeline->ClearPIBDRequests();
				m_pPeer = nullptr;
				return RequestPIBDState(syncStatus);
			}

			// Detect stall: peer connected but no PIBD progress
			const uint64_t currentLeaves = syncStatus.GetPIBDCompletedLeaves();
			if (m_lastPIBDProgressLeaves == std::numeric_limits<uint64_t>::max()
				|| currentLeaves > m_lastPIBDProgressLeaves) {
				m_lastPIBDProgressLeaves = currentLeaves;
				m_lastPIBDProgressTime = std::chrono::steady_clock::now();
				m_pibdNoPeerSinceSet = false;
			} else {
				const auto stallSecs = std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::steady_clock::now() - m_lastPIBDProgressTime).count();
				const int64_t stallThreshold = PIBD::SEGMENT_REQUEST_TIMEOUT_SECS * 5;
				if (stallSecs >= stallThreshold) {
					LOG_WARNING(StringUtil::Format(
						"PIBD stalled with peer {} for {}s (threshold={}s), switching peer.",
						m_pPeer, stallSecs, stallThreshold));
					bool hasAlternativePIBDPeer = false;
					for (ConnectedPeer& cp : pConnectionManager->GetConnectedPeers()) {
						if (cp.GetPeer() != nullptr
							&& cp.GetPeer() != m_pPeer
							&& pConnectionManager->IsConnected(cp.GetSocketAddress())
							&& cp.GetPeer()->GetCapabilities().HasCapability(Capabilities::PIBD_HIST_1)
							&& cp.GetHeight() >= minPeerHeight) {
							hasAlternativePIBDPeer = true;
							break;
						}
					}

					if (hasAlternativePIBDPeer) {
						m_blockedPIBDPeers[SocketAddress(m_pPeer->GetIPAddress(), m_pPeer->GetPort()).Format()] = std::chrono::steady_clock::now() + PIBD_STALLED_PEER_BACKOFF;
						m_pPipeline->ClearPIBDRequests();
						m_pPeer = nullptr;
					} else {
						LOG_WARNING(StringUtil::Format("PIBD peer {} stalled, but no alternative PIBD peer is available; continuing retries.", m_pPeer));
						m_lastPIBDProgressTime = std::chrono::steady_clock::now();
					}
				}
			}
		}

		if (m_pPeer != nullptr) {
			// Collect all currently eligible PIBD peers (not just the primary)
			// so requests can be distributed across multiple peers in parallel.
			std::vector<PeerConstPtr> activePibdPeers;
			activePibdPeers.push_back(m_pPeer);

			const uint64_t headerHeight = syncStatus.GetHeaderHeight();
			const uint64_t archiveHeight = m_requestedHeight != 0
				? m_requestedHeight
				: Consensus::GetTxHashSetArchiveHeight(headerHeight);
			const uint64_t minPeerHeight = GetPIBDMinPeerHeight(headerHeight, archiveHeight);
			for (ConnectedPeer& cp : pConnectionManager->GetConnectedPeers()) {
				if (cp.GetPeer() != nullptr
					&& cp.GetPeer() != m_pPeer
					&& pConnectionManager->IsConnected(cp.GetSocketAddress())
					&& cp.GetPeer()->GetCapabilities().HasCapability(Capabilities::PIBD_HIST_1)
					&& cp.GetHeight() >= minPeerHeight) {
					activePibdPeers.push_back(cp.GetPeer());
				}
			}

			const size_t eligiblePibdPeerCount = activePibdPeers.size();
			if (activePibdPeers.size() > PIBD::PIBD_MAX_PARALLEL_PEERS) {
				activePibdPeers.resize(PIBD::PIBD_MAX_PARALLEL_PEERS);
			}

			if (eligiblePibdPeerCount > 1
				&& (eligiblePibdPeerCount != m_lastLoggedPIBDPeerCount
					|| activePibdPeers.size() != m_lastLoggedPIBDPeerLimit)) {
				LOG_DEBUG(StringUtil::Format("Using {} eligible PIBD peer(s), capped at {} peer(s) for parallel requests.",
					eligiblePibdPeerCount,
					activePibdPeers.size()));
				m_lastLoggedPIBDPeerCount = eligiblePibdPeerCount;
				m_lastLoggedPIBDPeerLimit = activePibdPeers.size();
			}

			if (m_pPipeline->RequestNextPIBDSegments(pConnectionManager, activePibdPeers)) {
				return true;
			} else if (syncStatus.GetPIBDErrored()) {
				return false;
			}
			if (m_pPeer != nullptr) {
				return true;
			}
		}
	}

	if (!Global::IsRunning()) {
		return false;
	}

	const uint64_t headerHeight = syncStatus.GetHeaderHeight();
	const uint64_t requestedHeight = Consensus::GetTxHashSetArchiveHeight(headerHeight);
	BlockHeaderPtr pHeader = m_pBlockChain->GetBlockHeaderByHeight(requestedHeight, EChainType::CANDIDATE);
	if (pHeader == nullptr) {
		return false;
	}

	const auto pConnectionManager = m_pConnectionManager.lock();
	if (pConnectionManager == nullptr) {
		return false;
	}

	std::vector<ConnectedPeer> connectedPeers = pConnectionManager->GetConnectedPeers();
	const uint64_t minPeerHeight = GetPIBDMinPeerHeight(headerHeight, requestedHeight);

	const auto now = std::chrono::steady_clock::now();
	for (auto iter = m_blockedPIBDPeers.begin(); iter != m_blockedPIBDPeers.end(); ) {
		if (iter->second <= now) {
			iter = m_blockedPIBDPeers.erase(iter);
		} else {
			++iter;
		}
	}

	std::vector<ConnectedPeer*> pibdPeers;
	for (ConnectedPeer& peer : connectedPeers) {
		const std::string peerAddress = peer.GetSocketAddress().Format();
		if (peer.GetPeer() != nullptr
			&& pConnectionManager->IsConnected(peer.GetSocketAddress())
			&& peer.GetPeer()->GetCapabilities().HasCapability(Capabilities::PIBD_HIST_1)
			&& peer.GetHeight() >= minPeerHeight
			&& m_blockedPIBDPeers.find(peerAddress) == m_blockedPIBDPeers.end()) {
			pibdPeers.push_back(&peer);
		}
	}

	if (pibdPeers.empty()) {
		// Track how long we have had no eligible PIBD peer
		if (!m_pibdNoPeerSinceSet) {
			m_pibdNoPeerSince = std::chrono::steady_clock::now();
			m_pibdNoPeerSinceSet = true;
		}
		m_pibdFirstEligibleSinceSet = false;
		const auto noPeerSecs = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::steady_clock::now() - m_pibdNoPeerSince).count();
		const int64_t noPeerThreshold = 300; // 5 minutes

		if (noPeerSecs >= noPeerThreshold) {
			LOG_WARNING(StringUtil::Format(
				"PIBD has had no eligible peer for {}s, aborting PIBD to trigger re-sync.",
				noPeerSecs));
			m_pPipeline->AbortPIBD();
			syncStatus.UpdateStatus(ESyncStatus::TXHASHSET_SYNC_FAILED);
			m_pibdNoPeerSinceSet = false;
			m_lastPIBDProgressLeaves = std::numeric_limits<uint64_t>::max();
		} else {
			LOG_TRACE(StringUtil::Format(
				"No eligible PIBD_HIST_1 peer found. connected={}, min_height={}, archive_height={}, header_height={}, no_peer_for={}s.",
				connectedPeers.size(), minPeerHeight, requestedHeight, headerHeight, noPeerSecs));
		}
		return false;
	}

	// Found at least one eligible peer - reset no-peer timer
	m_pibdNoPeerSinceSet = false;

	const size_t startMinPeers = (std::min)(
		static_cast<size_t>(PIBD::PIBD_START_MIN_PEERS),
		static_cast<size_t>(PIBD::PIBD_MAX_PARALLEL_PEERS));
	if (m_requestedHeight == 0 && pibdPeers.size() < startMinPeers) {
		if (!m_pibdFirstEligibleSinceSet) {
			m_pibdFirstEligibleSince = std::chrono::steady_clock::now();
			m_pibdFirstEligibleSinceSet = true;
		}

		const auto firstEligibleSecs = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::steady_clock::now() - m_pibdFirstEligibleSince).count();
		if (firstEligibleSecs < PIBD::PIBD_START_GRACE_SECS) {
			LOG_DEBUG(StringUtil::Format(
				"Waiting briefly for more PIBD peers before starting. eligible={}, wanted={}, grace={}s/{}s, archive_height={}, min_height={}.",
				pibdPeers.size(),
				startMinPeers,
				firstEligibleSecs,
				PIBD::PIBD_START_GRACE_SECS,
				requestedHeight,
				minPeerHeight));
			syncStatus.UpdateStatus(ESyncStatus::SYNCING_TXHASHSET_PIBD);
			return true;
		}

		LOG_WARNING(StringUtil::Format(
			"Starting PIBD with only {} eligible peer(s) after waiting {}s.",
			pibdPeers.size(),
			firstEligibleSecs));
	}
	m_pibdFirstEligibleSinceSet = false;

	std::sort(
		pibdPeers.begin(),
		pibdPeers.end(),
		[](const ConnectedPeer* lhs, const ConnectedPeer* rhs) {
			if (lhs->GetTotalDifficulty() != rhs->GetTotalDifficulty()) {
				return lhs->GetTotalDifficulty() > rhs->GetTotalDifficulty();
			}
			if (lhs->GetDirection() != rhs->GetDirection()) {
				return lhs->GetDirection() == EDirection::OUTBOUND;
			}
			return lhs->GetHeight() > rhs->GetHeight();
		});

	if (!m_pPipeline->StartPIBD(pHeader)) {
		return false;
	}

	ConnectedPeer* pSelectedPeer = pibdPeers.front();
	m_pPeer = pSelectedPeer->GetPeer();
	m_timeRequested = std::chrono::system_clock::now();
	m_requestedHeight = requestedHeight;
	m_lastPIBDProgressLeaves = syncStatus.GetPIBDCompletedLeaves();
	m_lastPIBDProgressTime = std::chrono::steady_clock::now();

	// Build peer list: primary first, then any additional eligible peers
	std::vector<PeerConstPtr> pibdPeerList;
	for (ConnectedPeer* cp : pibdPeers) {
		pibdPeerList.push_back(cp->GetPeer());
	}
	if (pibdPeerList.size() > PIBD::PIBD_MAX_PARALLEL_PEERS) {
		pibdPeerList.resize(PIBD::PIBD_MAX_PARALLEL_PEERS);
	}

	LOG_INFO(StringUtil::Format(
		"Selected {} eligible PIBD peer(s), using {} peer(s), archive_height={}, archive_hash={}, min_height={}, primary: {} height={} difficulty={}.",
		pibdPeers.size(),
		pibdPeerList.size(),
		requestedHeight,
		pHeader->GetHash(),
		minPeerHeight,
		m_pPeer,
		pSelectedPeer->GetHeight(),
		pSelectedPeer->GetTotalDifficulty()));
	return m_pPipeline->RequestNextPIBDSegments(pConnectionManager, pibdPeerList);
}

bool StateSyncer::RequestZipState(SyncStatus& syncStatus)
{
	if (m_pPeer != nullptr)
	{
		LOG_WARNING(StringUtil::Format("TxHashSet/PIBD timeout from peer: {}, trying zip fallback.", m_pPeer));
		m_pPipeline->AbortPIBD();
		m_pPeer = nullptr;
		syncStatus.UpdateStatus(ESyncStatus::SYNCING_TXHASHSET);
	}

	if (Global::IsRunning())
	{
		const auto pConnectionManager = m_pConnectionManager.lock();
		if (pConnectionManager == nullptr) {
			return false;
		}

		const uint64_t headerHeight = syncStatus.GetHeaderHeight();
		const uint64_t requestedHeight = Consensus::GetTxHashSetArchiveHeight(headerHeight);
		BlockHeaderPtr pHeader = m_pBlockChain->GetBlockHeaderByHeight(requestedHeight, EChainType::CANDIDATE);
		if (pHeader == nullptr) {
			return false;
		}

		Hash hash = pHeader->GetHash();
		const TxHashSetRequestMessage txHashSetRequestMessage(std::move(hash), requestedHeight);
		m_pPeer = pConnectionManager->SendMessageToMostWorkPeer(txHashSetRequestMessage);

		if (m_pPeer != nullptr)
		{
			m_timeRequested = std::chrono::system_clock::now();
			m_requestedHeight = requestedHeight;
		}
	}

	return m_pPeer != nullptr;
}
