#include "KernelMMR.h"
#include "Common/MMRUtil.h"
#include "Common/MMRHashUtil.h"

#include <Core/Global.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Traits/Lockable.h>
#include <Common/Util/FileUtil.h>
#include <Common/Util/StringUtil.h>
#include <Common/Logger.h>

namespace
{
	static std::vector<uint8_t> SerializeOffset(const uint64_t offset)
	{
		Serializer serializer;
		serializer.Append<uint64_t>(offset);
		return serializer.GetBytes();
	}
}

KernelMMR::KernelMMR(
	std::shared_ptr<HashFile> pHashFile,
	std::shared_ptr<DataFile<KERNEL_SIZE>> pDataFile)
	: m_pHashFile(std::move(pHashFile)),
	m_storageFormat(KernelStorageFormat::Fixed),
	m_pFixedDataFile(std::move(pDataFile))
{

}

KernelMMR::KernelMMR(
	std::shared_ptr<HashFile> pHashFile,
	std::shared_ptr<AppendOnlyFile> pDataFile,
	std::shared_ptr<DataFile<KERNEL_OFFSET_SIZE>> pOffsetFile)
	: m_pHashFile(std::move(pHashFile)),
	m_storageFormat(KernelStorageFormat::Variable),
	m_pVariableDataFile(std::move(pDataFile)),
	m_pOffsetFile(std::move(pOffsetFile))
{

}

std::shared_ptr<KernelMMR> KernelMMR::Load(const fs::path& txHashSetPath, const bool includeGenesis)
{
	const fs::path kernelDir = txHashSetPath / "kernel";
	const fs::path hashPath = kernelDir / "pmmr_hash.bin";
	const fs::path rawPath = kernelDir / "pmmr_data.bin";
	const fs::path offsetPath = kernelDir / "pmmr_offsets.bin";

	auto pHashFile = HashFile::Load(hashPath);

	if (pHashFile->GetSize() == 0) {
		auto pDataFile = DataFile<KERNEL_SIZE>::Load(rawPath);
		auto pKernelMMR = std::make_shared<KernelMMR>(pHashFile, pDataFile);
		if (includeGenesis) {
			pKernelMMR->ApplyKernel(Global::GetGenesisBlock().GetKernels().front());
		}
		return pKernelMMR;
	}

	const bool hasOffsetFile = FileUtil::Exists(offsetPath);
	const uint64_t rawFileSize = FileUtil::GetFileSize(rawPath);
	const bool useVariableFormat = hasOffsetFile || (rawFileSize % KERNEL_SIZE != 0);

	LOG_DEBUG_F("KernelMMR::Load: hash_size={}, raw_size={}, has_offset_file={}, format={}",
		pHashFile->GetSize(), rawFileSize, hasOffsetFile, (useVariableFormat ? "Variable" : "Fixed"));

	if (!useVariableFormat) {
		auto pDataFile = DataFile<KERNEL_SIZE>::Load(rawPath);
		auto pKernelMMR = std::make_shared<KernelMMR>(pHashFile, pDataFile);
		if (includeGenesis && pKernelMMR->GetSize() >= 1) {
			const auto pGenesisKernel = pKernelMMR->GetKernelAt(LeafIndex::At(0));
			const Hash genesisRoot = Global::GetGenesisHeader()->GetKernelRoot();
			if (pGenesisKernel == nullptr || pKernelMMR->Root(1) != genesisRoot) {
				if (pKernelMMR->GetSize() == 1) {
					LOG_WARNING("Kernel PMMR genesis entry is inconsistent; rebuilding genesis kernel.");
					pKernelMMR->Rewind(0);
					pKernelMMR->ApplyKernel(Global::GetGenesisBlock().GetKernels().front());
				} else {
					LOG_WARNING_F("Kernel PMMR genesis entry is inconsistent in non-empty PMMR with size {}.", pKernelMMR->GetSize());
				}
			}
		}
		return pKernelMMR;
	}

	auto pRawFile = std::make_shared<AppendOnlyFile>(rawPath);
	pRawFile->Load();

	auto pOffsetFile = DataFile<KERNEL_OFFSET_SIZE>::Load(offsetPath);
	auto pKernelMMR = std::make_shared<KernelMMR>(pHashFile, pRawFile, pOffsetFile);
	if (!hasOffsetFile) {
		pKernelMMR->BuildKernelOffsetsFromRawFile();
	}
	if (includeGenesis && pKernelMMR->GetSize() >= 1) {
		const auto pGenesisKernel = pKernelMMR->GetKernelAt(LeafIndex::At(0));
		const Hash genesisRoot = Global::GetGenesisHeader()->GetKernelRoot();
		if (pGenesisKernel == nullptr || pKernelMMR->Root(1) != genesisRoot) {
			if (pKernelMMR->GetSize() == 1) {
				LOG_WARNING("Kernel PMMR genesis entry is inconsistent; rebuilding genesis kernel.");
				pKernelMMR->Rewind(0);
				pKernelMMR->ApplyKernel(Global::GetGenesisBlock().GetKernels().front());
			} else {
				LOG_WARNING_F("Kernel PMMR genesis entry is inconsistent in non-empty PMMR with size {}.", pKernelMMR->GetSize());
			}
		}
	}
	return pKernelMMR;
}

Hash KernelMMR::Root(const uint64_t size) const
{
	return MMRHashUtil::Root(m_pHashFile, size, nullptr);
}

uint64_t KernelMMR::GetNumKernels() const
{
	if (m_storageFormat == KernelStorageFormat::Fixed) {
		return m_pFixedDataFile->GetSize();
	}

	return m_pOffsetFile->GetSize();
}

uint64_t KernelMMR::GetKernelOffset(const uint64_t leafIndex) const
{
	if (m_storageFormat == KernelStorageFormat::Fixed) {
		return leafIndex * KERNEL_SIZE;
	}

	if (leafIndex >= m_pOffsetFile->GetSize()) {
		return m_pVariableDataFile->GetSize();
	}

	const std::vector<uint8_t> data = m_pOffsetFile->GetDataAt(leafIndex);
	ByteBuffer buffer(data);
	return buffer.ReadU64();
}

uint64_t KernelMMR::GetKernelRecordSize(const uint64_t leafIndex) const
{
	const uint64_t startOffset = GetKernelOffset(leafIndex);
	const uint64_t endOffset = (leafIndex + 1 < GetNumKernels())
		? GetKernelOffset(leafIndex + 1)
		: (m_storageFormat == KernelStorageFormat::Fixed ? (GetNumKernels() * KERNEL_SIZE) : m_pVariableDataFile->GetSize());

	return endOffset - startOffset;
}

std::unique_ptr<TransactionKernel> KernelMMR::DeserializeKernel(
	const std::vector<uint8_t>& data,
	const EProtocolVersion protocolVersion) const
{
	ByteBuffer byteBuffer(data, protocolVersion);
	return std::make_unique<TransactionKernel>(TransactionKernel::Deserialize(byteBuffer));
}

std::unique_ptr<TransactionKernel> KernelMMR::GetKernelAt(const LeafIndex& leaf_idx) const
{
	if (leaf_idx.Get() >= GetNumKernels()) {
		return nullptr;
	}

	const uint64_t recordSize = GetKernelRecordSize(leaf_idx.Get());
	if (recordSize == 0) {
		return nullptr;
	}

	std::vector<uint8_t> data;
	if (m_storageFormat == KernelStorageFormat::Fixed) {
		data = m_pFixedDataFile->GetDataAt(leaf_idx.Get());
		return DeserializeKernel(data, EProtocolVersion::V1);
	}

	if (!m_pVariableDataFile->Read(GetKernelOffset(leaf_idx.Get()), recordSize, data)) {
		return nullptr;
	}

	return DeserializeKernel(data, EProtocolVersion::V2);
}

std::vector<Hash> KernelMMR::GetLastLeafHashes(const uint64_t numHashes) const
{
	return MMRHashUtil::GetLastLeafHashes(m_pHashFile, nullptr, nullptr, numHashes);
}

bool KernelMMR::Rewind(const uint64_t num_kernels)
{
	if (m_storageFormat == KernelStorageFormat::Fixed) {
		m_pHashFile->Rewind(LeafIndex::At(num_kernels).GetPosition());
		m_pFixedDataFile->Rewind(num_kernels);
		return true;
	}

	const uint64_t rawSize = (num_kernels < GetNumKernels())
		? GetKernelOffset(num_kernels)
		: m_pVariableDataFile->GetSize();

	m_pHashFile->Rewind(LeafIndex::At(num_kernels).GetPosition());
	m_pVariableDataFile->Rewind(rawSize);
	m_pOffsetFile->Rewind(num_kernels);
	return true;
}

void KernelMMR::Commit()
{
	m_pHashFile->Commit();
	if (m_storageFormat == KernelStorageFormat::Fixed) {
		m_pFixedDataFile->Commit();
	} else {
		m_pVariableDataFile->Commit();
		m_pOffsetFile->Commit();
	}
}

void KernelMMR::Rollback() noexcept
{
	m_pHashFile->Rollback();
	if (m_storageFormat == KernelStorageFormat::Fixed) {
		m_pFixedDataFile->Rollback();
	} else {
		m_pVariableDataFile->Rollback();
		m_pOffsetFile->Rollback();
	}
}

void KernelMMR::ApplyKernel(const TransactionKernel& kernel)
{
	if (m_storageFormat == KernelStorageFormat::Fixed) {
		std::vector<uint8_t> serialized = kernel.Serialized(EProtocolVersion::V1);
		m_pFixedDataFile->AddData(serialized);
		MMRHashUtil::AddHashes(m_pHashFile, serialized, nullptr);
		return;
	}

	const uint64_t offset = m_pVariableDataFile->GetSize();
	m_pOffsetFile->AddData(SerializeOffset(offset));

	std::vector<uint8_t> storedData = kernel.Serialized(EProtocolVersion::V2);
	m_pVariableDataFile->Append(storedData);

	// Hash must be computed from V1 serialization for protocol compatibility with all nodes
	std::vector<uint8_t> hashData = kernel.Serialized(EProtocolVersion::V1);
	MMRHashUtil::AddHashes(m_pHashFile, hashData, nullptr);
}

bool KernelMMR::ApplySegment(const Segment<KERNEL_SIZE, TransactionKernel>& segment)
{
	if (!segment.GetHashPositions().empty()) {
		LOG_WARNING_F("Kernel PIBD segment {}:{} contains hash-only positions",
			segment.GetIdentifier().GetHeight(),
			segment.GetIdentifier().GetIndex());
		return false;
	}

	if (segment.GetLeafPositions().size() != segment.GetLeaves().size()) {
		return false;
	}

	uint64_t nextKernel = GetNumKernels();
	for (size_t i = 0; i < segment.GetLeaves().size(); ++i) {
		const uint64_t leafPosition = segment.GetLeafPositions()[i];
		const uint64_t expectedPosition = LeafIndex::At(nextKernel).GetPosition();
		if (leafPosition < expectedPosition) {
			continue;
		}

		if (leafPosition != expectedPosition) {
			LOG_WARNING_F("Kernel PIBD segment {}:{} is not contiguous at position {}. Expected {}.",
				segment.GetIdentifier().GetHeight(),
				segment.GetIdentifier().GetIndex(),
				leafPosition,
				expectedPosition);
			return false;
		}

		ApplyKernel(segment.GetLeaves()[i]);
		++nextKernel;
	}

	return true;
}

void KernelMMR::BuildKernelOffsetsFromRawFile()
{
	if (m_storageFormat != KernelStorageFormat::Variable) {
		return;
	}

	const uint64_t rawSize = m_pVariableDataFile->GetSize();
	if (rawSize == 0) {
		return;
	}

	LOG_INFO_F("Building kernel offsets from raw file ({} bytes)", rawSize);

	std::vector<uint8_t> rawBytes;
	if (!FileUtil::ReadFile(m_pVariableDataFile->GetPath(), rawBytes)) {
		throw FILE_EXCEPTION_F("Failed to read raw kernel data from {}", m_pVariableDataFile->GetPath());
	}

	m_pOffsetFile->Rewind(0);

	ByteBuffer byteBuffer(std::move(rawBytes), EProtocolVersion::V2);

	uint64_t offset = 0;
	uint64_t kernelIndex = 0;
	while (byteBuffer.GetRemainingSize() > 0) {
		const uint64_t before = byteBuffer.GetRemainingSize();
		TransactionKernel::Deserialize(byteBuffer);
		const uint64_t consumed = before - byteBuffer.GetRemainingSize();
		if (consumed == 0 || consumed > before) {
			throw FILE_EXCEPTION_F("Failed to parse kernel record at offset {}", offset);
		}

		m_pOffsetFile->AddData(SerializeOffset(offset));
		offset += consumed;
		++kernelIndex;

		if (kernelIndex % 100000 == 0) {
			LOG_DEBUG_F("Built kernel offsets for {} records (offset={})", kernelIndex, offset);
		}
	}

	m_pOffsetFile->Commit();
	LOG_INFO_F("Built kernel offsets for {} records", kernelIndex);
}
