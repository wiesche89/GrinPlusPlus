#pragma once

#include <BlockChain/BlockChain.h>
#include <Core/Models/DTOs/OutputDTO.h>
#include <Core/Models/FullBlock.h>
#include <Core/Models/TransactionOutput.h>
#include <Core/Util/JsonUtil.h>
#include <Crypto/Hasher.h>
#include <Database/BlockDb.h>
#include <Net/SocketAddress.h>
#include <json/json.h>

namespace NodeAPI
{
	static std::string FormatOutputType(const EOutputFeatures features)
	{
		return features == EOutputFeatures::COINBASE_OUTPUT ? "Coinbase" : "Transaction";
	}

	static SocketAddress ParseSocketAddrParam(const Json::Value& param)
	{
		const std::optional<std::string> peer = JsonUtil::ConvertToStringOpt(param);
		if (!peer.has_value())
		{
			throw DESERIALIZATION_EXCEPTION("Expected peer address string");
		}

		return SocketAddress::Parse(peer.value());
	}

	static Json::Value BuildOutputPrintable(
		const EOutputFeatures features,
		const Commitment& commitment,
		const RangeProof& rangeProof,
		const bool spent,
		const std::optional<uint64_t> blockHeight,
		const std::optional<uint64_t> mmrIndex,
		const bool includeProof)
	{
		Json::Value output;
		output["output_type"] = FormatOutputType(features);
		output["commit"] = commitment.ToHex();
		output["spent"] = spent;
		output["proof"] = includeProof ? Json::Value(rangeProof.Format()) : Json::nullValue;

		Serializer proofSerializer;
		rangeProof.Serialize(proofSerializer);
		output["proof_hash"] = Hasher::Blake2b(proofSerializer.GetBytes()).ToHex();

		output["block_height"] = blockHeight.has_value() ? Json::Value(Json::UInt64(blockHeight.value())) : Json::nullValue;
		output["merkle_proof"] = Json::nullValue;
		output["mmr_index"] = mmrIndex.has_value() ? Json::Value(Json::UInt64(mmrIndex.value())) : Json::Value(Json::UInt64(0));
		return output;
	}

	static Json::Value BuildOutputPrintable(const OutputDTO& outputDTO, const bool includeProof)
	{
		return BuildOutputPrintable(
			outputDTO.GetFeatures(),
			outputDTO.GetCommitment(),
			outputDTO.GetRangeProof(),
			outputDTO.IsSpent(),
			outputDTO.GetBlockHeight(),
			outputDTO.GetMMRPosition() + 1,
			includeProof);
	}

	static Json::Value BuildOutputPrintable(
		const TransactionOutput& output,
		const std::shared_ptr<const IBlockDB>& pBlockDB,
		const uint64_t blockHeight,
		const bool includeProof)
	{
		std::unique_ptr<OutputLocation> pLocation = pBlockDB->GetOutputPosition(output.GetCommitment());
		std::optional<uint64_t> mmrIndex = std::nullopt;
		std::optional<uint64_t> outputHeight = blockHeight;
		if (pLocation != nullptr)
		{
			mmrIndex = pLocation->GetPosition() + 1;
			outputHeight = pLocation->GetBlockHeight();
		}

		return BuildOutputPrintable(
			output.GetFeatures(),
			output.GetCommitment(),
			output.GetRangeProof(),
			pLocation == nullptr,
			outputHeight,
			mmrIndex,
			includeProof);
	}

	static bool TryGetPMMRIndexRange(
		const IBlockChain::Ptr& pBlockChain,
		const uint64_t startBlockHeight,
		const std::optional<uint64_t> endBlockHeightOpt,
		uint64_t& startMMRIndex,
		uint64_t& endMMRIndex)
	{
		const BlockHeaderPtr pTip = pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
		if (pTip == nullptr)
		{
			return false;
		}

		const uint64_t endBlockHeight = endBlockHeightOpt.value_or(pTip->GetHeight());
		const BlockHeaderPtr pEndHeader = pBlockChain->GetBlockHeaderByHeight(endBlockHeight, EChainType::CONFIRMED);
		if (pEndHeader == nullptr)
		{
			return false;
		}

		if (startBlockHeight == 0)
		{
			startMMRIndex = 0;
		}
		else
		{
			const BlockHeaderPtr pStartPrevHeader = pBlockChain->GetBlockHeaderByHeight(startBlockHeight - 1, EChainType::CONFIRMED);
			if (pStartPrevHeader == nullptr)
			{
				return false;
			}

			startMMRIndex = pStartPrevHeader->GetOutputMMRSize() + 1;
		}

		endMMRIndex = pEndHeader->GetOutputMMRSize();
		return true;
	}

	static Json::Value BuildBlockPrintable(
		const FullBlock& block,
		const std::shared_ptr<const IBlockDB>& pBlockDB,
		const bool includeProof)
	{
		Json::Value blockJson;
		blockJson["header"] = block.GetHeader()->ToJSON();

		Json::Value inputs(Json::arrayValue);
		for (const TransactionInput& input : block.GetInputs())
		{
			inputs.append(input.GetCommitment().ToHex());
		}
		blockJson["inputs"] = inputs;

		Json::Value outputs(Json::arrayValue);
		for (const TransactionOutput& output : block.GetOutputs())
		{
			outputs.append(BuildOutputPrintable(output, pBlockDB, block.GetHeight(), includeProof));
		}
		blockJson["outputs"] = outputs;

		Json::Value kernels(Json::arrayValue);
		for (const TransactionKernel& kernel : block.GetKernels())
		{
			Json::Value kernelJson = kernel.ToJSON();
			if (kernelJson["features"].isObject() && kernelJson["features"].size() == 1)
			{
				const std::string feature = kernelJson["features"].getMemberNames().front();
				Json::Value featureAttrs = kernelJson["features"][feature];
				kernelJson["features"] = feature;
				if (featureAttrs.isMember("fee"))
				{
					kernelJson["fee"] = featureAttrs["fee"];
				}
				if (featureAttrs.isMember("lock_height"))
				{
					kernelJson["lock_height"] = featureAttrs["lock_height"];
				}
				if (!kernelJson.isMember("fee"))
				{
					kernelJson["fee"] = Json::UInt64(0);
				}
				if (!kernelJson.isMember("fee_shift"))
				{
					kernelJson["fee_shift"] = Json::UInt(0);
				}
				if (!kernelJson.isMember("lock_height"))
				{
					kernelJson["lock_height"] = Json::UInt64(0);
				}
			}
			kernels.append(kernelJson);
		}
		blockJson["kernels"] = kernels;

		return blockJson;
	}
}
