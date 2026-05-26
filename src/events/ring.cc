/*
 * src/events/ring.cc
 *
 * Implementation of osw::events::Ring. See header for the contract.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/events/ring.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace osw::events {

Ring::Ring(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

bool Ring::Push(RingEntry entry, std::uint64_t* dropped_out) noexcept {
    bool evicted = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (q_.size() >= capacity_) {
            // FIFO eviction: drop the oldest entry to make room.
            q_.pop_front();
            evicted = true;
        }
        // Track the highest seq we have EVER seen so SnapshotFromSeq
        // can distinguish "fresh ring" from "ring whose contents were
        // evicted" (Codex W2 I-6). Monotonic-non-decreasing under mu_.
        if (entry.seq > max_seq_ever_pushed_) {
            max_seq_ever_pushed_ = entry.seq;
        }
        q_.push_back(std::move(entry));
    }
    if (evicted && dropped_out) {
        // The W2 contract specifies a counter per tier; we increment by 1
        // per eviction so the Health snapshot reflects the cumulative
        // drop count.
        ++*dropped_out;
    }
    // Wake exactly one consumer — the broadcaster pulls in batches, so
    // notify_one() is sufficient and avoids the thundering-herd cost of
    // notify_all() when many producers fire in close succession.
    cv_.notify_one();
    return !evicted;
}

std::optional<RingEntry> Ring::TryPop() noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.empty()) {
        return std::nullopt;
    }
    RingEntry e = std::move(q_.front());
    q_.pop_front();
    // I-4: notify the parent RingSet when the ring transitions to empty
    // so Module::Shutdown's drain wait wakes without a 10ms busy-poll.
    if (q_.empty() && drain_notifier_) {
        drain_notifier_();
    }
    return e;
}

std::vector<RingEntry> Ring::WaitAndPopBatch(std::size_t max_batch,
                                             std::chrono::milliseconds timeout) noexcept {
    std::unique_lock<std::mutex> lk(mu_);
    if (q_.empty() && !closed_.load(std::memory_order_acquire)) {
        // Block until the producer signals OR Close() broadcasts OR
        // the timeout expires.
        cv_.wait_for(lk, timeout, [this]() noexcept {
            return !q_.empty() || closed_.load(std::memory_order_acquire);
        });
    }
    std::vector<RingEntry> out;
    if (q_.empty()) {
        // Either closed-empty or timed-out empty. The broadcaster
        // distinguishes via IsClosed().
        return out;
    }
    out.reserve(std::min(max_batch, q_.size()));
    while (!q_.empty() && out.size() < max_batch) {
        out.push_back(std::move(q_.front()));
        q_.pop_front();
    }
    // I-4: notify the parent RingSet when the batch leaves the ring
    // empty. Single notification per drain transition — cheaper than
    // notify-on-every-pop and matches what Module::Shutdown actually
    // wants to wake on (the "is everything drained?" condition).
    if (q_.empty() && drain_notifier_) {
        drain_notifier_();
    }
    return out;
}

Ring::ReplaySnapshot Ring::SnapshotFromSeq(std::uint64_t since_seq) const noexcept {
    ReplaySnapshot snap;
    std::lock_guard<std::mutex> lk(mu_);

    if (q_.empty()) {
        // Empty ring — distinguish two cases (Codex W2 I-6):
        //   (a) fresh ring (no Push ever happened):
        //         max_seq_ever_pushed_ == 0. since_seq is meaningless
        //         against an empty fresh ring; we attach at HEAD and
        //         the next produced event is delivered live. Report
        //         found_in_window=true.
        //   (b) drained ring (Push happened, all entries since
        //         evicted): max_seq_ever_pushed_ > 0. If the client
        //         asked to resume at a seq below max_seq_ever_pushed_,
        //         their resume point was definitely evicted (any
        //         entry whose seq we ever observed has since been
        //         removed). Report found_in_window=false so the
        //         handler returns RESOURCE_EXHAUSTED.
        //
        // Edge case: since_seq >= max_seq_ever_pushed_ on a drained
        // ring still means "caught up; just live-tail" — in window.
        if (max_seq_ever_pushed_ == 0 || since_seq >= max_seq_ever_pushed_) {
            snap.found_in_window = true;
        } else {
            snap.found_in_window = false;
            snap.current_max_seq = max_seq_ever_pushed_;
        }
        return snap;
    }

    snap.current_min_seq = q_.front().seq;
    snap.current_max_seq = q_.back().seq;

    if (since_seq == 0) {
        // Live tail only — no replay.
        snap.found_in_window = true;
        return snap;
    }

    // since_seq must satisfy: ring contains at least one entry with
    // seq > since_seq AND the requested resume point (since_seq+1)
    // is still in the window. Equivalently: since_seq + 1 >= min_seq.
    //
    // If since_seq + 1 < min_seq, the requested resume point has been
    // evicted. The handler returns RESOURCE_EXHAUSTED.
    if (since_seq + 1 < snap.current_min_seq) {
        snap.found_in_window = false;
        return snap;
    }
    snap.found_in_window = true;

    // Slice entries whose seq > since_seq. Because the ring is FIFO and
    // sequences are monotonic per tier, this is a contiguous tail of q_.
    // We linear-scan from the front to find the first qualifying entry;
    // q_ is bounded so this is O(capacity) worst case (acceptable).
    snap.entries.reserve(q_.size());
    for (const auto& e : q_) {
        if (e.seq > since_seq) {
            snap.entries.push_back(e);  // shared_ptr<const string> shallow-copies
        }
    }
    return snap;
}

std::size_t Ring::Size() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
}

void Ring::Close() noexcept {
    closed_.store(true, std::memory_order_release);
    cv_.notify_all();
}

void Ring::SetDrainNotifier(std::function<void()> notifier) noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    drain_notifier_ = std::move(notifier);
}

}  // namespace osw::events
