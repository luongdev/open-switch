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
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/core/module.h"

#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include <switch.h>  // FF-014 environment, switch_version_*

#include "osw/control/server.h"
#include "osw/core/config_fs.h"
#include "osw/observability/log.h"

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

        // 6. Start gRPC server.
        grpc_server_ = std::make_unique<control::GrpcServer>(&health_);
        grpc_server_->SetVersions(kModuleVersion, fs_ver);
        if (!grpc_server_->Start(config_)) {
            osw::log::Error(kSubsystem, "gRPC server failed to start");
            grpc_server_.reset();
            return false;
        }

        // 7. Flip lifecycle to Serving. Health status is set inside.
        lifecycle_.TransitionToServing();
        loaded_ = true;
        osw::log::Info(kSubsystem,
                       "mod_open_switch v%s loaded; gRPC bound at %s",
                       kModuleVersion,
                       config_.grpc_listen_address.c_str());
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

        // 1. Signal drain. W1 sets the flag; W2+ event flush, W3+
        //    Originate refusal, W4+ media bug close happens here.
        lifecycle_.SignalDrain();

        // 2. Drain the gRPC server.
        if (grpc_server_) {
            const auto deadline = std::chrono::system_clock::now() +
                                  std::chrono::seconds(config_.grpc_drain_deadline_seconds);
            grpc_server_->Drain(deadline);
            grpc_server_.reset();
        }

        // 3. Mark Lifecycle stopped + Health NOT_SERVING.
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
