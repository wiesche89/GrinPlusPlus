#pragma once

#include "Common/MMR.h"
#include "Common/HashFile.h"

#include <Core/File/AppendOnlyFile.h>
#include <Core/File/DataFile.h>
#include <Core/Models/TransactionKernel.h>
#include <Core/Models/FullBlock.h>
#include <Core/Traits/Lockable.h>
#include <Crypto/Models/Hash.h>
#include <PMMR/Common/Segment.h>
#include <PMMR/Common/LeafIndex.h>
#include <filesystem.h>
#include <cstdint>

#define KERNEL_SIZE 114
#define KERNEL_OFFSET_SIZE 8

class KernelMMR : public MMR
{
public:
	KernelMMR(
		std::shared_ptr<HashFile> pHashFile,
		std::shared_ptr<DataFile<KERNEL_SIZE>> pDataFile
	);
	KernelMMR(
		std::shared_ptr<HashFile> pHashFile,
		std::shared_ptr<AppendOnlyFile> pDataFile,
		std::shared_ptr<DataFile<KERNEL_OFFSET_SIZE>> pOffsetFile
	);
	virtual ~KernelMMR() = default;

	static std::shared_ptr<KernelMMR> Load(const fs::path & txHashSetPath, const bool includeGenesis = true);

	std::unique_ptr<TransactionKernel> GetKernelAt(const LeafIndex& leaf_idx) const;
	bool Rewind(const uint64_t size);

	Hash Root(const uint64_t size) const final;
	uint64_t GetSize() const final { return m_pHashFile->GetSize(); }
	uint64_t GetNumKernels() const;

	std::unique_ptr<Hash> GetHashAt(const Index& mmrIndex) const final
	{
		return std::make_unique<Hash>(m_pHashFile->GetDataAt(mmrIndex.GetPosition()));
	}

	std::vector<Hash> GetLastLeafHashes(const uint64_t numHashes) const final;

	void Commit() final;
	void Rollback() noexcept final;

	void ApplyKernel(const TransactionKernel& kernel);
	bool ApplySegment(const Segment<KERNEL_SIZE, TransactionKernel>& segment);

private:
	enum class KernelStorageFormat
	{
		Fixed,
		Variable
	};

	uint64_t GetKernelOffset(const uint64_t leafIndex) const;
	uint64_t GetKernelRecordSize(const uint64_t leafIndex) const;
	std::unique_ptr<TransactionKernel> DeserializeKernel(
		const std::vector<uint8_t>& data,
		const EProtocolVersion protocolVersion
	) const;
	void BuildKernelOffsetsFromRawFile();

	mutable std::shared_ptr<HashFile> m_pHashFile;
	KernelStorageFormat m_storageFormat;
	mutable std::shared_ptr<DataFile<KERNEL_SIZE>> m_pFixedDataFile;
	mutable std::shared_ptr<AppendOnlyFile> m_pVariableDataFile;
	mutable std::shared_ptr<DataFile<KERNEL_OFFSET_SIZE>> m_pOffsetFile;
};
