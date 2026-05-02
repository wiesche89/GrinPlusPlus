#pragma once

#include <string>
#include <Core/Models/BlockSums.h>
#include <Core/Models/BlockHeader.h>
#include <Core/Models/OutputLocation.h>
#include <Core/Models/DTOs/OutputRange.h>
#include <Core/Models/TxHashSetRoots.h>
#include <Core/Traits/Batchable.h>
#include <BlockChain/Chain.h>
#include <Crypto/Models/Hash.h>
#include <Core/Models/OutputIdentifier.h>
#include <Core/Models/TransactionKernel.h>
#include <Crypto/Models/RangeProof.h>
#include <PMMR/Common/BitmapSegment.h>
#include <PMMR/Common/Segment.h>
#include <PMMR/Common/SegmentId.h>
#include <PMMR/PIBDParams.h>
#include <optional>

// Forward Declarations
class Config;
class FullBlock;
class Commitment;
class IBlockChain;
class IBlockDB;
class Transaction;
class TransactionBody;
class SyncStatus;
class BitmapAccumulator;

class ITxHashSet : public Traits::IBatchable
{
public:
	virtual ~ITxHashSet() = default;

	//
	// Validates all hashes, signatures, etc in the entire TxHashSet.
	// This is typically only used during initial sync.
	//
	virtual std::unique_ptr<BlockSums> ValidateTxHashSet(
		const BlockHeader& header,
		const IBlockChain& blockChain,
		SyncStatus& syncStatus
	) = 0;

	//
	// Saves the commitments, MMR indices, and block height for all unspent outputs in the block.
	// This is typically only used during initial sync.
	//
	virtual void SaveOutputPositions(
		const Chain::CPtr& pChain,
		std::shared_ptr<IBlockDB> pBlockDB
	) const = 0;

	//
	// Returns true if all inputs in the transaction are valid and unspent. Otherwise, false.
	//
	virtual bool IsValid(
		std::shared_ptr<const IBlockDB> pBlockDB,
		const Transaction& transaction
	) const = 0;

	//
	// Appends all new kernels, outputs, and rangeproofs to the MMRs, and prunes all of the inputs.
	//
	virtual bool ApplyBlock(
		std::shared_ptr<IBlockDB> pBlockDB,
		const FullBlock& block
	) = 0;

	virtual bool ValidateNRDKernelRules(
		std::shared_ptr<const IBlockDB> pBlockDB,
		const FullBlock& block
	) const = 0;

	//
	// Validates that the kernel, output and rangeproof MMR roots match those specified in the given header.
	//
	virtual bool ValidateRoots(
		const BlockHeader& blockHeader
	) const = 0;

	//
	// Returns the roots and sizes of each of the MMRs.
	//
	virtual TxHashSetRoots GetRoots(
		const std::shared_ptr<const IBlockDB>& pBlockDB,
		const TransactionBody& body
	) = 0;



	//
	// Get last n kernel hashes.
	//
	virtual std::vector<Hash> GetLastKernelHashes(
		const uint64_t numberOfKernels
	) const = 0;

	//
	// Get last n output hashes.
	//
	virtual std::vector<Hash> GetLastOutputHashes(
		const uint64_t numberOfOutputs
	) const = 0;

	//
	// Get last n rangeproof hashes.
	//
	virtual std::vector<Hash> GetLastRangeProofHashes(
		const uint64_t numberOfRangeProofs
	) const = 0;

	//
	// Get outputs by leaf/insertion index.
	//
	virtual OutputRange GetOutputsByLeafIndex(
		std::shared_ptr<const IBlockDB> pBlockDB,
		const uint64_t startIndex,
		const uint64_t maxNumOutputs
	) const = 0;

	//
	// Get outputs by leaf/insertion index.
	//
	virtual std::vector<OutputDTO> GetOutputsByMMRIndex(
		std::shared_ptr<const IBlockDB> pBlockDB,
		const uint64_t startIndex,
		const uint64_t lastIndex
	) const = 0;

	//
	// Get Output by OutputLocation
	//
	virtual OutputDTO GetOutput(const OutputLocation& location) const = 0;

	virtual BlockHeaderPtr GetFlushedBlockHeader() const noexcept = 0;
	virtual uint64_t GetOutputMMRSize() const = 0;
	virtual uint64_t GetRangeProofMMRSize() const = 0;
	virtual uint64_t GetKernelMMRSize() const = 0;

	virtual std::optional<BitmapSegment> GetOutputBitmapSegment(
		const SegmentIdentifier& identifier
	) const = 0;

	virtual std::optional<Segment<PIBD::OUTPUT_DATA_SIZE, OutputIdentifier>> GetOutputSegment(
		const SegmentIdentifier& identifier
	) const = 0;

	virtual Hash GetOutputRoot(
		const uint64_t outputMMRSize
	) const = 0;

	virtual Hash GetOutputBitmapRoot(
		const uint64_t numOutputs
	) const = 0;

	virtual std::optional<Segment<PIBD::RANGE_PROOF_DATA_SIZE, RangeProof>> GetRangeProofSegment(
		const SegmentIdentifier& identifier
	) const = 0;

	virtual std::optional<Segment<PIBD::KERNEL_DATA_SIZE, TransactionKernel>> GetKernelSegment(
		const SegmentIdentifier& identifier
	) const = 0;

	virtual bool ApplyOutputSegment(
		const Segment<PIBD::OUTPUT_DATA_SIZE, OutputIdentifier>& segment,
		const uint64_t targetMMRSize
	) = 0;

	virtual bool ApplyRangeProofSegment(
		const Segment<PIBD::RANGE_PROOF_DATA_SIZE, RangeProof>& segment,
		const uint64_t targetMMRSize
	) = 0;

	virtual bool ApplyKernelSegment(
		const Segment<PIBD::KERNEL_DATA_SIZE, TransactionKernel>& segment,
		const uint64_t targetMMRSize
	) = 0;

	virtual void UpdateLeafSets(
		const BitmapAccumulator& outputBitmap,
		const uint64_t numOutputs
	) = 0;



	//
	// Rewinds the kernel, output, and rangeproof MMRs to the given block.
	//
	virtual void Rewind(
		std::shared_ptr<IBlockDB> pBlockDB,
		const BlockHeader& header
	) = 0;

	//
	// Flushes all changes to disk.
	//
	virtual void Commit() = 0;

	//
	// Discards all changes since the last commit.
	//
	virtual void Rollback() noexcept = 0;

	//
	// Removes pruned leaves and hashes from the output and rangeproof PMMRs to reduce disk usage.
	//
	virtual void Compact() = 0;
};

typedef std::shared_ptr<ITxHashSet> ITxHashSetPtr;
typedef std::shared_ptr<const ITxHashSet> ITxHashSetConstPtr;
