#pragma once

#include <TxPool/TransactionPool.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>

class GetStempoolSizeHandler : public RPCMethod
{
public:
	explicit GetStempoolSizeHandler(const ITransactionPool::Ptr& pTransactionPool)
		: m_pTransactionPool(pTransactionPool) { }
	~GetStempoolSizeHandler() override = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		Json::Value result;
		result["Ok"] = Json::UInt64(m_pTransactionPool->GetStemPoolSize());
		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	ITransactionPool::Ptr m_pTransactionPool;
};
