#pragma once

#include <TxPool/TransactionPool.h>
#include <TxPool/PoolType.h>
#include <TxPool/DandelionStatus.h>
#include <Core/Models/Transaction.h>
#include <Net/Clients/RPC/RPC.h>
#include <Net/Servers/RPC/RPCMethod.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>

class GetUnconfirmedTransactionsHandler : public RPCMethod
{
public:
	explicit GetUnconfirmedTransactionsHandler(const ITransactionPool::Ptr& pTransactionPool)
		: m_pTransactionPool(pTransactionPool) { }
	~GetUnconfirmedTransactionsHandler() override = default;

	RPC::Response Handle(const RPC::Request& request) const final
	{
		std::vector<TransactionPoolEntry> transactions = m_pTransactionPool->GetTransactions(EPoolType::MEMPOOL);

		Json::Value entries(Json::arrayValue);
		for (const TransactionPoolEntry& entry : transactions)
		{
			std::string source = "Unknown";
			switch (entry.status)
			{
				case EDandelionStatus::FLUFFED:
				case EDandelionStatus::TO_FLUFF:
					source = "Broadcast";
					break;
				case EDandelionStatus::TO_STEM:
				case EDandelionStatus::STEMMED:
					source = "Stem";
					break;
				default:
					source = "Queued";
					break;
			}

			Json::Value txEntry;
			txEntry["src"] = source;
			txEntry["tx"] = entry.pTransaction->ToJSON();
			if (entry.timestamp != std::chrono::system_clock::time_point{})
			{
				txEntry["tx_at"] = FormatTimestamp(entry.timestamp);
			}
			else
			{
				txEntry["tx_at"] = Json::nullValue;
			}
			entries.append(txEntry);
		}

		Json::Value result;
		result["Ok"] = entries;
		return request.BuildResult(result);
	}

	bool ContainsSecrets() const noexcept final { return false; }

private:
	ITransactionPool::Ptr m_pTransactionPool;

	static std::string FormatTimestamp(const std::chrono::system_clock::time_point& timestamp)
	{
		const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timestamp.time_since_epoch());
		const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(timestamp.time_since_epoch() - seconds);
		const std::time_t timeSec = std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point(seconds));

		std::tm tmUtc{};
#ifdef _WIN32
		gmtime_s(&tmUtc, &timeSec);
#else
		gmtime_r(&timeSec, &tmUtc);
#endif
		std::ostringstream stream;
		stream << std::put_time(&tmUtc, "%Y-%m-%dT%H:%M:%S");
		stream << '.' << std::setw(6) << std::setfill('0') << micros.count() << 'Z';
		return stream.str();
	}
};
