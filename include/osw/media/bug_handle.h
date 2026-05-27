/*
 * include/osw/media/bug_handle.h
 *
 * RAII lease returned by MediaBugManager::Attach.  Destruction triggers
 * Detach.  Move-only.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_MEDIA_BUG_HANDLE_H_
#define OSW_MEDIA_BUG_HANDLE_H_

#include <cstdint>
#include <string>

#include "osw/media/purpose.h"

namespace osw::media {

class MediaBugManager;

/// Opaque handle returned by MediaBugManager::Attach.
/// Destruction calls MediaBugManager::Detach (idempotent).
/// Move-only — the manager records the active bug_id.
class BugHandle {
  public:
    BugHandle() noexcept = default;
    ~BugHandle() noexcept;  // calls manager_->Detach if attached
    BugHandle(BugHandle&&) noexcept;
    BugHandle& operator=(BugHandle&&) noexcept;
    BugHandle(const BugHandle&) = delete;
    BugHandle& operator=(const BugHandle&) = delete;

    /// True iff this handle is bound to an active bug.
    [[nodiscard]] bool attached() const noexcept;

    /// The Purpose this handle was created for.
    [[nodiscard]] Purpose purpose() const noexcept;

    /// The channel UUID this handle was created for.
    [[nodiscard]] std::string channel_uuid() const noexcept;

    /// Release ownership without detaching.  Rare — used when the
    /// handler hands the bug over to a long-lived owner.  Caller
    /// becomes responsible for calling MediaBugManager::Detach manually.
    void release() noexcept;

  private:
    friend class MediaBugManager;

    // Private constructor used by MediaBugManager::Attach.
    BugHandle(MediaBugManager* mgr,
              std::uint64_t bug_id,
              Purpose purpose,
              std::string channel_uuid) noexcept;

    MediaBugManager* manager_ = nullptr;
    std::uint64_t bug_id_ = 0;
    Purpose purpose_ = Purpose::kUnspecified;
    std::string channel_uuid_;
};

}  // namespace osw::media

#endif  // OSW_MEDIA_BUG_HANDLE_H_
