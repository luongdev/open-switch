/*
 * include/osw/raii/xml_node.h
 *
 * osw::XmlNode — RAII pairing of
 *   switch_xml_open_cfg(file, &node, params) / switch_xml_free(root).
 *
 * Per FREESWITCH-FACTS FF-015:
 *   - switch_xml_open_cfg returns the root XML* or NULL. The root is
 *     reference-counted; switch_xml_free decrements; only the
 *     last-decrement actually deallocates buffers.
 *   - switch_xml_free(NULL) is a safe no-op.
 *
 * W1 ships this helper but adds NO callers of switch_xml_open_*.
 * Config loading goes through switch_xml_config_parse_module_settings
 * (FF-013) which manages XML lifetime internally. The helper exists
 * because designs/memory-management.md requires it and Codex review
 * expects the full RAII toolkit. W4 (SIGHUP hot-reload) may add the
 * first production user when it walks raw XML directly.
 *
 * Source: designs/memory-management.md §"osw::XmlNode" — this is a
 * thin RAII wrapper around switch_xml_free.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_RAII_XML_NODE_H_
#define OSW_RAII_XML_NODE_H_

#include "osw/raii/fs_api.h"

namespace osw {

/// RAII guard for a FreeSWITCH switch_xml_t root handle.
///
/// Construction either takes an already-opened root (use `XmlNode::adopt(root)`)
/// or opens a config file via the ctor that mirrors switch_xml_open_cfg
/// (FF-015). Destruction calls switch_xml_free on the root iff non-null.
///
/// `cfg()` returns the `<configuration>` child captured at open time
/// (only meaningful when opened via the open_cfg ctor).
///
/// Cited FACTs:
/// - FF-015 — switch_xml_open_cfg / switch_xml_free refcount + NULL-safe.
class XmlNode {
 public:
    /// Default-constructed: empty.
    XmlNode() noexcept = default;

    /// Opens `file_path` via switch_xml_open_cfg. On failure the guard
    /// holds null and operator bool() is false.
    ///
    /// `params` may be null. When non-null it is forwarded to the FS
    /// XML facility to drive parameterised config lookup; the guard
    /// does NOT take ownership of `params`.
    XmlNode(const char* file_path, switch_event_t* params) noexcept
        : root_(::osw::raii::fs::XmlOpenCfg(file_path, &cfg_, params)) {}

    /// Adopts an existing root handle. Useful when the open was done
    /// elsewhere (e.g., via switch_xml_open_root).
    static XmlNode adopt(switch_xml_t root) noexcept {
        XmlNode n;
        n.root_ = root;
        n.cfg_  = nullptr;
        return n;
    }

    ~XmlNode() noexcept {
        if (root_) {
            ::osw::raii::fs::XmlFree(root_);
        }
    }

    XmlNode(const XmlNode&)            = delete;
    XmlNode& operator=(const XmlNode&) = delete;

    XmlNode(XmlNode&& other) noexcept : root_(other.root_), cfg_(other.cfg_) {
        other.root_ = nullptr;
        other.cfg_  = nullptr;
    }

    XmlNode& operator=(XmlNode&& other) noexcept {
        if (this != &other) {
            if (root_) {
                ::osw::raii::fs::XmlFree(root_);
            }
            root_       = other.root_;
            cfg_        = other.cfg_;
            other.root_ = nullptr;
            other.cfg_  = nullptr;
        }
        return *this;
    }

    /// Root XML handle. null if the open failed or after release().
    [[nodiscard]] switch_xml_t root() const noexcept { return root_; }

    /// `<configuration>` child captured at open time. null if not
    /// opened via the cfg ctor.
    [[nodiscard]] switch_xml_t cfg() const noexcept { return cfg_; }

    /// True iff the guard holds a non-null root handle.
    explicit operator bool() const noexcept { return root_ != nullptr; }

    /// Frees the root eagerly. Safe to call multiple times.
    void reset() noexcept {
        if (root_) {
            ::osw::raii::fs::XmlFree(root_);
            root_ = nullptr;
            cfg_  = nullptr;
        }
    }

    /// Hand the root to the caller; the guard becomes empty without
    /// freeing.
    [[nodiscard]] switch_xml_t release() noexcept {
        switch_xml_t r = root_;
        root_          = nullptr;
        cfg_           = nullptr;
        return r;
    }

 private:
    switch_xml_t root_ = nullptr;
    switch_xml_t cfg_  = nullptr;
};

}  // namespace osw

#endif  // OSW_RAII_XML_NODE_H_
