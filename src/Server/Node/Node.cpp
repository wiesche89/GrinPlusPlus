#include "Node.h"
#include "NodeRPCServer.h"
#include "NodeClients/DefaultNodeClient.h"
#include "../console.h"

#include <Consensus.h>
#include <Core/Context.h>
#include <P2P/P2PServer.h>
#include <Core/Config.h>
#include <BlockChain/BlockChain.h>
#include <Database/Database.h>
#include <Common/Logger.h>
#include <PMMR/TxHashSetManager.h>

#include <iostream>
#include <thread>

Node::Node(
	const Context::Ptr& pContext,
	std::unique_ptr<NodeRPCServer>&& pNodeRPCServer,
	std::shared_ptr<DefaultNodeClient> pNodeClient)
	: m_pContext(pContext),
	m_pNodeRPCServer(std::move(pNodeRPCServer)),
	m_pNodeClient(pNodeClient)
{

}

Node::~Node()
{
	LOG_INFO("Shutting down node daemon");
}

std::unique_ptr<Node> Node::Create(const Context::Ptr& pContext, const ServerPtr& pServer)
{
    auto pNodeClient = DefaultNodeClient::Create(pContext);
    auto pNodeRPCServer = NodeRPCServer::Create(
        pServer,
        pNodeClient->GetNodeContext()
    );


	return std::make_unique<Node>(
		pContext,
		std::move(pNodeRPCServer),
		pNodeClient
	);
}

std::shared_ptr<INodeClient> Node::GetNodeClient() const
{
	return m_pNodeClient;
}

void Node::UpdateDisplay(const int secondsRunning)
{
	SyncStatusConstPtr pSyncStatus = m_pNodeClient->GetP2PServer()->GetSyncStatus();

	IO::Clear();

	std::cout << "Time Running: " << secondsRunning << "s";

	const ESyncStatus status = pSyncStatus->GetStatus();
	if (status == ESyncStatus::NOT_SYNCING)
	{
		std::cout << "\nStatus: Running";
	}
	else if (status == ESyncStatus::WAITING_FOR_PEERS)
	{
		std::cout << "\nStatus: Waiting for Peers";
	}
	else if (status == ESyncStatus::SYNCING_HEADERS)
	{
		const uint64_t networkHeight = pSyncStatus->GetNetworkHeight();
		const uint64_t percentage = networkHeight > 0 ? (pSyncStatus->GetHeaderHeight() * 100 / networkHeight) : 0;
		std::cout << "\nStatus: Syncing Headers (" << percentage << "%)";
	}
	else if (status == ESyncStatus::SYNCING_TXHASHSET)
	{
		const uint64_t downloaded = pSyncStatus->GetDownloaded();
		const uint64_t downloadSize = pSyncStatus->GetDownloadSize();
		const uint64_t percentage = downloadSize > 0 ? (downloaded * 100) / downloadSize : 0;

		std::cout << "\nStatus: Syncing TxHashSet " << downloaded << "/" << downloadSize << "(" << percentage << "%)";
	}
	else if (status == ESyncStatus::SYNCING_TXHASHSET_PIBD)
	{
		const uint64_t completedLeaves = pSyncStatus->GetPIBDCompletedLeaves();
		const uint64_t leavesRequired = pSyncStatus->GetPIBDLeavesRequired();
		const uint64_t percentage = leavesRequired > 0 ? (completedLeaves * 100) / leavesRequired : 0;

		std::cout << "\nStatus: Syncing TxHashSet PIBD "
			<< completedLeaves << "/" << leavesRequired << " leaves (" << percentage << "%)"
			<< " height " << pSyncStatus->GetPIBDCompletedToHeight() << "/" << pSyncStatus->GetPIBDRequiredHeight()
			<< " aborted=" << (pSyncStatus->GetPIBDAborted() ? "true" : "false")
			<< " errored=" << (pSyncStatus->GetPIBDErrored() ? "true" : "false");
	}
	else if (status == ESyncStatus::PROCESSING_TXHASHSET)
	{
		std::cout << "\nStatus: Validating TxHashSet (" << (int)pSyncStatus->GetProcessingStatus() << "%)";
	}
	else if (status == ESyncStatus::TXHASHSET_PIBD_LEAFSET_UPDATE)
	{
		std::cout << "\nStatus: Updating TxHashSet PIBD leafsets";
	}
	else if (status == ESyncStatus::TXHASHSET_SETUP)
	{
		std::cout << "\nStatus: Setting up TxHashSet validation (" << (int)pSyncStatus->GetProcessingStatus() << "%)";
	}
	else if (status == ESyncStatus::TXHASHSET_RANGE_PROOFS_VALIDATION)
	{
		std::cout << "\nStatus: Validating TxHashSet range proofs (" << (int)pSyncStatus->GetProcessingStatus() << "%)";
		if (pSyncStatus->GetProcessingTotal() > 0)
		{
			std::cout << " leaf " << pSyncStatus->GetProcessingCurrent() << "/" << pSyncStatus->GetProcessingTotal();
		}
	}
	else if (status == ESyncStatus::TXHASHSET_KERNEL_HISTORY_VALIDATION)
	{
		std::cout << "\nStatus: Validating TxHashSet kernel history (" << (int)pSyncStatus->GetProcessingStatus() << "%)";
		if (pSyncStatus->GetProcessingTotal() > 0)
		{
			std::cout << " header " << pSyncStatus->GetProcessingCurrent() << "/" << pSyncStatus->GetProcessingTotal();
		}
	}
	else if (status == ESyncStatus::TXHASHSET_KERNEL_SUMS_VALIDATION)
	{
		std::cout << "\nStatus: Validating TxHashSet kernel sums (" << (int)pSyncStatus->GetProcessingStatus() << "%)";
		if (pSyncStatus->GetProcessingTotal() > 0)
		{
			std::cout << " item " << pSyncStatus->GetProcessingCurrent() << "/" << pSyncStatus->GetProcessingTotal();
		}
	}
	else if (status == ESyncStatus::TXHASHSET_NRD_KERNELS_VALIDATION)
	{
		std::cout << "\nStatus: Validating TxHashSet NRD kernels (" << (int)pSyncStatus->GetProcessingStatus() << "%)";
		if (pSyncStatus->GetProcessingTotal() > 0)
		{
			std::cout << " kernel " << pSyncStatus->GetProcessingCurrent() << "/" << pSyncStatus->GetProcessingTotal();
		}
	}
	else if (status == ESyncStatus::TXHASHSET_KERNEL_SIGNATURES_VALIDATION)
	{
		std::cout << "\nStatus: Validating TxHashSet kernel signatures (" << (int)pSyncStatus->GetProcessingStatus() << "%)";
		if (pSyncStatus->GetProcessingTotal() > 0)
		{
			std::cout << " kernel " << pSyncStatus->GetProcessingCurrent() << "/" << pSyncStatus->GetProcessingTotal();
		}
	}
	else if (status == ESyncStatus::TXHASHSET_SAVE)
	{
		std::cout << "\nStatus: Saving TxHashSet";
	}
	else if (status == ESyncStatus::TXHASHSET_DONE)
	{
		std::cout << "\nStatus: TxHashSet Done";
	}
	else if (status == ESyncStatus::TXHASHSET_SYNC_FAILED)
	{
		std::cout << "\nStatus: TxHashSet Sync Failed - Trying Again";
	}
	else if (status == ESyncStatus::SYNCING_BLOCKS)
	{
		std::cout << "\nStatus: Syncing blocks";
	}

	if (pSyncStatus->IsTxHashSetSyncStatus() && status != ESyncStatus::SYNCING_TXHASHSET_PIBD)
	{
		std::cout << "\nPIBD: "
			<< pSyncStatus->GetPIBDCompletedLeaves() << "/" << pSyncStatus->GetPIBDLeavesRequired()
			<< " leaves, height " << pSyncStatus->GetPIBDCompletedToHeight() << "/" << pSyncStatus->GetPIBDRequiredHeight()
			<< ", aborted=" << (pSyncStatus->GetPIBDAborted() ? "true" : "false")
			<< ", errored=" << (pSyncStatus->GetPIBDErrored() ? "true" : "false");
	}

	std::cout << "\nNumConnections: " << pSyncStatus->GetNumActiveConnections();
	std::cout << "\nHeader Height: " << pSyncStatus->GetHeaderHeight();
	std::cout << "\nHeader Difficulty: " << pSyncStatus->GetHeaderDifficulty();
	std::cout << "\nBlock Height: " << pSyncStatus->GetBlockHeight();
	std::cout << "\nBlock Difficulty: " << pSyncStatus->GetBlockDifficulty();
	std::cout << "\nNetwork Height: " << pSyncStatus->GetNetworkHeight();
	std::cout << "\nNetwork Difficulty: " << pSyncStatus->GetNetworkDifficulty();
	std::cout << "\n\nPress Ctrl-C to exit...";

	IO::Flush();
}
