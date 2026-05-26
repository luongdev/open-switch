/*
 * include/osw/core/module.h
 *
 * osw::Module — module-wide singleton owning Config, Health,
 * Lifecycle, and (W1) the gRPC control server. Constructed in
 * mod_open_switch_load; destroyed in mod_open_switch_shutdown.
 *
 * W1 deliberately keeps the Module surface narrow:
 *   - Load(pool, interface) — initialise log default sink, load config,
 *     validate, store FS interface ptr, build Health, build Lifecycle,
 *     transition to Serving, start gRPC server.
 *   - Shutdown() — drain (W1: just flip Lifecycle flag), stop gRPC
 *     server, mark stopped, release resources.
 *
 * The singleton uses a static-local for thread-safe lazy init at first
 * Instance() call. mod_open_switch.cc calls Instance().Load(...) in
 * SWITCH_MODULE_LOAD_FUNCTION.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CORE_MODULE_H_
#define OSW_CORE_MODULE_H_

#include <memory>
#include <mutex>

#include "osw/core/config.h"
#include "osw/core/lifecycle.h"
#include "osw/observability/health.h"

// Forward-declare FreeSWITCH opaque types; mod_open_switch.cc includes
// <switch.h> for the actual definitions. Module's header consumers
// (other osw::* internals that just need to call Instance().something)
// can include this without <switch.h>.
extern "C" {
struct apr_pool_t;
using switch_memory_pool_t = apr_pool_t;
struct switch_loadable_module_interface;
using switch_loadable_module_interface_t = switch_loadable_module_interface;
}

namespace osw {

namespace control {
class GrpcServer;  // forward; defined in osw/control/server.h
}

namespace events {
class Binder;          // forward; defined in osw/events/binder.h
class Broadcaster;     // forward; defined in osw/events/subscribe/broadcaster.h
class RingSet;         // forward; defined in osw/events/binder.h
class TierClassifier;  // forward; defined in osw/events/tier.h
}  // namespace events

class Module {
  public:
    /// Returns the singleton instance (lazy-initialised on first call).
    static Module& Instance() noexcept;

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    Module(Module&&) = delete;
    Module& operator=(Module&&) = delete;

    /// Initialises everything. Called exactly once from
    /// mod_open_switch_load. Returns true on success; false otherwise.
    /// On failure the caller returns SWITCH_STATUS_GENERR to FS and
    /// the module is unloaded.
    bool Load(switch_memory_pool_t* pool, switch_loadable_module_interface_t* iface) noexcept;

    /// Initiates drain + tears down everything. Called exactly once
    /// from mod_open_switch_shutdown. Returns true if shutdown was
    /// clean.
    bool Shutdown() noexcept;

    // --- Accessors (used by handlers + tests) ---------------------------
    [[nodiscard]] const Config& GetConfig() const noexcept { return config_; }
    [[nodiscard]] Health& GetHealth() noexcept { return health_; }
    [[nodiscard]] const Health& GetHealth() const noexcept { return health_; }
    [[nodiscard]] Lifecycle& GetLifecycle() noexcept { return lifecycle_; }
    [[nodiscard]] const Lifecycle& GetLifecycle() const noexcept { return lifecycle_; }

  private:
    Module() noexcept;
    ~Module() noexcept;

    std::mutex mu_;  // serialises Load/Shutdown
    bool loaded_ = false;
    switch_memory_pool_t* pool_ = nullptr;
    switch_loadable_module_interface_t* iface_ = nullptr;
    Config config_;
    Health health_;
    Lifecycle lifecycle_;
    std::unique_ptr<control::GrpcServer> grpc_server_;

    // W2 event-plane subsystems. Construction order in Module::Load:
    //   1. classifier_  (FS-agnostic; built from tier rules)
    //   2. rings_       (per-tier MPSC FIFO rings)
    //   3. binder_      (switch_event_bind shim; Init() registers with FS)
    //   4. broadcaster_ (3 worker threads draining rings into subscribers)
    // Destruction order is the reverse: broadcaster.Stop() → binder.Stop()
    // → rings.~RingSet → classifier.~TierClassifier. See Module::Shutdown.
    std::unique_ptr<events::TierClassifier> classifier_;
    std::unique_ptr<events::RingSet> rings_;
    std::unique_ptr<events::Binder> binder_;
    std::unique_ptr<events::Broadcaster> broadcaster_;
};

}  // namespace osw

#endif  // OSW_CORE_MODULE_H_
