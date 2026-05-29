/*
 * src/core/module.cc — osw::Module singleton implementation.
 *
 * This TU includes <switch.h> because Module::Load receives the FS
 * memory pool and module interface pointer, and it calls the FS
 * version-string accessors to populate Health.
 *
 * FACTs cited:
 *   - FF-014 — switch_loadable_module_create_module_interface pool
 *     ownership (the caller — mod_open_switch.cc — passes us the
 *     already-created interface*; we store it as a non-owning view).
 *   - FF-018 — switch_event_bind lifecycle; consumed by W2 Binder.
 *
 * W2 wiring (Module::Load AFTER grpc_server_->Start()):
 *   - TierClassifier from operator-config tier-rules (W2 uses
 *     MakeDefaultRules() for now; operator overrides land with the
 *     config-extension commit).
 *   - RingSet(config.event_ring_capacity_tier{1,2,3}).
 *   - Binder(rings, classifier, envelope_cfg, node_id, &health_)
 *     and Binder::Init() — registers switch_event_bind for
 *     SWITCH_EVENT_ALL (FF-018). Producers start delivering after.
 *   - Broadcaster(rings, &health_); Broadcaster::Start() — three
 *     worker threads, one per tier.
 *   - GrpcServer::SetEventPlane(broadcaster, rings, max_subscribers,
 *     send_queue_capacity) so the SubscribeEvents handler sees the
 *     event plane.
 *   - osw::audit::Emit("module_loaded", ...) — re-enters the event
 *     pipeline via the Binder we just installed.
 *
 * W2 teardown (Module::Shutdown):
 *   - Lifecycle::SignalDrain → Health::DRAINING.
 *   - Binder::Stop() — FF-018 unbind under wrlock; in-flight dispatch
 *     completes before this returns. No more producers.
 *   - Wait up to config.event_drain_timeout_seconds for rings to
 *     reach AllEmpty(). Best-effort.
 *   - Broadcaster::Stop() — closes rings, joins worker threads,
 *     kicks every subscriber with kShutdown.
 *   - If rings still non-empty after the drain deadline: log a WARN-
 *     level "module_shutdown_drain_timeout" line (FS log only — Codex
 *     W2 B-2). We deliberately do NOT call osw::audit::Emit() here:
 *     the binder is stopped by step 2, so an audit emit at this point
 *     is dead-lettered (it never enters our rings, so gRPC subscribers
 *     never see it). Operator visibility is served by the FS log line
 *     plus the tier_dropped_total counters on Health.
 *   - GrpcServer::Drain(deadline).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/core/module.h"

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <switch.h>  // FF-014 environment, switch_version_*

#include "osw/control/active_bots.h"
#include "osw/control/active_media_streams.h"
#include "osw/control/idempotency_cache.h"
#include "osw/control/rpc_metrics.h"
#include "osw/control/server.h"
#include "osw/core/config_fs.h"
#include "osw/events/binder.h"
#include "osw/events/envelope.h"
#include "osw/events/ring.h"
#include "osw/events/subscribe/broadcaster.h"
#include "osw/events/tier.h"
#include "osw/media/bug_manager.h"
#include "osw/media/silence_driver.h"
#include "osw/observability/audit.h"
#include "osw/observability/health_metrics.h"
#include "osw/observability/log.h"
#include "osw/observability/metrics_server.h"
#include "osw/observability/prometheus.h"
#include "osw/security/eavesdrop_detector.h"

namespace osw {

namespace {

constexpr const char* kSubsystem = "core";
constexpr const char* kConfigFileName = "open_switch.conf";
constexpr const char* kModuleVersion = "0.1.0";

// Compile the configured pattern strings into std::regex. Logs and
// skips any pattern that fails (Validate already rejected them; this
// is a defensive belt).
std::vector<std::regex> CompilePatterns(const std::vector<std::string>& src) {
    std::vector<std::regex> out;
    out.reserve(src.size());
    for (const auto& p : src) {
        try {
            out.emplace_back(p);
        } catch (const std::regex_error& e) {
            osw::log::Error(kSubsystem, "skipping invalid PII redaction regex: %s", e.what());
        }
    }
    return out;
}

std::string FreeSwitchVersionString() {
    // switch_version_full() returns e.g. "1.10.12-dev~20260101..." in
    // v1.10.12. We use it verbatim; the Health proto field is free-form.
    const char* v = switch_version_full();
    return v ? std::string(v) : std::string("unknown");
}

std::string ResolveNodeId() {
    // W2 default: use the host's name. Operators can override via a
    // future config knob (W2's contract reserved the slot but didn't
    // wire a knob; gethostname() is the documented default for now).
    // The EventEnvelope.node_id field is free-form so this is safe
    // even with weird hostnames; subscribers filter by exact match.
    char buf[256] = {0};
    if (gethostname(buf, sizeof(buf) - 1) == 0) {
        return std::string(buf);
    }
    return std::string("unknown-node");
}

}  // namespace

Module::Module() noexcept : lifecycle_(&health_) {}

Module::~Module() noexcept = default;

Module& Module::Instance() noexcept {
    static Module instance;
    return instance;
}

bool Module::Load(switch_memory_pool_t* pool, switch_loadable_module_interface_t* iface) noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    if (loaded_) {
        osw::log::Warn(kSubsystem, "Module::Load called twice; ignoring second call");
        return true;
    }

    try {
        pool_ = pool;
        iface_ = iface;

        // Codex W2 I-5: RAII guard for the partial-init window. If any step
        // fails or throws, the guard resets all module members to avoid
        // leaking background threads (metrics, gRPC) while the module
        // singleton stays alive in an unloaded state.
        struct LoadGuard {
            Module* mod;
            bool committed = false;
            void commit() noexcept { committed = true; }
            ~LoadGuard() noexcept {
                if (committed)
                    return;
                // Full cleanup on load failure to prevent active resource leaks.
                if (mod->broadcaster_) {
                    try {
                        mod->broadcaster_->Stop();
                    } catch (...) {
                    }
                    mod->broadcaster_.reset();
                }
                osw::security::UnbindEavesdropDetector();
                if (mod->binder_) {
                    try {
                        mod->binder_->Stop();
                    } catch (...) {
                    }
                    mod->binder_.reset();
                }
                mod->rings_.reset();
                mod->classifier_.reset();
                if (mod->metrics_server_) {
                    try {
                        mod->metrics_server_->Stop();
                    } catch (...) {
                    }
                    mod->metrics_server_.reset();
                }
                if (mod->grpc_server_) {
                    try {
                        mod->grpc_server_->Drain(std::chrono::system_clock::now() +
                                                 std::chrono::seconds(2));
                    } catch (...) {
                    }
                    mod->grpc_server_.reset();
                }
                mod->idempotency_cache_.reset();
                if (mod->bug_manager_) {
                    try {
                        mod->bug_manager_->UnregisterStateHandlers();
                    } catch (...) {
                    }
                }
                if (mod->active_bots_) {
                    mod->active_bots_->DrainAll(mod->active_media_streams_.get());
                    mod->active_bots_.reset();
                }
                mod->active_media_streams_.reset();
                if (mod->bug_manager_) {
                    mod->bug_manager_->SetSilenceDriverRegistry(nullptr);
                }
                if (mod->silence_driver_registry_) {
                    try {
                        mod->silence_driver_registry_->DrainAll();
                    } catch (...) {
                    }
                    mod->silence_driver_registry_.reset();
                }
                mod->bug_manager_.reset();
                mod->rpc_metrics_.reset();
                mod->health_metrics_.reset();
                mod->prometheus_registry_.reset();
                mod->pool_ = nullptr;
                mod->iface_ = nullptr;
            }
        };
        LoadGuard load_guard{this, false};

        // 1. Install the FS-backed default log sink so subsequent log
        //    lines actually reach switch_log_printf. log.cc keeps a
        //    null sink before this call.
        osw::log::InstallDefaultSinkForModule();

        osw::log::Info(kSubsystem, "mod_open_switch v%s loading", kModuleVersion);

        // 2. Load config. FF-013: switch_xml_config_parse_module_settings
        //    returns SUCCESS even when <settings> is missing; FALSE
        //    only when the file itself can't be opened. We treat the
        //    latter as "use compiled-in defaults" with a WARN log.
        const bool parsed = LoadConfigFromFile(kConfigFileName, config_);
        if (!parsed) {
            osw::log::Warn(kSubsystem,
                           "config file '%s' not found in conf/autoload_configs; "
                           "using compiled-in defaults",
                           kConfigFileName);
        }

        // 3. Validate.
        auto v = Validate(config_);
        if (!v.ok) {
            osw::log::Error(kSubsystem, "config validation failed: %s", v.error.c_str());
            return false;
        }

        // 4. Publish PII redaction patterns.
        osw::log::SetRedactionPatterns(CompilePatterns(config_.pii_redaction_patterns));

        // 5. Populate Health with versions.
        const std::string fs_ver = FreeSwitchVersionString();
        health_.SetVersions(kModuleVersion, fs_ver);

        // 5a. Build the observability plane: single shared prometheus::Registry,
        //     HealthMetrics adapter, RpcMetrics adapter, and the HTTP server.
        //     The registry is created here and owned by the Module for its
        //     full lifetime. All adapters hold non-owning raw pointers into it.
        prometheus_registry_ = std::make_unique<observability::prometheus::Registry>();

        health_metrics_ =
            std::make_unique<observability::HealthMetrics>(prometheus_registry_.get());

        rpc_metrics_ = std::make_unique<control::RpcMetrics>(prometheus_registry_.get());

        // 5b. Start the MetricsServer when metrics_enabled (default true).
        //     Bind failure is non-fatal: log Error and continue. Metrics are
        //     observability, not safety — a port conflict must not block load.
        if (config_.metrics_enabled) {
            observability::prometheus::Registry* reg_ptr = prometheus_registry_.get();
            observability::HealthMetrics* hm_ptr = health_metrics_.get();
            osw::Health* health_ptr = &health_;
            metrics_server_ = std::make_unique<observability::MetricsServer>(
                [reg_ptr, hm_ptr, health_ptr]() -> std::string {
                    hm_ptr->Refresh(*health_ptr);
                    return reg_ptr->Render();
                });
            if (!metrics_server_->Start(config_.metrics_bind_address, config_.metrics_port)) {
                osw::log::Error(kSubsystem,
                                "MetricsServer failed to bind on %s:%u; "
                                "continuing without metrics HTTP endpoint",
                                config_.metrics_bind_address.c_str(),
                                static_cast<unsigned>(config_.metrics_port));
                metrics_server_.reset();
            }
        } else {
            osw::log::Info(kSubsystem, "metrics_enabled=false; metrics HTTP endpoint not started");
        }

        // 6. Start gRPC server.
        grpc_server_ = std::make_unique<control::GrpcServer>(&health_);
        grpc_server_->SetVersions(kModuleVersion, fs_ver);
        grpc_server_->SetRpcMetrics(rpc_metrics_.get());

        // 5.5. Construct and inject the W5B idempotency cache BEFORE
        //      Start(): GrpcServer::SetIdempotencyCache now stashes the
        //      pointer in pending_cache_ and Start() applies it the moment
        //      the skeleton is built (Gemini W5 P3-1 fix — closes the race
        //      window where the gRPC server was accepting RPCs but the
        //      skeleton's cache pointer was still null).
        idempotency_cache_ = std::make_unique<control::IdempotencyCache>(
            static_cast<std::size_t>(config_.idempotency_cache_capacity),
            std::chrono::seconds(config_.idempotency_ttl_seconds),
            std::chrono::seconds(config_.idempotency_in_flight_max_wait_seconds));
        grpc_server_->SetIdempotencyCache(idempotency_cache_.get());

        // 5.4. Construct W6C media-plane subsystems and inject into GrpcServer
        //      BEFORE Start() so RPC threads always see valid pointers.
        //
        // W6.5 P1-003 fix: create ActiveMediaStreams FIRST so RegisterStateHandlers
        // can wire the CS_DESTROY hook to clean up both registries on hangup.
        // The hook calls ActiveMediaStreams::RemoveForChannel via the
        // function-pointer indirection (avoids a control→media→control
        // header cycle).
        active_media_streams_ = std::make_unique<control::ActiveMediaStreams>();
        active_bots_ = std::make_unique<control::ActiveBots>();
        silence_driver_registry_ = std::make_unique<media::SilenceDriverRegistry>(config_);
        bug_manager_ = std::make_unique<media::MediaBugManager>();
        bug_manager_->SetSilenceDriverRegistry(silence_driver_registry_.get());
        bug_manager_->RegisterStateHandlers(
            /*active_streams_opaque=*/static_cast<void*>(this),
            /*cleanup_fn=*/[](void* opaque, std::string_view uuid) {
                auto* mod = static_cast<osw::Module*>(opaque);
                if (mod->active_bots_) {
                    mod->active_bots_->StopByChannel(uuid, mod->active_media_streams_.get());
                }
                if (mod->active_media_streams_) {
                    mod->active_media_streams_->RemoveForChannel(uuid);
                }
            });
        grpc_server_->SetMediaBugManager(bug_manager_.get());
        grpc_server_->SetActiveMediaStreams(active_media_streams_.get());
        grpc_server_->SetActiveBots(active_bots_.get());
        grpc_server_->SetMediaConfig(&config_);

        if (!grpc_server_->Start(config_)) {
            osw::log::Error(kSubsystem, "gRPC server failed to start");
            return false;
        }

        // 7. Wire the W2 event plane. Order matters:
        //   classifier → rings → binder.Init() → broadcaster.Start()
        //   → grpc_server.SetEventPlane → audit emit module_loaded.
        // The Binder MUST be Init()'d BEFORE the broadcaster threads
        // start so the first event reaching the broadcaster has a
        // corresponding subscriber path; conversely the broadcaster
        // MUST be Start()'d before SubscribeEvents handlers can match
        // (the gRPC server is already accepting RPCs but
        // ControlServiceSkeleton::broadcaster_ is still null at this
        // point — the SubscribeEvents handler short-circuits with
        // UNIMPLEMENTED for the brief window until SetEventPlane runs).
        classifier_ = std::make_unique<events::TierClassifier>(events::MakeDefaultRules());
        rings_ = std::make_unique<events::RingSet>(config_.event_ring_capacity_tier1,
                                                   config_.event_ring_capacity_tier2,
                                                   config_.event_ring_capacity_tier3);

        const std::string node_id = ResolveNodeId();
        binder_ = std::make_unique<events::Binder>(rings_.get(),
                                                   classifier_.get(),
                                                   events::MakeDefaultEnvelopeConfig(),
                                                   node_id,
                                                   &health_);
        if (!binder_->Init()) {
            osw::log::Error(kSubsystem, "events::Binder::Init failed; aborting load");
            return false;
        }
        if (!osw::security::BindEavesdropDetector()) {
            osw::log::Warn(kSubsystem,
                           "MEDIA_BUG_START eavesdrop detector unavailable; "
                           "Layer-2 eavesdrop audit disabled");
        }

        broadcaster_ = std::make_unique<events::Broadcaster>(rings_.get(), &health_);
        broadcaster_->Start();

        grpc_server_->SetEventPlane(broadcaster_.get(),
                                    rings_.get(),
                                    config_.max_subscribers,
                                    config_.subscriber_send_queue_capacity);

        // 8. Flip lifecycle to Serving. Health status is set inside.
        lifecycle_.TransitionToServing();
        loaded_ = true;
        load_guard.commit();

        // 9. Audit emit module_loaded — re-enters the binder we just
        //    installed; lands in Tier-1 ring + flows to any subscriber
        //    that connected before us (none on first load; relevant on
        //    a SIGHUP/reload sequence).
        osw::audit::Emit("module_loaded",
                         {{"module_version", kModuleVersion},
                          {"freeswitch_version", fs_ver},
                          {"node_id", node_id}});

        osw::log::Info(kSubsystem,
                       "mod_open_switch v%s loaded; gRPC bound at %s "
                       "(node_id='%s', rings=[%u/%u/%u])",
                       kModuleVersion,
                       config_.grpc_listen_address.c_str(),
                       node_id.c_str(),
                       config_.event_ring_capacity_tier1,
                       config_.event_ring_capacity_tier2,
                       config_.event_ring_capacity_tier3);
        return true;
    } catch (const std::exception& e) {
        osw::log::Error(kSubsystem, "Module::Load threw: %s", e.what());
        return false;
    } catch (...) {
        osw::log::Error(kSubsystem, "Module::Load threw unknown exception");
        return false;
    }
}

bool Module::Shutdown() noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    if (!loaded_) {
        return true;  // never loaded; nothing to tear down
    }

    try {
        osw::log::Info(kSubsystem, "mod_open_switch shutdown initiated");

        // 1. Signal drain — Lifecycle → kDraining; Health → kDraining.
        lifecycle_.SignalDrain();
        osw::security::UnbindEavesdropDetector();

        // 2. Stop the producer side first. FF-018 unbind-under-wrlock
        //    waits for in-flight HandleEvent calls to complete before
        //    returning. After this, no new entries land in any ring.
        if (binder_) {
            binder_->Stop();
        }

        // 3. Wait up to event_drain_timeout_seconds for the rings to
        //    empty (the broadcaster's worker threads are still running;
        //    they should drain the tail of in-flight events to
        //    subscribers).
        //
        // Gemini W2.5 I-4: condvar-based wait (RingSet::WaitUntilAllEmpty)
        // replaces the previous `sleep_for(10ms)` busy-poll. The
        // broadcaster's per-tier WaitAndPopBatch fires a drain-notifier
        // when a ring transitions to empty; the condvar wakes the
        // shutdown wait promptly and the deadline still bounds the
        // wait absolutely.
        if (rings_) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(config_.event_drain_timeout_seconds);
            // WaitUntilAllEmpty returns true on full drain, false on
            // timeout. We could either use that directly or re-check
            // AllEmpty(); we re-check for the (rare) race where the
            // last drain notification raced the deadline (the function
            // already does a final post-timeout peek, but a separate
            // re-check makes the pending-event accounting below
            // straightforward).
            (void)rings_->WaitUntilAllEmpty(deadline);
            const bool pending = !rings_->AllEmpty();
            if (pending) {
                // FS-log-only audit (Codex W2 B-2). Previously this
                // called osw::audit::Emit which routes through
                // switch_event_fire → our own osw_event_handler — but
                // the binder is stopped at this point (FF-018 unbind
                // ran above) so that path is dead-lettered: the event
                // never enters our rings and gRPC SubscribeEvents
                // subscribers never see it. The audit's purpose is
                // operator visibility, which is already served by:
                //   - FS log lines (mod_logfile, mod_console)
                //   - the tier_dropped_total counters on Health
                // Renaming the emission to module_shutdown_drain_timeout
                // makes the FS-log-only semantics explicit. gRPC
                // subscribers MUST NOT rely on receiving this audit.
                osw::log::Warn(kSubsystem,
                               "module_shutdown_drain_timeout: event rings not fully drained "
                               "after %us; tier_dropped_total counters on Health reflect any "
                               "lost events",
                               config_.event_drain_timeout_seconds);
            }
        }

        // 4. Stop the broadcaster. Closes the rings (any remaining
        //    pop wakes), joins the 3 worker threads, kicks every live
        //    subscriber with kShutdown (writer loops exit; handler
        //    returns grpc::Status::OK).
        if (broadcaster_) {
            broadcaster_->Stop();
        }

        // 5. Stop the MetricsServer BEFORE draining gRPC so that no scrape
        //    thread is reading the registry during teardown. The HTTP server
        //    is observability only — stop it first.
        if (metrics_server_) {
            metrics_server_->Stop();
            metrics_server_.reset();
        }

        // 6. Drain the gRPC server. SubscribeEvents writer threads have
        //    already exited via the kShutdown kick; this catches any
        //    in-flight unary RPCs.
        if (grpc_server_) {
            const auto deadline = std::chrono::system_clock::now() +
                                  std::chrono::seconds(config_.grpc_drain_deadline_seconds);
            grpc_server_->Drain(deadline);
            grpc_server_.reset();
        }

        // 7. Tear down the event-plane subsystems in reverse-construction
        //    order.
        broadcaster_.reset();
        binder_.reset();
        rings_.reset();
        classifier_.reset();

        // 7.5. Tear down idempotency cache. The gRPC server was already
        //      drained in step 5 (no more handler threads accessing the
        //      cache), so this is safe.
        idempotency_cache_.reset();

        // 7.6. Tear down W6C media-plane subsystems.
        //      Order:
        //        (a) UnregisterStateHandlers FIRST so the CS_DESTROY hook
        //            stops pointing at the soon-to-be-destroyed registries.
        //            W6.5 P1-003 fix — was missing entirely; the hook
        //            stayed registered after module unload and would
        //            dereference freed memory if FS still had channels.
        //        (b) active_media_streams_.reset() — its TearDown calls
        //            client->Close() (joins reader thread) then bugs.clear()
        //            (calls MediaBugManager::Detach via BugHandle dtor),
        //            so bug_manager_ must still be alive at this point.
        //        (c) silence_driver_registry_->DrainAll() then reset while
        //            bug_manager_ still exists but after stream handles have
        //            detached their WRITE_REPLACE bugs.
        //        (d) bug_manager_.reset() last.  Reversing (b) and (d)
        //            would leave BugHandle dtors trying to deref a freed
        //            MediaBugManager.
        if (bug_manager_) {
            bug_manager_->UnregisterStateHandlers();
        }
        if (active_bots_) {
            active_bots_->DrainAll(active_media_streams_.get());
            active_bots_.reset();
        }
        active_media_streams_.reset();
        if (bug_manager_) {
            bug_manager_->SetSilenceDriverRegistry(nullptr);
        }
        if (silence_driver_registry_) {
            silence_driver_registry_->DrainAll();
            silence_driver_registry_.reset();
        }
        bug_manager_.reset();

        // 8. Tear down the observability plane. rpc_metrics_ and
        //    health_metrics_ hold raw pointers into prometheus_registry_;
        //    reset them before dropping the registry.
        //    (metrics_server_ was already stopped + reset in step 5.)
        rpc_metrics_.reset();
        health_metrics_.reset();
        prometheus_registry_.reset();

        // 9. Mark Lifecycle stopped + Health NOT_SERVING.
        lifecycle_.MarkStopped();
        loaded_ = false;

        osw::log::Info(kSubsystem, "mod_open_switch shutdown complete");
        return true;
    } catch (const std::exception& e) {
        osw::log::Error(kSubsystem, "Module::Shutdown threw: %s", e.what());
        return false;
    } catch (...) {
        osw::log::Error(kSubsystem, "Module::Shutdown threw unknown exception");
        return false;
    }
}

}  // namespace osw
