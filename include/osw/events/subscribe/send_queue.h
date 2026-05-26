/*
 * include/osw/events/subscribe/send_queue.h
 *
 * osw::events::SendQueue — per-subscriber bounded outbox.
 *
 * The broadcaster pushes envelope bytes onto a subscriber's SendQueue
 * (after filter match); the subscriber's gRPC writer thread pops and
 * calls grpc::ServerWriter::Write. When the queue is full, the
 * broadcaster:
 *   - increments the per-subscriber `dropped_count`
 *   - flips the `close_flag` so the writer thread exits on next pop
 *
 * The W2 contract specifies the kick-on-overflow path: a slow
 * subscriber gets booted with RESOURCE_EXHAUSTED rather than
 * backpressuring the broadcaster — backpressure would propagate to
 * EVERY subscriber and to the FS dispatch threads. Kicking the slow
 * one keeps the fast subscribers fed.
 *
 * Lock order (W2-wide):
 *   tier ring mu → subscriber send queue mu.
 *
 * SendQueue::TryPush is called by the broadcaster while it holds NO
 * ring lock (it's already moved the entry out of the ring) — but the
 * broadcaster MUST never re-acquire the ring mu while holding the
 * send-queue mu. SendQueue::WaitAndPop is called by the writer thread.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_EVENTS_SUBSCRIBE_SEND_QUEUE_H_
#define OSW_EVENTS_SUBSCRIBE_SEND_QUEUE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace osw::events {

/// The bounded outbox for a single subscriber.
class SendQueue {
  public:
    explicit SendQueue(std::size_t capacity);

    SendQueue(const SendQueue&) = delete;
    SendQueue& operator=(const SendQueue&) = delete;

    /// Attempt to enqueue. Returns:
    ///   - true: enqueued successfully.
    ///   - false: queue full; nothing enqueued. Caller must trigger
    ///            the subscriber's kick path.
    /// Never blocks. Will return false (without enqueue) if Close()
    /// has already been called.
    bool TryPush(std::shared_ptr<const std::string> bytes) noexcept;

    /// Pop one entry, blocking up to `timeout` for one to arrive.
    /// Returns std::nullopt on timeout-empty OR closed-empty.
    /// The writer thread distinguishes by checking IsClosed() after.
    [[nodiscard]] std::optional<std::shared_ptr<const std::string>> WaitAndPop(
        std::chrono::milliseconds timeout) noexcept;

    /// Close the queue. Broadcasts the condvar so any waiting WaitAndPop
    /// returns promptly. Idempotent.
    void Close() noexcept;

    [[nodiscard]] bool IsClosed() const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }

  private:
    const std::size_t capacity_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<const std::string>> q_;  // guarded by mu_
    std::atomic<bool> closed_{false};
};

}  // namespace osw::events

#endif  // OSW_EVENTS_SUBSCRIBE_SEND_QUEUE_H_
