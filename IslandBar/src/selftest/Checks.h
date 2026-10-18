// =====================================================================
//  Checks.h -- every check the self-test runs, in the files that own a subject
//
//  The self-test used to be one 8,968-line file. It is now one file per subject (geometry, window,
//  motion, json, cards, input) plus Diagnostics.cpp, which keeps the two entry points and calls
//  these. Nothing about the checks changed: the bodies moved, and this header is how the entry
//  points can still see them.
// =====================================================================
#pragma once

namespace island {

void TestGeometryPredicate(float originX, float originY, float monitorWidthDip, float workTopDip);
void TestWindow(IslandWindow& window, const MonitorGeometry& monitor);
void TestClickThrough(IslandWindow& window, const MonitorGeometry& monitor);
void TestRenderer(IslandWindow& window);
void TestLensProfile();
void TestMotion(float originX, float originY, float monitorWidthDip);
void TestClock();
void TestMiniJson();
void TestNotifications();
void TestCardLayout();
void TestIslandSurface();
void TestImageCodec();
void TestAccentColour();
void TestLyrics();
void TestMusicCardLayout();
void TestHoverSequence();
void TestPointerStateCycle();
void TestWorkerThread();

} // namespace island
