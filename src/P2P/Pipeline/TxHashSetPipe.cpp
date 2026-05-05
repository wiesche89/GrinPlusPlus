#include "TxHashSetPipe.h"
#include "../Connection.h"
#include "../ConnectionManager.h"
#include "../Messages/TxHashSetArchiveMessage.h"
#include "../Messages/SegmentResponseMessage.h"
#include "../Messages/SegmentRequestMessage.h"

#include <Common/Util/HexUtil.h>
#include <Common/Util/FileUtil.h>
#include <Common/Util/ThreadUtil.h>
#include <Common/Logger.h>
#include <Core/File/FileRemover.h>
#include <Core/Global.h>
#include <Net/SocketAddress.h>
#include <BlockChain/BlockChain.h>
#include <PMMR/TxHashSet.h>
#include <PMMR/PIBDParams.h>

#include <filesystem.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <optional>
#include <vector>

static const int BUFFER_SIZE = 128 * 1024;
static const unsigned long TXHASHSET_RECEIVE_TIMEOUT_MS = 60 * 1000;

static uint64_t ElapsedMillis(const std::chrono::steady_clock::time_point& start)
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count();
}

static bool IsEligiblePIBDConnection(const Connection::Ptr& pConnection)
{
	return pConnection != nullptr && pConnection->IsConnectionActive();
}

static bool IsExpectedPIBDSegment(
	const SegmentRequestTracker& requests,
	const SegmentTypeIdentifier& typeId,
	const Connection::Ptr& pConnection)
{
	// Accept the segment if it was requested from any peer (multi-peer PIBD support).
	// IsPending() is sufficient: we only process segments we actually asked for,
	// and segment Merkle proof validation guards against bad data.
	return pConnection != nullptr && requests.IsPending(typeId);
}

static const char* GetPIBDSegmentTypeName(const SegmentType type) noexcept
{
	switch (type)
	{
		case SegmentType::OutputBitmap:
			return "output bitmap";
		case SegmentType::Output:
			return "output";
		case SegmentType::RangeProof:
			return "rangeproof";
		case SegmentType::Kernel:
			return "kernel";
	}

	return "unknown";
}

static uint8_t GetPIBDSegmentServeTypePriority(const SegmentType type) noexcept
{
	switch (type)
	{
		case SegmentType::OutputBitmap:
			return 0;
		case SegmentType::Output:
			return 1;
		case SegmentType::RangeProof:
			return 2;
		case SegmentType::Kernel:
			return 3;
	}

	return 4;
}

static uint64_t GetPeerReportedHeight(const std::shared_ptr<ConnectionManager>& pConnectionManager, const PeerConstPtr& pPeer)
{
	if (pConnectionManager == nullptr || pPeer == nullptr) {
		return 0;
	}

	for (const ConnectedPeer& connectedPeer : pConnectionManager->GetConnectedPeers()) {
		if (connectedPeer.GetPeer() != nullptr
			&& connectedPeer.GetSocketAddress() == SocketAddress(pPeer->GetIPAddress(), pPeer->GetPort())) {
			return connectedPeer.GetHeight();
		}
	}

	return 0;
}

static std::string GetPIBDSegmentPeerKey(const Connection::Ptr& pConnection, const Hash& blockHash)
{
	return StringUtil::Format("{}|{}", pConnection != nullptr ? pConnection->GetSocketAddress().Format() : std::string("unknown"), blockHash);
}

TxHashSetPipe::~TxHashSetPipe()
{
	{
		std::lock_guard<std::mutex> lock(m_pibdSegmentServeMutex);
		m_stopPIBDSegmentServeWorkers = true;
	}
	m_pibdSegmentServeCondition.notify_all();
	ThreadUtil::JoinAll(m_pibdSegmentServeWorkers);

	ThreadUtil::Join(m_txHashSetThread);
	if (Global::IsRunning()) {
		ThreadUtil::JoinAll(m_sendThreads);
	} else {
		for (std::thread& thread : m_sendThreads) {
			ThreadUtil::Detach(thread);
		}
	}
}

std::shared_ptr<TxHashSetPipe> TxHashSetPipe::Create(
	const ConnectionManagerPtr& pConnectionManager,
	const IBlockChain::Ptr& pBlockChain,
	std::shared_ptr<Locked<TxHashSetManager>> pTxHashSetManager,
	SyncStatusPtr pSyncStatus)
{
	return std::shared_ptr<TxHashSetPipe>(new TxHashSetPipe(
		pConnectionManager,
		pBlockChain,
		std::move(pTxHashSetManager),
		pSyncStatus
	));
}

void TxHashSetPipe::ReceiveTxHashSet(const Connection::Ptr& pConnection, const TxHashSetArchiveMessage& archive_msg)
{
	if (m_pSyncStatus->GetStatus() != ESyncStatus::SYNCING_TXHASHSET)
	{
		LOG_WARNING_F("Received TxHashSet from {} when not requested.", *pConnection);
		// connection.BanPeer(EBanReason::Abusive);
		// return;
	}

	const bool processing = m_processing.exchange(true);
	if (processing) {
		LOG_WARNING_F("Received TxHashSet from {} when already processing another.", *pConnection);
		pConnection->BanPeer(EBanReason::Abusive);
		return;
	}

	LOG_INFO_F("Disabling receives for {}", pConnection);
	pConnection->DisableReceives(true);

	ThreadUtil::Join(m_txHashSetThread);

	m_txHashSetThread = std::thread(
		Thread_ProcessTxHashSet,
		std::ref(*this),
		pConnection,
		archive_msg.GetZippedSize(),
		archive_msg.GetBlockHash()
	);
}

void TxHashSetPipe::Thread_ProcessTxHashSet(TxHashSetPipe& pipeline, Connection::Ptr pConnection, const uint64_t zipped_size, const Hash blockHash)
{
	const std::string fileName = StringUtil::Format(
		"txhashset_{}.zip",
		HASH::ShortHash(blockHash)
	);
	const fs::path txHashSetPath = fs::temp_directory_path() / fileName;

	try
	{
		LoggerAPI::SetThreadName("TXHASHSET_PIPE");
		LOG_TRACE("BEGIN");

		LOG_INFO_F("Downloading TxHashSet from {} (zipped_size={})", *pConnection, zipped_size);

		if (zipped_size == 0) {
			LOG_WARNING_F("Peer {} sent TxHashSet with zipped_size=0, skipping.", *pConnection);
			pipeline.m_processing = false;
			pipeline.m_pSyncStatus->UpdateStatus(ESyncStatus::TXHASHSET_SYNC_FAILED);
			pConnection->DisableReceives(false);
			return;
		}

		pipeline.m_pSyncStatus->UpdateDownloaded(0);
		pipeline.m_pSyncStatus->UpdateDownloadSize(zipped_size);

		SocketPtr pSocket = pConnection->GetSocket();
		pSocket->SetDefaultOptions();
		// SetDefaultOptions() resets the Windows socket timeouts, so apply the
		// longer TxHashSet timeout afterwards.
		pSocket->SetReceiveTimeout(TXHASHSET_RECEIVE_TIMEOUT_MS);
		pSocket->SetBlocking(true);
		pSocket->SetReceiveBufferSize(BUFFER_SIZE);

		std::ofstream fout;
		fout.open(txHashSetPath, std::ios::binary | std::ios::out | std::ios::trunc);

		size_t bytesReceived = 0;
		std::vector<uint8_t> buffer(BUFFER_SIZE, 0);
		while (bytesReceived < zipped_size) {
			const size_t bytesToRead = (std::min)((size_t)(zipped_size - bytesReceived), (size_t)BUFFER_SIZE);

			const bool received = pConnection->ReceiveSync(buffer, bytesToRead);
			if (!received || !Global::IsRunning()) {
				LOG_INFO_F("bytesReceived: {} zipped_size: {} user_agent: {}",
					bytesReceived,
					zipped_size,
					pConnection->GetPeer()->GetUserAgent());

				LOG_ERROR("Transmission ended abruptly");
				fout.close();
				FileUtil::RemoveFile(txHashSetPath);
				pipeline.m_processing = false;
				pipeline.m_pSyncStatus->UpdateStatus(ESyncStatus::TXHASHSET_SYNC_FAILED);
				pConnection->DisableReceives(false);
				return;
			}

			fout.write((char*)&buffer[0], bytesToRead);
			bytesReceived += bytesToRead;

			pipeline.m_pSyncStatus->UpdateDownloaded(bytesReceived);
		}

		fout.close();

		LOG_INFO("Downloading successful");


		SyncStatusPtr pSyncStatus = pipeline.m_pSyncStatus;

		pSyncStatus->UpdateProcessingStatus(0);
		pSyncStatus->UpdateStatus(ESyncStatus::PROCESSING_TXHASHSET);

		EBlockChainStatus processStatus = pipeline.m_pBlockChain->ProcessTransactionHashSet(blockHash, txHashSetPath, *pSyncStatus);
		if (processStatus == EBlockChainStatus::INVALID)
		{
			LOG_ERROR_F("Invalid TxHashSet received. user_agent: {}",pConnection->GetPeer()->GetUserAgent());
			pSyncStatus->UpdateStatus(ESyncStatus::TXHASHSET_SYNC_FAILED);
			pConnection->BanPeer(EBanReason::BadTxHashSet);
		}
		else
		{
			pSyncStatus->UpdateStatus(ESyncStatus::SYNCING_BLOCKS);
		}

		LOG_TRACE("END");
	}
	catch (const std::exception& e)
	{
		LOG_ERROR_F("Exception thrown while downloading/processing TxHashSet from {} user_agent: {} error: {}",
			*pConnection,
			pConnection->GetPeer()->GetUserAgent(),
			e.what());

		pipeline.m_pSyncStatus->UpdateStatus(ESyncStatus::TXHASHSET_SYNC_FAILED);
		FileUtil::RemoveFile(txHashSetPath);
	}
	catch (...)
	{
		LOG_ERROR_F("Unknown exception thrown while downloading/processing TxHashSet from {} user_agent: {}",
			*pConnection,
			pConnection->GetPeer()->GetUserAgent());

		pipeline.m_pSyncStatus->UpdateStatus(ESyncStatus::TXHASHSET_SYNC_FAILED);
		FileUtil::RemoveFile(txHashSetPath);
	}

	pipeline.m_processing = false;
	pConnection->DisableReceives(false);
}

void TxHashSetPipe::SendTxHashSet(const std::shared_ptr<Connection>& pConnection, const Hash& block_hash)
{
	const time_t maxTxHashSetRequest = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() - std::chrono::hours(2));
	if (pConnection->GetPeer()->GetLastTxHashSetRequest() > maxTxHashSetRequest) {
		LOG_WARNING_F("Peer '{}' requested multiple TxHashSet's within 2 hours.", pConnection->GetIPAddress());
		pConnection->BanPeer(EBanReason::Abusive);
		return;
	}

	LOG_INFO_F("Sending TxHashSet snapshot to {}", pConnection);
	pConnection->GetPeer()->UpdateLastTxHashSetRequest();
	pConnection->DisableSends(true);

	std::thread send_thread = std::thread(
		Thread_SendTxHashSet,
		m_pBlockChain,
		pConnection,
		block_hash
	);
	m_sendThreads.push_back(std::move(send_thread));
}

void TxHashSetPipe::SendOutputBitmapSegment(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
{
	EnqueuePIBDSegmentServeRequest(SegmentType::OutputBitmap, pConnection, blockHash, identifier);
}

void TxHashSetPipe::SendOutputSegment(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
{
	EnqueuePIBDSegmentServeRequest(SegmentType::Output, pConnection, blockHash, identifier);
}

void TxHashSetPipe::SendRangeProofSegment(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
{
	EnqueuePIBDSegmentServeRequest(SegmentType::RangeProof, pConnection, blockHash, identifier);
}

void TxHashSetPipe::SendKernelSegment(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
{
	EnqueuePIBDSegmentServeRequest(SegmentType::Kernel, pConnection, blockHash, identifier);
}

bool TxHashSetPipe::EnqueuePIBDSegmentServeRequest(
	const SegmentType type,
	const std::shared_ptr<Connection>& pConnection,
	const Hash& blockHash,
	const SegmentIdentifier& identifier)
{
	if (pConnection == nullptr) {
		return false;
	}

	std::unique_lock<std::mutex> lock(m_pibdSegmentServeMutex);
	if (m_stopPIBDSegmentServeWorkers || m_pibdSegmentServeQueue.size() >= PIBD_SEGMENT_SERVE_QUEUE_CAPACITY) {
		LOG_DEBUG(StringUtil::Format("Dropping PIBD {} segment {}:{} for {} to {}; serve queue full ({}/{}).",
			GetPIBDSegmentTypeName(type),
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash,
			pConnection,
			m_pibdSegmentServeQueue.size(),
			PIBD_SEGMENT_SERVE_QUEUE_CAPACITY));
		return false;
	}

	m_pibdSegmentServeQueue.push_back(PIBDSegmentServeRequest{
		type,
		pConnection,
		blockHash,
		identifier,
		std::chrono::steady_clock::now(),
		m_pibdSegmentServeSequence.fetch_add(1) });
	const size_t queueSize = m_pibdSegmentServeQueue.size();
	lock.unlock();
	m_pibdSegmentServeCondition.notify_one();

	LOG_TRACE(StringUtil::Format("Enqueued PIBD {} segment {}:{} for {} to {} (queue_depth={}/{}).",
		GetPIBDSegmentTypeName(type),
		identifier.GetHeight(),
		identifier.GetIndex(),
		blockHash,
		pConnection,
		queueSize,
		PIBD_SEGMENT_SERVE_QUEUE_CAPACITY));
	return true;
}

std::deque<TxHashSetPipe::PIBDSegmentServeRequest>::iterator TxHashSetPipe::SelectNextPIBDSegmentServeRequest()
{
	return std::min_element(
		m_pibdSegmentServeQueue.begin(),
		m_pibdSegmentServeQueue.end(),
		[](const PIBDSegmentServeRequest& lhs, const PIBDSegmentServeRequest& rhs) {
			const bool lhsBitmap = lhs.type == SegmentType::OutputBitmap;
			const bool rhsBitmap = rhs.type == SegmentType::OutputBitmap;
			if (lhsBitmap != rhsBitmap) {
				return lhsBitmap;
			}

			const uint64_t lhsIndex = lhs.identifier.GetIndex();
			const uint64_t rhsIndex = rhs.identifier.GetIndex();
			if (lhsIndex != rhsIndex) {
				return lhsIndex < rhsIndex;
			}

			const uint8_t lhsTypePriority = GetPIBDSegmentServeTypePriority(lhs.type);
			const uint8_t rhsTypePriority = GetPIBDSegmentServeTypePriority(rhs.type);
			if (lhsTypePriority != rhsTypePriority) {
				return lhsTypePriority < rhsTypePriority;
			}

			return lhs.sequence < rhs.sequence;
		});
}

void TxHashSetPipe::Thread_ServePIBDSegments()
{
	LoggerAPI::SetThreadName("PIBD_SEG_SERVE");
	while (Global::IsRunning()) {
		std::optional<PIBDSegmentServeRequest> request;
		{
			std::unique_lock<std::mutex> lock(m_pibdSegmentServeMutex);
			m_pibdSegmentServeCondition.wait(lock, [this]() {
				return m_stopPIBDSegmentServeWorkers || !m_pibdSegmentServeQueue.empty();
			});

			if (m_stopPIBDSegmentServeWorkers && m_pibdSegmentServeQueue.empty()) {
				return;
			}

			const auto iter = SelectNextPIBDSegmentServeRequest();
			request = std::move(*iter);
			m_pibdSegmentServeQueue.erase(iter);
			LOG_TRACE(StringUtil::Format("Starting PIBD {} segment {}:{} for {} to {} after {}ms (queue_depth_after_pop={}).",
				GetPIBDSegmentTypeName(request->type),
				request->identifier.GetHeight(),
				request->identifier.GetIndex(),
				request->blockHash,
				request->pConnection,
				ElapsedMillis(request->enqueuedAt),
				m_pibdSegmentServeQueue.size()));
		}

		try {
			ProcessPIBDSegmentServeRequest(std::move(request.value()));
		} catch (const std::exception& e) {
			LOG_WARNING(StringUtil::Format("Failed to serve PIBD segment request: {}", e.what()));
		} catch (...) {
			LOG_WARNING("Failed to serve PIBD segment request: unknown exception.");
		}
	}
}

void TxHashSetPipe::ProcessPIBDSegmentServeRequest(PIBDSegmentServeRequest&& request)
{
	LOG_TRACE(StringUtil::Format("PIBD {} segment worker start {}:{} for {} to {} (queued_ms={}).",
		GetPIBDSegmentTypeName(request.type),
		request.identifier.GetHeight(),
		request.identifier.GetIndex(),
		request.blockHash,
		request.pConnection,
		ElapsedMillis(request.enqueuedAt)));

	const auto start = std::chrono::steady_clock::now();
	if (!IsEligiblePIBDConnection(request.pConnection)) {
		LOG_TRACE(StringUtil::Format("Skipping PIBD {} segment {}:{} for {}; connection is not active.",
			GetPIBDSegmentTypeName(request.type),
			request.identifier.GetHeight(),
			request.identifier.GetIndex(),
			request.blockHash));
	} else {
		switch (request.type)
		{
			case SegmentType::OutputBitmap:
				ProcessOutputBitmapSegmentRequest(request.pConnection, request.blockHash, request.identifier);
				break;
			case SegmentType::Output:
				ProcessOutputSegmentRequest(request.pConnection, request.blockHash, request.identifier);
				break;
			case SegmentType::RangeProof:
				ProcessRangeProofSegmentRequest(request.pConnection, request.blockHash, request.identifier);
				break;
			case SegmentType::Kernel:
				ProcessKernelSegmentRequest(request.pConnection, request.blockHash, request.identifier);
				break;
		}
	}

	const uint64_t totalMs = ElapsedMillis(start);
	const std::string logMessage = StringUtil::Format("PIBD {} segment worker finish {}:{} for {} to {} (total_ms={}).",
		GetPIBDSegmentTypeName(request.type),
		request.identifier.GetHeight(),
		request.identifier.GetIndex(),
		request.blockHash,
		request.pConnection,
		totalMs);
	if (totalMs >= 1000) {
		LOG_DEBUG(logMessage);
	} else {
		LOG_TRACE(logMessage);
	}
}

std::shared_ptr<const BitmapAccumulator> TxHashSetPipe::GetCachedOutputBitmapAccumulator(
	const ITxHashSetConstPtr& pTxHashSet,
	const Hash& blockHash,
	const uint64_t outputMMRSize)
{
	{
		std::lock_guard<std::mutex> lock(m_pibdSegmentBitmapCacheMutex);
		if (m_pibdSegmentBitmapCache.pAccumulator != nullptr
			&& m_pibdSegmentBitmapCache.blockHash.has_value()
			&& m_pibdSegmentBitmapCache.blockHash.value() == blockHash
			&& m_pibdSegmentBitmapCache.outputMMRSize == outputMMRSize) {
			LOG_TRACE(StringUtil::Format("PIBD output bitmap accumulator cache hit for {} (output_mmr_size={}).",
				blockHash,
				outputMMRSize));
			return m_pibdSegmentBitmapCache.pAccumulator;
		}
	}

	const auto start = std::chrono::steady_clock::now();
	BitmapAccumulator accumulator = pTxHashSet->GetOutputBitmapAccumulator();
	auto pAccumulator = std::make_shared<const BitmapAccumulator>(std::move(accumulator));
	{
		std::lock_guard<std::mutex> lock(m_pibdSegmentBitmapCacheMutex);
		m_pibdSegmentBitmapCache.blockHash = blockHash;
		m_pibdSegmentBitmapCache.outputMMRSize = outputMMRSize;
		m_pibdSegmentBitmapCache.pAccumulator = pAccumulator;
		m_pibdSegmentBitmapCache.outputBitmapRoot.reset();
	}

	LOG_TRACE(StringUtil::Format("PIBD output bitmap accumulator cache miss for {} (output_mmr_size={}) built in {}ms.",
		blockHash,
		outputMMRSize,
		ElapsedMillis(start)));
	return pAccumulator;
}

Hash TxHashSetPipe::GetCachedOutputBitmapRoot(
	const ITxHashSetConstPtr& pTxHashSet,
	const Hash& blockHash,
	const uint64_t outputMMRSize,
	const uint64_t numOutputs)
{
	{
		std::lock_guard<std::mutex> lock(m_pibdSegmentBitmapCacheMutex);
		if (m_pibdSegmentBitmapCache.outputBitmapRoot.has_value()
			&& m_pibdSegmentBitmapCache.blockHash.has_value()
			&& m_pibdSegmentBitmapCache.blockHash.value() == blockHash
			&& m_pibdSegmentBitmapCache.outputMMRSize == outputMMRSize
			&& m_pibdSegmentBitmapCache.numOutputs == numOutputs) {
			LOG_TRACE(StringUtil::Format("PIBD output bitmap root cache hit for {} (num_outputs={}).",
				blockHash,
				numOutputs));
			return m_pibdSegmentBitmapCache.outputBitmapRoot.value();
		}
	}

	const auto start = std::chrono::steady_clock::now();
	const Hash outputBitmapRoot = pTxHashSet->GetOutputBitmapRoot(numOutputs);
	{
		std::lock_guard<std::mutex> lock(m_pibdSegmentBitmapCacheMutex);
		m_pibdSegmentBitmapCache.blockHash = blockHash;
		m_pibdSegmentBitmapCache.outputMMRSize = outputMMRSize;
		m_pibdSegmentBitmapCache.numOutputs = numOutputs;
		m_pibdSegmentBitmapCache.outputBitmapRoot = outputBitmapRoot;
	}

	LOG_TRACE(StringUtil::Format("PIBD output bitmap root cache miss for {} (num_outputs={}) built in {}ms.",
		blockHash,
		numOutputs,
		ElapsedMillis(start)));
	return outputBitmapRoot;
}

void TxHashSetPipe::ProcessOutputBitmapSegmentRequest(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
{
	const auto pHeader = m_pBlockChain->GetBlockHeaderByHash(blockHash);
	if (pHeader == nullptr) {
		LOG_DEBUG(StringUtil::Format("Cannot send output bitmap segment {}:{} for {}; header not found.",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash));
		return;
	}

	const auto snapshotStart = std::chrono::steady_clock::now();
	const auto pTxHashSet = GetSegmentTxHashSet(blockHash);
	LOG_TRACE(StringUtil::Format("PIBD output bitmap segment {}:{} snapshot lookup for {} took {}ms.",
		identifier.GetHeight(),
		identifier.GetIndex(),
		blockHash,
		ElapsedMillis(snapshotStart)));
	if (pTxHashSet == nullptr) {
		LOG_DEBUG(StringUtil::Format("Cannot send output bitmap segment {}:{} for {}; TxHashSet is not open.",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash));
		return;
	}

	const auto buildStart = std::chrono::steady_clock::now();
	std::optional<BitmapSegment> segment = pTxHashSet->GetOutputBitmapSegment(identifier);
	LOG_TRACE(StringUtil::Format("Built PIBD output bitmap segment {}:{} for {} in {}ms.",
		identifier.GetHeight(),
		identifier.GetIndex(),
		blockHash,
		ElapsedMillis(buildStart)));
	if (segment.has_value()) {
		const auto rootStart = std::chrono::steady_clock::now();
		const Hash outputRoot = pTxHashSet->GetOutputRoot(pHeader->GetOutputMMRSize());
		LOG_TRACE(StringUtil::Format("PIBD output root for bitmap segment {}:{} for {} built in {}ms.",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash,
			ElapsedMillis(rootStart)));
		const auto bitmapStart = std::chrono::steady_clock::now();
		const std::shared_ptr<const BitmapAccumulator> pOutputBitmap = GetCachedOutputBitmapAccumulator(
			pTxHashSet,
			blockHash,
			pHeader->GetOutputMMRSize());
		LOG_TRACE(StringUtil::Format("PIBD output bitmap accumulator lookup for bitmap segment {}:{} for {} took {}ms.",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash,
			ElapsedMillis(bitmapStart)));
		if (IsEligiblePIBDConnection(pConnection)) {
			pConnection->SendAsync(OutputBitmapSegmentMessage(blockHash, std::move(segment.value()), outputRoot));
			MarkOutputBitmapSegmentServed(pConnection, blockHash, identifier, pOutputBitmap->GetMMRSize());
		}
	} else {
		LOG_DEBUG(StringUtil::Format("No output bitmap segment {}:{} available for {}.",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash));
	}
}

void TxHashSetPipe::ProcessOutputSegmentRequest(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
{
	const auto pHeader = m_pBlockChain->GetBlockHeaderByHash(blockHash);
	const auto snapshotStart = std::chrono::steady_clock::now();
	const auto pTxHashSet = GetSegmentTxHashSet(blockHash);
	LOG_TRACE(StringUtil::Format("PIBD output segment {}:{} snapshot lookup for {} took {}ms.",
		identifier.GetHeight(),
		identifier.GetIndex(),
		blockHash,
		ElapsedMillis(snapshotStart)));
	if (pHeader == nullptr || pTxHashSet == nullptr) {
		return;
	}

	const auto buildStart = std::chrono::steady_clock::now();
	std::optional<Segment<PIBD::OUTPUT_DATA_SIZE, OutputIdentifier>> segment = pTxHashSet->GetOutputSegment(identifier);
	LOG_TRACE(StringUtil::Format("Built PIBD output segment {}:{} for {} in {}ms.",
		identifier.GetHeight(),
		identifier.GetIndex(),
		blockHash,
		ElapsedMillis(buildStart)));
	if (segment.has_value()) {
		const auto bitmapStart = std::chrono::steady_clock::now();
		const std::shared_ptr<const BitmapAccumulator> pOutputBitmap = GetCachedOutputBitmapAccumulator(
			pTxHashSet,
			blockHash,
			pHeader->GetOutputMMRSize());
		LOG_TRACE(StringUtil::Format("PIBD output bitmap accumulator lookup for output segment {}:{} for {} took {}ms.",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash,
			ElapsedMillis(bitmapStart)));
		if (ShouldDeferTxHashSetDataSegment(pConnection, blockHash, identifier, pOutputBitmap->GetMMRSize())) {
			return;
		}

		const auto rootStart = std::chrono::steady_clock::now();
		const Hash outputBitmapRoot = GetCachedOutputBitmapRoot(
			pTxHashSet,
			blockHash,
			pHeader->GetOutputMMRSize(),
			pHeader->GetNumOutputs());
		LOG_TRACE(StringUtil::Format("PIBD output bitmap root lookup for output segment {}:{} for {} took {}ms.",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash,
			ElapsedMillis(rootStart)));
		const auto validateStart = std::chrono::steady_clock::now();
		const bool validates = segment->ValidateWith(
			pHeader->GetOutputMMRSize(),
			pHeader->GetOutputRoot(),
			pHeader->GetOutputMMRSize(),
			outputBitmapRoot,
			false,
			pOutputBitmap.get());
		LOG_TRACE(StringUtil::Format("Validated PIBD output segment {}:{} for {} in {}ms (valid={}).",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash,
			ElapsedMillis(validateStart),
			validates));
		if (!validates) {
			LOG_WARNING(StringUtil::Format("Not sending invalid PIBD output segment {}:{} for {}.",
				identifier.GetHeight(),
				identifier.GetIndex(),
				blockHash));
			return;
		}

		LOG_TRACE(StringUtil::Format("Sending PIBD output segment {}:{} for {} to {} (leaves={}, hashes={}, proof_hashes={}, bitmap_root={}).",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash,
			pConnection,
			segment->GetLeaves().size(),
			segment->GetHashes().size(),
			segment->GetProof().GetHashes().size(),
			outputBitmapRoot));
		if (IsEligiblePIBDConnection(pConnection)) {
			pConnection->SendAsync(OutputSegmentMessage(blockHash, std::move(segment.value()), outputBitmapRoot));
		}
	} else {
		LOG_WARNING(StringUtil::Format("No PIBD output segment {}:{} available for {}.",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash));
	}
}

void TxHashSetPipe::ProcessRangeProofSegmentRequest(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
{
	const auto pHeader = m_pBlockChain->GetBlockHeaderByHash(blockHash);
	const auto snapshotStart = std::chrono::steady_clock::now();
	const auto pTxHashSet = GetSegmentTxHashSet(blockHash);
	LOG_TRACE(StringUtil::Format("PIBD rangeproof segment {}:{} snapshot lookup for {} took {}ms.",
		identifier.GetHeight(),
		identifier.GetIndex(),
		blockHash,
		ElapsedMillis(snapshotStart)));
	if (pHeader == nullptr || pTxHashSet == nullptr) {
		return;
	}

	const auto buildStart = std::chrono::steady_clock::now();
	std::optional<Segment<PIBD::RANGE_PROOF_DATA_SIZE, RangeProof>> segment = pTxHashSet->GetRangeProofSegment(identifier);
	LOG_TRACE(StringUtil::Format("Built PIBD rangeproof segment {}:{} for {} in {}ms.",
		identifier.GetHeight(),
		identifier.GetIndex(),
		blockHash,
		ElapsedMillis(buildStart)));
	if (segment.has_value()) {
		const auto bitmapStart = std::chrono::steady_clock::now();
		const std::shared_ptr<const BitmapAccumulator> pOutputBitmap = GetCachedOutputBitmapAccumulator(
			pTxHashSet,
			blockHash,
			pHeader->GetOutputMMRSize());
		LOG_TRACE(StringUtil::Format("PIBD output bitmap accumulator lookup for rangeproof segment {}:{} for {} took {}ms.",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash,
			ElapsedMillis(bitmapStart)));
		if (ShouldDeferTxHashSetDataSegment(pConnection, blockHash, identifier, pOutputBitmap->GetMMRSize())) {
			return;
		}

		const auto validateStart = std::chrono::steady_clock::now();
		const bool validatesWithBitmap = segment->Validate(
			pHeader->GetOutputMMRSize(),
			pHeader->GetRangeProofRoot(),
			pOutputBitmap.get());
		LOG_TRACE(StringUtil::Format("Validated PIBD rangeproof segment {}:{} for {} in {}ms (valid={}).",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash,
			ElapsedMillis(validateStart),
			validatesWithBitmap));
		if (!validatesWithBitmap) {
			LOG_WARNING(StringUtil::Format("Not sending invalid PIBD rangeproof segment {}:{} for {}.",
				identifier.GetHeight(),
				identifier.GetIndex(),
				blockHash));
			return;
		}

		LOG_TRACE(StringUtil::Format("Sending PIBD rangeproof segment {}:{} for {} to {} (leaves={}, hashes={}, proof_hashes={}).",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash,
			pConnection,
			segment->GetLeaves().size(),
			segment->GetHashes().size(),
			segment->GetProof().GetHashes().size()));
		if (IsEligiblePIBDConnection(pConnection)) {
			pConnection->SendAsync(RangeProofSegmentMessage(blockHash, std::move(segment.value())));
		}
	} else {
		LOG_WARNING(StringUtil::Format("No PIBD rangeproof segment {}:{} available for {}.",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash));
	}
}

void TxHashSetPipe::ProcessKernelSegmentRequest(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
{
	const auto pHeader = m_pBlockChain->GetBlockHeaderByHash(blockHash);
	const auto snapshotStart = std::chrono::steady_clock::now();
	const auto pTxHashSet = GetSegmentTxHashSet(blockHash);
	LOG_TRACE(StringUtil::Format("PIBD kernel segment {}:{} snapshot lookup for {} took {}ms.",
		identifier.GetHeight(),
		identifier.GetIndex(),
		blockHash,
		ElapsedMillis(snapshotStart)));
	if (pHeader == nullptr || pTxHashSet == nullptr) {
		return;
	}

	const auto buildStart = std::chrono::steady_clock::now();
	std::optional<Segment<PIBD::KERNEL_DATA_SIZE, TransactionKernel>> segment = pTxHashSet->GetKernelSegment(identifier);
	LOG_TRACE(StringUtil::Format("Built PIBD kernel segment {}:{} for {} in {}ms.",
		identifier.GetHeight(),
		identifier.GetIndex(),
		blockHash,
		ElapsedMillis(buildStart)));
	if (segment.has_value()) {
		if (identifier.GetIndex() == 0 && std::find(segment->GetLeafPositions().begin(), segment->GetLeafPositions().end(), 0) == segment->GetLeafPositions().end()) {
			LOG_WARNING(StringUtil::Format("Not sending PIBD kernel segment {}:{} for {}; Genesis kernel leaf at position 0 is missing.",
				identifier.GetHeight(),
				identifier.GetIndex(),
				blockHash));
			return;
		}

		const auto validateStart = std::chrono::steady_clock::now();
		const bool validates = segment->Validate(
			pHeader->GetKernelMMRSize(),
			pHeader->GetKernelRoot());
		LOG_TRACE(StringUtil::Format("Validated PIBD kernel segment {}:{} for {} in {}ms (valid={}).",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash,
			ElapsedMillis(validateStart),
			validates));
		if (!validates) {
			LOG_WARNING(StringUtil::Format("Not sending invalid PIBD kernel segment {}:{} for {}.",
				identifier.GetHeight(),
				identifier.GetIndex(),
				blockHash));
			return;
		}

		if (IsEligiblePIBDConnection(pConnection)) {
			pConnection->SendAsync(KernelSegmentMessage(blockHash, std::move(segment.value())));
		}
	} else {
		LOG_WARNING(StringUtil::Format("No PIBD kernel segment {}:{} available for {}.",
			identifier.GetHeight(),
			identifier.GetIndex(),
			blockHash));
	}
}

ITxHashSetConstPtr TxHashSetPipe::GetSegmentTxHashSet(const Hash& blockHash)
{
	{
		const auto txHashSetReader = m_pTxHashSetManager->Read();
		const auto pLiveTxHashSet = txHashSetReader->GetTxHashSet();
		if (pLiveTxHashSet != nullptr && pLiveTxHashSet->GetFlushedBlockHeader()->GetHash() == blockHash) {
			LOG_TRACE(StringUtil::Format("PIBD segment TxHashSet live cache hit for {}.", blockHash));
			return pLiveTxHashSet;
		}
	}

	{
		std::unique_lock<std::mutex> lock(m_pibdSegmentTxHashSetMutex);
		while (m_pibdSegmentTxHashSetCreatingHash.has_value()) {
			if (m_pibdSegmentTxHashSet != nullptr
				&& m_pibdSegmentTxHashSetHash.has_value()
				&& m_pibdSegmentTxHashSetHash.value() == blockHash) {
				LOG_TRACE(StringUtil::Format("PIBD segment TxHashSet snapshot cache hit for {} while creating {}.",
					blockHash,
					m_pibdSegmentTxHashSetCreatingHash.value()));
				return m_pibdSegmentTxHashSet;
			}

			LOG_TRACE(StringUtil::Format("PIBD segment TxHashSet snapshot wait for {}; currently creating {}.",
				blockHash,
				m_pibdSegmentTxHashSetCreatingHash.value()));
			m_pibdSegmentTxHashSetCondition.wait(lock);
		}

		if (m_pibdSegmentTxHashSet != nullptr && m_pibdSegmentTxHashSetHash.has_value() && m_pibdSegmentTxHashSetHash.value() == blockHash) {
			LOG_TRACE(StringUtil::Format("PIBD segment TxHashSet snapshot cache hit for {}.", blockHash));
			return m_pibdSegmentTxHashSet;
		}

		LOG_DEBUG(StringUtil::Format("PIBD segment TxHashSet snapshot cache miss for {}.", blockHash));
		m_pibdSegmentTxHashSetCreatingHash = blockHash;
	}

	const auto pHeader = m_pBlockChain->GetBlockHeaderByHash(blockHash);
	if (pHeader == nullptr) {
		LOG_DEBUG(StringUtil::Format("Cannot create PIBD segment TxHashSet for {}; header not found.", blockHash));
		{
			std::lock_guard<std::mutex> lock(m_pibdSegmentTxHashSetMutex);
			m_pibdSegmentTxHashSetCreatingHash.reset();
		}
		m_pibdSegmentTxHashSetCondition.notify_all();
		return nullptr;
	}

	try
	{
		const auto snapshotStart = std::chrono::steady_clock::now();
		LOG_DEBUG(StringUtil::Format("Creating PIBD segment TxHashSet for {} at height {}.", blockHash, pHeader->GetHeight()));
		ITxHashSetPtr pSnapshot = m_pBlockChain->CreateTxHashSetSnapshot(pHeader);
		{
			std::lock_guard<std::mutex> lock(m_pibdSegmentTxHashSetMutex);
			m_pibdSegmentTxHashSet = pSnapshot;
			m_pibdSegmentTxHashSetHash = blockHash;
			m_pibdSegmentTxHashSetCreatingHash.reset();
		}
		{
			std::lock_guard<std::mutex> lock(m_pibdSegmentBitmapCacheMutex);
			m_pibdSegmentBitmapCache = PIBDSegmentBitmapCache();
		}
		m_pibdSegmentTxHashSetCondition.notify_all();

		LOG_DEBUG(StringUtil::Format("PIBD segment TxHashSet ready for {} at height {} in {}ms.",
			blockHash,
			pHeader->GetHeight(),
			ElapsedMillis(snapshotStart)));
		return pSnapshot;
	}
	catch (std::exception& e)
	{
		LOG_WARNING(StringUtil::Format("Failed to create PIBD segment TxHashSet for {}: {}", blockHash, e.what()));
		{
			std::lock_guard<std::mutex> lock(m_pibdSegmentTxHashSetMutex);
			m_pibdSegmentTxHashSetCreatingHash.reset();
		}
		m_pibdSegmentTxHashSetCondition.notify_all();
		return nullptr;
	}
	catch (...)
	{
		LOG_WARNING(StringUtil::Format("Failed to create PIBD segment TxHashSet for {}: unknown exception.", blockHash));
		{
			std::lock_guard<std::mutex> lock(m_pibdSegmentTxHashSetMutex);
			m_pibdSegmentTxHashSetCreatingHash.reset();
		}
		m_pibdSegmentTxHashSetCondition.notify_all();
		return nullptr;
	}
}

void TxHashSetPipe::MarkOutputBitmapSegmentServed(
	const std::shared_ptr<Connection>& pConnection,
	const Hash& blockHash,
	const SegmentIdentifier& identifier,
	const uint64_t bitmapMMRSize)
{
	const size_t totalBitmapSegments = SegmentIdentifier::CountSegmentsRequired(
		bitmapMMRSize,
		PIBD::BITMAP_SEGMENT_HEIGHT);

	std::lock_guard<std::mutex> lock(m_servedBitmapSegmentsMutex);
	ServedOutputBitmapState& state = m_servedBitmapSegments[GetPIBDSegmentPeerKey(pConnection, blockHash)];
	state.segmentIndices.insert(identifier.GetIndex());
	if (totalBitmapSegments > 0
		&& state.segmentIndices.size() >= totalBitmapSegments
		&& !state.completedAt.has_value()) {
		state.completedAt = std::chrono::steady_clock::now();
		LOG_DEBUG(StringUtil::Format("PIBD output bitmap completed for {} to {} ({} segments).",
			blockHash,
			pConnection,
			totalBitmapSegments));
	}
}

bool TxHashSetPipe::ShouldDeferTxHashSetDataSegment(
	const std::shared_ptr<Connection>& pConnection,
	const Hash& blockHash,
	const SegmentIdentifier& requestedIdentifier,
	const uint64_t bitmapMMRSize) const
{
	(void)pConnection;
	(void)blockHash;
	(void)requestedIdentifier;
	(void)bitmapMMRSize;
	return false;
}

void TxHashSetPipe::ReceiveOutputBitmapSegment(const std::shared_ptr<Connection>& pConnection, const OutputBitmapSegmentMessage& message)
{
	LOG_TRACE(StringUtil::Format("Received output bitmap segment {}:{} for {} from {}.",
		message.GetSegment().GetIdentifier().GetHeight(),
		message.GetSegment().GetIdentifier().GetIndex(),
		message.GetBlockHash(),
		pConnection));

	std::lock_guard<std::mutex> lock(m_pibdMutex);
	if (!m_pDesegmenter || !m_pibdBlockHash.has_value() || m_pibdBlockHash.value() != message.GetBlockHash()) {
		return;
	}

	if (!IsEligiblePIBDConnection(pConnection)) {
		return;
	}

	const SegmentTypeIdentifier typeId(SegmentType::OutputBitmap, message.GetSegment().GetIdentifier());
	if (m_pDesegmenter->IsApplied(typeId)) {
		m_segmentRequests.MarkReceived(typeId);
		LOG_TRACE(StringUtil::Format("Ignoring already applied output bitmap segment {}:{} from {}.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	if (!IsExpectedPIBDSegment(m_segmentRequests, typeId, pConnection)) {
		LOG_TRACE(StringUtil::Format("Ignoring unexpected output bitmap segment {}:{} from {}.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	if (!m_pDesegmenter->AddBitmapSegment(message.GetSegment(), message.GetOutputRoot())) {
		m_segmentRequests.MarkReceived(typeId);
		LOG_WARNING(StringUtil::Format("PIBD output bitmap segment {}:{} from {} failed validation; request will be retried without banning the peer.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	m_segmentRequests.MarkReceived(typeId);
	while (m_pDesegmenter->ApplyNextBitmapSegment()) {
	}
	UpdatePIBDStatus();
}

void TxHashSetPipe::ReceiveOutputSegment(const std::shared_ptr<Connection>& pConnection, const OutputSegmentMessage& message)
{
	LOG_TRACE(StringUtil::Format("Received output segment {}:{} for {} from {}.",
		message.GetSegment().GetIdentifier().GetHeight(),
		message.GetSegment().GetIdentifier().GetIndex(),
		message.GetBlockHash(),
		pConnection));

	std::lock_guard<std::mutex> lock(m_pibdMutex);
	if (!m_pDesegmenter || !m_pibdBlockHash.has_value() || m_pibdBlockHash.value() != message.GetBlockHash()) {
		return;
	}

	if (!IsEligiblePIBDConnection(pConnection)) {
		return;
	}

	const SegmentTypeIdentifier typeId(SegmentType::Output, message.GetSegment().GetIdentifier());
	if (m_pDesegmenter->IsApplied(typeId)) {
		m_segmentRequests.MarkReceived(typeId);
		LOG_TRACE(StringUtil::Format("Ignoring already applied output segment {}:{} from {}.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	if (!IsExpectedPIBDSegment(m_segmentRequests, typeId, pConnection)) {
		LOG_TRACE(StringUtil::Format("Ignoring unexpected output segment {}:{} from {}.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	if (!m_pDesegmenter->AddOutputSegment(message.GetSegment(), message.GetOutputBitmapRoot())) {
		m_segmentRequests.MarkReceived(typeId);
		LOG_WARNING(StringUtil::Format("PIBD output segment {}:{} from {} failed validation; request will be retried without banning the peer.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	m_segmentRequests.MarkReceived(typeId);
	UpdatePIBDStatus();
}

void TxHashSetPipe::ReceiveRangeProofSegment(const std::shared_ptr<Connection>& pConnection, const RangeProofSegmentMessage& message)
{
	LOG_TRACE(StringUtil::Format("Received rangeproof segment {}:{} for {} from {}.",
		message.GetSegment().GetIdentifier().GetHeight(),
		message.GetSegment().GetIdentifier().GetIndex(),
		message.GetBlockHash(),
		pConnection));

	std::lock_guard<std::mutex> lock(m_pibdMutex);
	if (!m_pDesegmenter || !m_pibdBlockHash.has_value() || m_pibdBlockHash.value() != message.GetBlockHash()) {
		return;
	}

	if (!IsEligiblePIBDConnection(pConnection)) {
		return;
	}

	const SegmentTypeIdentifier typeId(SegmentType::RangeProof, message.GetSegment().GetIdentifier());
	if (m_pDesegmenter->IsApplied(typeId)) {
		m_segmentRequests.MarkReceived(typeId);
		LOG_TRACE(StringUtil::Format("Ignoring already applied rangeproof segment {}:{} from {}.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	if (!IsExpectedPIBDSegment(m_segmentRequests, typeId, pConnection)) {
		LOG_TRACE(StringUtil::Format("Ignoring unexpected rangeproof segment {}:{} from {}.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	if (!m_pDesegmenter->AddRangeProofSegment(message.GetSegment())) {
		m_segmentRequests.MarkReceived(typeId);
		LOG_WARNING(StringUtil::Format("PIBD rangeproof segment {}:{} from {} failed validation; request will be retried without banning the peer.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	m_segmentRequests.MarkReceived(typeId);
	UpdatePIBDStatus();
}

void TxHashSetPipe::ReceiveKernelSegment(const std::shared_ptr<Connection>& pConnection, const KernelSegmentMessage& message)
{
	LOG_TRACE(StringUtil::Format("Received kernel segment {}:{} for {} from {}.",
		message.GetSegment().GetIdentifier().GetHeight(),
		message.GetSegment().GetIdentifier().GetIndex(),
		message.GetBlockHash(),
		pConnection));

	std::lock_guard<std::mutex> lock(m_pibdMutex);
	if (!m_pDesegmenter || !m_pibdBlockHash.has_value() || m_pibdBlockHash.value() != message.GetBlockHash()) {
		return;
	}

	if (!IsEligiblePIBDConnection(pConnection)) {
		return;
	}

	const SegmentTypeIdentifier typeId(SegmentType::Kernel, message.GetSegment().GetIdentifier());
	if (m_pDesegmenter->IsApplied(typeId)) {
		m_segmentRequests.MarkReceived(typeId);
		LOG_TRACE(StringUtil::Format("Ignoring already applied kernel segment {}:{} from {}.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	if (!IsExpectedPIBDSegment(m_segmentRequests, typeId, pConnection)) {
		LOG_TRACE(StringUtil::Format("Ignoring unexpected kernel segment {}:{} from {}.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	if (!m_pDesegmenter->AddKernelSegment(message.GetSegment())) {
		m_segmentRequests.MarkReceived(typeId);
		LOG_WARNING(StringUtil::Format("PIBD kernel segment {}:{} from {} failed validation; request will be retried without banning the peer.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	m_segmentRequests.MarkReceived(typeId);
	UpdatePIBDStatus();
}

bool TxHashSetPipe::StartPIBD(const BlockHeaderPtr& pArchiveHeader)
{
	if (pArchiveHeader == nullptr) {
		return false;
	}

	std::lock_guard<std::mutex> lock(m_pibdMutex);
	BlockHeaderPtr pEffectiveArchiveHeader = pArchiveHeader;

	try {
		auto txHashSetReader = m_pTxHashSetManager->Read();
		const std::optional<Hash> resumeHeaderHash = txHashSetReader->GetPIBDResumeHeaderHash();
		if (resumeHeaderHash.has_value() && resumeHeaderHash.value() != pArchiveHeader->GetHash()) {
			BlockHeaderPtr pResumeHeader = m_pBlockChain->GetBlockHeaderByHash(resumeHeaderHash.value());
			if (pResumeHeader != nullptr) {
				BlockHeaderPtr pCandidateHeader = m_pBlockChain->GetBlockHeaderByHeight(
					pResumeHeader->GetHeight(),
					EChainType::CANDIDATE);
				if (pCandidateHeader != nullptr && pCandidateHeader->GetHash() == pResumeHeader->GetHash()) {
					pEffectiveArchiveHeader = pResumeHeader;
					LOG_INFO(StringUtil::Format(
						"Continuing PIBD from stored archive header {} at height {} instead of new archive header {} at height {}.",
						pResumeHeader->GetHash(),
						pResumeHeader->GetHeight(),
						pArchiveHeader->GetHash(),
						pArchiveHeader->GetHeight()));
				} else {
					LOG_WARNING(StringUtil::Format(
						"Stored PIBD archive header {} at height {} is no longer on the candidate chain; starting PIBD from new archive header {} at height {}.",
						pResumeHeader->GetHash(),
						pResumeHeader->GetHeight(),
						pArchiveHeader->GetHash(),
						pArchiveHeader->GetHeight()));
				}
			} else {
				LOG_WARNING(StringUtil::Format(
					"Stored PIBD archive header {} is not available locally; starting PIBD from new archive header {} at height {}.",
					resumeHeaderHash.value(),
					pArchiveHeader->GetHash(),
					pArchiveHeader->GetHeight()));
			}
		}
	} catch (const std::exception& e) {
		LOG_WARNING(StringUtil::Format("Failed to inspect PIBD resume metadata: {}", e.what()));
	} catch (...) {
		LOG_WARNING("Failed to inspect PIBD resume metadata.");
	}

	if (m_pDesegmenter != nullptr && m_pibdBlockHash.has_value() && m_pibdBlockHash.value() == pEffectiveArchiveHeader->GetHash()) {
		return true;
	}

	try {
		auto txHashSetWriter = m_pTxHashSetManager->Write();
		auto pTxHashSet = txHashSetWriter->OpenForPIBD(pEffectiveArchiveHeader);
		if (pTxHashSet == nullptr) {
			return false;
		}

		m_pDesegmenter = std::make_unique<TxHashSetDesegmenter>(*pEffectiveArchiveHeader);
		m_pDesegmenter->SetAppliedMMRSizes(
			pTxHashSet->GetOutputMMRSize(),
			pTxHashSet->GetRangeProofMMRSize(),
			pTxHashSet->GetKernelMMRSize());
	} catch (const std::exception& e) {
		LOG_ERROR_F("Failed to start PIBD for header {}: {}", *pEffectiveArchiveHeader, e.what());
		return false;
	} catch (...) {
		LOG_ERROR_F("Unknown error while starting PIBD for header {}", *pEffectiveArchiveHeader);
		return false;
	}

	m_pibdBlockHash = pEffectiveArchiveHeader->GetHash();
	m_segmentRequests = SegmentRequestTracker();
	m_lastLoggedPIBDCompletedLeaves = std::numeric_limits<uint64_t>::max();
	m_cachedPIBDCompletedToHeight = 0;
	m_lastPIBDStatusHeightCalcLeaves = std::numeric_limits<uint64_t>::max();
	m_lastPIBDStatusHeightCalcTime = {};
	m_pibdNextPeerIndex = 0;
	m_uncommittedPIBDSegments = 0;
	m_processing = true;
	UpdatePIBDStatus();
	LOG_INFO_F("Started PIBD for header {}", *pEffectiveArchiveHeader);
	return true;
}

bool TxHashSetPipe::RequestNextPIBDSegments(const std::shared_ptr<ConnectionManager>& pConnectionManager, const std::vector<PeerConstPtr>& peers)
{
	std::unique_lock<std::mutex> lock(m_pibdMutex);
	if (!m_pDesegmenter || !m_pibdBlockHash.has_value() || peers.empty()) {
		return false;
	}

	{
		auto txHashSetWriter = m_pTxHashSetManager->Write();
		auto pTxHashSet = txHashSetWriter->GetTxHashSet();
		if (pTxHashSet == nullptr) {
			return false;
		}

		const BitmapAccumulator& outputBitmap = m_pDesegmenter->GetBitmapAccumulator();
		const std::optional<size_t> appliedSegments = m_pDesegmenter->ApplyReadySegments(
			[pTxHashSet, &outputBitmap](const TxHashSetDesegmenter::OutputPMMRSegment& segment, const uint64_t targetSize) {
				const bool applied = pTxHashSet->ApplyOutputSegment(segment, targetSize, &outputBitmap);
				if (!applied) {
					LOG_WARNING(StringUtil::Format("Failed to apply PIBD output segment {}:{}.",
						segment.GetIdentifier().GetHeight(),
						segment.GetIdentifier().GetIndex()));
				}
				return applied;
			},
			[pTxHashSet, &outputBitmap](const TxHashSetDesegmenter::RangeProofPMMRSegment& segment, const uint64_t targetSize) {
				const bool applied = pTxHashSet->ApplyRangeProofSegment(segment, targetSize, &outputBitmap);
				if (!applied) {
					LOG_WARNING(StringUtil::Format("Failed to apply PIBD rangeproof segment {}:{}.",
						segment.GetIdentifier().GetHeight(),
						segment.GetIdentifier().GetIndex()));
				}
				return applied;
			},
			[pTxHashSet](const TxHashSetDesegmenter::KernelPMMRSegment& segment, const uint64_t targetSize) {
				const bool applied = pTxHashSet->ApplyKernelSegment(segment, targetSize);
				if (!applied) {
					LOG_WARNING(StringUtil::Format("Failed to apply PIBD kernel segment {}:{}.",
						segment.GetIdentifier().GetHeight(),
						segment.GetIdentifier().GetIndex()));
				}
				return applied;
			});
		if (!appliedSegments.has_value()) {
			UpdatePIBDStatus(false, true);
			return false;
		}

		m_uncommittedPIBDSegments += appliedSegments.value();
		if (m_uncommittedPIBDSegments >= PIBD::PIBD_COMMIT_SEGMENT_THRESHOLD) {
			pTxHashSet->Commit();
			LOG_DEBUG(StringUtil::Format("Committed {} applied PIBD segment(s).", m_uncommittedPIBDSegments));
			m_uncommittedPIBDSegments = 0;
		}
	}
	UpdatePIBDStatus();

	if (m_pDesegmenter->IsComplete()) {
		const Hash pibdBlockHash = m_pibdBlockHash.value();
		{
			auto txHashSetWriter = m_pTxHashSetManager->Write();
			auto pTxHashSet = txHashSetWriter->GetTxHashSet();
			if (pTxHashSet == nullptr) {
				LOG_ERROR("PIBD TxHashSet is null before validation.");
				m_processing = false;
				UpdatePIBDStatus(false, true);
				return false;
			}

			LOG_DEBUG("Updating PIBD leafsets from output bitmap before validation.");
			m_pSyncStatus->UpdateStatus(ESyncStatus::TXHASHSET_PIBD_LEAFSET_UPDATE);
			pTxHashSet->UpdateLeafSets(
				m_pDesegmenter->GetBitmapAccumulator(),
				m_pDesegmenter->GetArchiveHeader().GetNumOutputs());
			pTxHashSet->Commit();
			m_uncommittedPIBDSegments = 0;
		}

		if (m_pPIBDValidationJob->IsRunning()) {
			return true;
		}

		if (!m_pPIBDValidationJob->Start(pibdBlockHash)) {
			LOG_WARNING_F("PIBD validation job is already running for {}", pibdBlockHash);
			return true;
		}

		return true;
	}

	const std::vector<SegmentRequestTracker::PendingRequest> timedOutRequests = m_segmentRequests.GetTimedOutRequests();
	if (!timedOutRequests.empty()) {
		LOG_DEBUG(StringUtil::Format("Retrying {} timed-out PIBD segment requests.", timedOutRequests.size()));
		if (m_uncommittedPIBDSegments >= PIBD::PIBD_COMMIT_SEGMENT_THRESHOLD) {
			auto txHashSetWriter = m_pTxHashSetManager->Write();
			auto pTxHashSet = txHashSetWriter->GetTxHashSet();
			if (pTxHashSet != nullptr) {
				pTxHashSet->Commit();
				LOG_DEBUG(StringUtil::Format("Committed {} applied PIBD segment(s) before retrying timed-out requests.", m_uncommittedPIBDSegments));
			}
			m_uncommittedPIBDSegments = 0;
		}
		m_segmentRequests.RemoveTimedOutRequests();
	}

	std::vector<PeerConstPtr> requestPeers;
	requestPeers.reserve(peers.size());
	for (const PeerConstPtr& peer : peers) {
		if (peer != nullptr) {
			requestPeers.push_back(peer);
		}
	}

	if (requestPeers.empty()) {
		UpdatePIBDStatus();
		return true;
	}

	const size_t pendingCount = m_segmentRequests.GetPendingRequests().size();
	const size_t requestBudget = pendingCount < PIBD::SEGMENT_REQUEST_COUNT ? PIBD::SEGMENT_REQUEST_COUNT - pendingCount : 0;
	const size_t desiredCandidateCount = PIBD::SEGMENT_REQUEST_COUNT + pendingCount;
	const std::vector<SegmentTypeIdentifier> desired = m_pDesegmenter->NextDesiredSegments(desiredCandidateCount);
	std::vector<SegmentTypeIdentifier> requestsToSend;
	std::vector<SegmentTypeIdentifier> retryRequests;
	requestsToSend.reserve(requestBudget + PIBD::BLOCKING_SEGMENT_RETRY_COUNT + PIBD::BLOCKING_SEGMENT_HEDGE_COUNT);
	retryRequests.reserve(PIBD::BLOCKING_SEGMENT_RETRY_COUNT + PIBD::BLOCKING_SEGMENT_HEDGE_COUNT);
	size_t newRequestCount = 0;
	const auto blockingRetryDelay = std::chrono::seconds(PIBD::BLOCKING_SEGMENT_RETRY_SECS);
	for (const SegmentTypeIdentifier& typeId : desired) {
		if (!m_segmentRequests.IsPending(typeId)) {
			if (newRequestCount < requestBudget) {
				requestsToSend.push_back(typeId);
				++newRequestCount;
			}
		} else if (retryRequests.size() < PIBD::BLOCKING_SEGMENT_RETRY_COUNT && m_segmentRequests.CanRetry(typeId, blockingRetryDelay)) {
			requestsToSend.push_back(typeId);
			retryRequests.push_back(typeId);
			LOG_DEBUG(StringUtil::Format("Retrying blocked PIBD {} segment {}:{} after {}s.",
				GetPIBDSegmentTypeName(typeId.GetSegmentType()),
				typeId.GetIdentifier().GetHeight(),
				typeId.GetIdentifier().GetIndex(),
				PIBD::BLOCKING_SEGMENT_RETRY_SECS));
		}

		if (newRequestCount >= requestBudget && retryRequests.size() >= PIBD::BLOCKING_SEGMENT_RETRY_COUNT) {
			break;
		}
	}

	if (requestPeers.size() > 1 && retryRequests.size() < PIBD::BLOCKING_SEGMENT_HEDGE_COUNT) {
		const std::vector<SegmentTypeIdentifier> blockers = m_pDesegmenter->GetBlockingSegments();
		for (const SegmentTypeIdentifier& typeId : blockers) {
			if (retryRequests.size() >= PIBD::BLOCKING_SEGMENT_HEDGE_COUNT) {
				break;
			}

			const uint16_t hedgeDelaySecs = typeId.GetSegmentType() == SegmentType::OutputBitmap
				? PIBD::BITMAP_BLOCKING_SEGMENT_HEDGE_SECS
				: PIBD::BLOCKING_SEGMENT_HEDGE_SECS;
			const auto hedgeDelay = std::chrono::seconds(hedgeDelaySecs);
			if (!m_segmentRequests.IsPending(typeId) || !m_segmentRequests.CanHedge(typeId, hedgeDelay)) {
				continue;
			}

			const bool alreadyQueued = std::any_of(
				requestsToSend.begin(),
				requestsToSend.end(),
				[&typeId](const SegmentTypeIdentifier& requestTypeId) { return requestTypeId == typeId; });
			if (alreadyQueued) {
				continue;
			}

			requestsToSend.push_back(typeId);
			retryRequests.push_back(typeId);
			LOG_DEBUG(StringUtil::Format("Hedging blocked PIBD {} segment {}:{} after {}s.",
				GetPIBDSegmentTypeName(typeId.GetSegmentType()),
				typeId.GetIdentifier().GetHeight(),
				typeId.GetIdentifier().GetIndex(),
				hedgeDelaySecs));
		}
	}

	if (!requestsToSend.empty()) {
		const BlockHeader& archiveHeader = m_pDesegmenter->GetArchiveHeader();
		LOG_TRACE(StringUtil::Format(
			"Requesting {} PIBD segment(s) across {} peer(s) (pending={}, budget={}, archive_height={}, archive_hash={}).",
			requestsToSend.size(),
			requestPeers.size(),
			pendingCount,
			requestBudget,
			archiveHeader.GetHeight(),
			archiveHeader.GetHash()));
	}

	// Distribute requests round-robin across all available PIBD peers, keeping
	// rotation state across calls so the head segment does not always hit peer 0.
	size_t peerIdx = m_pibdNextPeerIndex % requestPeers.size();
	for (const SegmentTypeIdentifier& typeId : requestsToSend) {
		const bool retryRequest = std::any_of(
			retryRequests.begin(),
			retryRequests.end(),
			[&typeId](const SegmentTypeIdentifier& retryTypeId) { return retryTypeId == typeId; });
		const std::optional<SegmentRequestTracker::PendingRequest> previousRequest = retryRequest
			? m_segmentRequests.GetPendingRequest(typeId)
			: std::nullopt;

		std::unique_ptr<IMessage> pMessage;
		switch (typeId.GetSegmentType())
		{
			case SegmentType::OutputBitmap:
				pMessage = std::make_unique<GetOutputBitmapSegmentMessage>(m_pibdBlockHash.value(), typeId.GetIdentifier());
				break;
			case SegmentType::Output:
				pMessage = std::make_unique<GetOutputSegmentMessage>(m_pibdBlockHash.value(), typeId.GetIdentifier());
				break;
			case SegmentType::RangeProof:
				pMessage = std::make_unique<GetRangeProofSegmentMessage>(m_pibdBlockHash.value(), typeId.GetIdentifier());
				break;
			case SegmentType::Kernel:
				pMessage = std::make_unique<GetKernelSegmentMessage>(m_pibdBlockHash.value(), typeId.GetIdentifier());
				break;
		}

		if (pMessage == nullptr) {
			continue;
		}

		// Try peers starting from peerIdx, advance on success
		bool sent = false;
		for (size_t attempt = 0; attempt < requestPeers.size(); ++attempt) {
			const PeerConstPtr& pCurrentPeer = requestPeers[(peerIdx + attempt) % requestPeers.size()];
			if (previousRequest.has_value()
				&& requestPeers.size() > 1
				&& previousRequest->peerId == SocketAddress(pCurrentPeer->GetIPAddress(), pCurrentPeer->GetPort()).Format()) {
				continue;
			}

			if (pConnectionManager->SendMessageToPeer(*pMessage, pCurrentPeer)) {
				m_segmentRequests.AddOrRefresh(typeId, SocketAddress(pCurrentPeer->GetIPAddress(), pCurrentPeer->GetPort()).Format());
				const BlockHeader& archiveHeader = m_pDesegmenter->GetArchiveHeader();
				const uint64_t peerHeight = GetPeerReportedHeight(pConnectionManager, pCurrentPeer);
				LOG_TRACE(StringUtil::Format("Requested PIBD {} segment {}:{} for archive {}:{} from {} (peer_height={}).",
					GetPIBDSegmentTypeName(typeId.GetSegmentType()),
					typeId.GetIdentifier().GetHeight(),
					typeId.GetIdentifier().GetIndex(),
					archiveHeader.GetHeight(),
					archiveHeader.GetHash(),
					pCurrentPeer,
					peerHeight));
				peerIdx = (peerIdx + attempt + 1) % requestPeers.size();
				m_pibdNextPeerIndex = peerIdx;
				sent = true;
				break;
			}
		}

		if (!sent) {
			LOG_WARNING(StringUtil::Format("Failed to request PIBD {} segment {}:{} from any peer.",
				GetPIBDSegmentTypeName(typeId.GetSegmentType()),
				typeId.GetIdentifier().GetHeight(),
				typeId.GetIdentifier().GetIndex()));
		}
	}

	UpdatePIBDStatus();
	return true;
}

void TxHashSetPipe::UpdatePIBDStatus(const bool aborted, const bool errored)
{
	if (m_pDesegmenter == nullptr) {
		return;
	}

	const uint64_t completedLeaves = m_pDesegmenter->GetCompletedLeaves();
	const uint64_t leavesRequired = m_pDesegmenter->GetLeavesRequired();
	const uint64_t requiredHeight = m_pDesegmenter->GetRequiredHeight();
	const bool complete = completedLeaves >= leavesRequired && leavesRequired > 0;
	const auto now = std::chrono::steady_clock::now();
	const bool heightCalcUninitialized = m_lastPIBDStatusHeightCalcLeaves == std::numeric_limits<uint64_t>::max();
	const bool enoughLeavesApplied = !heightCalcUninitialized
		&& completedLeaves >= m_lastPIBDStatusHeightCalcLeaves + PIBD::PIBD_STATUS_UPDATE_LEAF_DELTA;
	const bool enoughTimePassed = m_lastPIBDStatusHeightCalcTime.time_since_epoch().count() == 0
		|| std::chrono::duration_cast<std::chrono::seconds>(now - m_lastPIBDStatusHeightCalcTime).count() >= PIBD::PIBD_STATUS_UPDATE_INTERVAL_SECS;
	const bool refreshHeight = aborted || errored || complete || heightCalcUninitialized || enoughLeavesApplied || enoughTimePassed;

	if (refreshHeight) {
		m_cachedPIBDCompletedToHeight = GetPIBDCompletedToHeight();
		m_lastPIBDStatusHeightCalcLeaves = completedLeaves;
		m_lastPIBDStatusHeightCalcTime = now;
	}

	m_pSyncStatus->UpdatePIBDStatus(
		aborted,
		errored,
		completedLeaves,
		leavesRequired,
		m_cachedPIBDCompletedToHeight,
		requiredHeight);

	if ((aborted || errored || refreshHeight || complete) && completedLeaves != m_lastLoggedPIBDCompletedLeaves) {
		LOG_DEBUG(StringUtil::Format(
			"PIBD progress: {}/{} txhashset leaves, complete_height {}/{} (aborted={}, errored={}).",
			completedLeaves,
			leavesRequired,
			m_cachedPIBDCompletedToHeight,
			requiredHeight,
			aborted,
			errored));
		m_lastLoggedPIBDCompletedLeaves = completedLeaves;
	}
}

uint64_t TxHashSetPipe::GetPIBDCompletedToHeight() const
{
	if (m_pDesegmenter == nullptr) {
		return 0;
	}

	if (m_pBlockChain == nullptr) {
		return m_pDesegmenter->GetCompletedToHeight();
	}

	const uint64_t archiveHeight = m_pDesegmenter->GetRequiredHeight();
	const uint64_t outputMMRSize = (std::min)(
		m_pDesegmenter->GetAppliedOutputMMRSize(),
		m_pDesegmenter->GetAppliedRangeProofMMRSize());
	const uint64_t kernelMMRSize = m_pDesegmenter->GetAppliedKernelMMRSize();

	uint64_t low = 0;
	uint64_t high = archiveHeight;
	uint64_t completedHeight = 0;

	while (low <= high) {
		const uint64_t mid = low + ((high - low) / 2);
		const BlockHeaderPtr pHeader = m_pBlockChain->GetBlockHeaderByHeight(mid, EChainType::CANDIDATE);
		if (pHeader == nullptr) {
			if (mid == 0) {
				break;
			}

			high = mid - 1;
			continue;
		}

		if (pHeader->GetOutputMMRSize() <= outputMMRSize && pHeader->GetKernelMMRSize() <= kernelMMRSize) {
			completedHeight = mid;
			if (mid == archiveHeight) {
				break;
			}

			low = mid + 1;
		} else {
			if (mid == 0) {
				break;
			}

			high = mid - 1;
		}
	}

	return completedHeight;
}

void TxHashSetPipe::ClearPIBDRequests()
{
	std::lock_guard<std::mutex> lock(m_pibdMutex);
	if (m_uncommittedPIBDSegments > 0) {
		auto txHashSetWriter = m_pTxHashSetManager->Write();
		auto pTxHashSet = txHashSetWriter->GetTxHashSet();
		if (pTxHashSet != nullptr) {
			pTxHashSet->Commit();
			LOG_DEBUG(StringUtil::Format("Committed {} applied PIBD segment(s) before clearing requests.", m_uncommittedPIBDSegments));
		}
	}
	m_segmentRequests = SegmentRequestTracker();
	m_pibdNextPeerIndex = 0;
	m_uncommittedPIBDSegments = 0;
	m_cachedPIBDCompletedToHeight = 0;
	m_lastPIBDStatusHeightCalcLeaves = std::numeric_limits<uint64_t>::max();
	m_lastPIBDStatusHeightCalcTime = {};
}

void TxHashSetPipe::AbortPIBD()
{
	std::lock_guard<std::mutex> lock(m_pibdMutex);
	if (m_uncommittedPIBDSegments > 0) {
		auto txHashSetWriter = m_pTxHashSetManager->Write();
		auto pTxHashSet = txHashSetWriter->GetTxHashSet();
		if (pTxHashSet != nullptr) {
			pTxHashSet->Commit();
			LOG_DEBUG(StringUtil::Format("Committed {} applied PIBD segment(s) before abort.", m_uncommittedPIBDSegments));
		}
	}
	UpdatePIBDStatus(true, false);
	m_pDesegmenter.reset();
	m_pibdBlockHash.reset();
	m_segmentRequests = SegmentRequestTracker();
	m_lastLoggedPIBDCompletedLeaves = std::numeric_limits<uint64_t>::max();
	m_cachedPIBDCompletedToHeight = 0;
	m_lastPIBDStatusHeightCalcLeaves = std::numeric_limits<uint64_t>::max();
	m_lastPIBDStatusHeightCalcTime = {};
	m_pibdNextPeerIndex = 0;
	m_uncommittedPIBDSegments = 0;
	m_processing = false;
}

bool TxHashSetPipe::IsPIBDComplete() const
{
	std::lock_guard<std::mutex> lock(m_pibdMutex);
	return m_pDesegmenter != nullptr && m_pDesegmenter->IsComplete();
}

bool TxHashSetPipe::IsPIBDValidationRunning() const
{
	return m_pPIBDValidationJob != nullptr && m_pPIBDValidationJob->IsRunning();
}

void TxHashSetPipe::Thread_SendTxHashSet(
	IBlockChain::Ptr pBlockChain,
	std::shared_ptr<Connection> pConnection,
	Hash block_hash)
{
	auto pHeader = pBlockChain->GetBlockHeaderByHash(block_hash);
	if (pHeader == nullptr) {
		return;
	}

	fs::path zipFilePath;

	try {
		zipFilePath = pBlockChain->SnapshotTxHashSet(pHeader);
	}
	catch (std::exception&) {
		return;
	}

	// Hack until I can determine why zips aren't deleted.
	FileRemover remover(zipFilePath);

	std::ifstream file(zipFilePath, std::ios::in | std::ios::ate | std::ios::binary);
	if (!file.is_open()) {
		FileUtil::RemoveFile(zipFilePath);
		return;
	}

	try {
		const uint64_t fileSize = FileUtil::GetFileSize(zipFilePath);
		file.seekg(0);

		pConnection->SendSync(TxHashSetArchiveMessage{ pHeader->GetHash(), pHeader->GetHeight(), fileSize });

		std::vector<uint8_t> buffer(BUFFER_SIZE, 0);

		while (file.read((char*)buffer.data(), BUFFER_SIZE)) {
			std::vector<uint8_t> bytesToSend(
				buffer.cbegin(),
				buffer.cbegin() + file.gcount()
			);
			bool sent = pConnection->GetSocket()->SendSync(bytesToSend, false);
			if (!sent || !Global::IsRunning()) {
				throw std::runtime_error("Transmission ended abruptly");
			}
		}

		pConnection->DisableSends(false);
	}
	catch (std::exception& e) {
		LOG_ERROR_F("Exception thrown while sending TxHashSet: {}", e.what());
	}

	file.close();
	FileUtil::RemoveFile(zipFilePath);
}
