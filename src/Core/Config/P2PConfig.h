#pragma once

#include "ConfigProps.h"

#include <cstdint>
#include <limits>
#include <json/json.h>
#include <Common/Logger.h>
#include <Core/Enums/Environment.h>
#include <Net/SocketAddress.h>

#include <Core/Exceptions/FileException.h>
#include <Core/Global.h>

class P2PConfig
{
public:
	int GetMaxConnections() const noexcept { return m_maxConnections; }
	void SetMaxConnections(const int max_connections) noexcept { m_maxConnections = max_connections; }

	int GetMinConnections() const noexcept { return m_minConnections; }
	void SetMinConnections(const int min_connections) noexcept { m_minConnections = min_connections; }

	uint16_t GetP2PPort() const noexcept { return m_port; }
	uint16_t GetListenPort() const noexcept { return m_listenPort; }
	// The protocol-default port for remote peers (never overridden by config).
	// Use this when connecting OUT to other nodes or resolving DNS seeds.
	uint16_t GetNetworkDefaultPort() const noexcept { return m_networkDefaultPort; }
	const std::vector<uint8_t>& GetMagicBytes() const noexcept { return m_magicBytes; }

	uint8_t GetMinSyncPeers() const noexcept { return m_minSyncPeers; }

	const std::vector<SocketAddress>& GetPreferredPeerAddresses() const noexcept { return m_preferredPeerAddresses; }
    const std::unordered_set<IPAddress>& GetPreferredPeers() const noexcept { return m_peferredPeers; }
	const std::unordered_set<IPAddress>& GetAllowedPeers() const noexcept { return m_allowedPeers; }
	const std::unordered_set<IPAddress>& GetBlockedPeers() const noexcept { return m_blockedPeers; }

	void SetPreferredPeers(const std::unordered_set<IPAddress>& peers) noexcept { m_peferredPeers = peers; };
	void SetAllowedPeers(const std::unordered_set<IPAddress>& peers) noexcept { m_allowedPeers = peers; };
	void SetBlockedPeers(const std::unordered_set<IPAddress>& peers) noexcept { m_blockedPeers = peers; };
		
	//
	// Constructor
	//
	P2PConfig(const Environment env, const Json::Value& json)
	{
		m_port = 13414;
		m_networkDefaultPort = 13414;
		m_magicBytes = { 83, 59 };

		m_maxConnections = 60;
		m_minConnections = 10;

		m_minSyncPeers = 1;

		if (env == Environment::MAINNET) {
			m_port = 3414;
			m_networkDefaultPort = 3414;
			m_magicBytes = { 97, 61 };
		}
		m_listenPort = m_port;

		if (!json.isMember(ConfigProps::P2P::P2P)) {
			return;
		}

		const Json::Value& p2pJSON = json[ConfigProps::P2P::P2P];

		if (p2pJSON.isMember(ConfigProps::P2P::P2P_PORT)) {
			m_port = (uint16_t)p2pJSON.get(ConfigProps::P2P::P2P_PORT, m_port).asUInt();
		}

		if (p2pJSON.isMember(ConfigProps::P2P::MAX_PEERS)) {
			m_maxConnections = p2pJSON.get(ConfigProps::P2P::MAX_PEERS, 60).asInt();
		}

		if (p2pJSON.isMember(ConfigProps::P2P::MIN_PEERS)) {
			m_minConnections = p2pJSON.get(ConfigProps::P2P::MIN_PEERS, 10).asInt();
		}

		if (p2pJSON.isMember(ConfigProps::P2P::LISTEN_PORT)) {
			m_listenPort = (uint16_t)p2pJSON.get(ConfigProps::P2P::LISTEN_PORT, m_port).asUInt();
		}

		if (p2pJSON.isMember(ConfigProps::P2P::MIN_SYNC_PEERS)) {
			m_minSyncPeers = (uint8_t)p2pJSON.get(ConfigProps::P2P::MIN_SYNC_PEERS, m_minSyncPeers).asUInt();
		}

		if (p2pJSON.isMember(ConfigProps::P2P::PREFERRED_PEERS)) {
			Json::Value peers = p2pJSON.get(ConfigProps::P2P::PREFERRED_PEERS, Json::Value(Json::nullValue));
			for (auto& peer : peers) { 
				const std::string& addressStr = peer.asCString();
				const std::vector<SocketAddress>& socketAddresses = GetValidSocketAddresses(addressStr, m_port);
				for (const SocketAddress& socketAddress : socketAddresses)
				{
					m_preferredPeerAddresses.push_back(socketAddress);
					m_peferredPeers.insert(socketAddress.GetIPAddress());
				}
			}
		}

		if (p2pJSON.isMember(ConfigProps::P2P::ALLOWED_PEERS)) {
			Json::Value peers = p2pJSON.get(ConfigProps::P2P::ALLOWED_PEERS, Json::Value(Json::nullValue));
			for (auto& peer : peers) {
				const std::string& addressStr = peer.asCString();
				const std::vector<IPAddress>& iPAddresses = GetValidIPAddress(addressStr);
				m_allowedPeers.insert(iPAddresses.begin(), iPAddresses.end());
			}
		}

		if (p2pJSON.isMember(ConfigProps::P2P::BLOCKED_PEERS)) {
			Json::Value peers = p2pJSON.get(ConfigProps::P2P::BLOCKED_PEERS, Json::Value(Json::nullValue));
			for (auto& peer : peers) {
				const std::string& addressStr = peer.asCString();
				const std::vector<IPAddress>& iPAddresses = GetValidIPAddress(addressStr);
				m_blockedPeers.insert(iPAddresses.begin(), iPAddresses.end());
			}
		}
	}

private:
	int m_maxConnections;
	int m_minConnections;
	uint16_t m_port;
	uint16_t m_networkDefaultPort;
	uint16_t m_listenPort;
	std::vector<uint8_t> m_magicBytes;
	uint8_t m_minSyncPeers;
	std::vector<SocketAddress> m_preferredPeerAddresses;
	std::unordered_set<IPAddress> m_peferredPeers;
	std::unordered_set<IPAddress> m_allowedPeers;
	std::unordered_set<IPAddress> m_blockedPeers;

	static std::vector<IPAddress> GetValidIPAddress(const std::string& addressStr)
    {
		std::vector<IPAddress> ipList;

        if (IPAddress::IsValidIPAddress(addressStr)) {
			try {
				ipList.push_back(IPAddress::Parse(addressStr));
			} catch (std::exception& e) {
				LOG_DEBUG_F("Error parsing IP Address: {} {}", addressStr, e.what());
			}
		} else {
			LOG_INFO_F("Resolving domain: {}", addressStr);
			try {
				for (const IPAddress ipAddress : IPAddress::Resolve(addressStr))
				{
					LOG_INFO_F("Resolved domain: {}", ipAddress);
					ipList.push_back(ipAddress);
				}
			} catch (std::exception& e) {
				LOG_DEBUG_F("Failed to resolve domain: {} {}", addressStr, e.what());
			}
		}

		return ipList;
    };

	static std::vector<SocketAddress> GetValidSocketAddresses(const std::string& addressStr, const uint16_t defaultPort)
	{
		std::vector<SocketAddress> socketAddresses;

		const auto colonPos = addressStr.find_last_of(':');
		const bool hasExplicitPort = colonPos != std::string::npos && addressStr.find(':') == colonPos;
		const std::string host = hasExplicitPort ? addressStr.substr(0, colonPos) : addressStr;
		uint16_t port = defaultPort;
		if (hasExplicitPort) {
			try {
				const unsigned long parsedPort = std::stoul(addressStr.substr(colonPos + 1));
				if (parsedPort == 0 || parsedPort > std::numeric_limits<uint16_t>::max()) {
					LOG_DEBUG_F("Invalid peer port in address: {}", addressStr);
					return socketAddresses;
				}
				port = static_cast<uint16_t>(parsedPort);
			} catch (const std::exception& e) {
				LOG_DEBUG_F("Error parsing peer port: {} {}", addressStr, e.what());
				return socketAddresses;
			}
		}

		if (IPAddress::IsValidIPAddress(host)) {
			try {
				socketAddresses.emplace_back(IPAddress::Parse(host), port);
			} catch (std::exception& e) {
				LOG_DEBUG_F("Error parsing SocketAddress: {} {}", addressStr, e.what());
			}
		} else {
			LOG_DEBUG_F("Resolving domain: {}", host);
			try {
				for (const IPAddress& ipAddress : IPAddress::Resolve(host))
				{
					LOG_DEBUG_F("Resolved domain: {}", ipAddress);
					socketAddresses.emplace_back(ipAddress, port);
				}
			} catch (std::exception& e) {
				LOG_DEBUG_F("Failed to resolve domain: {} {}", addressStr, e.what());
			}
		}

		return socketAddresses;
	}
};
