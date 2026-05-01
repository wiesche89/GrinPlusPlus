#include "MMRUtil.h"

#include <Common/Util/BitUtil.h>

//
// Calculates the postorder traversal index of all peaks in an MMR with the given size (# of nodes).
// Returns empty when the size does not represent a complete MMR (ie. siblings exist, but no parent).
//
std::vector<uint64_t> MMRUtil::GetPeakIndices(const uint64_t size)
{
	std::vector<uint64_t> peakIndices;
	if (size > 0) {
		uint64_t peakSize = BitUtil::FillOnesToRight(size);
		uint64_t numLeft = size;
		uint64_t sumPrevPeaks = 0;
		while (peakSize != 0) {
			if (numLeft >= peakSize) {
				peakIndices.push_back(sumPrevPeaks + peakSize - 1);
				sumPrevPeaks += peakSize;
				numLeft -= peakSize;
			}

			peakSize >>= 1;
		}

		if (numLeft > 0) {
			return std::vector<uint64_t>();
		}
	}

	return peakIndices;
}

uint64_t MMRUtil::CountLeaves(const uint64_t size)
{
	if (size == 0) {
		return 0;
	}

	return LeafIndex::AtPos(size).Get();
}

bool MMRUtil::IsLeftSibling(const uint64_t position)
{
	const Index index = Index::At(position);
	const Index parent = index.GetParent();
	return parent.GetLeftChild() == index;
}

std::vector<std::pair<uint64_t, uint64_t>> MMRUtil::FamilyBranch(const uint64_t position, const uint64_t size)
{
	std::vector<std::pair<uint64_t, uint64_t>> branch;
	if (size == 0 || position >= size) {
		return branch;
	}

	Index current = Index::At(position);
	Index parent = current.GetParent();
	while (parent.GetPosition() < size) {
		branch.emplace_back(parent.GetPosition(), current.GetSibling().GetPosition());
		current = parent;
		parent = current.GetParent();
	}

	return branch;
}
