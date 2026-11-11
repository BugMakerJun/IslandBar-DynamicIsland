// =====================================================================
//  PurityCheck.cpp -- ONE COMPILE-TIME RULE, ENFORCED BY THE COMPILER
//
//  WHY IT LIVES IN core/ AND NOT in domain/ (2026-10-16): it includes core/ProcessMetrics.h to prove that
//  header is pure too, so it was the ONE include in the project that pointed at a layer above its own --
//  which made the layer rule "every layer may include only the ones below it" untrue. The check itself is
//  unchanged: it is compiled one target later, and a domain header that grows a <windows.h> still fails the
//  build.
//
//  WHAT IT IS. This translation unit includes the program's pure-logic headers and then fails to compile if
//  <windows.h> has arrived with them. It has no runtime behaviour and no other job.
//
//  WHY IT EXISTS. The rule "the layout, the animation and the hit test do not know about Windows" was true
//  in spirit and false in fact: core/Geometry.h pulled in <windows.h>, and everything downstream -- CardLayout,
//  TaskStripLayout, Motion, StatePolicy, the whole self-test -- was compiled against the entire Win32 surface
//  without ever using it. A rule that lives in a comment survives until the next person needs one typedef. This
//  file makes the next person's build stop instead, at the moment they add it, with the reason in the message.
//
//  WHAT IT DOES NOT CLAIM. This is not "the pure layer is pure C++": MiniJson.h and ProcessMetrics.h touch the
//  standard library only, but nothing here forbids, say, <stdio.h>. What it forbids is the specific dependency
//  that made the layers indistinguishable -- the platform header -- and it does that by inspection, not by
//  convention, which is the difference between a rule and a wish.
//
//  WHEN IT FIRES, the fix is almost never "add an exemption here": it is to move the Win32-typed helper out of
//  the pure header and into a platform/ header, exactly as the two DIP conversion helpers were moved the day
//  this file was written.
// =====================================================================

#include "domain/CardLayout.h"       // brings Geometry.h, TaskStripLayout.h, Motion.h with it
#include "domain/Geometry.h"
#include "domain/MiniJson.h"
#include "domain/Motion.h"
#include "core/ProcessMetrics.h"
#include "domain/StartSearch.h"
#include "domain/SettingsLayout.h"
#include "domain/ScrollTiling.h"
#include "domain/StatePolicy.h"
#include "domain/TaskStripLayout.h"
#include "domain/TrayEncoding.h"
#include "domain/TrayLedger.h"
#include "domain/TrayRegistration.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS)
#error "The pure-logic headers in core/ must not include <windows.h>. Move whatever needs a Win32 type into a platform/ header (see core/Geometry.h for the one that was moved out)."
#endif

namespace island {
namespace {

// Something real for the compiler to chew on, so an empty translation unit warning never hides the rule above.
constexpr bool kPureHeadersCompiled = true;

}  // namespace
}  // namespace island
