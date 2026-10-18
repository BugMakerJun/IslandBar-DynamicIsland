# =====================================================================
#  tools/ambientpreview/CMakeLists.txt
#  Renders every island::AmbientKind to a PNG over a glass pill, so the
#  weather effects can be judged without waiting for the weather.
#
#  Not part of the shipped bar, like every other probe here.
# =====================================================================

add_executable(ambientpreview
    main.cpp
    "${CMAKE_SOURCE_DIR}/src/render/Ambient.cpp"
)
target_include_directories(ambientpreview PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(ambientpreview PRIVATE d2d1 d3d11 dxgi dwrite windowscodecs ole32)
