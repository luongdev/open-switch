/*
 * src/control/control_service_skeleton.h
 *
 * ControlServiceSkeleton — concrete grpc service that overrides every
 * method of open_switch::control::v1::ControlService::Service. W1
 * ships a working Health handler; all other RPCs return a uniform
 * UNIMPLEMENTED status via osw::control::handlers::Unimplemented.
 *
 * Private to src/control/. Header lives here (not under include/)
 * because the skeleton consumes the generated proto types and we do
 * NOT want a public dependency on the gRPC C++ API surface from
 * outside the control subsystem.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_CONTROL_SERVICE_SKELETON_H_
#define OSW_CONTROL_CONTROL_SERVICE_SKELETON_H_

#include <atomic>
#include <cstdint>
#include <string>

#include <grpcpp/grpcpp.h>

#include "open_switch/control/v1/control.grpc.pb.h"

namespace osw {
class Health;

namespace events {
class Broadcaster;
class RingSet;
}  // namespace events

namespace control {

class IdempotencyCache;  // forward; defined in osw/control/idempotency_cache.h

class ControlServiceSkeleton final : public open_switch::control::v1::ControlService::Service {
  public:
    /// `health` is the module-wide Health aggregator. Non-owning.
    /// Use fully qualified ::osw::Health to disambiguate from the
    /// inherited `Health` RPC method declared by the generated
    /// ControlService::Service base class.
    explicit ControlServiceSkeleton(::osw::Health* health) noexcept;

    /// Set version strings reported by Health RPC. Called by GrpcServer
    /// before the server starts serving.
    void SetVersions(std::string module_version, std::string freeswitch_version);

    /// Inject the W5B idempotency cache. Called by Module::Load before
    /// Start()-ing the gRPC server (or immediately after; RPCs that arrive
    /// before the cache is wired will bypass deduplication — acceptable
    /// because the module is not yet in SERVING state at that point).
    ///
    /// Non-owning pointer. The Module owns the IdempotencyCache and must
    /// ensure it outlives the gRPC server's RPC threads. When null (default),
    /// Originate/Bridge/Execute handlers skip all cache logic.
    void SetIdempotencyCache(IdempotencyCache* cache) noexcept;

    /// Inject the W2 event-plane bridges. Called by Module::Load after
    /// Broadcaster + RingSet are constructed (and before Start()-ing the
    /// gRPC server, so the SubscribeEvents handler always sees a valid
    /// broadcaster pointer when the first RPC arrives).
    ///
    /// Pre-W2 builds (and tests that don't exercise SubscribeEvents)
    /// leave these as nullptr; SubscribeEvents then returns UNIMPLEMENTED
    /// rather than crashing.
    ///
    /// Both pointers are non-owning. The Module singleton outlives the
    /// gRPC server's RPC threads (Drain joins them before tearing down
    /// Module-owned subsystems).
    void SetEventPlane(events::Broadcaster* broadcaster,
                       events::RingSet* rings,
                       std::uint32_t max_subscribers,
                       std::uint32_t subscriber_send_queue_capacity) noexcept;

    /// Per-subscriber default send-queue capacity (set via SetEventPlane).
    [[nodiscard]] std::uint32_t SubscriberSendQueueCapacity() const noexcept {
        return subscriber_send_queue_capacity_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint32_t MaxSubscribers() const noexcept {
        return max_subscribers_.load(std::memory_order_acquire);
    }
    [[nodiscard]] events::Broadcaster* Broadcaster() const noexcept {
        return broadcaster_.load(std::memory_order_acquire);
    }
    [[nodiscard]] events::RingSet* RingSet() const noexcept {
        return rings_.load(std::memory_order_acquire);
    }
    [[nodiscard]] IdempotencyCache* GetIdempotencyCache() const noexcept {
        return idempotency_cache_.load(std::memory_order_acquire);
    }

    // --- Health (real impl) ----------------------------------------
    grpc::Status Health(grpc::ServerContext* ctx,
                        const open_switch::control::v1::HealthRequest* req,
                        open_switch::control::v1::HealthResponse* resp) override;

    // --- Unimplemented RPCs ----------------------------------------
    grpc::Status Originate(grpc::ServerContext* ctx,
                           const open_switch::control::v1::OriginateRequest* req,
                           open_switch::control::v1::OriginateResponse* resp) override;

    grpc::Status Hangup(grpc::ServerContext* ctx,
                        const open_switch::control::v1::HangupRequest* req,
                        open_switch::control::v1::HangupResponse* resp) override;

    grpc::Status HangupMany(grpc::ServerContext* ctx,
                            const open_switch::control::v1::HangupManyRequest* req,
                            open_switch::control::v1::HangupManyResponse* resp) override;

    grpc::Status Bridge(grpc::ServerContext* ctx,
                        const open_switch::control::v1::BridgeRequest* req,
                        open_switch::control::v1::BridgeResponse* resp) override;

    grpc::Status Execute(grpc::ServerContext* ctx,
                         const open_switch::control::v1::ExecuteRequest* req,
                         open_switch::control::v1::ExecuteResponse* resp) override;

    grpc::Status SetVariables(grpc::ServerContext* ctx,
                              const open_switch::control::v1::SetVariablesRequest* req,
                              open_switch::control::v1::SetVariablesResponse* resp) override;

    grpc::Status Hold(grpc::ServerContext* ctx,
                      const open_switch::control::v1::HoldRequest* req,
                      open_switch::control::v1::HoldResponse* resp) override;

    grpc::Status Unhold(grpc::ServerContext* ctx,
                        const open_switch::control::v1::UnholdRequest* req,
                        open_switch::control::v1::UnholdResponse* resp) override;

    grpc::Status BlindTransfer(grpc::ServerContext* ctx,
                               const open_switch::control::v1::BlindTransferRequest* req,
                               open_switch::control::v1::BlindTransferResponse* resp) override;

    grpc::Status SubscribeEvents(
        grpc::ServerContext* ctx,
        const open_switch::control::v1::SubscribeEventsRequest* req,
        grpc::ServerWriter<open_switch::events::v1::EventEnvelope>* writer) override;

  private:
    osw::Health* health_;
    std::string module_version_;
    std::string freeswitch_version_;
    // W2 event-plane bridges. nullptr in W1-only builds + tests.
    //
    // Codex W2 review C-3: these are written by `SetEventPlane`
    // (Module::Load step 7) and read by every SubscribeEvents RPC
    // handler. The grpc server starts accepting RPCs in step 6, so
    // any RPC that wins the step-6→step-7 race reads the default-
    // initialised value while another thread writes the bridge in.
    // Plain pointer reads/writes are NOT atomic per the C++ memory
    // model — TSAN strict mode will flag this. Use `std::atomic`
    // with acquire/release so the write happens-before the read on
    // any architecture.
    std::atomic<events::Broadcaster*> broadcaster_{nullptr};
    std::atomic<events::RingSet*> rings_{nullptr};
    std::atomic<std::uint32_t> max_subscribers_{0};
    std::atomic<std::uint32_t> subscriber_send_queue_capacity_{4096};
    // W5B idempotency cache. Written once by SetIdempotencyCache (called
    // from Module::Load before the gRPC server accepts traffic), then only
    // read by handler threads. atomic<> ensures a happens-before edge
    // between the write in Load and any RPC handler read (same rationale
    // as broadcaster_ above — Codex W2 C-3).
    std::atomic<IdempotencyCache*> idempotency_cache_{nullptr};
};

}  // namespace control
}  // namespace osw

#endif  // OSW_CONTROL_CONTROL_SERVICE_SKELETON_H_
