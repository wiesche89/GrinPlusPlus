#pragma once

#include <PMMR/TxHashSet.h>
#include <Core/Util/JsonUtil.h>
#include <Database/Database.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <Crypto/Hasher.h>

class GetPMMRIndicesHandler : public RPCMethod
{
public:
	GetPMMRIndicesHandler(const std::weak_ptr<ITxHashSet>& pTxHashSet, const IDatabasePtr& pDatabase)
		: m_pTxHashSet(pTxHashSet), m_pDatabase(pDatabase) { }
	~GetPMMRIndicesHandler() override = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		auto pTxHashSet = m_pTxHashSet.lock();
		if (!pTxHashSet)
		{
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, "TxHashSet not available");
		}

		uint64_t startIndex = 1;
		uint64_t max = 100;

		if (request.GetParams().has_value())
		{
			const Json::Value& params = request.GetParams().value();
			if (!params.isArray())
			{
				return request.BuildError("INVALID_PARAMS", "Expected params array");
			}

			if (params.size() > 0 && !params[0].isNull())
			{
				startIndex = JsonUtil::ConvertToUInt64(params[0]);
			}

			if (params.size() > 1 && !params[1].isNull())
			{
				max = JsonUtil::ConvertToUInt64(params[1]);
			}
		}

		if (max > 1000)
		{
			max = 1000;
		}

		auto pBlockDB = m_pDatabase->GetBlockDB()->Read();
		OutputRange range = pTxHashSet->GetOutputsByLeafIndex(pBlockDB.GetShared(), startIndex, max);

		Json::Value outputs(Json::arrayValue);
		for (const OutputDTO& info : range.GetOutputs())
		{
			Json::Value output;
			output["output_type"] = OutputFeatures::ToString(info.GetIdentifier().GetFeatures());
			output["commit"] = info.GetIdentifier().GetCommitment().ToHex();
			output["spent"] = info.IsSpent();
			output["proof"] = info.GetRangeProof().Format();

			Serializer proofSerializer;
			info.GetRangeProof().Serialize(proofSerializer);
			output["proof_hash"] = Hasher::Blake2b(proofSerializer.GetBytes()).ToHex();

			output["block_height"] = info.GetLocation().GetBlockHeight();
			output["merkle_proof"] = Json::nullValue;
			output["mmr_index"] = info.GetLeafIndex().GetPosition() + 1;

			outputs.append(output);
		}

		Json::Value ok;
		ok["highest_index"] = Json::UInt64(range.GetHighestIndex());
		ok["last_retrieved_index"] = Json::UInt64(range.GetLastRetrievedIndex());
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
