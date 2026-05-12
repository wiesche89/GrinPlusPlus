#pragma once

#include <BlockChain/BlockChain.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <API/Wallet/Owner/Models/Errors.h>
#include "NodeAPIUtils.h"
#include <algorithm>
#include <optional>

class GetUnspentOutputsHandler : public RPCMethod
{
public:
	GetUnspentOutputsHandler(const std::weak_ptr<ITxHashSet>& pTxHashSet, const IDatabasePtr& pDatabase)
		: m_pTxHashSet(pTxHashSet), m_pDatabase(pDatabase) { }
	~GetUnspentOutputsHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		auto tx = m_pTxHashSet.lock();
		if (!tx)
		{
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, "TxHashSet not available");
		}

		if (!request.GetParams().has_value())
		{
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		const Json::Value& params = request.GetParams().value();
		if (!params.isArray() || params.size() < 3 || params[0].isNull() || params[2].isNull())
		{
			return request.BuildError("INVALID_PARAMS", "Expected parameters: start_index, end_index, max, include_proof");
		}

		uint64_t startIndex = JsonUtil::ConvertToUInt64(params[0]);
		std::optional<uint64_t> endIndex = std::nullopt;
		if (params.size() > 1 && !params[1].isNull())
		{
			endIndex = JsonUtil::ConvertToUInt64(params[1]);
		}
		uint64_t max = JsonUtil::ConvertToUInt64(params[2]);
		if (max > 10000)
		{
			max = 10000;
		}
		const bool includeProof = params.size() > 3 && !params[3].isNull() ? params[3].asBool() : false;

		auto pBlockDB = m_pDatabase->GetBlockDB()->Read();
		const uint64_t highestIndex = endIndex.value_or(tx->GetOutputMMRSize());
		const uint64_t lastIndex = highestIndex;
		const uint64_t startPMMRIndex = startIndex > 0 ? startIndex - 1 : 0;
		const uint64_t lastPMMRIndex = lastIndex > 0 ? lastIndex - 1 : 0;

		Json::Value outputs(Json::arrayValue);
		uint64_t lastRetrievedIndex = startIndex;
		uint64_t count = 0;
		if (startPMMRIndex <= lastPMMRIndex)
		{
			const uint64_t chunkSize = 1000;
			uint64_t chunkStart = startPMMRIndex;
			while (chunkStart <= lastPMMRIndex && count < max)
			{
				const uint64_t chunkEnd = (std::min)(lastPMMRIndex, chunkStart + chunkSize - 1);
				std::vector<OutputDTO> range = tx->GetOutputsByMMRIndex(pBlockDB.GetShared(), chunkStart, chunkEnd);
				lastRetrievedIndex = chunkEnd + 1;

				for (const OutputDTO& info : range)
				{
					if (count >= max)
					{
						break;
					}

					outputs.append(NodeAPI::BuildOutputPrintable(info, includeProof));
					lastRetrievedIndex = info.GetMMRPosition() + 1;
					++count;
				}

				if (chunkEnd == lastPMMRIndex)
				{
					break;
				}
				chunkStart = chunkEnd + 1;
			}
		}

		Json::Value ok;
		ok["highest_index"] = Json::UInt64(highestIndex);
		ok["last_retrieved_index"] = Json::UInt64(lastRetrievedIndex);
		ok["outputs"] = outputs;

		Json::Value result;
		result["Ok"] = ok;

		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	std::weak_ptr<ITxHashSet> m_pTxHashSet;
	IDatabasePtr m_pDatabase;
};
