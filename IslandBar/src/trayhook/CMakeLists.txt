# =====================================================================
#  src/trayhook/CMakeLists.txt -- the module that lives inside other applications
#
#  A DLL rather than part of the bar, because it has to be loaded into OTHER processes: the bar injects it
#  with SetWindowsHookEx(WH_GETMESSAGE) on the target's threads (see TrayHook.cpp). It lands next to
#  island_bar.exe, which is where the bar loads it from.
#
#  The static runtime set at the top level applies here too, so this module brings no dependency of its own.
# =====================================================================

add_library(island_tray_hook SHARED
    TrayHook.cpp
    ../platform/TaskbarRecovery.cpp
)

target_link_libraries(island_tray_hook PRIVATE user32 shell32)

# src/ is the include root, exactly as it is for the bar: the module includes "domain/TrayRegistration.h", and a
# relative path here would be the only place in the project that spells an include differently.
target_include_directories(island_tray_hook PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Beside the bar, not in a separate bin directory: the bar looks for it by its own module's path, and a
# deployment that copies one file without the other would otherwise be a silent, half-working install.
set_target_properties(island_tray_hook PROPERTIES
    OUTPUT_NAME "island_tray_hook"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/src/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/src/Release"
)
