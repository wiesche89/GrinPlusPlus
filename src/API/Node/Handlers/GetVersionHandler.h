#pragma once

#include <BlockChain/BlockChain.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <GrinVersion.h>

class GetVersionHandler : public RPCMethod
{
public:
	explicit GetVersionHandler(const IBlockChain::Ptr& pBlockChain)
		: m_pBlockChain(pBlockChain) { }

	RPC::Response Handle(const RPC::Request& request) const final
	{
		Json::Value json;
		json["node_version"] = GRINPP_VERSION;

		if (m_pBlockChain == nullptr)
		{
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, "BlockChain unavailable");
		}

		const BlockHeaderPtr pTip = m_pBlockChain->GetTipBlockHeader(EChainType::CONFIRMED);
		if (pTip == nullptr)
		{
			return request.BuildError(RPC::ErrorCode::INTERNAL_ERROR, "Failed to retrieve chain tip");
		}

		const uint16_t headerVersion = pTip->GetVersion();

		json["block_header_version"] = Json::UInt(headerVersion);

		Json::Value result;
		result["Ok"] = json;
		
		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	IBlockChain::Ptr m_pBlockChain;
};
