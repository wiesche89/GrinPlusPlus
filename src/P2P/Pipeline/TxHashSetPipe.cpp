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
#include <BlockChain/BlockChain.h>
#include <PMMR/TxHashSet.h>
#include <PMMR/PIBDParams.h>

#include <filesystem.h>
#include <algorithm>
#include <fstream>

static const int BUFFER_SIZE = 128 * 1024;
static const unsigned long TXHASHSET_RECEIVE_TIMEOUT_MS = 60 * 1000;

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

TxHashSetPipe::~TxHashSetPipe()
{
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
	const auto pHeader = m_pBlockChain->GetBlockHeaderByHash(blockHash);
	const auto txHashSetReader = m_pTxHashSetManager->Read();
	const auto pTxHashSet = txHashSetReader->GetTxHashSet();
	if (pHeader == nullptr || pTxHashSet == nullptr || pTxHashSet->GetFlushedBlockHeader()->GetHash() != blockHash) {
		return;
	}

	std::optional<BitmapSegment> segment = pTxHashSet->GetOutputBitmapSegment(identifier);
	if (segment.has_value()) {
		pConnection->SendAsync(OutputBitmapSegmentMessage(blockHash, std::move(segment.value()), pHeader->GetOutputRoot()));
	}
}

void TxHashSetPipe::SendOutputSegment(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
{
	const auto pHeader = m_pBlockChain->GetBlockHeaderByHash(blockHash);
	const auto txHashSetReader = m_pTxHashSetManager->Read();
	const auto pTxHashSet = txHashSetReader->GetTxHashSet();
	if (pHeader == nullptr || pTxHashSet == nullptr || pTxHashSet->GetFlushedBlockHeader()->GetHash() != blockHash) {
		return;
	}

	std::optional<Segment<PIBD::OUTPUT_DATA_SIZE, OutputIdentifier>> segment = pTxHashSet->GetOutputSegment(identifier);
	if (segment.has_value()) {
		pConnection->SendAsync(OutputSegmentMessage(blockHash, std::move(segment.value()), pHeader->GetOutputRoot()));
	}
}

void TxHashSetPipe::SendRangeProofSegment(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
{
	const auto txHashSetReader = m_pTxHashSetManager->Read();
	const auto pTxHashSet = txHashSetReader->GetTxHashSet();
	if (pTxHashSet == nullptr || pTxHashSet->GetFlushedBlockHeader()->GetHash() != blockHash) {
		return;
	}

	std::optional<Segment<PIBD::RANGE_PROOF_DATA_SIZE, RangeProof>> segment = pTxHashSet->GetRangeProofSegment(identifier);
	if (segment.has_value()) {
		pConnection->SendAsync(RangeProofSegmentMessage(blockHash, std::move(segment.value())));
	}
}

void TxHashSetPipe::SendKernelSegment(const std::shared_ptr<Connection>& pConnection, const Hash& blockHash, const SegmentIdentifier& identifier)
{
	const auto txHashSetReader = m_pTxHashSetManager->Read();
	const auto pTxHashSet = txHashSetReader->GetTxHashSet();
	if (pTxHashSet == nullptr || pTxHashSet->GetFlushedBlockHeader()->GetHash() != blockHash) {
		return;
	}

	std::optional<Segment<PIBD::KERNEL_DATA_SIZE, TransactionKernel>> segment = pTxHashSet->GetKernelSegment(identifier);
	if (segment.has_value()) {
		pConnection->SendAsync(KernelSegmentMessage(blockHash, std::move(segment.value())));
	}
}

void TxHashSetPipe::ReceiveOutputBitmapSegment(const std::shared_ptr<Connection>& pConnection, const OutputBitmapSegmentMessage& message)
{
	LOG_DEBUG(StringUtil::Format("Received output bitmap segment {}:{} for {} from {}.",
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
		LOG_DEBUG(StringUtil::Format("Ignoring already applied output bitmap segment {}:{} from {}.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	if (!IsExpectedPIBDSegment(m_segmentRequests, typeId, pConnection)) {
		LOG_DEBUG(StringUtil::Format("Ignoring unexpected output bitmap segment {}:{} from {}.",
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
	LOG_DEBUG(StringUtil::Format("Received output segment {}:{} for {} from {}.",
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
		LOG_DEBUG(StringUtil::Format("Ignoring already applied output segment {}:{} from {}.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	if (!IsExpectedPIBDSegment(m_segmentRequests, typeId, pConnection)) {
		LOG_DEBUG(StringUtil::Format("Ignoring unexpected output segment {}:{} from {}.",
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
	LOG_DEBUG(StringUtil::Format("Received rangeproof segment {}:{} for {} from {}.",
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
		LOG_DEBUG(StringUtil::Format("Ignoring already applied rangeproof segment {}:{} from {}.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	if (!IsExpectedPIBDSegment(m_segmentRequests, typeId, pConnection)) {
		LOG_DEBUG(StringUtil::Format("Ignoring unexpected rangeproof segment {}:{} from {}.",
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
	LOG_DEBUG(StringUtil::Format("Received kernel segment {}:{} for {} from {}.",
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
		LOG_DEBUG(StringUtil::Format("Ignoring already applied kernel segment {}:{} from {}.",
			message.GetSegment().GetIdentifier().GetHeight(),
			message.GetSegment().GetIdentifier().GetIndex(),
			pConnection));
		return;
	}

	if (!IsExpectedPIBDSegment(m_segmentRequests, typeId, pConnection)) {
		LOG_DEBUG(StringUtil::Format("Ignoring unexpected kernel segment {}:{} from {}.",
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

		if (!m_pDesegmenter->ApplyReadySegments(
			[pTxHashSet](const TxHashSetDesegmenter::OutputPMMRSegment& segment, const uint64_t targetSize) {
				const bool applied = pTxHashSet->ApplyOutputSegment(segment, targetSize);
				if (!applied) {
					LOG_WARNING(StringUtil::Format("Failed to apply PIBD output segment {}:{}.",
						segment.GetIdentifier().GetHeight(),
						segment.GetIdentifier().GetIndex()));
				}
				return applied;
			},
			[pTxHashSet](const TxHashSetDesegmenter::RangeProofPMMRSegment& segment, const uint64_t targetSize) {
				const bool applied = pTxHashSet->ApplyRangeProofSegment(segment, targetSize);
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
			})) {
			UpdatePIBDStatus(false, true);
			return false;
		}
		pTxHashSet->Commit();
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
		}

		m_pSyncStatus->UpdateStatus(ESyncStatus::TXHASHSET_SETUP);
		lock.unlock();
		const EBlockChainStatus status = m_pBlockChain->ProcessPIBDTransactionHashSet(pibdBlockHash, *m_pSyncStatus);
		lock.lock();
		if (status == EBlockChainStatus::SUCCESS) {
			m_processing = false;
			m_pSyncStatus->UpdateStatus(ESyncStatus::TXHASHSET_DONE);
			m_pSyncStatus->UpdateStatus(ESyncStatus::SYNCING_BLOCKS);
			LOG_INFO_F("PIBD complete for {}", pibdBlockHash);
			return true;
		}

		m_processing = false;
		UpdatePIBDStatus(false, true);
		return false;
	}

	const std::vector<SegmentRequestTracker::PendingRequest> timedOutRequests = m_segmentRequests.GetTimedOutRequests();
	if (!timedOutRequests.empty()) {
		LOG_DEBUG(StringUtil::Format("Retrying {} timed-out PIBD segment requests.", timedOutRequests.size()));
		m_segmentRequests.RemoveTimedOutRequests();
	}

	const size_t pendingCount = m_segmentRequests.GetPendingRequests().size();
	if (pendingCount >= PIBD::SEGMENT_REQUEST_COUNT) {
		UpdatePIBDStatus();
		return true;
	}

	const size_t requestBudget = PIBD::SEGMENT_REQUEST_COUNT - pendingCount;
	const size_t desiredCandidateCount = PIBD::SEGMENT_REQUEST_COUNT + pendingCount;
	const std::vector<SegmentTypeIdentifier> desired = m_pDesegmenter->NextDesiredSegments(desiredCandidateCount);
	std::vector<SegmentTypeIdentifier> requestsToSend;
	requestsToSend.reserve(requestBudget);
	for (const SegmentTypeIdentifier& typeId : desired) {
		if (!m_segmentRequests.IsPending(typeId)) {
			requestsToSend.push_back(typeId);
			if (requestsToSend.size() >= requestBudget) {
				break;
			}
		}
	}

	if (!requestsToSend.empty()) {
		LOG_DEBUG(StringUtil::Format(
			"Requesting {} PIBD segment(s) across {} peer(s) (pending={}, budget={}).",
			requestsToSend.size(),
			peers.size(),
			pendingCount,
			requestBudget));
	}

	// Distribute requests round-robin across all available PIBD peers
	size_t peerIdx = 0;
	for (const SegmentTypeIdentifier& typeId : requestsToSend) {
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
		for (size_t attempt = 0; attempt < peers.size(); ++attempt) {
			const PeerConstPtr& pCurrentPeer = peers[(peerIdx + attempt) % peers.size()];
			if (pConnectionManager->SendMessageToPeer(*pMessage, pCurrentPeer)) {
				m_segmentRequests.AddOrRefresh(typeId, pCurrentPeer->GetIPAddress().Format());
				LOG_DEBUG(StringUtil::Format("Requested PIBD {} segment {}:{} from {}.",
					GetPIBDSegmentTypeName(typeId.GetSegmentType()),
					typeId.GetIdentifier().GetHeight(),
					typeId.GetIdentifier().GetIndex(),
					pCurrentPeer));
				peerIdx = (peerIdx + attempt + 1) % peers.size();
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
	const uint64_t completedToHeight = GetPIBDCompletedToHeight();
	const uint64_t requiredHeight = m_pDesegmenter->GetRequiredHeight();

	m_pSyncStatus->UpdatePIBDStatus(
		aborted,
		errored,
		completedLeaves,
		leavesRequired,
		completedToHeight,
		requiredHeight);

	if (aborted || errored || completedLeaves != m_lastLoggedPIBDCompletedLeaves) {
		LOG_DEBUG(StringUtil::Format(
			"PIBD progress: {}/{} txhashset leaves, complete_height {}/{} (aborted={}, errored={}).",
			completedLeaves,
			leavesRequired,
			completedToHeight,
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
	m_segmentRequests = SegmentRequestTracker();
}

void TxHashSetPipe::AbortPIBD()
{
	std::lock_guard<std::mutex> lock(m_pibdMutex);
	UpdatePIBDStatus(true, false);
	m_pDesegmenter.reset();
	m_pibdBlockHash.reset();
	m_segmentRequests = SegmentRequestTracker();
	m_lastLoggedPIBDCompletedLeaves = std::numeric_limits<uint64_t>::max();
	m_processing = false;
}

bool TxHashSetPipe::IsPIBDComplete() const
{
	std::lock_guard<std::mutex> lock(m_pibdMutex);
	return m_pDesegmenter != nullptr && m_pDesegmenter->IsComplete();
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
