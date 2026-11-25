# =====================================================================
#  tools/tray_probe/CMakeLists.txt
#  Independent tray data probe and its observer-only hook.
# =====================================================================

add_executable(tray_probe
    main.cpp
)

target_include_directories(tray_probe PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(tray_probe PRIVATE
    island_domain
    user32
    shell32
    advapi32
)
target_compile_definitions(tray_probe PRIVATE
    _CONSOLE
    ISLANDBAR_TRAY_PROBE_RING=1
)

add_library(island_tray_probe_hook SHARED
    "${CMAKE_SOURCE_DIR}/src/trayhook/TrayHook.cpp"
)

target_include_directories(island_tray_probe_hook PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(island_tray_probe_hook PRIVATE
    user32
    shell32
)
target_compile_definitions(island_tray_probe_hook PRIVATE
    ISLANDBAR_TRAY_PROBE_RING=1
)

set_target_properties(island_tray_probe_hook PROPERTIES
    OUTPUT_NAME "island_tray_probe_hook"
)

add_dependencies(tray_probe island_tray_probe_hook)

add_executable(tray_cache_test
    cache_test.cpp
    "${CMAKE_SOURCE_DIR}/src/shell/TrayCallbackCache.cpp"
)
target_include_directories(tray_cache_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(tray_cache_test PRIVATE
    island_core
    user32
)
target_compile_definitions(tray_cache_test PRIVATE _CONSOLE)

add_executable(tray_protocol_test
    protocol_test.cpp
)
target_include_directories(tray_protocol_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(tray_protocol_test PRIVATE island_domain)
target_compile_definitions(tray_protocol_test PRIVATE
    _CONSOLE
    ISLANDBAR_TRAY_PROBE_RING=1
)
