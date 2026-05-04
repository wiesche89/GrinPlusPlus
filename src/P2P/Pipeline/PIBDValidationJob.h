#pragma once

#include <BlockChain/BlockChain.h>
#include <Crypto/Models/Hash.h>
#include <P2P/SyncStatus.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

class PIBDValidationJob
{
public:
	enum class EState
	{
		IDLE,
		RUNNING,
		SUCCESS,
		FAILED
	};

	PIBDValidationJob(
		IBlockChain::Ptr pBlockChain,
		SyncStatusPtr pSyncStatus,
		std::function<void(EBlockChainStatus)> onComplete);
	~PIBDValidationJob();

	PIBDValidationJob(const PIBDValidationJob&) = delete;
	PIBDValidationJob& operator=(const PIBDValidationJob&) = delete;

	bool Start(const Hash& blockHash);
	bool IsRunning() const noexcept { return m_state == EState::RUNNING; }
	EState GetState() const noexcept { return m_state; }

private:
	static void Thread_Validate(PIBDValidationJob& job, Hash blockHash);

	IBlockChain::Ptr m_pBlockChain;
	SyncStatusPtr m_pSyncStatus;
	std::function<void(EBlockChainStatus)> m_onComplete;
	std::atomic<EState> m_state;
	std::mutex m_mutex;
	std::thread m_thread;
};
