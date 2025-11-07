#pragma once

#include <BlockChain/BlockChain.h>
#include <PMMR/TxHashSet.h>
#include <P2P/SyncStatus.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>

class ValidateChainHandler : public RPCMethod
{
public:
	ValidateChainHandler(
		const IBlockChain::Ptr& pBlockChain,
		const std::weak_ptr<ITxHashSet>& pTxHashSet)
		: m_pBlockChain(pBlockChain), m_pTxHashSet(pTxHashSet) { }
	~ValidateChainHandler() override = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		auto pTxHashSet = m_pTxHashSet.lock();
		if (!pTxHashSet)
		{
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, "TxHashSet not available");
		}

		BlockHeaderPtr pTip = m_pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
		if (pTip == nullptr)
		{
			return request.BuildError("INVALID_CHAIN_STATUS", "Failed to locate confirmed tip");
		}

		SyncStatus syncStatus;
		try
		{
			pTxHashSet->ValidateTxHashSet(*pTip, *m_pBlockChain, syncStatus);
		}
		catch (const std::exception& e)
		{
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, e.what());
		}

		Json::Value result;
		result["Ok"] = Json::nullValue;
		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	IBlockChain::Ptr m_pBlockChain;
	std::weak_ptr<ITxHashSet> m_pTxHashSet;
};
