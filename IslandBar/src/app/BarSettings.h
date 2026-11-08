// =====================================================================
//  BarSettings.h -- the bar's own settings, on disk
//
//  What lives here: the things about the BAR itself that the user changes and expects to survive a
//  restart. Today that is one key -- the collapsed shape (see domain/BarShape.h) -- and the file is
//  deliberately a key=value text file beside theme.txt rather than a registry key or a binary blob:
//  it can be read, edited and backed up by hand, and it follows the convention the theme already set.
//
//      %USERPROFILE%\IslandBar\bar.txt
//
//      # The bar's own settings. One key per line.
//      shape=capsule        (floating pill, every corner rounded -- the default)
//      shape=notch          (hangs from the screen's top edge, its top corners cut by it)
//
//  WHAT THIS FILE DOES NOT HOLD: the theme (that is theme.txt, and has been since the first version),
//  the Start card's folder list (start_folder.txt), the weather location (weather.txt) or anything the
//  tray learns. One file per thing, and this one is the bar's own geometry.
// =====================================================================
#pragma once

#include "domain/BarShape.h"

namespace island {

// The shape bar.txt names. A missing file is the default (capsule) and is not an error; a file that names
// something this build does not know is REFUSED with a line in the log and the default is used, because a
// typo that is silently ignored is a setting the user thinks is in force and is not.
BarShape LoadBarShape();

// Writes the whole file back with the chosen shape. Returns false when the file cannot be written; the
// caller keeps the shape in force either way (the session is what the user asked for; the file is what
// survives it) and the log says which of the two happened.
bool SaveBarShape(BarShape shape);

} // namespace island
