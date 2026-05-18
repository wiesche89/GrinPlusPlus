#pragma once

#include "../ConnectionManager.h"
#include "../Connection.h"
#include "BlockPipe.h"
#include "TransactionPipe.h"
#include "TxHashSetPipe.h"

#include <P2P/SyncStatus.h>
#include <BlockChain/BlockChain.h>
#include <PMMR/TxHashSetManager.h>
#include <PMMR/Common/SegmentId.h>
#include <memory>

// Forward Declarations
class Connection;

class Pipeline
{
public:
	static std::shared_ptr<Pipeline> Create(
		const Config& config,
		ConnectionManagerPtr pConnectionManager,
		const IBlockChain::Ptr& pBlockChain,
		std::shared_ptr<Locked<TxHashSetManager>> pTxHashSetManager,
		SyncStatusPtr pSyncStatus)
	{
		std::shared_ptr<BlockPipe> pBlockPipe = BlockPipe::Create(config, pBlockChain);
		std::shared_ptr<TransactionPipe> pTransactionPipe = TransactionPipe::Create(config, pConnectionManager, pBlockChain);
		std::shared_ptr<TxHashSetPipe> pTxHashSetPipe = TxHashSetPipe::Create(pConnectionManager, pBlockChain, pTxHashSetManager, pSyncStatus);

		return std::shared_ptr<Pipeline>(new Pipeline(pBlockPipe, pTransactionPipe, pTxHashSetPipe));
	}

	std::shared_ptr<BlockPipe> GetBlockPipe() { return m_pBlockPipe; }
	std::shared_ptr<TransactionPipe> GetTransactionPipe() { return m_pTransactionPipe; }
	std::shared_ptr<TxHashSetPipe> GetTxHashSetPipe() { return m_pTxHashSetPipe; }

	bool ProcessBlock(Connection& connection, const FullBlock& block)
	{
		return m_pBlockPipe->AddBlockToProcess(connection.GetPeer(), block);
	}

	void ProcessTransaction(Connection& connection, const TransactionPtr& pTransaction, const EPoolType poolType)
	{
		m_pTransactionPipe->AddTransactionToProcess(connection, pTransaction, poolType);
	}

	void ReceiveTxHashSet(const Connection::Ptr& pConnection, const TxHashSetArchiveMessage& message)
	{
		m_pTxHashSetPipe->ReceiveTxHashSet(pConnection, message);
	}

	void SendTxHashSet(const Connection::Ptr& pConnection, const Hash& block_hash)
	{
		m_pTxHashSetPipe->SendTxHashSet(pConnection, block_hash);
	}

	void SendOutputBitmapSegment(const Connection::Ptr& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
	{
		m_pTxHashSetPipe->SendOutputBitmapSegment(pConnection, blockHash, identifier);
	}

	void SendOutputSegment(const Connection::Ptr& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
	{
		m_pTxHashSetPipe->SendOutputSegment(pConnection, blockHash, identifier);
	}

	void SendRangeProofSegment(const Connection::Ptr& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
	{
		m_pTxHashSetPipe->SendRangeProofSegment(pConnection, blockHash, identifier);
	}

	void SendKernelSegment(const Connection::Ptr& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
	{
		m_pTxHashSetPipe->SendKernelSegment(pConnection, blockHash, identifier);
	}

	void ReceiveOutputBitmapSegment(const Connection::Ptr& pConnection, const OutputBitmapSegmentMessage& message)
	{
		m_pTxHashSetPipe->ReceiveOutputBitmapSegment(pConnection, message);
	}

	void ReceiveOutputSegment(const Connection::Ptr& pConnection, const OutputSegmentMessage& message)
	{
		m_pTxHashSetPipe->ReceiveOutputSegment(pConnection, message);
	}

	void ReceiveRangeProofSegment(const Connection::Ptr& pConnection, const RangeProofSegmentMessage& message)
	{
		m_pTxHashSetPipe->ReceiveRangeProofSegment(pConnection, message);
	}

	void ReceiveKernelSegment(const Connection::Ptr& pConnection, const KernelSegmentMessage& message)
	{
		m_pTxHashSetPipe->ReceiveKernelSegment(pConnection, message);
	}

	bool StartPIBD(const BlockHeaderPtr& pArchiveHeader)
	{
		return m_pTxHashSetPipe->StartPIBD(pArchiveHeader);
	}

	bool RequestNextPIBDSegments(const std::shared_ptr<ConnectionManager>& pConnectionManager, const std::vector<PeerConstPtr>& peers)
	{
		return m_pTxHashSetPipe->RequestNextPIBDSegments(pConnectionManager, peers);
	}

	void ClearPIBDRequests()
	{
		m_pTxHashSetPipe->ClearPIBDRequests();
	}

	void AbortPIBD()
	{
		m_pTxHashSetPipe->AbortPIBD();
	}

	bool IsPIBDComplete() const
	{
		return m_pTxHashSetPipe->IsPIBDComplete();
	}

	bool IsPIBDValidationRunning() const
	{
		return m_pTxHashSetPipe->IsPIBDValidationRunning();
	}

private:
	Pipeline(
		std::shared_ptr<BlockPipe> pBlockPipe,
		std::shared_ptr<TransactionPipe> pTransactionPipe,
		std::shared_ptr<TxHashSetPipe> pTxHashSetPipe)
		: m_pBlockPipe(pBlockPipe),
		m_pTransactionPipe(pTransactionPipe),
		m_pTxHashSetPipe(pTxHashSetPipe)
	{

	}

	std::shared_ptr<BlockPipe> m_pBlockPipe;
	std::shared_ptr<TransactionPipe> m_pTransactionPipe;
	std::shared_ptr<TxHashSetPipe> m_pTxHashSetPipe;
};
