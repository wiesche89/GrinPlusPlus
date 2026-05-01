#include "HeaderSyncer.h"
#include "../BlockLocator.h"
#include "../HeaderBatchCache.h"
#include "../Messages/GetHeadersMessage.h"
#include "../Messages/HeaderSegmentMessage.h"

#include <Common/Logger.h>
#include <P2P/Capabilities.h>
#include <P2P/Common.h>
#include <algorithm>

static constexpr size_t PIHD_MAX_PARALLEL_PEERS = 3;
static constexpr std::chrono::seconds PIHD_HEADER_TIMEOUT(10);

bool HeaderSyncer::SyncHeaders(const SyncStatus& syncStatus, const bool startup)
{
	const uint64_t chainHeight = syncStatus.GetHeaderHeight();
	const uint64_t networkHeight = syncStatus.GetNetworkHeight();

	// This solves an issue that occurs due to the handshake not containing height.
	if (networkHeight == 0)
	{
		return true;
	}

	if (networkHeight >= (chainHeight + 5) || (startup && networkHeight > chainHeight))
	{
		if (IsHeaderSyncDue(syncStatus))
		{
			RequestHeaders(syncStatus);
		}

		return true;
	}

	m_pendingRequests.clear();
	HeaderBatchCache::Get().Clear();

	return false;
}

bool HeaderSyncer::IsHeaderSyncDue(const SyncStatus& syncStatus)
{
	if (m_pendingRequests.empty())
	{
		return true;
	}

	const uint64_t height = syncStatus.GetHeaderHeight();

	// Check if headers were received, and we're ready to request the next locator.
	if (height > m_lastHeight)
	{
		LOG_TRACE("PIHD headers received. Requesting next batch.");
		m_pendingRequests.clear();
		return true;
	}

	const std::shared_ptr<ConnectionManager> pConnectionManager = m_pConnectionManager.lock();
	if (pConnectionManager == nullptr) {
		return false;
	}

	const auto now = std::chrono::system_clock::now();
	const size_t previousPendingCount = m_pendingRequests.size();
	m_pendingRequests.erase(
		std::remove_if(
			m_pendingRequests.begin(),
			m_pendingRequests.end(),
			[pConnectionManager, now](const PendingHeaderRequest& request) {
				return request.pPeer == nullptr
					|| !pConnectionManager->IsConnected(request.pPeer->GetIPAddress())
					|| request.timeout < now;
			}),
		m_pendingRequests.end());

	if (m_pendingRequests.empty()) {
		if (previousPendingCount > 0) {
			LOG_DEBUG("PIHD header requests timed out or peers disconnected. Requesting again.");
		}
		return true;
	}

	return false;
}

bool HeaderSyncer::RequestHeaders(const SyncStatus& syncStatus)
{
	LOG_TRACE("Requesting headers.");

	const std::shared_ptr<ConnectionManager> pConnectionManager = m_pConnectionManager.lock();
	if (pConnectionManager == nullptr) {
		return false;
	}

	std::vector<ConnectedPeer> peers = pConnectionManager->GetConnectedPeers();
	std::vector<ConnectedPeer> pihdPeers;
	const uint64_t mostWork = pConnectionManager->GetMostWork();
	for (const ConnectedPeer& peer : peers) {
		if (peer.GetTotalDifficulty() >= mostWork
			&& peer.GetPeer() != nullptr
			&& peer.GetPeer()->GetCapabilities().HasCapability(Capabilities::PIHD_HIST)) {
			pihdPeers.push_back(peer);
		}
	}

	size_t sentCount = 0;
	bool sentPIHDRequest = false;
	const auto timeout = std::chrono::system_clock::now() + PIHD_HEADER_TIMEOUT;
	m_pendingRequests.clear();
	const uint64_t nextHeight = syncStatus.GetHeaderHeight() + 1;
	uint64_t nextSegmentIndex = (nextHeight - 1) / (1ULL << P2P::PIHD_HEADER_SEGMENT_HEIGHT);
	for (ConnectedPeer& peer : pihdPeers) {
		if (sentCount >= PIHD_MAX_PARALLEL_PEERS) {
			break;
		}

		if (peer.GetHeight() < (nextSegmentIndex * (1ULL << P2P::PIHD_HEADER_SEGMENT_HEIGHT) + 1)) {
			continue;
		}

		SegmentIdentifier identifier(P2P::PIHD_HEADER_SEGMENT_HEIGHT, nextSegmentIndex++);
		const GetHeaderSegmentMessage getHeaderSegmentMessage(identifier);
		const PeerPtr pPeer = peer.GetPeer();
		if (pConnectionManager->SendMessageToPeer(getHeaderSegmentMessage, pPeer)) {
			m_pendingRequests.push_back(PendingHeaderRequest{ pPeer, identifier, timeout });
			LOG_DEBUG_F("PIHD requested header segment {}:{} from {}.",
				identifier.GetHeight(),
				identifier.GetIndex(),
				pPeer);
			++sentCount;
			sentPIHDRequest = true;
		}
	}

	if (sentCount == 0) {
		std::vector<Hash> locators = BlockLocator(m_pBlockChain).GetLocators(syncStatus);
		const GetHeadersMessage getHeadersMessage(std::move(locators));
		PeerPtr pPeer = pConnectionManager->SendMessageToMostWorkPeer(getHeadersMessage);
		if (pPeer != nullptr) {
			m_pendingRequests.push_back(PendingHeaderRequest{ pPeer, SegmentIdentifier(), timeout });
			sentCount = 1;
		}
	}

	if (sentCount > 0) {
		if (sentPIHDRequest) {
			LOG_DEBUG_F("PIHD requested header segments from {} peer(s).", sentCount);
		} else {
			LOG_DEBUG_F("HeaderSync requested headers from {} peer(s).", sentCount);
		}
		m_lastHeight = syncStatus.GetHeaderHeight();
	}

	return sentCount > 0;
}
