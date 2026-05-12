#pragma once

#include <BlockChain/BlockChain.h>
#include <Core/Models/FullBlock.h>
#include <Core/Util/JsonUtil.h>
#include <Database/Database.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include "NodeAPIUtils.h"

class GetBlocksHandler : public RPCMethod
{
public:
	explicit GetBlocksHandler(const IBlockChain::Ptr& pBlockChain, const IDatabasePtr& pDatabase)
		: m_pBlockChain(pBlockChain), m_pDatabase(pDatabase) { }
	~GetBlocksHandler() override = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (!request.GetParams().has_value())
		{
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		if (m_pBlockChain == nullptr)
		{
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, "BlockChain unavailable");
		}

		const Json::Value& params = request.GetParams().value();
		if (!params.isArray() || params.size() < 1 || params[0].isNull())
		{
			return request.BuildError("INVALID_PARAMS", "Expected parameters: start_height, end_height, max, include_proof");
		}

		uint64_t startHeight = JsonUtil::ConvertToUInt64(params[0]);
		uint64_t endHeight = startHeight;
		if (params.size() > 1 && !params[1].isNull())
		{
			endHeight = JsonUtil::ConvertToUInt64(params[1]);
		}

		uint64_t maxBlocks = 100;
		if (params.size() > 2 && !params[2].isNull())
		{
			maxBlocks = JsonUtil::ConvertToUInt64(params[2]);
		}
		if (maxBlocks > 1000)
		{
			maxBlocks = 1000;
		}
		bool includeProof = false;
		if (params.size() > 3 && !params[3].isNull())
		{
			includeProof = params[3].asBool();
		}

		if (endHeight < startHeight)
		{
			Json::Value listing;
			listing["last_retrieved_height"] = Json::UInt64(0);
			listing["blocks"] = Json::Value(Json::arrayValue);

			Json::Value result;
			result["Ok"] = listing;
			return request.BuildResult(result);
		}

		Json::Value blocks(Json::arrayValue);
		uint64_t lastRetrievedHeight = startHeight;
		uint64_t count = 0;

		for (uint64_t height = startHeight; height <= endHeight; ++height)
		{
			lastRetrievedHeight = height;
			std::unique_ptr<FullBlock> pBlock = m_pBlockChain->GetBlockByHeight(height);
			if (pBlock == nullptr)
			{
				continue;
			}

			Json::Value blockJson;
			{
				auto pBlockDB = m_pDatabase->GetBlockDB()->Read();
				blockJson = NodeAPI::BuildBlockPrintable(*pBlock, pBlockDB.GetShared(), includeProof);
			}

			blocks.append(blockJson);
			++count;
			if (maxBlocks > 0 && count == maxBlocks)
			{
				break;
			}
		}

		Json::Value listing;
		listing["last_retrieved_height"] = Json::UInt64(lastRetrievedHeight);
		listing["blocks"] = blocks;

		Json::Value result;
		result["Ok"] = listing;
		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	IBlockChain::Ptr m_pBlockChain;
	IDatabasePtr m_pDatabase;
};
