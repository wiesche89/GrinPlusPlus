#pragma once

#include <Core/Models/BlockHeader.h>
#include <Core/Models/OutputIdentifier.h>
#include <Common/Logger.h>
#include <Core/Models/TransactionKernel.h>
#include <Crypto/Models/RangeProof.h>
#include <PMMR/Common/BitmapAccumulator.h>
#include <PMMR/Common/BitmapSegment.h>
#include <PMMR/Common/Segment.h>
#include <PMMR/Common/SegmentType.h>
#include <PMMR/PIBDParams.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

class TxHashSetDesegmenter
{
public:
	using BitmapPMMRSegment = Segment<BitmapChunk::LEN_BYTES, BitmapChunk>;
	using OutputPMMRSegment = Segment<PIBD::OUTPUT_DATA_SIZE, OutputIdentifier>;
	using RangeProofPMMRSegment = Segment<PIBD::RANGE_PROOF_DATA_SIZE, RangeProof>;
	using KernelPMMRSegment = Segment<PIBD::KERNEL_DATA_SIZE, TransactionKernel>;

	explicit TxHashSetDesegmenter(BlockHeader archiveHeader)
		: m_archiveHeader(std::move(archiveHeader))
	{
		Reset();
	}

	void Reset()
	{
		m_bitmapAccumulator = BitmapAccumulator();
		m_bitmapCacheComplete = false;
		m_allSegmentsComplete = false;
		m_bitmapSegments.clear();
		m_outputSegments.clear();
		m_rangeProofSegments.clear();
		m_kernelSegments.clear();
		m_appliedBitmapMMRSize = 0;
		m_appliedOutputMMRSize = 0;
		m_appliedRangeProofMMRSize = 0;
		m_appliedKernelMMRSize = 0;
		m_nextDesiredTypeOffset = 0;
		CalculateBitmapMMRSizes();
	}

	const BlockHeader& GetArchiveHeader() const noexcept { return m_archiveHeader; }
	uint64_t GetExpectedBitmapMMRSize() const noexcept { return m_bitmapMMRSize; }
	const BitmapAccumulator& GetBitmapAccumulator() const noexcept { return m_bitmapAccumulator; }
	bool IsBitmapComplete() const noexcept { return m_bitmapCacheComplete; }
	bool IsComplete() const noexcept { return m_allSegmentsComplete; }
	bool IsApplied(const SegmentTypeIdentifier& typeId) const noexcept
	{
		const SegmentIdentifier& identifier = typeId.GetIdentifier();
		switch (typeId.GetSegmentType())
		{
			case SegmentType::OutputBitmap:
				return IsSegmentApplied(identifier, m_bitmapMMRSize, m_appliedBitmapMMRSize);
			case SegmentType::Output:
				return IsSegmentApplied(identifier, m_archiveHeader.GetOutputMMRSize(), m_appliedOutputMMRSize);
			case SegmentType::RangeProof:
				return IsSegmentApplied(identifier, m_archiveHeader.GetOutputMMRSize(), m_appliedRangeProofMMRSize);
			case SegmentType::Kernel:
				return IsSegmentApplied(identifier, m_archiveHeader.GetKernelMMRSize(), m_appliedKernelMMRSize);
		}

		return false;
	}
	uint64_t GetCompletedLeaves() const noexcept
	{
		return MMRUtil::CountLeaves(m_appliedOutputMMRSize)
			+ MMRUtil::CountLeaves(m_appliedRangeProofMMRSize)
			+ MMRUtil::CountLeaves(m_appliedKernelMMRSize);
	}
	uint64_t GetLeavesRequired() const noexcept
	{
		return (MMRUtil::CountLeaves(m_archiveHeader.GetOutputMMRSize()) * 2)
			+ MMRUtil::CountLeaves(m_archiveHeader.GetKernelMMRSize());
	}
	uint64_t GetRequiredHeight() const noexcept { return m_archiveHeader.GetHeight(); }
	uint64_t GetAppliedOutputMMRSize() const noexcept { return m_appliedOutputMMRSize; }
	uint64_t GetAppliedRangeProofMMRSize() const noexcept { return m_appliedRangeProofMMRSize; }
	uint64_t GetAppliedKernelMMRSize() const noexcept { return m_appliedKernelMMRSize; }
	void SetAppliedMMRSizes(
		const uint64_t outputMMRSize,
		const uint64_t rangeProofMMRSize,
		const uint64_t kernelMMRSize) noexcept
	{
		m_appliedOutputMMRSize = (std::min)(outputMMRSize, m_archiveHeader.GetOutputMMRSize());
		m_appliedRangeProofMMRSize = (std::min)(rangeProofMMRSize, m_archiveHeader.GetOutputMMRSize());
		m_appliedKernelMMRSize = (std::min)(kernelMMRSize, m_archiveHeader.GetKernelMMRSize());
	}
	uint64_t GetCompletedToHeight() const noexcept
	{
		const uint64_t leavesRequired = GetLeavesRequired();
		if (leavesRequired == 0) {
			return 0;
		}

		const uint64_t completedLeaves = (std::min)(GetCompletedLeaves(), leavesRequired);
		const uint64_t requiredHeight = GetRequiredHeight();
		return (std::min)(requiredHeight, (completedLeaves * requiredHeight) / leavesRequired);
	}

	bool AddBitmapSegment(const BitmapSegment& bitmapSegment, const Hash& outputRoot)
	{
		const BitmapPMMRSegment segment = bitmapSegment.ToSegment();
		if (!ValidateHeight(SegmentType::OutputBitmap, segment.GetIdentifier().GetHeight())) {
			return false;
		}

		if (!segment.ValidateWith(
			m_bitmapMMRSize,
			m_archiveHeader.GetOutputRoot(),
			m_archiveHeader.GetOutputMMRSize(),
			outputRoot,
			true)) {
			return false;
		}

		CacheSegment(m_bitmapSegments, segment);
		return true;
	}

	bool AddOutputSegment(const OutputPMMRSegment& segment, const Hash& bitmapRoot)
	{
		if (!ValidateHeight(SegmentType::Output, segment.GetIdentifier().GetHeight())) {
			return false;
		}

		if (!m_bitmapCacheComplete) {
			LOG_WARNING(StringUtil::Format("PIBD output segment {}:{} received before output bitmap was complete.",
				segment.GetIdentifier().GetHeight(),
				segment.GetIdentifier().GetIndex()));
			return false;
		}

		const std::optional<std::pair<Hash, uint64_t>> firstUnprunedParent = segment.FirstUnprunedParent(
			m_archiveHeader.GetOutputMMRSize(),
			&m_bitmapAccumulator);
		if (!firstUnprunedParent.has_value()) {
			LOG_WARNING(StringUtil::Format("PIBD output segment {}:{} failed: segment root could not be reconstructed (leaves={}, hashes={}, proof_hashes={}, output_mmr_size={}, bitmap_chunks={}, bitmap_mmr_size={}).",
				segment.GetIdentifier().GetHeight(),
				segment.GetIdentifier().GetIndex(),
				segment.GetLeaves().size(),
				segment.GetHashes().size(),
				segment.GetProof().GetHashes().size(),
				m_archiveHeader.GetOutputMMRSize(),
				m_bitmapAccumulator.GetChunks().size(),
				m_bitmapAccumulator.GetMMRSize()));
			return false;
		}

		const std::pair<uint64_t, uint64_t> posRange = segment.GetPositionRange(m_archiveHeader.GetOutputMMRSize());
		const Hash localBitmapRoot = m_bitmapAccumulator.Root();
		const bool mergedWithLocalBitmapRoot = segment.GetProof().ValidateWith(
			m_archiveHeader.GetOutputMMRSize(),
			posRange.first,
			posRange.second,
			firstUnprunedParent->first,
			firstUnprunedParent->second,
			m_archiveHeader.GetOutputMMRSize(),
			localBitmapRoot,
			false,
			m_archiveHeader.GetOutputRoot());
		if (!mergedWithLocalBitmapRoot) {
			const bool mergedWithMessageBitmapRoot = segment.GetProof().ValidateWith(
				m_archiveHeader.GetOutputMMRSize(),
				posRange.first,
				posRange.second,
				firstUnprunedParent->first,
				firstUnprunedParent->second,
				m_archiveHeader.GetOutputMMRSize(),
				bitmapRoot,
				false,
				m_archiveHeader.GetOutputRoot());
			LOG_WARNING(StringUtil::Format("PIBD output segment {}:{} failed proof validation (version={}, output_mmr_size={}, range={}..{}, segment_root={}, local_bitmap_root={}, message_bitmap_root={}, header_output_root={}, message_bitmap_ok={}).",
				segment.GetIdentifier().GetHeight(),
				segment.GetIdentifier().GetIndex(),
				(uint32_t)m_archiveHeader.GetVersion(),
				m_archiveHeader.GetOutputMMRSize(),
				posRange.first,
				posRange.second,
				firstUnprunedParent->first,
				localBitmapRoot,
				bitmapRoot,
				m_archiveHeader.GetOutputRoot(),
				mergedWithMessageBitmapRoot));
			return false;
		}

		CacheSegment(m_outputSegments, segment);
		return true;
	}

	bool AddRangeProofSegment(const RangeProofPMMRSegment& segment)
	{
		if (!ValidateHeight(SegmentType::RangeProof, segment.GetIdentifier().GetHeight())) {
			return false;
		}

		if (!m_bitmapCacheComplete) {
			return false;
		}

		if (!segment.Validate(m_archiveHeader.GetOutputMMRSize(), m_archiveHeader.GetRangeProofRoot(), &m_bitmapAccumulator)) {
			return false;
		}

		CacheSegment(m_rangeProofSegments, segment);
		return true;
	}

	bool AddKernelSegment(const KernelPMMRSegment& segment)
	{
		if (!ValidateHeight(SegmentType::Kernel, segment.GetIdentifier().GetHeight())) {
			return false;
		}

		if (!segment.Validate(m_archiveHeader.GetKernelMMRSize(), m_archiveHeader.GetKernelRoot())) {
			return false;
		}

		CacheSegment(m_kernelSegments, segment);
		return true;
	}

	std::vector<SegmentTypeIdentifier> NextDesiredSegments(const size_t maxElements)
	{
		std::vector<SegmentTypeIdentifier> desired;

		if (!m_bitmapCacheComplete) {
			AppendDesiredSegments(
				desired,
				maxElements,
				SegmentType::OutputBitmap,
				m_bitmapMMRSize,
				PIBD::BITMAP_SEGMENT_HEIGHT,
				m_appliedBitmapMMRSize,
				m_bitmapSegments);
			return desired;
		}

		std::vector<SegmentTypeIdentifier> outputDesired;
		std::vector<SegmentTypeIdentifier> rangeProofDesired;
		std::vector<SegmentTypeIdentifier> kernelDesired;
		AppendDesiredSegments(
			outputDesired,
			maxElements,
			SegmentType::Output,
			m_archiveHeader.GetOutputMMRSize(),
			PIBD::OUTPUT_SEGMENT_HEIGHT,
			m_appliedOutputMMRSize,
			m_outputSegments);
		AppendDesiredSegments(
			rangeProofDesired,
			maxElements,
			SegmentType::RangeProof,
			m_archiveHeader.GetOutputMMRSize(),
			PIBD::RANGEPROOF_SEGMENT_HEIGHT,
			m_appliedRangeProofMMRSize,
			m_rangeProofSegments);
		AppendDesiredSegments(
			kernelDesired,
			maxElements,
			SegmentType::Kernel,
			m_archiveHeader.GetKernelMMRSize(),
			PIBD::KERNEL_SEGMENT_HEIGHT,
			m_appliedKernelMMRSize,
			m_kernelSegments);

		const size_t typeOffset = m_nextDesiredTypeOffset++ % 3;
		for (size_t index = 0; desired.size() < maxElements; ++index) {
			bool appended = false;
			for (size_t typeIndex = 0; typeIndex < 3 && desired.size() < maxElements; ++typeIndex) {
				const size_t rotatedType = (typeOffset + typeIndex) % 3;
				if (rotatedType == 0 && index < outputDesired.size()) {
					desired.push_back(outputDesired[index]);
					appended = true;
				} else if (rotatedType == 1 && index < rangeProofDesired.size()) {
					desired.push_back(rangeProofDesired[index]);
					appended = true;
				} else if (rotatedType == 2 && index < kernelDesired.size()) {
					desired.push_back(kernelDesired[index]);
					appended = true;
				}
			}
			if (!appended) {
				break;
			}
		}

		m_allSegmentsComplete =
			desired.empty() &&
			m_appliedOutputMMRSize == m_archiveHeader.GetOutputMMRSize() &&
			m_appliedRangeProofMMRSize == m_archiveHeader.GetOutputMMRSize() &&
			m_appliedKernelMMRSize == m_archiveHeader.GetKernelMMRSize();

		return desired;
	}

	std::vector<SegmentTypeIdentifier> GetBlockingSegments() const
	{
		std::vector<SegmentTypeIdentifier> blockers;

		if (!m_bitmapCacheComplete) {
			AppendBlockingSegment(
				blockers,
				SegmentType::OutputBitmap,
				m_bitmapMMRSize,
				PIBD::BITMAP_SEGMENT_HEIGHT,
				m_appliedBitmapMMRSize,
				m_bitmapSegments);
			return blockers;
		}

		AppendBlockingSegment(
			blockers,
			SegmentType::Output,
			m_archiveHeader.GetOutputMMRSize(),
			PIBD::OUTPUT_SEGMENT_HEIGHT,
			m_appliedOutputMMRSize,
			m_outputSegments);
		AppendBlockingSegment(
			blockers,
			SegmentType::RangeProof,
			m_archiveHeader.GetOutputMMRSize(),
			PIBD::RANGEPROOF_SEGMENT_HEIGHT,
			m_appliedRangeProofMMRSize,
			m_rangeProofSegments);
		AppendBlockingSegment(
			blockers,
			SegmentType::Kernel,
			m_archiveHeader.GetKernelMMRSize(),
			PIBD::KERNEL_SEGMENT_HEIGHT,
			m_appliedKernelMMRSize,
			m_kernelSegments);

		return blockers;
	}

	bool ApplyNextBitmapSegment()
	{
		const uint64_t nextIndex = NextRequiredSegmentIndex(m_appliedBitmapMMRSize, PIBD::BITMAP_SEGMENT_HEIGHT);
		const auto iter = std::find_if(
			m_bitmapSegments.begin(),
			m_bitmapSegments.end(),
			[nextIndex](const BitmapPMMRSegment& segment) { return segment.GetIdentifier().GetIndex() == nextIndex; });
		if (iter == m_bitmapSegments.end()) {
			return false;
		}

		for (const BitmapChunk& chunk : iter->GetLeaves()) {
			m_bitmapAccumulator.AppendChunk(chunk);
		}
		m_appliedBitmapMMRSize = m_bitmapAccumulator.GetMMRSize();
		m_bitmapSegments.erase(iter);
		m_bitmapCacheComplete = m_appliedBitmapMMRSize == m_bitmapMMRSize;
		return true;
	}

	template<typename SEGMENT>
	std::vector<SEGMENT> TakeReadySegmentBatch(
		std::vector<SEGMENT>& cache,
		uint64_t& appliedMMRSize,
		const uint8_t height,
		const uint64_t targetMMRSize)
	{
		std::vector<SEGMENT> result;
		uint64_t nextIndex = NextRequiredSegmentIndex(appliedMMRSize, height);
		while (result.size() < PIBD::SEGMENT_APPLY_BATCH_SIZE) {
			const auto iter = std::find_if(
				cache.begin(),
				cache.end(),
				[nextIndex](const SEGMENT& segment) { return segment.GetIdentifier().GetIndex() == nextIndex; });
			if (iter == cache.end()) {
				break;
			}

			const std::pair<uint64_t, uint64_t> range = iter->GetIdentifier().GetPositionRange(targetMMRSize);
			appliedMMRSize = AppliedMMRSizeAfterSegment(iter->GetIdentifier(), targetMMRSize);
			result.push_back(std::move(*iter));
			cache.erase(iter);
			++nextIndex;
		}

		return result;
	}

	std::vector<OutputPMMRSegment> TakeReadyOutputSegments()
	{
		return TakeReadySegmentBatch(
			m_outputSegments,
			m_appliedOutputMMRSize,
			PIBD::OUTPUT_SEGMENT_HEIGHT,
			m_archiveHeader.GetOutputMMRSize());
	}

	std::vector<RangeProofPMMRSegment> TakeReadyRangeProofSegments()
	{
		return TakeReadySegmentBatch(
			m_rangeProofSegments,
			m_appliedRangeProofMMRSize,
			PIBD::RANGEPROOF_SEGMENT_HEIGHT,
			m_archiveHeader.GetOutputMMRSize());
	}

	std::vector<KernelPMMRSegment> TakeReadyKernelSegments()
	{
		return TakeReadySegmentBatch(
			m_kernelSegments,
			m_appliedKernelMMRSize,
			PIBD::KERNEL_SEGMENT_HEIGHT,
			m_archiveHeader.GetKernelMMRSize());
	}

	template<typename APPLY_OUTPUT, typename APPLY_RANGEPROOF, typename APPLY_KERNEL>
	std::optional<size_t> ApplyReadySegments(
		APPLY_OUTPUT applyOutputSegment,
		APPLY_RANGEPROOF applyRangeProofSegment,
		APPLY_KERNEL applyKernelSegment)
	{
		if (!m_bitmapCacheComplete) {
			return 0;
		}

		size_t appliedCount = 0;
		std::optional<size_t> outputCount = ApplyReadySegmentBatch(
			m_outputSegments,
			m_appliedOutputMMRSize,
			PIBD::OUTPUT_SEGMENT_HEIGHT,
			m_archiveHeader.GetOutputMMRSize(),
			"output",
			applyOutputSegment);
		if (!outputCount.has_value()) {
			return std::nullopt;
		}
		appliedCount += outputCount.value();

		std::optional<size_t> rangeProofCount = ApplyReadySegmentBatch(
			m_rangeProofSegments,
			m_appliedRangeProofMMRSize,
			PIBD::RANGEPROOF_SEGMENT_HEIGHT,
			m_archiveHeader.GetOutputMMRSize(),
			"rangeproof",
			applyRangeProofSegment);
		if (!rangeProofCount.has_value()) {
			return std::nullopt;
		}
		appliedCount += rangeProofCount.value();

		std::optional<size_t> kernelCount = ApplyReadySegmentBatch(
			m_kernelSegments,
			m_appliedKernelMMRSize,
			PIBD::KERNEL_SEGMENT_HEIGHT,
			m_archiveHeader.GetKernelMMRSize(),
			"kernel",
			applyKernelSegment);
		if (!kernelCount.has_value()) {
			return std::nullopt;
		}
		appliedCount += kernelCount.value();

		m_allSegmentsComplete =
			m_bitmapCacheComplete &&
			m_appliedOutputMMRSize == m_archiveHeader.GetOutputMMRSize() &&
			m_appliedRangeProofMMRSize == m_archiveHeader.GetOutputMMRSize() &&
			m_appliedKernelMMRSize == m_archiveHeader.GetKernelMMRSize();

		return appliedCount;
	}

private:
	static bool ValidateHeight(const SegmentType type, const uint8_t height) noexcept
	{
		switch (type)
		{
			case SegmentType::OutputBitmap:
				return height >= PIBD::BITMAP_SEGMENT_MIN_HEIGHT && height <= PIBD::BITMAP_SEGMENT_MAX_HEIGHT;
			case SegmentType::Output:
				return height >= PIBD::OUTPUT_SEGMENT_MIN_HEIGHT && height <= PIBD::OUTPUT_SEGMENT_MAX_HEIGHT;
			case SegmentType::RangeProof:
				return height >= PIBD::RANGEPROOF_SEGMENT_MIN_HEIGHT && height <= PIBD::RANGEPROOF_SEGMENT_MAX_HEIGHT;
			case SegmentType::Kernel:
				return height >= PIBD::KERNEL_SEGMENT_MIN_HEIGHT && height <= PIBD::KERNEL_SEGMENT_MAX_HEIGHT;
		}

		return false;
	}

	template<typename SEGMENT>
	static void CacheSegment(std::vector<SEGMENT>& cache, const SEGMENT& segment)
	{
		const auto iter = std::find_if(
			cache.begin(),
			cache.end(),
			[&segment](const SEGMENT& cached) { return cached.GetIdentifier() == segment.GetIdentifier(); });
		if (iter == cache.end()) {
			cache.push_back(segment);
		}
	}

	template<typename SEGMENT>
	static bool HasSegment(const std::vector<SEGMENT>& cache, const SegmentIdentifier& identifier)
	{
		return std::any_of(
			cache.begin(),
			cache.end(),
			[&identifier](const SEGMENT& segment) { return segment.GetIdentifier() == identifier; });
	}

	template<typename SEGMENT>
	void AppendDesiredSegments(
		std::vector<SegmentTypeIdentifier>& desired,
		const size_t maxElements,
		const SegmentType type,
		const uint64_t targetMMRSize,
		const uint8_t height,
		const uint64_t localMMRSize,
		const std::vector<SEGMENT>& cache) const
	{
		const size_t totalSegments = SegmentIdentifier::CountSegmentsRequired(targetMMRSize, height);
		uint64_t nextIndex = NextRequiredSegmentIndex(localMMRSize, height);

		while (desired.size() < maxElements && nextIndex < totalSegments) {
			SegmentIdentifier identifier(height, nextIndex);
			if (!IsSegmentApplied(identifier, targetMMRSize, localMMRSize) && !HasSegment(cache, identifier)) {
				desired.emplace_back(type, identifier);
				if (cache.size() >= PIBD::MAX_CACHED_SEGMENTS) {
					break;
				}
			}
			++nextIndex;
		}
	}

	template<typename SEGMENT>
	static void AppendBlockingSegment(
		std::vector<SegmentTypeIdentifier>& blockers,
		const SegmentType type,
		const uint64_t targetMMRSize,
		const uint8_t height,
		const uint64_t localMMRSize,
		const std::vector<SEGMENT>& cache)
	{
		const size_t totalSegments = SegmentIdentifier::CountSegmentsRequired(targetMMRSize, height);
		const uint64_t nextIndex = NextRequiredSegmentIndex(localMMRSize, height);
		if (nextIndex >= totalSegments) {
			return;
		}

		SegmentIdentifier identifier(height, nextIndex);
		if (!IsSegmentApplied(identifier, targetMMRSize, localMMRSize) && !HasSegment(cache, identifier)) {
			blockers.emplace_back(type, identifier);
		}
	}

	static uint64_t NextRequiredSegmentIndex(const uint64_t localMMRSize, const uint8_t height) noexcept
	{
		if (localMMRSize == 0 || localMMRSize == 1) {
			return 0;
		}

		uint64_t segmentCount = SegmentIdentifier::CountSegmentsRequired(localMMRSize, height);
		const uint64_t theoreticalSize = SegmentIdentifier::GetPMMRSize(segmentCount, height);
		if (segmentCount > 0 && localMMRSize < theoreticalSize) {
			--segmentCount;
		}

		return segmentCount;
	}

	static bool IsSegmentApplied(
		const SegmentIdentifier& identifier,
		const uint64_t targetMMRSize,
		const uint64_t appliedMMRSize) noexcept
	{
		if (appliedMMRSize == 0 || identifier.GetSegmentUnprunedSize(targetMMRSize) == 0) {
			return false;
		}

		const std::pair<uint64_t, uint64_t> range = identifier.GetPositionRange(targetMMRSize);
		return range.second < appliedMMRSize;
	}

	static uint64_t AppliedMMRSizeAfterSegment(
		const SegmentIdentifier& identifier,
		const uint64_t targetMMRSize) noexcept
	{
		const uint64_t fullSegmentMMRSize = SegmentIdentifier::GetPMMRSize(
			identifier.GetIndex() + 1,
			identifier.GetHeight());
		return (std::min)(fullSegmentMMRSize, targetMMRSize);
	}

	template<typename SEGMENT, typename APPLY_SEGMENT>
	static std::optional<size_t> ApplyReadySegmentBatch(
		std::vector<SEGMENT>& cache,
		uint64_t& appliedMMRSize,
		const uint8_t height,
		const uint64_t targetMMRSize,
		const char* segmentType,
		APPLY_SEGMENT applySegment)
	{
		uint64_t nextIndex = NextRequiredSegmentIndex(appliedMMRSize, height);
		size_t appliedCount = 0;
		uint64_t firstAppliedIndex = 0;
		uint64_t lastAppliedIndex = 0;
		uint64_t initialAppliedMMRSize = appliedMMRSize;
		while (appliedCount < PIBD::SEGMENT_APPLY_BATCH_SIZE) {
			const auto iter = std::find_if(
				cache.begin(),
				cache.end(),
				[nextIndex](const SEGMENT& segment) { return segment.GetIdentifier().GetIndex() == nextIndex; });
			if (iter == cache.end()) {
				if (cache.size() >= PIBD::MAX_CACHED_SEGMENTS) {
					uint64_t lowestCached = cache.front().GetIdentifier().GetIndex();
					uint64_t highestCached = lowestCached;
					for (const SEGMENT& cached : cache) {
						const uint64_t cachedIndex = cached.GetIdentifier().GetIndex();
						lowestCached = (std::min)(lowestCached, cachedIndex);
						highestCached = (std::max)(highestCached, cachedIndex);
					}
					LOG_DEBUG(StringUtil::Format(
						"PIBD {} apply blocked: expected segment {}:{}, cache_size={}, cached_range={}..{}, applied_mmr_size={}, target_mmr_size={}.",
						segmentType,
						height,
						nextIndex,
						cache.size(),
						lowestCached,
						highestCached,
						appliedMMRSize,
						targetMMRSize));
				}
				break;
			}

			if (!applySegment(*iter, targetMMRSize)) {
				LOG_WARNING(StringUtil::Format(
					"PIBD {} apply failed for segment {}:{} at applied_mmr_size={}, target_mmr_size={}.",
					segmentType,
					iter->GetIdentifier().GetHeight(),
					iter->GetIdentifier().GetIndex(),
					appliedMMRSize,
					targetMMRSize));
				return std::nullopt;
			}

			if (appliedCount == 0) {
				firstAppliedIndex = iter->GetIdentifier().GetIndex();
				initialAppliedMMRSize = appliedMMRSize;
			}
			lastAppliedIndex = iter->GetIdentifier().GetIndex();
			appliedMMRSize = AppliedMMRSizeAfterSegment(iter->GetIdentifier(), targetMMRSize);
			cache.erase(iter);
			++nextIndex;
			++appliedCount;
		}

		if (appliedCount > 0) {
			LOG_TRACE(StringUtil::Format(
				"PIBD {} applied {} segment(s) {}:{}..{}:{}, applied_mmr_size {} -> {}.",
				segmentType,
				appliedCount,
				height,
				firstAppliedIndex,
				height,
				lastAppliedIndex,
				initialAppliedMMRSize,
				appliedMMRSize));
		}

		return appliedCount;
	}

	void CalculateBitmapMMRSizes()
	{
		const uint64_t outputLeaves = MMRUtil::CountLeaves(m_archiveHeader.GetOutputMMRSize());
		const uint64_t bitmapLeaves = (outputLeaves + BitmapChunk::LEN_BITS - 1) / BitmapChunk::LEN_BITS;
		m_bitmapMMRSize = bitmapLeaves == 0 ? 0 : LeafIndex::At(bitmapLeaves).GetPosition();
	}

	BlockHeader m_archiveHeader;
	BitmapAccumulator m_bitmapAccumulator;
	bool m_bitmapCacheComplete{ false };
	bool m_allSegmentsComplete{ false };

	uint64_t m_bitmapMMRSize{ 0 };
	uint64_t m_appliedBitmapMMRSize{ 0 };
	uint64_t m_appliedOutputMMRSize{ 0 };
	uint64_t m_appliedRangeProofMMRSize{ 0 };
	uint64_t m_appliedKernelMMRSize{ 0 };
	size_t m_nextDesiredTypeOffset{ 0 };

	std::vector<BitmapPMMRSegment> m_bitmapSegments;
	std::vector<OutputPMMRSegment> m_outputSegments;
	std::vector<RangeProofPMMRSegment> m_rangeProofSegments;
	std::vector<KernelPMMRSegment> m_kernelSegments;
};
