#include <PMMR/TxHashSetManager.h>

#include "TxHashSetImpl.h"
#include "Zip/TxHashSetZip.h"
#include "Zip/Zipper.h"

#include <Common/Util/FileUtil.h>
#include <Common/Util/StringUtil.h>
#include <Core/File/FileRemover.h>
#include <Common/Logger.h>

#include <filesystem.h>
#include <fstream>
#include <thread>

static const char* PIBD_METADATA_FILE = "pibd_state.txt";

static bool IsPIBDResumeForHeader(const fs::path& txHashSetPath, const BlockHeaderPtr& pArchiveHeader)
{
	if (pArchiveHeader == nullptr) {
		return false;
	}

	std::ifstream file(txHashSetPath / PIBD_METADATA_FILE);
	if (!file.is_open()) {
		return false;
	}

	std::string headerHash;
	std::getline(file, headerHash);
	return headerHash == pArchiveHeader->GetHash().ToHex();
}

static void WritePIBDMetadata(const fs::path& txHashSetPath, const BlockHeaderPtr& pArchiveHeader)
{
	std::ofstream file(txHashSetPath / PIBD_METADATA_FILE, std::ios::out | std::ios::trunc);
	if (file.is_open()) {
		file << pArchiveHeader->GetHash().ToHex() << '\n';
		file << pArchiveHeader->GetHeight() << '\n';
	}
}

std::optional<Hash> TxHashSetManager::GetPIBDResumeHeaderHash() const
{
	const fs::path& txHashSetPath = Global::GetConfig().GetTxHashSetPath();
	std::ifstream file(txHashSetPath / PIBD_METADATA_FILE);
	if (!file.is_open()) {
		return std::nullopt;
	}

	std::string headerHash;
	std::getline(file, headerHash);
	if (headerHash.empty()) {
		return std::nullopt;
	}

	try {
		return Hash::FromHex(headerHash);
	} catch (...) {
		LOG_WARNING(StringUtil::Format("Ignoring invalid PIBD resume header hash '{}'.", headerHash));
		return std::nullopt;
	}
}

TxHashSetManager::TxHashSetManager(const Config& config)
	: m_config(config), m_pTxHashSet(nullptr)
{

}

std::shared_ptr<ITxHashSet> TxHashSetManager::Open(const BlockHeaderPtr& pConfirmedTip)
{
	Close();

	std::shared_ptr<KernelMMR> pKernelMMR;
	std::shared_ptr<OutputPMMR> pOutputPMMR;
	std::shared_ptr<RangeProofPMMR> pRangeProofPMMR;

	std::vector<std::thread> threads;
	threads.emplace_back(std::thread([this, &pKernelMMR] {
		pKernelMMR = KernelMMR::Load(Global::GetConfig().GetTxHashSetPath());
	}));
	threads.emplace_back(std::thread([this, &pOutputPMMR] {
		pOutputPMMR = OutputPMMR::Load(Global::GetConfig().GetTxHashSetPath());
	}));
	threads.emplace_back(std::thread([this, &pRangeProofPMMR] {
		pRangeProofPMMR = RangeProofPMMR::Load(Global::GetConfig().GetTxHashSetPath());
	}));

	for (auto& thread : threads) {
		if (thread.joinable()) {
			thread.join();
		}
	}

	m_pTxHashSet = std::shared_ptr<TxHashSet>(new TxHashSet(pKernelMMR, pOutputPMMR, pRangeProofPMMR, pConfirmedTip));

	return m_pTxHashSet;
}

std::shared_ptr<ITxHashSet> TxHashSetManager::OpenEmpty(const BlockHeaderPtr& pArchiveHeader)
{
	Close();

	const fs::path& txHashSetPath = Global::GetConfig().GetTxHashSetPath();
	try {
		fs::remove_all(txHashSetPath / "kernel");
		fs::remove_all(txHashSetPath / "output");
		fs::remove_all(txHashSetPath / "rangeproof");
		fs::create_directories(txHashSetPath / "kernel");
		fs::create_directories(txHashSetPath / "output");
		fs::create_directories(txHashSetPath / "rangeproof");
	} catch (const std::exception& e) {
		LOG_WARNING_F("Failed to prepare TxHashSet path before PIBD: {}", e.what());
	}

	try {
		std::shared_ptr<KernelMMR> pKernelMMR;
		std::shared_ptr<OutputPMMR> pOutputPMMR;
		std::shared_ptr<RangeProofPMMR> pRangeProofPMMR;

		pKernelMMR = KernelMMR::Load(txHashSetPath, false);
		pOutputPMMR = OutputPMMR::Load(txHashSetPath, false);
		pRangeProofPMMR = RangeProofPMMR::Load(txHashSetPath, false);

		m_pTxHashSet = std::shared_ptr<TxHashSet>(new TxHashSet(pKernelMMR, pOutputPMMR, pRangeProofPMMR, pArchiveHeader));
		WritePIBDMetadata(txHashSetPath, pArchiveHeader);
		return m_pTxHashSet;
	} catch (const std::exception& e) {
		LOG_ERROR_F("Failed to open empty TxHashSet for PIBD: {}", e.what());
		m_pTxHashSet.reset();
	}

	return nullptr;
}

std::shared_ptr<ITxHashSet> TxHashSetManager::OpenForPIBD(const BlockHeaderPtr& pArchiveHeader)
{
	Close();

	const fs::path& txHashSetPath = Global::GetConfig().GetTxHashSetPath();
	if (!IsPIBDResumeForHeader(txHashSetPath, pArchiveHeader)) {
		return OpenEmpty(pArchiveHeader);
	}

	try {
		std::shared_ptr<KernelMMR> pKernelMMR = KernelMMR::Load(txHashSetPath, false);
		std::shared_ptr<OutputPMMR> pOutputPMMR = OutputPMMR::Load(txHashSetPath, false);
		std::shared_ptr<RangeProofPMMR> pRangeProofPMMR = RangeProofPMMR::Load(txHashSetPath, false);

		m_pTxHashSet = std::shared_ptr<TxHashSet>(new TxHashSet(pKernelMMR, pOutputPMMR, pRangeProofPMMR, pArchiveHeader));

		if (m_pTxHashSet->GetOutputMMRSize() <= pArchiveHeader->GetOutputMMRSize()
			&& m_pTxHashSet->GetRangeProofMMRSize() <= pArchiveHeader->GetOutputMMRSize()
			&& m_pTxHashSet->GetKernelMMRSize() <= pArchiveHeader->GetKernelMMRSize()) {
			LOG_INFO(StringUtil::Format(
				"Resuming PIBD TxHashSet for header {} with output_mmr_size={}, rangeproof_mmr_size={}, kernel_mmr_size={}.",
				pArchiveHeader->GetHash(),
				m_pTxHashSet->GetOutputMMRSize(),
				m_pTxHashSet->GetRangeProofMMRSize(),
				m_pTxHashSet->GetKernelMMRSize()));
			return m_pTxHashSet;
		}

		LOG_WARNING_F("Existing PIBD TxHashSet for header {} is ahead of the archive header, starting over.", *pArchiveHeader);
	} catch (const std::exception& e) {
		LOG_WARNING_F("Failed to resume PIBD TxHashSet for header {}: {}", *pArchiveHeader, e.what());
	}

	m_pTxHashSet.reset();
	return OpenEmpty(pArchiveHeader);
}

std::shared_ptr<ITxHashSet> TxHashSetManager::LoadFromZip(const Config& config, const fs::path& zipFilePath, BlockHeaderPtr pHeader)
{
	FileRemover fileRemover(zipFilePath);

	const fs::path& txHashSetPath = config.GetTxHashSetPath();
	const TxHashSetZip zip(config);

	try
	{
		if (zip.Extract(zipFilePath, *pHeader))
		{
			LOG_INFO_F("{} extracted successfully", zipFilePath);
			FileUtil::RemoveFile(zipFilePath);

			// Rewind Kernel MMR
			std::shared_ptr<KernelMMR> pKernelMMR = KernelMMR::Load(txHashSetPath);
			pKernelMMR->Rewind(pHeader->GetNumKernels());
			pKernelMMR->Commit();

			// Rewind Output MMR
			std::shared_ptr<OutputPMMR> pOutputPMMR = OutputPMMR::Load(txHashSetPath);
			pOutputPMMR->Rewind(pHeader->GetNumOutputs(), {});
			pOutputPMMR->Commit();

			// Rewind RangeProof MMR
			std::shared_ptr<RangeProofPMMR> pRangeProofPMMR = RangeProofPMMR::Load(txHashSetPath);
			pRangeProofPMMR->Rewind(pHeader->GetNumOutputs(), {});
			pRangeProofPMMR->Commit();

			return std::shared_ptr<TxHashSet>(new TxHashSet(pKernelMMR, pOutputPMMR, pRangeProofPMMR, pHeader));
		}
	}
	catch (std::exception& e)
	{
		LOG_ERROR_F("Failed to load: {}", e.what());
	}

	return nullptr;
}

fs::path TxHashSetManager::SaveSnapshot(std::shared_ptr<IBlockDB> pBlockDB, BlockHeaderPtr pHeader) const
{
	if (m_pTxHashSet == nullptr)
	{
		throw std::exception();
	}

	fs::path snapshotDir = fs::temp_directory_path() / "Snapshots" / pHeader->ShortHash();
	FileRemover snapshotRemover(snapshotDir);

	const std::string fileName = StringUtil::Format("TxHashSet.{}.zip", pHeader->ShortHash());
	fs::path zipFilePath = fs::temp_directory_path() / "Snapshots" / fileName;

	try
	{
		BlockHeaderPtr pFlushedHeader = nullptr;

		{
			// Copy to Snapshots/Hash // TODO: If already exists, just use that.
			FileUtil::CopyDirectory(m_config.GetTxHashSetPath(), snapshotDir);

			pFlushedHeader = m_pTxHashSet->GetFlushedBlockHeader();
		}

		{
			// Load Snapshot TxHashSet
			auto pKernelMMR = KernelMMR::Load(snapshotDir);
			auto pOutputPMMR = OutputPMMR::Load(snapshotDir);
			auto pRangeProofPMMR = RangeProofPMMR::Load(snapshotDir);
			TxHashSet snapshotTxHashSet(pKernelMMR, pOutputPMMR, pRangeProofPMMR, pFlushedHeader);

			// Rewind Snapshot TxHashSet
			snapshotTxHashSet.Rewind(pBlockDB, *pHeader);

			// Flush Snapshot TxHashSet
			snapshotTxHashSet.Commit();
		}

		// Rename pmmr_leaf files
		const std::string newFileName = StringUtil::Format("pmmr_leaf.bin.{}", pHeader->ShortHash());
		FileUtil::RenameFile(
			snapshotDir / "output" / "pmmr_leaf.bin",
			snapshotDir / "output" / newFileName
		);

		FileUtil::RenameFile(
			snapshotDir / "rangeproof" / "pmmr_leaf.bin",
			snapshotDir / "rangeproof" / newFileName
		);

		// Create Zip
		const std::vector<fs::path> pathsToZip = {
			snapshotDir / "kernel",
			snapshotDir / "output",
			snapshotDir / "rangeproof"
		};
		Zipper::CreateZipFile(zipFilePath, pathsToZip);
	}
	catch (...)
	{
		FileUtil::RemoveFile(zipFilePath);
		throw;
	}

	return zipFilePath;
}
