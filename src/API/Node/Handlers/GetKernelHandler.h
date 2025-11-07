#pragma once

#include <BlockChain/BlockChain.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <Core/Models/FullBlock.h>
#include <Core/Models/TransactionKernel.h>
#include <Core/Models/Features.h>
#include <Core/Util/JsonUtil.h>
#include <PMMR/Common/LeafIndex.h>
#include <API/Wallet/Owner/Models/Errors.h>

class GetKernelHandler : public RPCMethod
{
public:
	GetKernelHandler(const IBlockChain::Ptr& pBlockChain)
		: m_pBlockChain(pBlockChain) { }
	~GetKernelHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (!request.GetParams().has_value()) {
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		if (m_pBlockChain == nullptr) {
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, "BlockChain unavailable");
		}

		const Json::Value& params = request.GetParams().value();
		if (!params.isArray() || params.size() < 3) {
			return request.BuildError("INVALID_PARAMS", "Expected 3 parameters: excess, min_height, max_height");
		}

		if (params[0].isNull()) {
			return request.BuildError("INVALID_PARAMS", "Kernel excess required");
		}

		Commitment kernelCommitment;
		try
		{
			kernelCommitment = JsonUtil::ConvertToCommitment(params[0]);
		}
		catch (const std::exception& e)
		{
			return request.BuildError("INVALID_PARAMS", e.what());
		}

		uint64_t minHeight = 0;
		if (!params[1].isNull()) {
			minHeight = JsonUtil::ConvertToUInt64(params[1]);
		}

		const BlockHeaderPtr pTip = m_pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
		if (pTip == nullptr) {
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, "Failed to retrieve chain tip");
		}

		uint64_t maxHeight = pTip->GetHeight();
		if (!params[2].isNull()) {
			maxHeight = JsonUtil::ConvertToUInt64(params[2]);
		}

		if (maxHeight > pTip->GetHeight()) {
			maxHeight = pTip->GetHeight();
		}

		if (minHeight > maxHeight) {
			return request.BuildError("INVALID_PARAMS", "min_height must be <= max_height");
		}

		uint64_t height = maxHeight;
		while (true)
		{
			BlockHeaderPtr pHeader = m_pBlockChain->GetBlockHeaderByHeight(height, EChainType::CONFIRMED);
			if (pHeader != nullptr)
			{
				std::unique_ptr<FullBlock> pBlock = m_pBlockChain->GetBlockByHeight(height);
				if (pBlock != nullptr)
				{
					const std::vector<TransactionKernel>& kernels = pBlock->GetKernels();
					const uint64_t totalKernels = pHeader->GetNumKernels();
					const uint64_t kernelsInBlock = kernels.size();
					const uint64_t prevKernelCount = (totalKernels >= kernelsInBlock) ? (totalKernels - kernelsInBlock) : 0;

					for (size_t kernelIdx = 0; kernelIdx < kernels.size(); ++kernelIdx)
					{
						const TransactionKernel& kernel = kernels[kernelIdx];
						if (kernel.GetExcessCommitment() == kernelCommitment)
						{
							const uint64_t leafIndex = prevKernelCount + kernelIdx;
							const LeafIndex mmrLeaf = LeafIndex::At(leafIndex);
							const uint64_t mmrIndex = mmrLeaf.GetPosition() + 1;

							Json::Value kernelJSON;
							kernelJSON["features"] = KernelFeatures::ToString(kernel.GetFeatures());
							kernelJSON["excess"] = kernel.GetExcessCommitment().ToHex();
							kernelJSON["excess_sig"] = kernel.GetExcessSignature().ToHex();

							Json::Value locatedKernel;
							locatedKernel["height"] = Json::UInt64(height);
							locatedKernel["mmr_index"] = Json::UInt64(mmrIndex);
							locatedKernel["tx_kernel"] = kernelJSON;

							Json::Value result;
							result["Ok"] = locatedKernel;
							return request.BuildResult(result);
						}
					}
				}
			}

			if (height == minHeight) {
				break;
			}

			--height;
		}

		Json::Value result;
		result["Ok"] = Json::nullValue;
		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	IBlockChain::Ptr m_pBlockChain;
};
