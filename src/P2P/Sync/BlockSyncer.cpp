#include "BlockSyncer.h"
#include "../ConnectionManager.h"
#include "../Messages/GetBlockMessage.h"

#include <Common/Logger.h>
#include <Common/Util/StringUtil.h>
#include <Crypto/CSPRNG.h>

#include <algorithm>

namespace
{
    static constexpr uint64_t BLOCK_SYNC_WINDOW_PER_PEER = 16;
    static constexpr uint64_t BLOCK_SYNC_REFILL_THRESHOLD_PER_PEER = BLOCK_SYNC_WINDOW_PER_PEER / 4;
}

bool BlockSyncer::SyncBlocks(const SyncStatus& syncStatus, const bool startup)
{
    const uint64_t chainHeight = syncStatus.GetBlockHeight();
    const uint64_t networkHeight = syncStatus.GetNetworkHeight();

    if (networkHeight >= (chainHeight + 5) || (startup && networkHeight > chainHeight)) {
        const bool allowRetries = (chainHeight == m_lastHeight);
        if (IsBlockSyncDue(syncStatus)) {
            RequestBlocks(allowRetries);

            m_lastHeight = chainHeight;
        }

        return true;
    }

    return false;
}

bool BlockSyncer::IsBlockSyncDue(const SyncStatus& syncStatus)
{
    const uint64_t chainHeight = syncStatus.GetBlockHeight();

    const uint64_t blocksDownloaded = (chainHeight - m_lastHeight);
    if (blocksDownloaded > 0) {
        LOG_TRACE_F("{} blocks received since last check.", blocksDownloaded);
    }

    // Remove downloaded blocks from queue.
    for (size_t i = m_lastHeight; i <= chainHeight; i++) {
        m_requestedBlocks.erase(i);
    }

    m_lastHeight = chainHeight;

    // Check if blocks were received, and we're ready to request next batch.
    const auto pConnectionManager = m_pConnectionManager.lock();
    const uint64_t eligiblePeerCount = pConnectionManager != nullptr
        ? (uint64_t)pConnectionManager->GetMostWorkPeers().size()
        : 0;
    const uint64_t refillThreshold = (std::max<uint64_t>)(
        BLOCK_SYNC_REFILL_THRESHOLD_PER_PEER * (std::max<uint64_t>)(eligiblePeerCount, 1),
        1);
    if (m_requestedBlocks.size() < refillThreshold) {
        LOG_DEBUG_F(
            "Requesting next block sync batch. pending_blocks={}, refill_threshold={}, eligible_peers={}",
            m_requestedBlocks.size(),
            refillThreshold,
            eligiblePeerCount);
        return true;
    }

    // Make sure we have valid requests for the first refill window.
    std::vector<std::pair<uint64_t, Hash>> blocksNeeded = m_pBlockChain->GetBlocksNeeded(refillThreshold);
    for (auto iter = blocksNeeded.cbegin(); iter != blocksNeeded.cend(); iter++) {
        auto requested_block = m_requestedBlocks.find(iter->first);
        if (requested_block == m_requestedBlocks.end()) {
            LOG_TRACE_F("Block {} not requested yet.", iter->first);
            return true;
        }

        if (IsSlowPeer(requested_block->second.PEER)) {
            LOG_TRACE_F(
                "Waiting on block {} from banned peer {}.",
                requested_block->second.BLOCK_HEIGHT,
                requested_block->second.PEER
            );
            return true;
        }

		if (requested_block->second.TIMEOUT < std::chrono::system_clock::now()) {
			if (m_pPipeline->GetBlockPipe()->IsProcessingBlock(iter->second)) {
				continue;
			}

			if (blocksDownloaded > 0) {
				continue;
			}

			LOG_WARNING_F("Block {} request timed out.", iter->first);
			return true;
		}
	}

    return false;
}

bool BlockSyncer::RequestBlocks(const bool allowRetries)
{
    LOG_TRACE("Requesting blocks.");

    auto pConnectionManager = m_pConnectionManager.lock();
    if (pConnectionManager == nullptr) {
        LOG_DEBUG("No connection manager available for block sync.");
        return false;
    }

    std::vector<PeerPtr> mostWorkPeers = pConnectionManager->GetMostWorkPeers();
    const uint64_t numPeers = mostWorkPeers.size();
    if (mostWorkPeers.empty()) {
        LOG_DEBUG("No most-work peers found.");
        return false;
    }

    const uint64_t pendingBefore = m_requestedBlocks.size();
    const uint64_t numBlocksNeeded = BLOCK_SYNC_WINDOW_PER_PEER * numPeers;
    std::vector<std::pair<uint64_t, Hash>> blocksNeeded = m_pBlockChain->GetBlocksNeeded(2 * numBlocksNeeded);
    if (blocksNeeded.empty()) {
        LOG_DEBUG_F(
            "No blocks needed. eligible_peers={}, pending_blocks={}",
            numPeers,
            pendingBefore);
        return false;
    }

    size_t blockIndex = 0;

    std::vector<std::pair<uint64_t, Hash>> blocksToRequest;

    while (blockIndex < blocksNeeded.size()) {
        if (m_pPipeline->GetBlockPipe()->IsProcessingBlock(blocksNeeded[blockIndex].second) || m_pBlockChain->HasBlock(blocksNeeded[blockIndex].first, blocksNeeded[blockIndex].second)) {
            ++blockIndex;
            continue;
        }

        auto iter = m_requestedBlocks.find(blocksNeeded[blockIndex].first);
        if (iter != m_requestedBlocks.end()) {
            if (IsSlowPeer(iter->second.PEER) || iter->second.TIMEOUT < std::chrono::system_clock::now()) {
                if (!allowRetries && !IsSlowPeer(iter->second.PEER)) {
                    ++blockIndex;
                    continue;
                }

                if (!iter->second.PEER->IsBanned()) {
                    if (!iter->second.RETRIED) {
                        LOG_INFO_F("Requesting block {} from peer {} again", iter->second.BLOCK_HEIGHT, iter->second.PEER);
                        GetBlockMessage getBlockMessage(blocksNeeded[blockIndex].second);
                        if (pConnectionManager->SendMessageToPeer(getBlockMessage, iter->second.PEER)) {
                            iter->second.TIMEOUT = std::chrono::system_clock::now() + std::chrono::seconds(5);
                            iter->second.RETRIED = true;
                            ++blockIndex;
                            continue;
                        }
                    }

                    LOG_ERROR_F("Banning peer {} for fraud height.", iter->second.PEER);
                    iter->second.PEER->Ban(EBanReason::FraudHeight);
                }

                m_slowPeers.insert(iter->second.PEER->GetIPAddress());

                blocksToRequest.emplace_back(blocksNeeded[blockIndex]);
            }
        } else {
            blocksToRequest.emplace_back(blocksNeeded[blockIndex]);
        }

        if (blocksToRequest.size() >= numBlocksNeeded) {
            break;
        }

        ++blockIndex;
    }

    size_t nextPeer = CSPRNG::GenerateRandom(0, mostWorkPeers.size() - 1);
    size_t sentBlocks = 0;
    uint64_t firstRequestedHeight = 0;
    uint64_t lastRequestedHeight = 0;
    for (size_t i = 0; i < blocksToRequest.size(); i++) {
        const GetBlockMessage getBlockMessage(blocksToRequest[i].second);
        if (pConnectionManager->SendMessageToPeer(getBlockMessage, mostWorkPeers[nextPeer])) {
            RequestedBlock blockRequested;
            blockRequested.BLOCK_HEIGHT = blocksToRequest[i].first;
            blockRequested.PEER = mostWorkPeers[nextPeer];
            blockRequested.TIMEOUT = std::chrono::system_clock::now() + std::chrono::seconds(10);
            blockRequested.RETRIED = false;

            m_requestedBlocks[blockRequested.BLOCK_HEIGHT] = std::move(blockRequested);
            if (firstRequestedHeight == 0) {
                firstRequestedHeight = blocksToRequest[i].first;
            }
            lastRequestedHeight = blocksToRequest[i].first;
            ++sentBlocks;
        }

        if ((i + 1) % BLOCK_SYNC_WINDOW_PER_PEER == 0) {
            nextPeer = (nextPeer + 1) % mostWorkPeers.size();
        }
    }

    if (!blocksToRequest.empty()) {
        LOG_DEBUG_F(
            "Body sync requested blocks. requested={}, sent={}, pending_before={}, pending_after={}, eligible_peers={}, window_per_peer={}, first_height={}, last_height={}",
            blocksToRequest.size(),
            sentBlocks,
            pendingBefore,
            m_requestedBlocks.size(),
            numPeers,
            BLOCK_SYNC_WINDOW_PER_PEER,
            firstRequestedHeight,
            lastRequestedHeight);
    }

    return true;
}
