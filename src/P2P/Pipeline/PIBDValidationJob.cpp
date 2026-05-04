#include "PIBDValidationJob.h"

#include <Common/Logger.h>
#include <Common/Util/ThreadUtil.h>
#include <Core/Global.h>

#include <utility>

PIBDValidationJob::PIBDValidationJob(
	IBlockChain::Ptr pBlockChain,
	SyncStatusPtr pSyncStatus,
	std::function<void(EBlockChainStatus)> onComplete)
	: m_pBlockChain(std::move(pBlockChain)),
	m_pSyncStatus(std::move(pSyncStatus)),
	m_onComplete(std::move(onComplete)),
	m_state(EState::IDLE)
{

}

PIBDValidationJob::~PIBDValidationJob()
{
	ThreadUtil::Join(m_thread);
}

bool PIBDValidationJob::Start(const Hash& blockHash)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_state == EState::RUNNING) {
		return false;
	}

	ThreadUtil::Join(m_thread);
	m_state = EState::RUNNING;
	m_thread = std::thread(Thread_Validate, std::ref(*this), blockHash);
	return true;
}

void PIBDValidationJob::Thread_Validate(PIBDValidationJob& job, Hash blockHash)
{
	LoggerAPI::SetThreadName("PIBD_VALIDATE");

	EBlockChainStatus status = EBlockChainStatus::INVALID;
	try {
		LOG_INFO_F("Starting PIBD validation for {}", blockHash);
		job.m_pSyncStatus->UpdateStatus(ESyncStatus::TXHASHSET_SETUP);
		status = job.m_pBlockChain->ProcessPIBDTransactionHashSet(blockHash, *job.m_pSyncStatus);

		if (!Global::IsRunning()) {
			job.m_state = EState::FAILED;
			LOG_INFO_F("PIBD validation interrupted by shutdown for {}", blockHash);
		}
		else if (status == EBlockChainStatus::SUCCESS) {
			job.m_state = EState::SUCCESS;
			job.m_pSyncStatus->UpdateStatus(ESyncStatus::TXHASHSET_DONE);
			job.m_pSyncStatus->UpdateStatus(ESyncStatus::SYNCING_BLOCKS);
			LOG_INFO_F("PIBD complete for {}", blockHash);
		} else {
			job.m_state = EState::FAILED;
			job.m_pSyncStatus->UpdateStatus(ESyncStatus::TXHASHSET_SYNC_FAILED);
			LOG_WARNING_F("PIBD validation failed for {}", blockHash);
		}
	}
	catch (const std::exception& e) {
		job.m_state = EState::FAILED;
		if (Global::IsRunning()) {
			job.m_pSyncStatus->UpdateStatus(ESyncStatus::TXHASHSET_SYNC_FAILED);
		}
		LOG_ERROR_F("Exception thrown while validating PIBD TxHashSet {}: {}", blockHash, e.what());
	}
	catch (...) {
		job.m_state = EState::FAILED;
		if (Global::IsRunning()) {
			job.m_pSyncStatus->UpdateStatus(ESyncStatus::TXHASHSET_SYNC_FAILED);
		}
		LOG_ERROR_F("Unknown exception thrown while validating PIBD TxHashSet {}", blockHash);
	}

	if (job.m_onComplete) {
		job.m_onComplete(status);
	}
}
