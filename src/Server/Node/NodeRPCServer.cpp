#include "NodeRPCServer.h"
#include "NodeContext.h"

#include "API/HeaderAPI.h"
#include "API/BlockAPI.h"
#include "API/ServerAPI.h"
#include "API/ChainAPI.h"
#include "API/PeersAPI.h"
#include "API/TxHashSetAPI.h"

#include <Common/Logger.h>
#include <Core/Global.h>
#include <Net/Util/HTTPUtil.h>

static int Shutdown_Handler(struct mg_connection* conn, void*)
{
	Global::Shutdown();
	return HTTPUtil::BuildSuccessResponse(conn, "");
}

NodeRPCServer::UPtr NodeRPCServer::Create(const ServerPtr& pServer, std::shared_ptr<NodeContext> pNodeContext)
{
	NodeServer::UPtr pServerV2 = NodeServer::Create(pServer,
													pNodeContext->m_pBlockChain,
													pNodeContext->m_pP2PServer,
													pNodeContext->m_pTxHashSetManager->GetTxHashSet(),
													pNodeContext->m_pDatabase,
													pNodeContext->m_pTransactionPool);

	return std::make_unique<NodeRPCServer>(pNodeContext, std::move(pServerV2));
}
