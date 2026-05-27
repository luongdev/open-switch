# tests/unit/CMakeLists.txt
#
# Aggregates per-subsystem unit-test subdirectories. Each subdir's
# CMakeLists.txt calls osw_add_unit_test(...) from the parent helper.

add_subdirectory(raii)
add_subdirectory(observability)
add_subdirectory(core)
add_subdirectory(control)
add_subdirectory(events)
add_subdirectory(media)
