#pragma once

#include <Consensus.h>
#include <BlockChain/BlockChain.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include "NodeAPIUtils.h"
#include <optional>

class UnbanPeerHandler : public RPCMethod
{
public:
	UnbanPeerHandler(const IP2PServerPtr& pP2PServer)
		: m_pP2PServer(pP2PServer) { }
	~UnbanPeerHandler() = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		if (!request.GetParams().has_value()) {
			return request.BuildError(RPC::Errors::PARAMS_MISSING);
		}

		const Json::Value params = request.GetParams().value();
		if (!params.isArray()) {
			return request.BuildError("INVALID_PARAMS", "Expected array");
		}

		std::vector<Json::Value> values_json;
		for (auto iter = params.begin(); iter != params.end(); iter++)
		{
			values_json.push_back(*iter);
		}

		if (values_json.size() == 0)
		{
			return request.BuildError("INVALID_PARAMS", "Empty array");
		}
		else
		{
			try
			{
				const SocketAddress peer = NodeAPI::ParseSocketAddrParam(values_json[0]);
				m_pP2PServer->UnbanPeer(peer);
			}
			catch (const std::exception&)
			{
				return request.BuildError("INVALID_PARAMS", "Invalid peer address");
			}
		}

		Json::Value result;
		result["Ok"] = Json::nullValue;

		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	IP2PServerPtr m_pP2PServer;
};
