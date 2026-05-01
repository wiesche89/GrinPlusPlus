#include "HeaderBatchCache.h"

#include <Common/Logger.h>
#include <Core/Exceptions/BadDataException.h>
#include <algorithm>

static constexpr size_t MAX_CACHED_HEADER_BATCHES = 16;

HeaderBatchCache& HeaderBatchCache::Get()
{
	static HeaderBatchCache cache;
	return cache;
}

void HeaderBatchCache::AddHeaders(
	const IBlockChain::Ptr& pBlockChain,
	const Connection::Ptr& pConnection,
	std::vector<BlockHeaderPtr> headers,
	const Source source)
{
	if (headers.empty()) {
		return;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	headers = TrimAlreadyKnownPrefix(pBlockChain, std::move(headers), pConnection, source);
	if (headers.empty()) {
		return;
	}

	if (IsDuplicate(headers)) {
		LOG_TRACE_F(
			"{} ignoring duplicate header batch {}..{} from {}.",
			GetSourceName(source),
			headers.front()->GetHeight(),
			headers.back()->GetHeight(),
			pConnection);
		return;
	}

	if (m_batches.size() >= MAX_CACHED_HEADER_BATCHES) {
		LOG_DEBUG("Header batch cache full; dropping oldest cached header batch.");
		m_batches.pop_front();
	}

	LOG_DEBUG_F(
		"{} cached header batch {}..{} from {}.",
		GetSourceName(source),
		headers.front()->GetHeight(),
		headers.back()->GetHeight(),
		pConnection);
	m_batches.push_back(CachedBatch{ std::move(headers), pConnection, source });
	ProcessReadyBatches(pBlockChain);
}

void HeaderBatchCache::Clear()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_batches.clear();
}

void HeaderBatchCache::ProcessReadyBatches(const IBlockChain::Ptr& pBlockChain)
{
	bool progressed = true;
	while (progressed) {
		progressed = false;
		std::sort(m_batches.begin(), m_batches.end(), StartsBefore);

		for (auto iter = m_batches.begin(); iter != m_batches.end(); ++iter) {
			iter->headers = TrimAlreadyKnownPrefix(
				pBlockChain,
				std::move(iter->headers),
				iter->pConnection,
				iter->source);
			if (iter->headers.empty()) {
				m_batches.erase(iter);
				progressed = true;
				break;
			}

			if (!IsReady(pBlockChain, iter->headers)) {
				continue;
			}

			const uint64_t firstHeight = iter->headers.front()->GetHeight();
			const uint64_t lastHeight = iter->headers.back()->GetHeight();
			Connection::Ptr pConnection = iter->pConnection;
			const Source source = iter->source;
			std::vector<BlockHeaderPtr> headers = std::move(iter->headers);
			m_batches.erase(iter);

			try {
				const EBlockChainStatus status = pBlockChain->AddBlockHeaders(headers);
				if (status == EBlockChainStatus::INVALID) {
					LOG_WARNING_F("{} invalid header batch {}..{} from {}.", GetSourceName(source), firstHeight, lastHeight, pConnection);
					if (pConnection != nullptr) {
						pConnection->BanPeer(EBanReason::BadBlockHeader);
					}
				} else if (status == EBlockChainStatus::SUCCESS || status == EBlockChainStatus::ALREADY_EXISTS) {
					LOG_DEBUG_F("{} processed header batch {}..{} from {}.", GetSourceName(source), firstHeight, lastHeight, pConnection);
				} else {
					LOG_DEBUG_F("{} could not process header batch {}..{} from {} (status={}).",
						GetSourceName(source),
						firstHeight,
						lastHeight,
						pConnection,
						(int)status);
				}
			} catch (const BadDataException& e) {
				LOG_WARNING_F("{} bad header batch {}..{} from {}: {}", GetSourceName(source), firstHeight, lastHeight, pConnection, e.what());
				if (pConnection != nullptr) {
					pConnection->BanPeer(e.GetReason());
				}
			} catch (const std::exception& e) {
				LOG_WARNING_F("{} failed to process header batch {}..{} from {}: {}", GetSourceName(source), firstHeight, lastHeight, pConnection, e.what());
			}

			progressed = true;
			break;
		}
	}
}

bool HeaderBatchCache::IsDuplicate(const std::vector<BlockHeaderPtr>& headers) const
{
	const Hash& firstHash = headers.front()->GetHash();
	const Hash& lastHash = headers.back()->GetHash();
	for (const CachedBatch& batch : m_batches) {
		if (batch.headers.empty()) {
			continue;
		}

		if (batch.headers.front()->GetHash() == firstHash || batch.headers.back()->GetHash() == lastHash) {
			return true;
		}
	}

	return false;
}

bool HeaderBatchCache::IsAlreadyOnCandidateChain(
	const IBlockChain::Ptr& pBlockChain,
	const std::vector<BlockHeaderPtr>& headers) const
{
	if (headers.empty()) {
		return false;
	}

	const BlockHeaderPtr pLastKnown = pBlockChain->GetBlockHeaderByHeight(
		headers.back()->GetHeight(),
		EChainType::CANDIDATE);
	return pLastKnown != nullptr && pLastKnown->GetHash() == headers.back()->GetHash();
}

std::vector<BlockHeaderPtr> HeaderBatchCache::TrimAlreadyKnownPrefix(
	const IBlockChain::Ptr& pBlockChain,
	std::vector<BlockHeaderPtr> headers,
	const Connection::Ptr& pConnection,
	const Source source) const
{
	size_t firstUnknown = 0;
	for (; firstUnknown < headers.size(); ++firstUnknown) {
		const BlockHeaderPtr& pHeader = headers[firstUnknown];
		const BlockHeaderPtr pKnown = pBlockChain->GetBlockHeaderByHeight(
			pHeader->GetHeight(),
			EChainType::CANDIDATE);
		if (pKnown == nullptr || pKnown->GetHash() != pHeader->GetHash()) {
			break;
		}
	}

	if (firstUnknown == 0) {
		return headers;
	}

	const uint64_t firstHeight = headers.front()->GetHeight();
	const uint64_t lastKnownHeight = headers[firstUnknown - 1]->GetHeight();
	if (firstUnknown == headers.size()) {
		LOG_TRACE_F(
			"{} ignoring already processed header batch {}..{} from {}.",
			GetSourceName(source),
			firstHeight,
			lastKnownHeight,
			pConnection);
		return {};
	}

	LOG_TRACE_F(
		"{} trimmed already processed header batch prefix {}..{} from {}.",
		GetSourceName(source),
		firstHeight,
		lastKnownHeight,
		pConnection);
	return std::vector<BlockHeaderPtr>(headers.begin() + firstUnknown, headers.end());
}

bool HeaderBatchCache::IsReady(const IBlockChain::Ptr& pBlockChain, const std::vector<BlockHeaderPtr>& headers) const
{
	if (headers.empty()) {
		return false;
	}

	if (headers.front()->GetHeight() == 0) {
		return true;
	}

	const uint64_t previousHeight = headers.front()->GetHeight() - 1;
	const BlockHeaderPtr pPrevious = pBlockChain->GetBlockHeaderByHeight(previousHeight, EChainType::CANDIDATE);
	return pPrevious != nullptr && pPrevious->GetHash() == headers.front()->GetPreviousHash();
}

const char* HeaderBatchCache::GetSourceName(const Source source) noexcept
{
	switch (source)
	{
		case Source::LegacyHeaders:
			return "HeaderSync";
		case Source::PIHDSegment:
			return "PIHD";
	}

	return "HeaderSync";
}

bool HeaderBatchCache::StartsBefore(const CachedBatch& lhs, const CachedBatch& rhs)
{
	if (lhs.headers.empty()) {
		return false;
	}

	if (rhs.headers.empty()) {
		return true;
	}

	return lhs.headers.front()->GetHeight() < rhs.headers.front()->GetHeight();
}
