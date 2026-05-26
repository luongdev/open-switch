/*
 * include/osw/core/config_fs.h
 *
 * Loader entry point that reads the module's XML config via
 * switch_xml_config_parse_module_settings (FF-013) and populates
 * an osw::Config.
 *
 * This header is included ONLY by code that has <switch.h> in scope
 * (e.g. src/core/config_fs.cc, src/mod_open_switch.cc). The data
 * struct + validation logic live in include/osw/core/config.h and
 * are FS-agnostic.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef OSW_CORE_CONFIG_FS_H_
#define OSW_CORE_CONFIG_FS_H_

#include "osw/core/config.h"

namespace osw {

/// Load the config from `${conf_dir}/autoload_configs/<file>`.
///
/// Per FF-013, switch_xml_config_parse_module_settings returns
/// SWITCH_STATUS_SUCCESS even when the <settings> block is absent (it
/// uses defaults from the instruction table). It returns
/// SWITCH_STATUS_FALSE when the file itself can't be opened.
///
/// This function:
///   1. Initialises `out` to the compiled-in defaults
///      (osw::Config{}).
///   2. Calls switch_xml_config_parse_module_settings.
///   3. Returns `true` on success (file present + parsed OR file absent
///      → defaults). Returns `false` only on a real parse error.
///
/// Caller MUST run Validate(out) after a successful load.
bool LoadConfigFromFile(const char* xml_file_name, Config& out);

}  // namespace osw

#endif  // OSW_CORE_CONFIG_FS_H_
