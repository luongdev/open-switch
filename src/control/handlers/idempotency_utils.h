/*
 * src/control/handlers/idempotency_utils.h
 *
 * Small handler-private wrapper for media RPC idempotency.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CONTROL_HANDLERS_IDEMPOTENCY_UTILS_H_
#define OSW_CONTROL_HANDLERS_IDEMPOTENCY_UTILS_H_

#include <chrono>
#include <string>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "osw/control/idempotency_cache.h"
#include "osw/observability/log.h"

namespace osw::control::handlers {

template <typename Response, typename Execute>
grpc::Status RunIdempotent(osw::control::IdempotencyCache* cache,
                           const char* method,
                           const std::string& tenant_id,
                           const std::string& request_id,
                           Response* resp,
                           Execute&& execute) {
    const std::string key = osw::control::MakeIdempotencyKey(method, tenant_id, request_id);
    if (!cache || key.empty() || !resp) {
        return execute();
    }

    auto result = cache->LookupOrReserve(key);
    if (result.state == osw::control::IdempotencyCache::State::kHit) {
        if (resp->ParseFromString(result.entry.serialized_response)) {
            osw::log::Debug("control.idempotency",
                            "%s: cache hit tenant_id=%s request_id=%s",
                            method,
                            tenant_id.c_str(),
                            request_id.c_str());
            return result.entry.status;
        }
        osw::log::Warn("control.idempotency",
                       "%s: cache hit parse failed tenant_id=%s request_id=%s",
                       method,
                       tenant_id.c_str(),
                       request_id.c_str());
    }

    grpc::Status status = execute();
    osw::control::IdempotencyCache::Entry entry;
    entry.status = status;
    if (!resp->SerializeToString(&entry.serialized_response)) {
        osw::log::Warn("control.idempotency",
                       "%s: response serialization failed tenant_id=%s request_id=%s",
                       method,
                       tenant_id.c_str(),
                       request_id.c_str());
        cache->Cancel(key);
        return status;
    }
    entry.expires_at = std::chrono::steady_clock::now() + cache->Ttl();
    cache->Store(key, std::move(entry));
    return status;
}

}  // namespace osw::control::handlers

#endif  // OSW_CONTROL_HANDLERS_IDEMPOTENCY_UTILS_H_
