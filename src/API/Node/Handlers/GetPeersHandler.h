#pragma once

#include <Consensus.h>
#include <BlockChain/BlockChain.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <optional>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <iterator>

class GetPeersHandler : public RPCMethod
{
public:
	GetPeersHandler(const IP2PServerPtr& pP2PServer)
		: m_pP2PServer(pP2PServer) { }
	~GetPeersHandler() = default;

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
		std::copy(params.begin(), params.end(), std::back_inserter(values_json));

		Json::Value json;

		std::unordered_set<IPAddress> peersWanted;
		for (const Json::Value& peerJson : values_json)
		{
			if (peerJson.isNull())
			{
				continue;
			}

			std::optional<std::string> peerStr = JsonUtil::ConvertToStringOpt(peerJson);
			if (peerStr.has_value())
			{
				try
				{
					peersWanted.insert(IPAddress::Parse(peerStr.value()));
				}
				catch (const std::exception&)
				{
					return request.BuildError("INVALID_PARAMS", "Invalid peer address");
				}
			}
		}

		if(peersWanted.empty())
		{
			const std::vector<PeerConstPtr> peers = m_pP2PServer->GetAllPeers();
			for (PeerConstPtr peer : peers)
			{
				Json::Value peerJson;
				peerJson["addr"] = peer->GetIPAddress().Format();
				peerJson["ban_reason"] = BanReason::Format(peer->GetBanReason());
				peerJson["capabilities"] = peer->GetCapabilities().ToJSON();
				peerJson["flags"] = peer->IsBanned() ? "Banned" : "Healthy";
				peerJson["last_banned"] = Json::UInt64(peer->GetLastBanTime());
				peerJson["last_connected"] = Json::UInt64(peer->GetLastContactTime());
				peerJson["user_agent"] = peer->GetUserAgent();
				json.append(peerJson);
			}
		}
		else
		{
			for (const IPAddress& peerWanted : peersWanted)
			{
				if (auto peerOpt = m_pP2PServer->GetPeer(peerWanted); peerOpt.has_value())
				{
					auto peer = peerOpt.value();
					Json::Value peerJson;
					peerJson["addr"] = peer->GetIPAddress().Format();
					peerJson["ban_reason"] = BanReason::Format(peer->GetBanReason());
					peerJson["capabilities"] = peer->GetCapabilities().ToJSON();
					peerJson["flags"] = peer->IsBanned() ? "Banned" : "Healthy";
					peerJson["last_banned"] = Json::UInt64(peer->GetLastBanTime());
					peerJson["last_connected"] = Json::UInt64(peer->GetLastContactTime());
					peerJson["user_agent"] = peer->GetUserAgent();
					json.append(peerJson);
				}
			}
		}

		Json::Value result;
		result["Ok"] = json;

		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	IP2PServerPtr m_pP2PServer;
};
