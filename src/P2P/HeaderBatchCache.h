#pragma once

#include "Connection.h"

#include <BlockChain/BlockChain.h>
#include <Core/Models/BlockHeader.h>
#include <deque>
#include <mutex>
#include <vector>

class HeaderBatchCache
{
public:
	enum class Source
	{
		LegacyHeaders,
		PIHDSegment
	};

	static HeaderBatchCache& Get();

	void AddHeaders(
		const IBlockChain::Ptr& pBlockChain,
		const Connection::Ptr& pConnection,
		std::vector<BlockHeaderPtr> headers,
		const Source source);

	void Clear();

private:
	struct CachedBatch
	{
		std::vector<BlockHeaderPtr> headers;
		Connection::Ptr pConnection;
		Source source;
	};

	HeaderBatchCache() = default;

	void ProcessReadyBatches(const IBlockChain::Ptr& pBlockChain);
	bool IsDuplicate(const std::vector<BlockHeaderPtr>& headers) const;
	bool IsAlreadyOnCandidateChain(const IBlockChain::Ptr& pBlockChain, const std::vector<BlockHeaderPtr>& headers) const;
	std::vector<BlockHeaderPtr> TrimAlreadyKnownPrefix(
		const IBlockChain::Ptr& pBlockChain,
		std::vector<BlockHeaderPtr> headers,
		const Connection::Ptr& pConnection,
		Source source) const;
	bool IsReady(const IBlockChain::Ptr& pBlockChain, const std::vector<BlockHeaderPtr>& headers) const;
	static const char* GetSourceName(const Source source) noexcept;
	static bool StartsBefore(const CachedBatch& lhs, const CachedBatch& rhs);

	std::mutex m_mutex;
	std::deque<CachedBatch> m_batches;
};
