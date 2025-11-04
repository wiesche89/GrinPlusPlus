#include "TxHashSetProcessor.h"

#include <Common/Logger.h>
#include <Database/BlockDb.h>
#include <Core/Models/BlockHeader.h>
#include <BlockChain/BlockChain.h>
#include <Common/Util/StringUtil.h>
#include <Common/Util/HexUtil.h>

TxHashSetProcessor::TxHashSetProcessor(
	const Config& config,
	IBlockChain& blockChain,
	std::shared_ptr<Locked<ChainState>> pChainState)
	: m_config(config),
	m_blockChain(blockChain),
	m_pChainState(pChainState)
{

}

bool TxHashSetProcessor::ProcessTxHashSet(const Hash& blockHash, const fs::path& path, SyncStatus& syncStatus)
{
	auto pHeader = m_pChainState->Read()->GetBlockHeaderByHash(blockHash);
	if (pHeader == nullptr)
	{
		LOG_ERROR_F("Header not found for hash {}.", blockHash);
		return false;
	}

	// 1. Close Existing TxHashSet
	{
		auto writer = m_pChainState->Write();
		writer->GetTxHashSetManager()->Close();
	}

	// 2. Load and Extract TxHashSet Zip
	ITxHashSetPtr pTxHashSet = TxHashSetManager::LoadFromZip(m_config, path, pHeader);
	if (pTxHashSet == nullptr)
	{
		LOG_ERROR_F("Failed to load {}", path);
		return false;
	}

	// 3. Validate entire TxHashSet
	auto pBlockSums = pTxHashSet->ValidateTxHashSet(*pHeader, m_blockChain, syncStatus);
	if (pBlockSums == nullptr)
	{
		LOG_ERROR_F("Validation of {} failed.", path);
		return false;
	}

	// 4. Add BlockSums to DB
	auto stateBatch   = m_pChainState->BatchWrite();
	auto txHashSetMgr = stateBatch->GetTxHashSetManager();
	stateBatch->GetBlockDB()->AddBlockSums(pHeader->GetHash(), *pBlockSums);

	// 5. Add Output positions to DB
	LOG_DEBUG("Saving output positions.");
	pTxHashSet->SaveOutputPositions(stateBatch->GetChainStore()->GetCandidateChain(), stateBatch->GetBlockDB());

	// 6. Store TxHashSet
	LOG_DEBUG("Using TxHashSet.");
	txHashSetMgr->SetTxHashSet(pTxHashSet);

	// 7. Update confirmed chain
	LOG_DEBUG("Updating confirmed chain.");
	if (!UpdateConfirmedChain(stateBatch, *pHeader))
	{
		LOG_ERROR_F("Failed to update confirmed chain for {}.", path);
		txHashSetMgr->Close();
		return false;
	}

	stateBatch->Commit();
	return true;
}

bool TxHashSetProcessor::UpdateConfirmedChain(Writer<ChainState> pLockedState, const BlockHeader& blockHeader)
{
	std::shared_ptr<Chain> pCandidateChain = pLockedState->GetChainStore()->GetCandidateChain();
	std::shared_ptr<Chain> pConfirmedChain = pLockedState->GetChainStore()->GetConfirmedChain();
	
	std::shared_ptr<const BlockIndex> pBlockIndex = pCandidateChain->GetByHeight(blockHeader.GetHeight());
	if (pBlockIndex->GetHash() != blockHeader.GetHash())
	{
		return false;
	}

	std::shared_ptr<const BlockIndex> pCommonIndex = pLockedState->GetChainStore()->FindCommonIndex(EChainType::CANDIDATE, EChainType::CONFIRMED);
	pConfirmedChain->Rewind(pCommonIndex->GetHeight());

	uint64_t height = pCommonIndex->GetHeight() + 1;
	while (height <= pBlockIndex->GetHeight())
	{
		pConfirmedChain->AddBlock(pCandidateChain->GetHash(height), height);
		height++;
	}

	return true;
}