# =====================================================================
#  tools/CMakeLists.txt -- the probes
#
#  None of these is shipped. Each exists to answer a question the bar could not
#  answer from inside itself: whether an API works on THIS machine, and what it
#  actually returns -- measured rather than assumed. The bar's own code has twice
#  reported success from every call while drawing nothing, which is the reason each
#  question gets a program that does nothing else.
#
#  They are built by default, so a question can be re-asked without editing the
#  build. scripts\probe-*.ps1 drive them; the answers are recorded in docs\.
# =====================================================================

# A toolchain smoke test: does this machine's MSVC build and run a C++20 binary,
# and decode the UTF-8 sources correctly?
option(ISLANDBAR_BUILD_SMOKE "Build the toolchain smoke test" ON)
if(ISLANDBAR_BUILD_SMOKE)
    add_subdirectory(smoke)
endif()

# A standalone harness for the JSON reader and the notification layer. It links
# only those two modules, so a hang in the parser cannot be mistaken for a hang in
# the DirectComposition setup that surrounds it in the main self-test.
option(ISLANDBAR_BUILD_JSONPROBE "Build the JSON/notification probe" ON)
if(ISLANDBAR_BUILD_JSONPROBE)
    add_subdirectory(jsonprobe)
endif()

# Asks whether WeChat reports its calls through the media session API. That
# decides whether the call pill can be driven from the C++/WinRT media path the
# bar already uses, or has to be driven from WeChat's call window instead. The
# question cannot be asked from PowerShell on this machine, whose WindowsRuntime
# interop is not loadable, so it needs a C++/WinRT program -- this one.
option(ISLANDBAR_BUILD_GSMTCPROBE "Build the media-session capability probe" ON)
if(ISLANDBAR_BUILD_GSMTCPROBE)
    add_subdirectory(gsmtcprobe)
endif()

# Asks Windows for the machine's own position. The weather on the status line needs
# coordinates, and the two sources that can supply them are a city in weather.txt and
# this -- so whether this works, and what it answers on a machine whose traffic leaves
# through a proxy, has to be measured rather than assumed.
option(ISLANDBAR_BUILD_GEOPROBE "Build the location capability probe" ON)
if(ISLANDBAR_BUILD_GEOPROBE)
    add_subdirectory(geoprobe)
endif()

# Two windows of one ordinary program, and a DWM thumbnail between them. The bar's own preview said
# "registered, visible, right place, nothing drawn", and separating the recipe from the machine needed a
# program that does nothing else.
option(ISLANDBAR_BUILD_THUMBPROBE "Build the DWM thumbnail probe" ON)
if(ISLANDBAR_BUILD_THUMBPROBE)
    add_subdirectory(thumbprobe)
endif()

# Asks whether a PILL-SIZED window can get a real blurred backdrop. The bar's three attempts were all made
# on a full-width 360 DIP window, where the one that worked (acrylic) would have smeared a blurred band
# across the top of the screen because it ignores the window region. A window whose rectangle IS the pill
# has no such band left to leak into -- so the only question is whether the blur stops at the corners.
option(ISLANDBAR_BUILD_GLASSPROBE "Build the acrylic pill probe" ON)
if(ISLANDBAR_BUILD_GLASSPROBE)
    add_subdirectory(glassprobe)
endif()

# Asks whether the DESKTOP'S PIXELS can be reached without capturing the screen: Windows.UI.Composition's
# host backdrop brush on a pill-sized window, and whether a hand-written IGraphicsEffect is accepted by
# CreateEffectFactory. That is the difference between frosted glass and a lens (HANDOFF 8.170).
option(ISLANDBAR_BUILD_LENSPROBE "Build the host-backdrop lens probe" ON)
if(ISLANDBAR_BUILD_LENSPROBE)
    add_subdirectory(lensprobe)
endif()

# Renders every island::AmbientKind to a PNG over a glass pill, so the weather effects can be judged
# without waiting for the weather.
option(ISLANDBAR_BUILD_AMBIENTPREVIEW "Build the ambient preview probe" ON)
if(ISLANDBAR_BUILD_AMBIENTPREVIEW)
    add_subdirectory(ambientpreview)
endif()

