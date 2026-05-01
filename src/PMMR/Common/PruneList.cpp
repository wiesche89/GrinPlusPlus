#include "PruneList.h"
#include "MMRUtil.h"

#include <Common/Util/FileUtil.h>

#pragma warning(disable:4244)

PruneList::PruneList(const fs::path& filePath, Roaring&& prunedRoots)
    : m_filePath(filePath), m_prunedRoots(std::move(prunedRoots))
{

}

std::shared_ptr<PruneList> PruneList::Load(const fs::path& filePath)
{
    std::vector<unsigned char> data;
    if (FileUtil::ReadFile(filePath, data)) {
        Roaring prunedRoots = Roaring::readSafe((const char*)data.data(), data.size());
        PruneList* pPruneList = new PruneList(filePath, std::move(prunedRoots));
        pPruneList->BuildPrunedCache();
        pPruneList->BuildShiftCaches();

        return std::shared_ptr<PruneList>(pPruneList);
    } else {
        return std::shared_ptr<PruneList>(new PruneList(filePath, Roaring()));
    }
}

void PruneList::Flush()
{
    if (!m_dirty) {
        return;
    }

    // Run the optimization step on the bitmap.
    m_prunedRoots.runOptimize();

    // Write the updated bitmap file to disk.
    const size_t size = m_prunedRoots.getSizeInBytes();
    if (size > 0) {
        std::vector<unsigned char> buffer(size);
        m_prunedRoots.write((char*)buffer.data());

        FileUtil::SafeWriteToFile(m_filePath, buffer);

        // Rebuild our "shift caches" here as we are flushing changes to disk
        // and the contents of our prune_list has likely changed.
        BuildPrunedCache();
        BuildShiftCaches();
    }

    m_dirty = false;
}

void PruneList::Rollback() noexcept
{
    try {
        std::vector<unsigned char> data;
        if (FileUtil::ReadFile(m_filePath, data)) {
            m_prunedRoots = Roaring::readSafe((const char*)data.data(), data.size());
        } else {
            m_prunedRoots = Roaring();
        }

        BuildPrunedCache();
        BuildShiftCaches();
        m_dirty = false;
    } catch (...) {
    }
}

// Push the node at the provided position in the prune list.
// Compacts the list if pruning the additional node means a parent can get pruned as well.
void PruneList::Add(const Index& position)
{
    m_dirty = true;
    Index current_idx = position;
    while (true) {
        uint64_t sibling_pos = current_idx.GetSibling().GetPosition();
        if (m_prunedRoots.contains(sibling_pos + 1) || m_prunedCache.contains(sibling_pos + 1)) {
            m_prunedCache.add(current_idx.GetPosition() + 1);
            m_prunedRoots.remove(sibling_pos + 1);
            current_idx = current_idx.GetParent();
        } else {
            m_prunedCache.add(current_idx.GetPosition() + 1);
            m_prunedRoots.add(current_idx.GetPosition() + 1);
            break;
        }
    }
}

void PruneList::AddPrunedRoot(const Index& mmrIndex)
{
    m_dirty = true;
    m_prunedRoots.add(mmrIndex.GetPosition() + 1);
}

void PruneList::RebuildCaches()
{
    BuildPrunedCache();
    BuildShiftCaches();
}

bool PruneList::IsPruned(const Index& mmr_index) const
{
    return m_prunedCache.contains(mmr_index.GetPosition() + 1);
}

bool PruneList::IsPrunedRoot(const Index& mmr_index) const
{
    return m_prunedRoots.contains(mmr_index.GetPosition() + 1);
}

bool PruneList::IsCompacted(const Index& mmr_index) const
{
    return IsPruned(mmr_index) && !IsPrunedRoot(mmr_index);
}

uint64_t PruneList::GetTotalShift() const
{
    return GetShift(Index::At(m_prunedRoots.maximum()));
}

uint64_t PruneList::GetShift(const Index& mmr_index) const
{
    if (m_prunedRoots.isEmpty()) {
        return 0;
    }

    if (m_shiftCache.empty()) {
        return 0;
    }

    const uint64_t index = m_prunedRoots.rank(mmr_index.GetPosition() + 1);
    if (index == 0) {
        return 0;
    }

    if (index > m_shiftCache.size()) {
        return m_shiftCache.back();
    } else {
        return m_shiftCache[index - 1];
    }
}

uint64_t PruneList::GetLeafShift(const Index& mmr_idx) const
{
    if (m_prunedRoots.isEmpty()) {
        return 0;
    }

    const uint64_t index = m_prunedRoots.rank(mmr_idx.GetPosition() + 1);
    if (index == 0) {
        return 0;
    }

    if (m_leafShiftCache.empty()) {
        return 0;
    }

    if (index > m_leafShiftCache.size()) {
        return m_leafShiftCache.back();
    } else {
        return m_leafShiftCache[index - 1];
    }
}

void PruneList::BuildPrunedCache()
{
    m_prunedCache = Roaring();
    if (m_prunedRoots.isEmpty()) {
        return;
    }

    const uint64_t maximum = m_prunedRoots.maximum();
    m_prunedCache = Roaring(roaring_bitmap_create_with_capacity(maximum));

    for (const uint64_t rootWirePos : m_prunedRoots) {
        const uint64_t rootPos = rootWirePos - 1;
        const uint64_t height = Index::At(rootPos).GetHeight();
        const uint64_t nodeCount = (1ULL << (height + 1)) - 1;
        const uint64_t firstPos = rootPos + 1 - nodeCount;
        for (uint64_t pos = firstPos; pos <= rootPos; ++pos) {
            m_prunedCache.add(pos + 1);
        }
    }

    m_prunedCache.runOptimize();
}

void PruneList::BuildShiftCaches()
{
    m_shiftCache.clear();
    m_leafShiftCache.clear();

    if (m_prunedRoots.isEmpty()) {
        return;
    }

    uint64_t totalShift = 0;
    uint64_t totalLeafShift = 0;
    for (const uint64_t rootWirePos : m_prunedRoots) {
        const uint64_t height = Index::At(rootWirePos - 1).GetHeight();

        totalShift += 2ULL * ((1ULL << height) - 1);
        m_shiftCache.push_back(totalShift);

        totalLeafShift += (height == 0) ? 0 : 1ULL << height;
        m_leafShiftCache.push_back(totalLeafShift);
    }
}
