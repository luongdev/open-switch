/*
 * src/events/subscribe/send_queue.cc
 *
 * Implementation of osw::events::SendQueue. See header.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/events/subscribe/send_queue.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace osw::events {

SendQueue::SendQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

bool SendQueue::TryPush(std::shared_ptr<const std::string> bytes) noexcept {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_.load(std::memory_order_acquire)) {
            return false;
        }
        if (q_.size() >= capacity_) {
            return false;  // caller kicks on full
        }
        q_.push_back(std::move(bytes));
    }
    cv_.notify_one();
    return true;
}

std::optional<std::shared_ptr<const std::string>> SendQueue::WaitAndPop(
    std::chrono::milliseconds timeout) noexcept {
    std::unique_lock<std::mutex> lk(mu_);
    if (q_.empty() && !closed_.load(std::memory_order_acquire)) {
        cv_.wait_for(lk, timeout, [this]() noexcept {
            return !q_.empty() || closed_.load(std::memory_order_acquire);
        });
    }
    if (q_.empty()) {
        return std::nullopt;
    }
    auto out = std::move(q_.front());
    q_.pop_front();
    return out;
}

void SendQueue::Close() noexcept {
    closed_.store(true, std::memory_order_release);
    cv_.notify_all();
}

bool SendQueue::IsClosed() const noexcept {
    return closed_.load(std::memory_order_acquire);
}

std::size_t SendQueue::Size() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
}

}  // namespace osw::events
