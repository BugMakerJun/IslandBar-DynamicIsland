# =====================================================================
#  src/CMakeLists.txt
#  The bar itself: one executable, built from the modules below.
#
#  The modules are directories, and an include is written from THIS directory --
#  `#include "core/Log.h"`, not `#include "Log.h"`. The include root is therefore
#  src/, and a file that moves between modules breaks the build loudly instead of
#  silently resolving through a stale entry on the search path.
#
#  NOTE: this file was lost once -- something in the tooling deleted it while the
#  project was being edited, and there was no version control to recover it from.
#  The target name, the manifest embedding and the C++/WinRT setup are load-bearing,
#  and the reasons are written down here because they were written down here once.
# =====================================================================

# --- domain: the pure logic, and the only target that must stay OS-free ----------------
#
#  Everything in here runs without a desktop: layout, animation, hit testing, state policy, the JSON reader.
#  That is what lets --self-test run 900+ assertions without creating a window, and it is the property most
#  easily lost by accident -- one <windows.h> in one header and the whole layer is compiled against Win32
#  again, which is exactly what had happened (core/Geometry.h used to include it, so every file that wanted
#  a rectangle got HWND, MSG and the min/max macros for free).
#
#  THE RULE IS ENFORCED, NOT DOCUMENTED. domain/PurityCheck.cpp includes every header in this target and then
#  fails with #error if <windows.h> has arrived. And this target may never gain a link library: a file that
#  needs a Win32 call does not belong in it.
add_library(island_domain STATIC
    domain/MiniJson.cpp
    domain/Motion.cpp
    domain/TrayLedger.cpp
)
target_include_directories(island_domain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# --- one target per layer, linked only downwards ---------------------------------------
#
#  WHY THE SOURCES ARE NOT ALL IN ONE add_executable ANY MORE. The layers used to be directory names and a
#  paragraph in ARCHITECTURE.md, and that paragraph said the modules "may call each other". They may not:
#  the direction is domain -> core -> platform -> media -> services -> shell -> render -> app -> cli, and it
#  is now a target graph, so the link stops a call that reaches a layer this one is not allowed to see.
#
#  WHAT THE LINK GRAPH DOES NOT CATCH, said plainly: an INCLUDE. The include root is src/ for every target
#  (the project's form is "module/File.h"), so a file in shell/ that includes render/Renderer.h compiles and
#  links exactly as if nothing had happened -- MEASURED 2026-10-16 by doing it on purpose. The include half
#  of the rule is check-layers.ps1, wired in below and run on every build.
#
#  island_domain is still the special one: it links nothing, and no header under it may include <windows.h>
#  (core/PurityCheck.cpp is the compile-time proof of that; it lives in core/ because it includes a core
#  header, and a layer may only include the ones below it).
add_library(island_core STATIC
    core/Log.cpp
    core/ProcessMetrics.cpp
    core/PurityCheck.cpp
)
target_link_libraries(island_core PUBLIC island_domain)

add_library(island_platform STATIC
    platform/DisplayBrightness.cpp
    platform/Http.cpp
    platform/IconRaster.cpp
    platform/Monitor.cpp
    platform/NativeTaskbar.cpp
    platform/ThumbnailPreview.cpp
    platform/WindowList.cpp
)
target_link_libraries(island_platform PUBLIC island_core)

add_library(island_media STATIC
    media/AudioVolume.cpp
    media/Lyrics.cpp
    media/MusicSession.cpp
    media/SoundOutput.cpp
    media/WicImage.cpp
)
target_link_libraries(island_media PUBLIC island_platform)
# C++/WinRT, for the system media controls (Windows.Media.Control). Applied to the layer that uses it
# rather than to the executable: the include directory and /await are PRIVATE, so nothing above this layer
# can see the projection.
islandbar_enable_cppwinrt(island_media)

add_library(island_services STATIC
    services/Notification.cpp
    services/NotificationIcon.cpp
    services/NotificationPipe.cpp
    services/ToastStore.cpp
    services/WeChatWatch.cpp
)
target_link_libraries(island_services PUBLIC island_media)

add_library(island_shell STATIC
    shell/StartApps.cpp
    shell/StartMenu.cpp
    shell/SystemStatus.cpp
    shell/SystemToggles.cpp
    shell/TrayIcons.cpp
    shell/TrayLearn.cpp
    shell/TrayMenu.cpp
    shell/TrayPress.cpp
    shell/TrayUia.cpp
    shell/WallpaperTint.cpp
    shell/Weather.cpp
)
target_link_libraries(island_shell PUBLIC island_services)
islandbar_enable_cppwinrt(island_shell)   # SystemToggles and Weather read the projection directly

add_library(island_render STATIC
    render/GlassMaterial.cpp
    render/Renderer.cpp
    render/TextRenderer.cpp
    render/Theme.cpp
)
target_link_libraries(island_render PUBLIC island_shell)

add_library(island_app STATIC
    app/AppBar.cpp
    app/BarLoop.cpp
    app/IslandWindow.cpp
    app/PlayerWindow.cpp
)
target_link_libraries(island_app PUBLIC island_render)

add_library(island_cli STATIC
    cli/Cli.cpp
)
target_link_libraries(island_cli PUBLIC island_app)

# --- the layer rule, checked on every build --------------------------------------------
#
#  The half a link line cannot see: an include that points upwards (see the note above). The script holds
#  the rule in one readable table and names the file when it fails.
if(WIN32)
    add_custom_target(islandbar_layers ALL
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
                "${CMAKE_SOURCE_DIR}/scripts/check-layers.ps1"
        COMMENT "checking that no layer includes one above it"
        VERBATIM
    )
endif()

# --- the executable: main.cpp, the self-test, and the assembly -------------------------
#
#  src/ is the include root, and it arrives through the chain above (island_domain carries it as PUBLIC);
#  Diagnostics.cpp is the -self-test entry point, so it belongs to the exe and may include every layer.
add_executable(island_bar
    main.cpp
    # --- selftest: the runtime assertions (-self-test), one file per subject -----------
    #  Diagnostics.cpp keeps the two entry points; the checks live in Checks*.cpp and the
    #  printing (Section/Check/Info) in Harness.cpp -- see selftest/Checks.h.
    selftest/Harness.cpp
    selftest/ChecksGeometry.cpp
    selftest/ChecksWindow.cpp
    selftest/ChecksMotion.cpp
    selftest/ChecksJson.cpp
    selftest/ChecksCards.cpp
    selftest/ChecksInput.cpp
    selftest/Diagnostics.cpp
)
target_link_libraries(island_bar PRIVATE island_cli)

# --- the application manifest ------------------------------------------------
#
# PerMonitorV2 DPI awareness. Declared in the manifest rather than at runtime so
# Windows applies it before the first window exists; a window created without it
# is scaled by the system and every layout metric is silently wrong.
#
# Embedded by the linker with /MANIFEST:EMBED and /MANIFESTINPUT rather than with
# a .rc file, because a resource script would need its own compiler pass and an
# .rc that references one file is not worth the extra target.
target_sources(island_bar PRIVATE island_bar.manifest)
target_sources(island_bar PRIVATE island_bar.rc)

if(MSVC)
    target_link_options(island_bar PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/island_bar.manifest"
    )
endif()

# --- a GUI subsystem binary with a console when there is one -----------------
#
# WIN32_EXECUTABLE, so double-clicking it does not open a console window. The
# program still borrows the parent console when started from a shell (see
# PrepareConsole in main.cpp), and it must never write to stdout unguarded: with
# no console attached the CRT's stdout has descriptor -1, and writing there trips
# an assertion dialog instead of printing nothing. Every console write goes
# through Log.cpp.
#
# /ENTRY:mainCRTStartup because WIN32_EXECUTABLE makes the linker look for
# WinMain, and this program has an ordinary main().
set_target_properties(island_bar PROPERTIES
    WIN32_EXECUTABLE TRUE
)

if(MSVC)
    target_link_options(island_bar PRIVATE /ENTRY:mainCRTStartup)
endif()

# --- third-party pieces ------------------------------------------------------
#
# SQLite is needed only by the notification database reader, which is optional and
# off by default. Linking it here only when it exists keeps the default build free
# of the dependency.
if(TARGET sqlite3)
    # ... and it is the services layer that reads that database, so it is this target that carries the
    # link and the define -- the executable only assembles.
    target_link_libraries(island_services PRIVATE sqlite3)
    target_compile_definitions(island_services PRIVATE ISLANDBAR_HAVE_SQLITE=1)
endif()

# --- the module that lives inside other applications -------------------------
#
# A separate binary on purpose: it is loaded into OTHER processes to watch them register their tray icons
# (see trayhook/TrayHook.cpp), and it is written beside the bar because that is where the bar loads it from.
add_subdirectory(trayhook)
