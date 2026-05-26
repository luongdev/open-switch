# Phase-1 stub build for mod_open_switch.so.
#
# This produces a minimal FreeSWITCH module whose load + shutdown
# entry points return SUCCESS. It is the seed for the implementation
# phase; subsequent commits add control / event / media subsystems
# behind their own subdirectories.
#
# Codex round-2 finding N6: without this file, the Docker builder
# stage `cp /usr/local/mod/mod_open_switch.so` fails because no .so
# is produced. The stub unblocks the end-to-end build pipeline.

add_library(mod_open_switch SHARED
    mod_open_switch.cc
)

# FreeSWITCH headers are header-only at module compile time — the
# .so is dlopened into the FS process and links against the
# FreeSWITCH C ABI via the loader, not at link time. We add the
# include directory so `#include <switch.h>` resolves.
target_include_directories(mod_open_switch
    PRIVATE
        ${OSW_FREESWITCH_INCLUDE_DIR}
)

# The output must be named `mod_open_switch.so` exactly (no `lib`
# prefix) so that FreeSWITCH's loader finds it under MOD_DIR.
set_target_properties(mod_open_switch PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mod_open_switch"
    SUFFIX ".so"
)

# The module's switch_loadable_module_function_table_t interface
# symbol is emitted with `SWITCH_MOD_DECLARE_DATA` which expands to
# `__attribute__((visibility("default")))` (see FREESWITCH-FACTS
# FF-009). All other module symbols are hidden by the
# `-fvisibility=hidden` global compile option set in the root
# CMakeLists.txt. The `-Wl,--exclude-libs,ALL` link option (also
# set globally) prevents transitive symbols from statically-linked
# deps (gRPC/protobuf/abseil, once those land in implementation
# phase) from leaking out of our .so. Combined with FreeSWITCH's
# default RTLD_LOCAL loader behaviour (FF-010), this isolates the
# module's namespace.
target_link_libraries(mod_open_switch
    PRIVATE
        Threads::Threads
)

if(OSW_INSTALL_MODULE)
    install(TARGETS mod_open_switch
        LIBRARY DESTINATION ${OSW_FREESWITCH_MOD_DIR}
    )
endif()
