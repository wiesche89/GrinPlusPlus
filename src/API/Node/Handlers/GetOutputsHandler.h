#pragma once

#include <Consensus.h>
#include <BlockChain/BlockChain.h>
#include <Core/Models/FullBlock.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include "NodeAPIUtils.h"
#include <optional>

class GetOutputsHandler : public RPCMethod
{
public:
	GetOutputsHandler(
		const std::weak_ptr<ITxHashSet>& pTxHashSet,
		const IDatabasePtr& pDatabase,
		const IBlockChain::Ptr& pBlockChain)
		: m_pTxHashSet(pTxHashSet), m_pDatabase(pDatabase), m_pBlockChain(pBlockChain) { }
	~GetOutputsHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		auto tx = m_pTxHashSet.lock();
		if (!tx)
		{
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, "TxHashSet not available");
		}
		if (m_pBlockChain == nullptr)
		{
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, "BlockChain unavailable");
		}

		if (!request.GetParams().has_value())
		{
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		const Json::Value& params = request.GetParams().value();
		if (!params.isArray())
		{
			return request.BuildError("INVALID_PARAMS", "Expected params array");
		}

		const bool includeProof = params.size() > 3 && !params[3].isNull() ? params[3].asBool() : false;

		Json::Value outputs(Json::arrayValue);

		if (params.size() > 0 && !params[0].isNull())
		{
			if (!params[0].isArray())
			{
				return request.BuildError("INVALID_PARAMS", "commits must be an array or null");
			}

			for (const Json::Value& commitJson : params[0])
			{
				const std::string commitStr = commitJson.asString();
				if (commitStr.length() != 66)
				{
					return request.BuildError("INVALID_PARAMS", "invalid commit length for " + commitStr);
				}

				try
				{
					const Commitment commitment = JsonUtil::ConvertToCommitment(commitJson);
					auto pBlockDB = m_pDatabase->GetBlockDB()->Read();
					std::unique_ptr<OutputLocation> pLocation = pBlockDB->GetOutputPosition(commitment);
					if (pLocation != nullptr)
					{
						outputs.append(NodeAPI::BuildOutputPrintable(tx->GetOutput(*pLocation), includeProof));
					}
				}
				catch (const std::exception& e)
				{
					return request.BuildError("INVALID_PARAMS", e.what());
				}
			}
		}

		if (params.size() > 1 && !params[1].isNull() && params.size() > 2 && !params[2].isNull())
		{
			const uint64_t startHeight = JsonUtil::ConvertToUInt64(params[1]);
			const uint64_t endHeight = JsonUtil::ConvertToUInt64(params[2]);
			for (uint64_t height = endHeight; height >= startHeight; --height)
			{
				std::unique_ptr<FullBlock> pBlock = m_pBlockChain->GetBlockByHeight(height);
				if (pBlock != nullptr)
				{
					auto pBlockDB = m_pDatabase->GetBlockDB()->Read();
					for (const TransactionOutput& output : pBlock->GetOutputs())
					{
						outputs.append(NodeAPI::BuildOutputPrintable(output, pBlockDB.GetShared(), height, includeProof));
					}
				}

				if (height == startHeight)
				{
					break;
				}
			}
		}

		Json::Value result;
		result["Ok"] = outputs;

		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	std::weak_ptr<ITxHashSet> m_pTxHashSet;
	IDatabasePtr m_pDatabase;
	IBlockChain::Ptr m_pBlockChain;
};
