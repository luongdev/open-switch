/*
 * src/media/bug_handle.cc — BugHandle RAII implementation.
 *
 * The handle holds a raw pointer to the owning MediaBugManager and a
 * bug_id.  On destruction the handle calls manager_->Detach(bug_id_)
 * which is idempotent and thread-safe.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/media/bug_handle.h"

#include "osw/media/bug_manager.h"

namespace osw::media {

BugHandle::BugHandle(MediaBugManager* mgr,
                     std::uint64_t bug_id,
                     Purpose purpose,
                     std::string channel_uuid) noexcept
    : manager_(mgr), bug_id_(bug_id), purpose_(purpose), channel_uuid_(std::move(channel_uuid)) {}

BugHandle::~BugHandle() noexcept {
    if (manager_ != nullptr) {
        manager_->Detach(bug_id_);
    }
}

BugHandle::BugHandle(BugHandle&& other) noexcept
    : manager_(other.manager_),
      bug_id_(other.bug_id_),
      purpose_(other.purpose_),
      channel_uuid_(std::move(other.channel_uuid_)) {
    other.manager_ = nullptr;
    other.bug_id_ = 0;
    other.purpose_ = Purpose::kUnspecified;
}

BugHandle& BugHandle::operator=(BugHandle&& other) noexcept {
    if (this != &other) {
        // Detach current bug before taking ownership of other.
        if (manager_ != nullptr) {
            manager_->Detach(bug_id_);
        }
        manager_ = other.manager_;
        bug_id_ = other.bug_id_;
        purpose_ = other.purpose_;
        channel_uuid_ = std::move(other.channel_uuid_);
        other.manager_ = nullptr;
        other.bug_id_ = 0;
        other.purpose_ = Purpose::kUnspecified;
    }
    return *this;
}

bool BugHandle::attached() const noexcept {
    return manager_ != nullptr && bug_id_ != 0;
}

Purpose BugHandle::purpose() const noexcept {
    return purpose_;
}

std::string BugHandle::channel_uuid() const noexcept {
    return channel_uuid_;
}

void BugHandle::release() noexcept {
    manager_ = nullptr;
    bug_id_ = 0;
    purpose_ = Purpose::kUnspecified;
    channel_uuid_.clear();
}

}  // namespace osw::media
