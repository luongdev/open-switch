# tests/unit/CMakeLists.txt
#
# Aggregates per-subsystem unit-test subdirectories. Each subdir's
# CMakeLists.txt calls osw_add_unit_test(...) from the parent helper.

add_subdirectory(raii)
add_subdirectory(observability)

# W1 additional unit-test subdirs are registered as they land:
#   add_subdirectory(core)           -- after config/lifecycle ship
#   add_subdirectory(control)        -- after gRPC server ships
