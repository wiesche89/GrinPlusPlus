#pragma once

#include <PMMR/Common/SegmentType.h>
#include <PMMR/PIBDParams.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class SegmentRequestTracker
{
public:
	struct PendingRequest
	{
		SegmentTypeIdentifier segment;
		std::string peerId;
		std::chrono::steady_clock::time_point firstRequestedAt;
		std::chrono::steady_clock::time_point lastRequestedAt;
		uint32_t attempts;
	};

	bool IsPending(const SegmentTypeIdentifier& segment) const
	{
		return std::any_of(
			m_pending.begin(),
			m_pending.end(),
			[&segment](const PendingRequest& request) { return request.segment == segment; });
	}

	bool IsPendingFrom(const SegmentTypeIdentifier& segment, const std::string& peerId) const
	{
		return std::any_of(
			m_pending.begin(),
			m_pending.end(),
			[&segment, &peerId](const PendingRequest& request) {
				return request.segment == segment && request.peerId == peerId;
			});
	}

	std::optional<PendingRequest> GetPendingRequest(const SegmentTypeIdentifier& segment) const
	{
		auto iter = std::find_if(
			m_pending.begin(),
			m_pending.end(),
			[&segment](const PendingRequest& request) { return request.segment == segment; });
		if (iter == m_pending.end()) {
			return std::nullopt;
		}

		return *iter;
	}

	bool CanRetry(const SegmentTypeIdentifier& segment, const std::chrono::seconds retryAfter) const
	{
		const std::optional<PendingRequest> request = GetPendingRequest(segment);
		return request.has_value() && std::chrono::steady_clock::now() - request->lastRequestedAt >= retryAfter;
	}

	bool CanHedge(const SegmentTypeIdentifier& segment, const std::chrono::seconds hedgeAfter) const
	{
		return CanRetry(segment, hedgeAfter);
	}

	void AddOrRefresh(const SegmentTypeIdentifier& segment, std::string peerId)
	{
		const auto now = std::chrono::steady_clock::now();
		auto iter = std::find_if(
			m_pending.begin(),
			m_pending.end(),
			[&segment](const PendingRequest& request) { return request.segment == segment; });
		if (iter == m_pending.end()) {
			m_pending.push_back(PendingRequest{ segment, std::move(peerId), now, now, 1 });
		} else {
			iter->peerId = std::move(peerId);
			iter->lastRequestedAt = now;
			++iter->attempts;
		}
	}

	void MarkReceived(const SegmentTypeIdentifier& segment)
	{
		m_pending.erase(
			std::remove_if(
				m_pending.begin(),
				m_pending.end(),
				[&segment](const PendingRequest& request) { return request.segment == segment; }),
			m_pending.end());
	}

	std::vector<PendingRequest> GetTimedOutRequests(
		const std::chrono::seconds timeout = std::chrono::seconds(PIBD::SEGMENT_REQUEST_TIMEOUT_SECS)) const
	{
		const auto now = std::chrono::steady_clock::now();
		std::vector<PendingRequest> timedOut;
		for (const PendingRequest& request : m_pending) {
			if (now - request.firstRequestedAt >= timeout) {
				timedOut.push_back(request);
			}
		}

		return timedOut;
	}

	void RemoveTimedOutRequests(
		const std::chrono::seconds timeout = std::chrono::seconds(PIBD::SEGMENT_REQUEST_TIMEOUT_SECS))
	{
		const auto now = std::chrono::steady_clock::now();
		m_pending.erase(
			std::remove_if(
				m_pending.begin(),
				m_pending.end(),
				[now, timeout](const PendingRequest& request) { return now - request.firstRequestedAt >= timeout; }),
			m_pending.end());
	}

	void Clear()
	{
		m_pending.clear();
	}

	const std::vector<PendingRequest>& GetPendingRequests() const noexcept { return m_pending; }

private:
	std::vector<PendingRequest> m_pending;
};
