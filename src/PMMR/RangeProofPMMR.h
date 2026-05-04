#pragma once

#include "Common/PruneableMMR.h"

#include <Core/Global.h>
#include <Crypto/Models/RangeProof.h>
#include <filesystem.h>

#define RANGE_PROOF_SIZE 683

class RangeProofPMMR : public PruneableMMR<RANGE_PROOF_SIZE, RangeProof>
{
public:
	static std::shared_ptr<RangeProofPMMR> Load(const fs::path& txHashSetPath, const bool includeGenesis = true)
	{
		std::shared_ptr<HashFile> pHashFile = HashFile::Load(txHashSetPath / "rangeproof" / "pmmr_hash.bin");

		if (!FileUtil::Exists(txHashSetPath / "rangeproof" / "pmmr_leafset.bin") && FileUtil::Exists(txHashSetPath / "rangeproof" / "pmmr_leaf.bin"))
		{
			Roaring outputBitmap;
			std::vector<unsigned char> outputBytes;
			if (FileUtil::ReadFile(txHashSetPath / "rangeproof" / "pmmr_leaf.bin", outputBytes))
			{
				outputBitmap = Roaring::readSafe((const char*)outputBytes.data(), outputBytes.size());
			}
			BitmapFile::Create(txHashSetPath / "rangeproof" / "pmmr_leafset.bin", outputBitmap);
		}

		std::shared_ptr<LeafSet> pLeafSet = LeafSet::Load(txHashSetPath / "rangeproof" / "pmmr_leafset.bin");
		std::shared_ptr<PruneList> pPruneList = PruneList::Load(txHashSetPath / "rangeproof" / "pmmr_prun.bin");
		std::shared_ptr<DataFile<RANGE_PROOF_SIZE>> pDataFile = DataFile<RANGE_PROOF_SIZE>::Load(txHashSetPath / "rangeproof" / "pmmr_data.bin");

		auto pPMMR = std::shared_ptr<RangeProofPMMR>(new RangeProofPMMR(pHashFile, pLeafSet, pPruneList, pDataFile));
		if (includeGenesis && pHashFile->GetSize() == 0)
		{
			pPMMR->Append(Global::GetGenesisBlock().GetOutputs().front().GetRangeProof());
		}
		else if (includeGenesis && pPMMR->GetSize() >= 1)
		{
			const auto pGenesisRangeProof = pPMMR->GetAt(LeafIndex::At(0));
			const Hash genesisRoot = Global::GetGenesisHeader()->GetRangeProofRoot();
			if (pGenesisRangeProof == nullptr || pPMMR->Root(1) != genesisRoot)
			{
				if (pPMMR->GetSize() == 1)
				{
					LOG_WARNING("RangeProof PMMR genesis entry is inconsistent; rebuilding genesis rangeproof.");
					pPMMR->ResetToEmpty();
					pPMMR->Append(Global::GetGenesisBlock().GetOutputs().front().GetRangeProof());
				}
				else
				{
					LOG_WARNING_F("RangeProof PMMR genesis entry is inconsistent in non-empty PMMR with size {}.", pPMMR->GetSize());
				}
			}
		}

		return pPMMR;
	}

	virtual ~RangeProofPMMR() = default;

private:
	RangeProofPMMR(
		std::shared_ptr<HashFile> pHashFile,
		std::shared_ptr<LeafSet> pLeafSet,
		std::shared_ptr<PruneList> pPruneList,
		std::shared_ptr<DataFile<RANGE_PROOF_SIZE>> pDataFile)
		: PruneableMMR<RANGE_PROOF_SIZE, RangeProof>(pHashFile, pLeafSet, pPruneList, pDataFile)
	{

	}
};
