add_executable(tiling_probe main.cpp)
target_compile_features(tiling_probe PRIVATE cxx_std_20)
target_link_libraries(tiling_probe PRIVATE user32 dwmapi shell32)
