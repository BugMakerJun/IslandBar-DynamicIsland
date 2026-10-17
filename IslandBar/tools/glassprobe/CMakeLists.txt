# =====================================================================
#  tools/glassprobe/CMakeLists.txt
#  One window, one question: can a pill-sized window get a REAL blurred backdrop that stops at its shape?
#
#  The bar asked this in 2026-10-16 and got three answers, all measured on a full-width 360 DIP window:
#  acrylic blurs but ignores the region, the system backdrop type does nothing on a per-pixel-alpha DComp
#  window, and DwmEnableBlurBehindWindow does nothing at all. What was never tried is the shape that makes
#  acrylic's one limitation harmless: a window whose RECTANGLE is the pill.
#
#  Not part of the shipped bar, like every other probe here.
# =====================================================================

add_executable(glassprobe main.cpp dcomp_probe.cpp)
target_include_directories(glassprobe PRIVATE "${CMAKE_SOURCE_DIR}/src")
