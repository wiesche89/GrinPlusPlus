#pragma once

#include <PMMR/TxHashSet.h>
#include <Core/Util/JsonUtil.h>
#include <Database/Database.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <Crypto/Hasher.h>
#include "NodeAPIUtils.h"

class GetPMMRIndicesHandler : public RPCMethod
{
public:
	GetPMMRIndicesHandler(
		const std::weak_ptr<ITxHashSet>& pTxHashSet,
		const IDatabasePtr& pDatabase,
		const IBlockChain::Ptr& pBlockChain)
		: m_pTxHashSet(pTxHashSet), m_pDatabase(pDatabase), m_pBlockChain(pBlockChain) { }
	~GetPMMRIndicesHandler() override = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (m_pBlockChain == nullptr)
		{
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, "BlockChain unavailable");
		}

		if (!request.GetParams().has_value())
		{
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		uint64_t startBlockHeight = 0;
		std::optional<uint64_t> endBlockHeight = std::nullopt;

		const Json::Value& params = request.GetParams().value();
		if (!params.isArray())
		{
			return request.BuildError("INVALID_PARAMS", "Expected params array");
		}
		if (params.size() == 0 || params[0].isNull())
		{
			return request.BuildError("INVALID_PARAMS", "Expected parameters: start_block_height, end_block_height");
		}

		startBlockHeight = JsonUtil::ConvertToUInt64(params[0]);
		if (params.size() > 1 && !params[1].isNull())
		{
			endBlockHeight = JsonUtil::ConvertToUInt64(params[1]);
		}

		uint64_t startMMRIndex = 0;
		uint64_t endMMRIndex = 0;
		if (!NodeAPI::TryGetPMMRIndexRange(m_pBlockChain, startBlockHeight, endBlockHeight, startMMRIndex, endMMRIndex))
		{
			return request.BuildError("NOT_FOUND", "Block height range not found");
		}

		Json::Value ok;
		ok["highest_index"] = Json::UInt64(endMMRIndex);
		ok["last_retrieved_index"] = Json::UInt64(startMMRIndex);
		ok["outputs"] = Json::Value(Json::arrayValue);

		Json::Value result;
		result["Ok"] = ok;

		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	std::weak_ptr<ITxHashSet> m_pTxHashSet;
	IDatabasePtr m_pDatabase;
	IBlockChain::Ptr m_pBlockChain;
};
