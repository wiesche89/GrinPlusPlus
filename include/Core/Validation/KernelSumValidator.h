#pragma once

#include <Core/Exceptions/BadDataException.h>
#include <Crypto/Crypto.h>
#include <Crypto/Models/Commitment.h>
#include <Crypto/Models/BlindingFactor.h>
#include <Core/Models/BlockSums.h>
#include <Core/Models/TransactionBody.h>
#include <Common/Util/FunctionalUtil.h>
#include <Common/Logger.h>
#include <cstddef>
#include <optional>

class KernelSumValidator
{
public:
	// Verify the sum of the kernel excesses equals the sum of the outputs, taking into account both the kernel_offset and overage.
	static BlockSums ValidateKernelSums(
		const TransactionBody& transactionBody,
		const int64_t overage,
		const BlindingFactor& kernelOffset,
		const std::optional<BlockSums>& blockSumsOpt)
	{
		// gather the commitments
		auto getInputCommitments = [](TransactionInput& input) -> Commitment { return input.GetCommitment(); };
		std::vector<Commitment> inputCommitments = FunctionalUtil::map<std::vector<Commitment>>(transactionBody.GetInputs(), getInputCommitments);

		auto getOutputCommitments = [](TransactionOutput& output) -> Commitment { return output.GetCommitment(); };
		std::vector<Commitment> outputCommitments = FunctionalUtil::map<std::vector<Commitment>>(transactionBody.GetOutputs(), getOutputCommitments);

		auto getKernelCommitments = [](TransactionKernel& kernel) -> Commitment { return kernel.GetExcessCommitment(); };
		std::vector<Commitment> kernelCommitments = FunctionalUtil::map<std::vector<Commitment>>(transactionBody.GetKernels(), getKernelCommitments);

		return ValidateKernelSums(inputCommitments, outputCommitments, kernelCommitments, overage, kernelOffset, blockSumsOpt);
	}

	static BlockSums ValidateKernelSums(
		const std::vector<Commitment>& inputs,
		const std::vector<Commitment>& outputs,
		const std::vector<Commitment>& kernels,
		const int64_t overage,
		const BlindingFactor& kernelOffset,
		const std::optional<BlockSums>& blockSumsOpt)
	{
		std::vector<Commitment> additionalInputs;
		std::vector<Commitment> additionalOutputs;
		if (overage > 0)
		{
			additionalOutputs.push_back(Crypto::CommitTransparent(overage));
		}
		else if (overage < 0)
		{
			additionalInputs.push_back(Crypto::CommitTransparent(0 - overage));
		}

		if (blockSumsOpt.has_value())
		{
			additionalOutputs.push_back(blockSumsOpt.value().GetOutputSum());
		}

		// Sum all input|output|overage commitments.
		Commitment utxoSum = AddCommitmentsChunked(
			outputs,
			inputs,
			additionalOutputs,
			additionalInputs,
			"UTXO"
		);

		// Sum the kernel excesses accounting for the kernel offset.
		std::vector<Commitment> additionalKernels;
		if (blockSumsOpt.has_value())
		{
			additionalKernels.push_back(blockSumsOpt.value().GetKernelSum());
		}

		Commitment kernelSum = AddCommitmentsChunked(
			kernels,
			std::vector<Commitment>(),
			additionalKernels,
			std::vector<Commitment>(),
			"kernel"
		);
		Commitment kernelSumPlusOffset = AddKernelOffset(kernelSum, kernelOffset);
		if (utxoSum != kernelSumPlusOffset) {
			LOG_ERROR_F(
				"UTXO sum {} does not match kernel sum plus offset {}. Kernel sum is {} and offset is {}.",
				utxoSum,
				kernelSumPlusOffset,
				kernelSum,
				kernelOffset.ToHex()
			);
			throw BAD_DATA_EXCEPTION(EBanReason::BadBlock, "UTXO sum does not match kernel sum plus offset");
		}

		return BlockSums(utxoSum, kernelSum);
	}

private:
	static constexpr size_t COMMITMENT_SUM_CHUNK_SIZE = 65536;

	static Commitment AddCommitmentsChunked(
		const std::vector<Commitment>& positive,
		const std::vector<Commitment>& negative,
		const std::vector<Commitment>& additionalPositive,
		const std::vector<Commitment>& additionalNegative,
		const char* label)
	{
		const size_t totalPositive = positive.size() + additionalPositive.size();
		const size_t totalNegative = negative.size() + additionalNegative.size();
		const size_t total = totalPositive + totalNegative;
		if (total <= COMMITMENT_SUM_CHUNK_SIZE)
		{
			std::vector<Commitment> positiveCommitments;
			positiveCommitments.reserve(totalPositive);
			positiveCommitments.insert(positiveCommitments.end(), positive.cbegin(), positive.cend());
			positiveCommitments.insert(positiveCommitments.end(), additionalPositive.cbegin(), additionalPositive.cend());

			std::vector<Commitment> negativeCommitments;
			negativeCommitments.reserve(totalNegative);
			negativeCommitments.insert(negativeCommitments.end(), negative.cbegin(), negative.cend());
			negativeCommitments.insert(negativeCommitments.end(), additionalNegative.cbegin(), additionalNegative.cend());

			return Crypto::AddCommitments(positiveCommitments, negativeCommitments);
		}

		LOG_DEBUG_F(
			"Summing {} commitments in chunks: positives={}, negatives={}, chunk_size={}",
			label,
			totalPositive,
			totalNegative,
			COMMITMENT_SUM_CHUNK_SIZE
		);
		LoggerAPI::Flush();

		std::vector<Commitment> positivePartials;
		std::vector<Commitment> negativePartials;
		AppendChunkSums(positive, additionalPositive, positivePartials, label, "positive");
		AppendChunkSums(negative, additionalNegative, negativePartials, label, "negative");

		LOG_DEBUG_F(
			"Combining {} partial sums: positives={}, negatives={}",
			label,
			positivePartials.size(),
			negativePartials.size()
		);
		LoggerAPI::Flush();

		Commitment sum = Crypto::AddCommitments(positivePartials, negativePartials);

		LOG_DEBUG_F("Finished summing {} commitments", label);
		LoggerAPI::Flush();
		return sum;
	}

	static void AppendChunkSums(
		const std::vector<Commitment>& commitments,
		const std::vector<Commitment>& additionalCommitments,
		std::vector<Commitment>& partialSums,
		const char* label,
		const char* side)
	{
		std::vector<Commitment> chunk;
		chunk.reserve(COMMITMENT_SUM_CHUNK_SIZE);

		size_t processed = 0;
		const size_t total = commitments.size() + additionalCommitments.size();
		auto appendCommitment = [&chunk, &partialSums, &processed, total, label, side](const Commitment& commitment) {
			chunk.push_back(commitment);
			++processed;
			if (chunk.size() >= COMMITMENT_SUM_CHUNK_SIZE)
			{
				partialSums.push_back(Crypto::AddCommitments(chunk, std::vector<Commitment>()));
				chunk.clear();

				if (partialSums.size() % 64 == 0 || processed == total)
				{
					LOG_DEBUG_F(
						"Summed {} {} commitment chunks: processed={}/{} partials={}",
						label,
						side,
						processed,
						total,
						partialSums.size()
					);
					LoggerAPI::Flush();
				}
			}
		};

		for (const Commitment& commitment : commitments)
		{
			appendCommitment(commitment);
		}

		for (const Commitment& commitment : additionalCommitments)
		{
			appendCommitment(commitment);
		}

		if (!chunk.empty())
		{
			partialSums.push_back(Crypto::AddCommitments(chunk, std::vector<Commitment>()));
		}

		if (total > 0)
		{
			LOG_DEBUG_F(
				"Summed {} {} commitments: processed={}/{} partials={}",
				label,
				side,
				processed,
				total,
				partialSums.size()
			);
			LoggerAPI::Flush();
		}
	}

	static Commitment AddKernelOffset(const Commitment& kernelSum, const BlindingFactor& totalKernelOffset)
	{
		if (totalKernelOffset == CBigInteger<32>::ValueOf(0)) {
			return kernelSum;
		}

		// Add the commitments along with the commit to zero built from the offset
		Commitment offset_commit = Crypto::CommitBlinded((uint64_t)0, totalKernelOffset);
		return Crypto::AddCommitments(
			std::vector<Commitment>{ kernelSum, std::move(offset_commit) },
			std::vector<Commitment>()
		);
	}
};
