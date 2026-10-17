# =====================================================================
#  tools/lensprobe/CMakeLists.txt
#  Can the island see the desktop's pixels? Host backdrop brush + a hand-written IGraphicsEffect blur,
#  clipped to an exact stadium. C++/WinRT; the setup lives in cmake/WinRt.cmake.
# =====================================================================

add_executable(lens_probe main.cpp)
target_compile_features(lens_probe PRIVATE cxx_std_20)

islandbar_enable_cppwinrt(lens_probe)

# CreateDispatcherQueueController (dispatcherqueue.h): the composition target needs a dispatcher on the
# thread that creates it, and this SDK's WinRT projection does not expose the controller's statics.
target_link_libraries(lens_probe PRIVATE CoreMessaging.lib)

target_compile_definitions(lens_probe PRIVATE _CONSOLE)
