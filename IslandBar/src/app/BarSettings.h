// =====================================================================
//  BarSettings.h -- the bar's own settings, on disk
//
//  What lives here: the things about the BAR itself that the user changes and expects to survive a
//  restart. The collapsed shape is ordinary appearance; scroll_tiling only enables the experimental
//  capability and deliberately does not enter the mode. The file is a key=value text file beside
//  theme.txt rather than a registry key or a binary blob: it can be read, edited and backed up by hand.
//
//      %USERPROFILE%\IslandBar\bar.txt
//
//      # The bar's own settings. One key per line.
//      shape=capsule        (floating pill, every corner rounded -- the default)
//      shape=notch          (hangs from the screen's top edge, its top corners cut by it)
//      scroll_tiling=disabled (default; enabled permits the separate Enter command)
//
//  WHAT THIS FILE DOES NOT HOLD: the theme (that is theme.txt, and has been since the first version),
//  the Start card's folder list (start_folder.txt), the weather location (weather.txt) or anything the
//  tray learns. One file per thing, and this one is the bar's own geometry.
// =====================================================================
#pragma once

#include "domain/BarShape.h"

namespace island {

struct BarSettings {
    BarShape shape = BarShape::Capsule;
    bool scrollTilingEnabled = false;
};

// A missing file is the default and is not an error. Unknown values are REFUSED with a line in the
// log and their field keeps its default, because a typo silently treated as the requested value is a
// setting the user thinks is in force and is not.
BarSettings LoadBarSettings();

// Writes every known key back. Returns false when the file cannot be written; the caller keeps the
// requested state in force for this session (the file is what survives it).
bool SaveBarSettings(const BarSettings& settings);

} // namespace island
