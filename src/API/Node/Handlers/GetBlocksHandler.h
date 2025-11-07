#pragma once

#include <BlockChain/BlockChain.h>
#include <Core/Models/FullBlock.h>
#include <Core/Util/JsonUtil.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>

class GetBlocksHandler : public RPCMethod
{
public:
	explicit GetBlocksHandler(const IBlockChain::Ptr& pBlockChain)
		: m_pBlockChain(pBlockChain) { }
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
		if (maxBlocks == 0)
		{
			maxBlocks = 1;
		}

		bool includeProof = true;
		if (params.size() > 3 && !params[3].isNull())
		{
			includeProof = params[3].asBool();
		}

		if (endHeight < startHeight)
		{
			endHeight = startHeight;
		}

		const BlockHeaderPtr pTip = m_pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
		if (pTip == nullptr)
		{
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, "Failed to retrieve chain tip");
		}

			if (startHeight > pTip->GetHeight())
	{
		Json::Value listing;
		listing["last_retrieved_height"] = Json::UInt64(pTip->GetHeight());
		listing["blocks"] = Json::Value(Json::arrayValue);

		Json::Value result;
		result["Ok"] = listing;
		return request.BuildResult(result);
	}

	if (endHeight > pTip->GetHeight())
		{
			endHeight = pTip->GetHeight();
		}

		Json::Value blocks(Json::arrayValue);
		uint64_t lastRetrievedHeight = startHeight;
		uint64_t count = 0;

		for (uint64_t height = startHeight; height <= endHeight && count < maxBlocks; ++height)
		{
			std::unique_ptr<FullBlock> pBlock = m_pBlockChain->GetBlockByHeight(height);
			if (pBlock == nullptr)
			{
				continue;
			}

			Json::Value blockJson = pBlock->ToJSON();
			if (!includeProof && blockJson.isMember("outputs"))
			{
				Json::Value& outputs = blockJson["outputs"];
				if (outputs.isArray())
				{
					for (Json::Value::ArrayIndex i = 0; i < outputs.size(); ++i)
					{
						outputs[i].removeMember("proof");
					}
				}
			}

			blocks.append(blockJson);
			lastRetrievedHeight = height;
			++count;
		}

		if (blocks.empty())
		{
			lastRetrievedHeight = startHeight;
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
};
