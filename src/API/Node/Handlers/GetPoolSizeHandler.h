#pragma once

#include <TxPool/TransactionPool.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>

class GetPoolSizeHandler : public RPCMethod
{
public:
	explicit GetPoolSizeHandler(const ITransactionPool::Ptr& pTransactionPool)
		: m_pTransactionPool(pTransactionPool) { }
	~GetPoolSizeHandler() override = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		Json::Value result;
		result["Ok"] = Json::UInt64(m_pTransactionPool->GetPoolSize());
		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	ITransactionPool::Ptr m_pTransactionPool;
};
