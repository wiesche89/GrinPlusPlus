#include "HeaderSyncer.h"
#include "../BlockLocator.h"
#include "../HeaderBatchCache.h"
#include "../Messages/GetHeadersMessage.h"
#include "../Messages/HeaderSegmentMessage.h"

#include <Common/Logger.h>
#include <P2P/Capabilities.h>
#include <P2P/Common.h>
#include <algorithm>

static constexpr size_t PIHD_MAX_IN_FLIGHT_SEGMENTS = 3;
static constexpr size_t PIHD_MAX_REQUESTS_PER_TICK = 3;
static constexpr size_t PIHD_MAX_IN_FLIGHT_SEGMENTS_PER_PEER = 3;
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

	const std::shared_ptr<ConnectionManager> pConnectionManager = m_pConnectionManager.lock();
	if (pConnectionManager == nullptr) {
		return false;
	}

	const auto now = std::chrono::system_clock::now();
	const size_t previousPendingCount = m_pendingRequests.size();
	bool removedPendingRequest = false;
	m_pendingRequests.erase(
		std::remove_if(
			m_pendingRequests.begin(),
			m_pendingRequests.end(),
			[pConnectionManager, now, height, &removedPendingRequest](const PendingHeaderRequest& request) {
				if (request.pPeer == nullptr
					|| !pConnectionManager->IsConnected(request.pPeer->GetIPAddress())
					|| request.timeout < now) {
					removedPendingRequest = true;
					return true;
				}

				if (request.identifier.GetHeight() == P2P::PIHD_HEADER_SEGMENT_HEIGHT) {
					const uint64_t segmentEnd = request.identifier.GetLeafOffset() + request.identifier.GetSegmentCapacity();
					const bool completed = height >= segmentEnd;
					removedPendingRequest = removedPendingRequest || completed;
					return completed;
				}

				const bool completed = height > request.identifier.GetLeafOffset();
				removedPendingRequest = removedPendingRequest || completed;
				return completed;
			}),
		m_pendingRequests.end());

	size_t pihdPeerCount = 0;
	const uint64_t mostWork = pConnectionManager->GetMostWork();
	for (const ConnectedPeer& peer : pConnectionManager->GetConnectedPeers()) {
		if (peer.GetTotalDifficulty() >= mostWork
			&& peer.GetPeer() != nullptr
			&& peer.GetPeer()->GetCapabilities().HasCapability(Capabilities::PIHD_HIST)) {
			++pihdPeerCount;
		}
	}

	const size_t targetPendingCount = pihdPeerCount > 0 ? PIHD_MAX_IN_FLIGHT_SEGMENTS : 0;
	if (removedPendingRequest || m_pendingRequests.size() < targetPendingCount) {
		if (previousPendingCount > 0) {
			LOG_TRACE("PIHD header requests completed, timed out, or peers disconnected. Requesting again.");
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
	const auto now = std::chrono::system_clock::now();
	for (const ConnectedPeer& peer : peers) {
		if (peer.GetTotalDifficulty() >= mostWork
			&& peer.GetPeer() != nullptr
			&& peer.GetPeer()->GetCapabilities().HasCapability(Capabilities::PIHD_HIST)) {
			pihdPeers.push_back(peer);
		}
	}

	size_t sentCount = 0;
	bool sentPIHDRequest = false;
	const auto timeout = now + PIHD_HEADER_TIMEOUT;
	const uint64_t nextHeight = syncStatus.GetHeaderHeight() + 1;
	uint64_t nextSegmentIndex = (nextHeight - 1) / (1ULL << P2P::PIHD_HEADER_SEGMENT_HEIGHT);

	size_t pihdPendingCount = 0;
	for (const PendingHeaderRequest& request : m_pendingRequests) {
		if (request.identifier.GetHeight() == P2P::PIHD_HEADER_SEGMENT_HEIGHT) {
			++pihdPendingCount;
			nextSegmentIndex = (std::max)(nextSegmentIndex, request.identifier.GetIndex() + 1);
		}
	}

	for (ConnectedPeer& peer : pihdPeers) {
		if (sentCount >= PIHD_MAX_REQUESTS_PER_TICK) {
			break;
		}

		if ((pihdPendingCount + sentCount) >= PIHD_MAX_IN_FLIGHT_SEGMENTS) {
			break;
		}

		while (sentCount < PIHD_MAX_REQUESTS_PER_TICK
			&& (pihdPendingCount + sentCount) < PIHD_MAX_IN_FLIGHT_SEGMENTS)
		{
			const PeerPtr pPeer = peer.GetPeer();
			const size_t peerPendingCount = std::count_if(
				m_pendingRequests.cbegin(),
				m_pendingRequests.cend(),
				[pPeer](const PendingHeaderRequest& request) {
					return request.pPeer != nullptr
						&& pPeer != nullptr
						&& request.pPeer->GetIPAddress() == pPeer->GetIPAddress()
						&& request.identifier.GetHeight() == P2P::PIHD_HEADER_SEGMENT_HEIGHT;
				});
			if (peerPendingCount >= PIHD_MAX_IN_FLIGHT_SEGMENTS_PER_PEER) {
				break;
			}

			if (peer.GetHeight() < (nextSegmentIndex * (1ULL << P2P::PIHD_HEADER_SEGMENT_HEIGHT) + 1)) {
				break;
			}

			SegmentIdentifier identifier(P2P::PIHD_HEADER_SEGMENT_HEIGHT, nextSegmentIndex++);
			const GetHeaderSegmentMessage getHeaderSegmentMessage(identifier);
			if (pConnectionManager->SendMessageToPeer(getHeaderSegmentMessage, pPeer)) {
				m_pendingRequests.push_back(PendingHeaderRequest{ pPeer, identifier, timeout });
				LOG_TRACE_F("PIHD requested header segment {}:{} from {}.",
					identifier.GetHeight(),
					identifier.GetIndex(),
					pPeer);
				++sentCount;
				sentPIHDRequest = true;
			} else {
				break;
			}
		}
	}

	if (sentCount == 0 && m_pendingRequests.empty()) {
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
			LOG_TRACE_F("PIHD requested header segments from {} peer(s).", sentCount);
		} else {
			LOG_TRACE_F("HeaderSync requested headers from {} peer(s).", sentCount);
		}
		m_lastHeight = syncStatus.GetHeaderHeight();
	}

	return sentCount > 0;
}
