#pragma once

#include <Net/SocketAddress.h>

#include <optional>
#include <string>

// Forward Declarations
struct mg_connection;

class PeersAPI
{
public:
	static int GetAllPeers_Handler(struct mg_connection* conn, void* pNodeContext);
	static int GetConnectedPeers_Handler(struct mg_connection* conn, void* pNodeContext);
	static int Peer_Handler(struct mg_connection* conn, void* pNodeContext);

private:
	static std::optional<SocketAddress> ParseSocketAddress(const std::string& request);
	static std::string ParseIPAddress(const std::string& request);
	static std::string ParseCommand(const std::string& request);
};
