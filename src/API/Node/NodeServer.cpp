#include <API/Node/NodeServer.h>

#include "Handlers/GetBlockHandler.h"
#include "Handlers/GetBlocksHandler.h"
#include "Handlers/GetHeaderHandler.h"
#include "Handlers/GetKernelHandler.h"
#include "Handlers/GetOutputsHandler.h"
#include "Handlers/GetPMMRIndicesHandler.h"
#include "Handlers/GetPoolSizeHandler.h"
#include "Handlers/GetStempoolSizeHandler.h"
#include "Handlers/GetTipHandler.h"
#include "Handlers/GetUnspentOutputsHandler.h"
#include "Handlers/GetUnconfirmedTransactionsHandler.h"
#include "Handlers/GetVersionHandler.h"
#include "Handlers/PushTransactionHandler.h"

#include "Handlers/BanPeerHandler.h"
#include "Handlers/CompactChainHandler.h"
#include "Handlers/GetConfigHandler.h"
#include "Handlers/GetConnectedPeersHandler.h"
#include "Handlers/GetPeersHandler.h"
#include "Handlers/GetStatusHandler.h"
#include "Handlers/ValidateChainHandler.h"
#include "Handlers/ShutdownHandler.h"
#include "Handlers/UnbanPeerHandler.h"
#include "Handlers/UpdateConfigHandler.h"

NodeServer::UPtr NodeServer::Create(const ServerPtr& pServer, const IBlockChain::Ptr& pBlockChain,
                                    const IP2PServerPtr& pP2PServer, const std::weak_ptr<ITxHashSet>& pTxHashSet,
                                    const IDatabasePtr& pDatabase, const ITransactionPool::Ptr& pTransactionPool)
{
    //https://docs.grin.mw/grin-rfcs/text/0007-node-api-v2/#foreign-api-endpoints
    //https://docs.rs/grin_api/latest/grin_api/foreign_rpc/enum.foreign_rpc.html
    RPCServer::Ptr pForeignServer = RPCServer::Create(pServer, "/v2/foreign", LoggerAPI::LogFile::NODE);
    pForeignServer->AddMethod("get_version", std::make_shared<GetVersionHandler>(pBlockChain));
    pForeignServer->AddMethod("get_header", std::make_shared<GetHeaderHandler>(pBlockChain));
    pForeignServer->AddMethod("get_blocks", std::make_shared<GetBlocksHandler>(pBlockChain));
    pForeignServer->AddMethod("get_block", std::make_shared<GetBlockHandler>(pBlockChain));
    pForeignServer->AddMethod("get_tip", std::make_shared<GetTipHandler>(pBlockChain));
    pForeignServer->AddMethod("get_kernel", std::make_shared<GetKernelHandler>(pBlockChain));
    pForeignServer->AddMethod("get_outputs", std::shared_ptr<RPCMethod>(new GetOutputsHandler(pTxHashSet, pDatabase)));
    pForeignServer->AddMethod("get_unspent_outputs", std::make_shared<GetUnspentOutputsHandler>(pTxHashSet, pDatabase));
    pForeignServer->AddMethod("get_pmmr_indices", std::make_shared<GetPMMRIndicesHandler>(pTxHashSet, pDatabase));
    pForeignServer->AddMethod("get_pool_size", std::make_shared<GetPoolSizeHandler>(pTransactionPool));
    pForeignServer->AddMethod("get_stempool_size", std::make_shared<GetStempoolSizeHandler>(pTransactionPool));
    pForeignServer->AddMethod("get_unconfirmed_transactions", std::make_shared<GetUnconfirmedTransactionsHandler>(pTransactionPool));
    pForeignServer->AddMethod("push_transaction", std::make_shared<PushTransactionHandler>(pBlockChain, pP2PServer));
    
    //https://docs.grin.mw/grin-rfcs/text/0007-node-api-v2/#owner-api-endpoints
    //https://docs.rs/grin_api/latest/grin_api/owner_rpc/trait.OwnerRpc.html
    RPCServer::Ptr pOwnerServer = RPCServer::Create(pServer, "/v2/owner", LoggerAPI::LogFile::NODE);
    pOwnerServer->AddMethod("get_status", std::shared_ptr<RPCMethod>(new GetStatusHandler(pBlockChain, pP2PServer)));
    pOwnerServer->AddMethod("validate_chain", std::make_shared<ValidateChainHandler>(pBlockChain, pTxHashSet));
    pOwnerServer->AddMethod("compact_chain", std::make_shared<CompactChainHandler>(pBlockChain, pTxHashSet, pDatabase));
    pOwnerServer->AddMethod("get_peers", std::shared_ptr<RPCMethod>(new GetPeersHandler(pP2PServer)));
    pOwnerServer->AddMethod("get_connected_peers", std::shared_ptr<RPCMethod>(new GetConnectedPeersHandler(pP2PServer)));
    pOwnerServer->AddMethod("ban_peer", std::shared_ptr<RPCMethod>(new BanPeerHandler(pP2PServer)));
    pOwnerServer->AddMethod("unban_peer", std::shared_ptr<RPCMethod>(new UnbanPeerHandler(pP2PServer)));
    
    //not available in the standard api version
    pOwnerServer->AddMethod("get_config", std::shared_ptr<RPCMethod>(new GetConfigHandler()));
    pOwnerServer->AddMethod("shutdown", std::shared_ptr<RPCMethod>(new ShutdownHandler()));
    pOwnerServer->AddMethod("update_config", std::shared_ptr<RPCMethod>(new UpdateConfigHandler()));

    return std::make_unique<NodeServer>(pForeignServer, pOwnerServer);
}
