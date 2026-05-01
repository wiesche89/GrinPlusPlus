#include "DNSSeeder.h"

#include <Common/Logger.h>
#include <Core/Config.h>
#include <Core/Global.h>
#include <Net/IPAddress.h>
#include <asio.hpp>
#include <algorithm>

std::vector<SocketAddress> DNSSeeder::GetPeersFromDNS()
{
	std::vector<SocketAddress> addresses;

	std::vector<std::string> dnsSeeds;
	if (Global::IsMainnet())
	{
		dnsSeeds = {
			"mainnet.grinffindor.org",
			"main.gri.mw",
			"grincoin.org",
			"main-seed.grin.money",
			"mainnet-seed.grinnode.live",
		};
	}
	else
	{
		dnsSeeds = {
			"testnet.grinffindor.org",
			"test.gri.mw",
			"testnet.grincoin.org",
			"test-seed.grin.money",
		};
	}

	for (const auto& seed : dnsSeeds)
	{
		LOG_INFO_F("Resolving DNS seed: {}", seed);
		std::vector<SocketAddress> resolvedAddresses = ResolveSeed(seed);
		LOG_INFO_F("DNS seed {} returned {} socket addresses", seed, resolvedAddresses.size());
		addresses.insert(addresses.end(), resolvedAddresses.begin(), resolvedAddresses.end());
	}

	std::sort(addresses.begin(), addresses.end());
	addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
	LOG_INFO_F("DNS seeding finished with {} unique socket addresses", addresses.size());

	return addresses;
}

std::vector<SocketAddress> DNSSeeder::ResolveSeed(const std::string& seedEntry)
{
	std::string hostname = seedEntry;
	uint16_t port = Global::GetConfig().GetP2PPort();

	const size_t colonIndex = seedEntry.rfind(':');
	if (colonIndex != std::string::npos)
	{
		const std::string portString = seedEntry.substr(colonIndex + 1);
		if (!portString.empty() && portString.find(':') == std::string::npos)
		{
			try
			{
				const uint16_t parsedPort = static_cast<uint16_t>(std::stoul(portString));
				if (parsedPort > 0)
				{
					hostname = seedEntry.substr(0, colonIndex);
					port = parsedPort;
				}
			}
			catch (const std::exception&)
			{
				// Leave hostname untouched and fall back to the configured default port.
			}
		}
	}

	std::vector<SocketAddress> addresses;
	std::vector<IPAddress> ips = IPAddress::Resolve(hostname);
	for (const auto& ipAddress : ips)
	{
		LOG_DEBUG_F("Resolved {} to {}", hostname, ipAddress);
		addresses.emplace_back(ipAddress, port);
	}

	return addresses;
}
