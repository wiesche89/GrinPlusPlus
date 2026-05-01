#pragma once

#include <PMMR/Common/Index.h>
#include <PMMR/Common/LeafIndex.h>
#include <cstdint>
#include <utility>
#include <vector>

class MMRUtil
{
public:
	static std::vector<uint64_t> GetPeakIndices(const uint64_t size);
	static uint64_t CountLeaves(const uint64_t size);
	static bool IsLeftSibling(const uint64_t position);
	static std::vector<std::pair<uint64_t, uint64_t>> FamilyBranch(const uint64_t position, const uint64_t size);
};
