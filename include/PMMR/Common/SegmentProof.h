#pragma once

#include <Crypto/Models/Hash.h>
#include <Core/Serialization/ByteBuffer.h>
#include <Core/Serialization/Serializer.h>
#include <Core/Traits/Serializable.h>
#include <Crypto/Hasher.h>
#include <PMMR/Common/MMRUtil.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

class SegmentProof : public Traits::ISerializable
{
	std::vector<Hash> m_hashes;

	static Hash HashParentWithIndex(const Hash& leftChild, const Hash& rightChild, const uint64_t parentIndex)
	{
		Serializer serializer;
		serializer.Append<uint64_t>(parentIndex);
		serializer.AppendBigInteger<32>(leftChild);
		serializer.AppendBigInteger<32>(rightChild);
		return Hasher::Blake2b(serializer.GetBytes());
	}

public:
	//
	// Constructors
	//
	SegmentProof() = default;
	SegmentProof(std::vector<Hash> hashes) : m_hashes(std::move(hashes)) {}
	SegmentProof(const SegmentProof& other) = default;
	SegmentProof(SegmentProof&& other) noexcept = default;

	//
	// Destructor
	//
	virtual ~SegmentProof() = default;

	//
	// Operators
	//
	SegmentProof& operator=(const SegmentProof& other) = default;
	SegmentProof& operator=(SegmentProof&& other) noexcept = default;

	//
	// Getters
	//
	const std::vector<Hash>& GetHashes() const noexcept { return m_hashes; }
	bool Empty() const noexcept { return m_hashes.empty(); }

	template<typename HASH_READER>
	static std::optional<SegmentProof> Generate(
		const uint64_t mmrSize,
		const uint64_t segmentFirstPos,
		const uint64_t segmentLastPos,
		const std::optional<uint64_t> startPos,
		HASH_READER getHashAt)
	{
		std::vector<Hash> hashes;
		const std::vector<std::pair<uint64_t, uint64_t>> branch = MMRUtil::FamilyBranch(segmentLastPos, mmrSize);
		for (const std::pair<uint64_t, uint64_t>& entry : branch) {
			const uint64_t parentPos = entry.first;
			const uint64_t siblingPos = entry.second;
			if (startPos.has_value() && parentPos < startPos.value()) {
				continue;
			}

			const std::optional<Hash> siblingHash = getHashAt(siblingPos);
			if (!siblingHash.has_value()) {
				return std::nullopt;
			}

			hashes.push_back(siblingHash.value());
		}

		const uint64_t peakPos = branch.empty() ? segmentLastPos : branch.back().first;
		const std::vector<uint64_t> peaks = MMRUtil::GetPeakIndices(mmrSize);

		std::optional<Hash> rhs;
		for (auto iter = peaks.crbegin(); iter != peaks.crend(); ++iter) {
			if (*iter <= peakPos) {
				continue;
			}

			const std::optional<Hash> peakHash = getHashAt(*iter);
			if (!peakHash.has_value()) {
				return std::nullopt;
			}

			rhs = rhs.has_value()
				? std::optional<Hash>(HashParentWithIndex(peakHash.value(), rhs.value(), mmrSize))
				: peakHash;
		}
		if (rhs.has_value()) {
			hashes.push_back(rhs.value());
		}

		for (auto iter = peaks.crbegin(); iter != peaks.crend(); ++iter) {
			if (*iter >= segmentFirstPos) {
				continue;
			}

			const std::optional<Hash> peakHash = getHashAt(*iter);
			if (!peakHash.has_value()) {
				return std::nullopt;
			}

			hashes.push_back(peakHash.value());
		}

		return SegmentProof(std::move(hashes));
	}

	std::optional<Hash> ReconstructRoot(
		const uint64_t mmrSize,
		const uint64_t segmentFirstPos,
		const uint64_t segmentLastPos,
		const Hash& segmentRoot,
		const uint64_t segmentUnprunedPos) const
	{
		Hash root = segmentRoot;
		size_t proofIdx = 0;

		const std::vector<std::pair<uint64_t, uint64_t>> branch = MMRUtil::FamilyBranch(segmentLastPos, mmrSize);
		for (const std::pair<uint64_t, uint64_t>& entry : branch) {
			const uint64_t parentPos = entry.first;
			const uint64_t siblingPos = entry.second;
			if (parentPos < segmentUnprunedPos) {
				continue;
			}

			if (proofIdx >= m_hashes.size()) {
				return std::nullopt;
			}

			const Hash& siblingHash = m_hashes[proofIdx++];
			if (MMRUtil::IsLeftSibling(siblingPos)) {
				root = HashParentWithIndex(siblingHash, root, parentPos);
			} else {
				root = HashParentWithIndex(root, siblingHash, parentPos);
			}
		}

		const uint64_t peakPos = branch.empty() ? segmentLastPos : branch.back().first;
		const std::vector<uint64_t> peaks = MMRUtil::GetPeakIndices(mmrSize);
		const auto rightPeakIter = std::find_if(
			peaks.begin(),
			peaks.end(),
			[peakPos](const uint64_t pos) { return pos > peakPos; });
		if (rightPeakIter != peaks.end()) {
			if (proofIdx >= m_hashes.size()) {
				return std::nullopt;
			}

			root = HashParentWithIndex(root, m_hashes[proofIdx++], mmrSize);
		}

		for (auto iter = peaks.crbegin(); iter != peaks.crend(); ++iter) {
			if (*iter >= segmentFirstPos) {
				continue;
			}

			if (proofIdx >= m_hashes.size()) {
				return std::nullopt;
			}

			root = HashParentWithIndex(m_hashes[proofIdx++], root, mmrSize);
		}

		if (proofIdx != m_hashes.size()) {
			return std::nullopt;
		}

		return root;
	}

	bool Validate(
		const uint64_t mmrSize,
		const uint64_t segmentFirstPos,
		const uint64_t segmentLastPos,
		const Hash& segmentRoot,
		const uint64_t segmentUnprunedPos,
		const Hash& expectedRoot) const
	{
		const std::optional<Hash> root = ReconstructRoot(mmrSize, segmentFirstPos, segmentLastPos, segmentRoot, segmentUnprunedPos);
		return root.has_value() && root.value() == expectedRoot;
	}

	bool ValidateWith(
		const uint64_t mmrSize,
		const uint64_t segmentFirstPos,
		const uint64_t segmentLastPos,
		const Hash& segmentRoot,
		const uint64_t segmentUnprunedPos,
		const uint64_t hashLastPos,
		const Hash& otherRoot,
		const bool otherIsLeft,
		const Hash& expectedRoot) const
	{
		const std::optional<Hash> root = ReconstructRoot(mmrSize, segmentFirstPos, segmentLastPos, segmentRoot, segmentUnprunedPos);
		if (!root.has_value()) {
			return false;
		}

		const Hash mergedRoot = otherIsLeft
			? HashParentWithIndex(otherRoot, root.value(), hashLastPos)
			: HashParentWithIndex(root.value(), otherRoot, hashLastPos);
		return mergedRoot == expectedRoot;
	}


	//
	// Serialization/Deserialization
	//
	void Serialize(Serializer& serializer) const final
	{
		serializer.Append<uint64_t>(m_hashes.size());
		for (const auto& hash : m_hashes) {
			serializer.AppendBigInteger<32>(hash);
		}
	}

	static SegmentProof Deserialize(ByteBuffer& byteBuffer)
	{
		const uint64_t size = byteBuffer.Read<uint64_t>();
		std::vector<Hash> hashes;
		hashes.reserve(size);
		for (uint64_t i = 0; i < size; ++i) {
			hashes.push_back(byteBuffer.ReadBigInteger<32>());
		}
		return SegmentProof(std::move(hashes));
	}
};
