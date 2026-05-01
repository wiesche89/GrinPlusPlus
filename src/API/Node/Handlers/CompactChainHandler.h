#pragma once

#include <BlockChain/BlockChain.h>
#include <PMMR/TxHashSet.h>
#include <Database/Database.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>

class CompactChainHandler : public RPCMethod
{
public:
	CompactChainHandler(
		const IBlockChain::Ptr& pBlockChain,
		const std::weak_ptr<ITxHashSet>& pTxHashSet,
		const IDatabasePtr& pDatabase)
		: m_pBlockChain(pBlockChain), m_pTxHashSet(pTxHashSet), m_pDatabase(pDatabase) { }
	~CompactChainHandler() override = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		auto pTxHashSet = m_pTxHashSet.lock();
		if (!pTxHashSet)
		{
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, "TxHashSet not available");
		}

		(void)m_pBlockChain;
		(void)m_pDatabase;

		try
		{
			pTxHashSet->Compact();
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
	IDatabasePtr m_pDatabase;
};
