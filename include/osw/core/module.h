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
#include "osw/observability/prometheus.h"

// Forward-declare FreeSWITCH opaque types; mod_open_switch.cc includes
// <switch.h> for the actual definitions. Module's header consumers
// (other osw::* internals that just need to call Instance().something)
// can include this without <switch.h>.
//
// IMPORTANT: the typedef names below must match <switch_types.h> exactly
// (`typedef struct fspr_pool_t switch_memory_pool_t;`). The earlier
// version used `apr_pool_t`, which compiled in isolation but conflicted
// when a TU pulled in both this header and <switch.h>.
extern "C" {
typedef struct fspr_pool_t switch_memory_pool_t;
typedef struct switch_loadable_module_interface switch_loadable_module_interface_t;
}

namespace osw {

namespace control {
class ActiveBots;          // forward; defined in osw/control/active_bots.h
class ActiveMediaStreams;  // forward; defined in osw/control/active_media_streams.h
class GrpcServer;          // forward; defined in osw/control/server.h
class IdempotencyCache;    // forward; defined in osw/control/idempotency_cache.h
class RpcMetrics;          // forward; defined in osw/control/rpc_metrics.h
}  // namespace control

namespace media {
class MediaBugManager;        // forward; defined in osw/media/bug_manager.h
class SilenceDriverRegistry;  // forward; defined in osw/media/silence_driver.h
}  // namespace media

namespace events {
class Binder;          // forward; defined in osw/events/binder.h
class Broadcaster;     // forward; defined in osw/events/subscribe/broadcaster.h
class RingSet;         // forward; defined in osw/events/binder.h
class TierClassifier;  // forward; defined in osw/events/tier.h
}  // namespace events

namespace observability {
class HealthMetrics;  // forward; defined in osw/observability/health_metrics.h
class MetricsServer;  // forward; defined in osw/observability/metrics_server.h
}  // namespace observability

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

    // W4C/W5A observability plane. Construction order in Module::Load:
    //   1. prometheus_registry_  (owns all metric objects; module-lifetime)
    //   2. health_metrics_       (registers Health-derived gauges into registry)
    //   3. rpc_metrics_          (registers per-RPC counters/histograms)
    //   4. metrics_server_       (HTTP /metrics endpoint; started if metrics_enabled)
    // Destruction order: metrics_server_.Stop() first (it reads registry during
    // scrapes), then the adapters, then registry_ last.
    std::unique_ptr<observability::prometheus::Registry> prometheus_registry_;
    std::unique_ptr<observability::HealthMetrics> health_metrics_;
    std::unique_ptr<control::RpcMetrics> rpc_metrics_;
    std::unique_ptr<observability::MetricsServer> metrics_server_;

    // W5B idempotency cache. Constructed from config in Module::Load and
    // injected into ControlServiceSkeleton via GrpcServer::SetIdempotencyCache.
    std::unique_ptr<control::IdempotencyCache> idempotency_cache_;

    // W6C media-plane subsystems. Construction order in Module::Load step 5.4:
    //   1. active_media_streams_ (per-stream BugHandle + StreamClient registry)
    //   2. silence_driver_registry_ (W6.6 write-side parked-channel driver)
    //   3. bug_manager_         (MediaBugManager; CS_DESTROY handler registered here)
    // Destruction order in Module::Shutdown step 7.5 (after gRPC server drained):
    //   active_media_streams_ first (TearDown calls client->Close(), then
    //   bugs.clear() which calls bug_manager_->Detach), then silence registry,
    //   then bug_manager_.
    std::unique_ptr<media::MediaBugManager> bug_manager_;
    std::unique_ptr<media::SilenceDriverRegistry> silence_driver_registry_;
    std::unique_ptr<control::ActiveBots> active_bots_;
    std::unique_ptr<control::ActiveMediaStreams> active_media_streams_;

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
