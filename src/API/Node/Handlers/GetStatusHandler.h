#pragma once

#include <Consensus.h>
#include <BlockChain/BlockChain.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <optional>

class GetStatusHandler : public RPCMethod
{
public:
	GetStatusHandler(const IBlockChain::Ptr& pBlockChain, const IP2PServerPtr& pP2PServer)
		: m_pBlockChain(pBlockChain), m_pP2PServer(pP2PServer) { }
	~GetStatusHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		Json::Value statusNode;
		statusNode["chain"] = GetChainString();
		statusNode["protocol_version"] = P2P::PROTOCOL_VERSION;
		statusNode["user_agent"] = P2P::USER_AGENT;
		statusNode["connections"] = m_pP2PServer->GetConnectedPeers().size();

		auto pTip = m_pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);

		if (pTip == nullptr)
		{
			return request.BuildError("INVALID_CHAIN_STATUS", "Failed to find tip.");
		}

		Json::Value tipNode;
		tipNode["height"] = pTip->GetHeight();
		tipNode["last_block_pushed"] = pTip->GetHash().ToHex();
		tipNode["prev_block_to_last"] = pTip->GetPreviousHash().ToHex();
		tipNode["total_difficulty"] = pTip->GetTotalDifficulty();
		statusNode["tip"] = tipNode;	

		SyncStatusConstPtr pSyncStatus = m_pP2PServer->GetSyncStatus();
		statusNode["sync_status"] = GetStatusString(*pSyncStatus);
		
		Json::Value syncInfo;
		bool hasSyncInfo = false;
		switch (pSyncStatus->GetStatus())
		{
			case ESyncStatus::SYNCING_HEADERS:
			{
				syncInfo["highest_height"] = pSyncStatus->GetNetworkHeight();
				syncInfo["current_height"] = m_pBlockChain->GetHeight(EChainType::CANDIDATE);
				hasSyncInfo = true;
				break;
			}
			case ESyncStatus::SYNCING_TXHASHSET_PIBD:
			{
				syncInfo["aborted"] = pSyncStatus->GetPIBDAborted();
				syncInfo["errored"] = pSyncStatus->GetPIBDErrored();
				syncInfo["completed_leaves"] = Json::UInt64(pSyncStatus->GetPIBDCompletedLeaves());
				syncInfo["leaves_required"] = Json::UInt64(pSyncStatus->GetPIBDLeavesRequired());
				syncInfo["completed_to_height"] = Json::UInt64(pSyncStatus->GetPIBDCompletedToHeight());
				syncInfo["required_height"] = Json::UInt64(pSyncStatus->GetPIBDRequiredHeight());
				hasSyncInfo = true;
				break;
			}
			case ESyncStatus::SYNCING_TXHASHSET:
			case ESyncStatus::TXHASHSET_SYNC_FAILED:
			{
				syncInfo["downloaded_size"] = Json::UInt64(pSyncStatus->GetDownloaded());
				syncInfo["total_size"] = Json::UInt64(pSyncStatus->GetDownloadSize());
				hasSyncInfo = true;
				break;
			}
			case ESyncStatus::PROCESSING_TXHASHSET:
			case ESyncStatus::TXHASHSET_PIBD_LEAFSET_UPDATE:
			case ESyncStatus::TXHASHSET_SETUP:
			case ESyncStatus::TXHASHSET_KERNEL_SUMS_VALIDATION:
			{
				syncInfo["headers"] = Json::Value(Json::nullValue);
				syncInfo["headers_total"] = Json::Value(Json::nullValue);
				syncInfo["kernel_pos"] = Json::Value(Json::nullValue);
				syncInfo["kernel_pos_total"] = Json::Value(Json::nullValue);
				hasSyncInfo = true;
				break;
			}
			case ESyncStatus::TXHASHSET_KERNEL_HISTORY_VALIDATION:
			{
				syncInfo["headers"] = Json::UInt64(pSyncStatus->GetProcessingCurrent());
				syncInfo["headers_total"] = Json::UInt64(pSyncStatus->GetProcessingTotal());
				syncInfo["kernel_pos"] = Json::Value(Json::nullValue);
				syncInfo["kernel_pos_total"] = Json::Value(Json::nullValue);
				hasSyncInfo = true;
				break;
			}
			case ESyncStatus::TXHASHSET_NRD_KERNELS_VALIDATION:
			{
				syncInfo["headers"] = Json::Value(Json::nullValue);
				syncInfo["headers_total"] = Json::Value(Json::nullValue);
				syncInfo["kernel_pos"] = Json::UInt64(pSyncStatus->GetProcessingCurrent());
				syncInfo["kernel_pos_total"] = Json::UInt64(pSyncStatus->GetProcessingTotal());
				hasSyncInfo = true;
				break;
			}
			case ESyncStatus::TXHASHSET_RANGE_PROOFS_VALIDATION:
			{
				syncInfo["rproofs"] = Json::UInt64(pSyncStatus->GetProcessingCurrent());
				syncInfo["rproofs_total"] = Json::UInt64(pSyncStatus->GetProcessingTotal());
				hasSyncInfo = true;
				break;
			}
			case ESyncStatus::TXHASHSET_KERNEL_SIGNATURES_VALIDATION:
			{
				syncInfo["kernels"] = Json::UInt64(pSyncStatus->GetProcessingCurrent());
				syncInfo["kernels_total"] = Json::UInt64(pSyncStatus->GetProcessingTotal());
				hasSyncInfo = true;
				break;
			}
			case ESyncStatus::TXHASHSET_SAVE:
			case ESyncStatus::TXHASHSET_DONE:
			case ESyncStatus::WAITING_FOR_PEERS:
			case ESyncStatus::NOT_SYNCING:
			{
				break;
			}
			case ESyncStatus::SYNCING_BLOCKS:
			{
				syncInfo["current_height"] = pSyncStatus->GetBlockHeight();
				syncInfo["highest_height"] = pSyncStatus->GetNetworkHeight();
				hasSyncInfo = true;
				break;
			}
		}
		if (hasSyncInfo) {
			statusNode["sync_info"] = syncInfo;
		}

		Json::Value result;
		result["Ok"] = statusNode;
		return request.BuildResult(result);
	}

	static std::string GetStatusString(const SyncStatus& syncStatus)
	{
		const ESyncStatus status = syncStatus.GetStatus();

		switch (status)
		{
			case ESyncStatus::SYNCING_HEADERS:
			{
				return "header_sync";
			}
			case ESyncStatus::SYNCING_TXHASHSET:
			{
				return "txhashset_download";
			}
			case ESyncStatus::SYNCING_TXHASHSET_PIBD:
			{
				return "txhashsetpibd_download";
			}
			case ESyncStatus::TXHASHSET_SYNC_FAILED:
			{
				return "txhashset_download";
			}
			case ESyncStatus::PROCESSING_TXHASHSET:
			case ESyncStatus::TXHASHSET_PIBD_LEAFSET_UPDATE:
			case ESyncStatus::TXHASHSET_SETUP:
			case ESyncStatus::TXHASHSET_KERNEL_HISTORY_VALIDATION:
			case ESyncStatus::TXHASHSET_NRD_KERNELS_VALIDATION:
			case ESyncStatus::TXHASHSET_KERNEL_SUMS_VALIDATION:
			{
				return "txhashset_setup";
			}
			case ESyncStatus::TXHASHSET_RANGE_PROOFS_VALIDATION:
			{
				return "txhashset_rangeproofs_validation";
			}
			case ESyncStatus::TXHASHSET_KERNEL_SIGNATURES_VALIDATION:
			{
				return "txhashset_kernels_validation";
			}
			case ESyncStatus::TXHASHSET_SAVE:
			{
				return "txhashset_save";
			}
			case ESyncStatus::TXHASHSET_DONE:
			{
				return "txhashset_done";
			}
			case ESyncStatus::SYNCING_BLOCKS:
			{
				return "body_sync";
			}
			case ESyncStatus::WAITING_FOR_PEERS:
			{
				return "awaiting_peers";
			}
			case ESyncStatus::NOT_SYNCING:
			{
				return "no_sync";
			}
		}

		return "unknown";
	}

	static std::string GetChainString()
	{
		return Global::IsMainnet() ? "main" : "test";
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	IBlockChain::Ptr m_pBlockChain;
	IP2PServerPtr m_pP2PServer;
};



