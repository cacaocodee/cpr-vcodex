#pragma once

// Debloat feature gates. All flags default ON so the normal envs build
// unchanged; [env:debloat] passes -DCPR_ENABLE_X=0 to strip a feature.
// Follows the CPR_ENABLE_GERMAN_HYPHENATION value-style flag precedent.

#ifndef CPR_ENABLE_OPDS
#define CPR_ENABLE_OPDS 1
#endif

#ifndef CPR_ENABLE_DICTIONARY
#define CPR_ENABLE_DICTIONARY 1
#endif

// Gates the Lyra and Lyra Carousel theme variants. The Lyra vCodex
// (LyraCustom) default theme always stays available.
#ifndef CPR_ENABLE_EXTRA_THEMES
#define CPR_ENABLE_EXTRA_THEMES 1
#endif

// 0 = Bookerly-only built-in reader fonts (all sizes/styles) plus the Ubuntu
// UI and small fonts; drops the Lexend and NotoSans families. SD-card fonts
// are unaffected.
#ifndef CPR_ENABLE_EXTRA_FONTS
#define CPR_ENABLE_EXTRA_FONTS 1
#endif

// 1 = draw a one-line page-render timing overlay in the reader (load /
// prewarm / render / display / total ms + free heap, values from the
// previous page turn). Debug aid for devices where serial is unavailable.
#ifndef CPR_PERF_OVERLAY
#define CPR_PERF_OVERLAY 0
#endif

// 0 = keep the reading-stats store unloaded to free heap (~5-25KB depending
// on library size and history). Code stays compiled and the stats UI remains
// reachable but shows an empty store; persistence is suspended via the
// recovery-mode path, so the stats JSON on the SD card is never overwritten
// and reappears intact on a build with this flag on. The BLE build no longer
// sets this — it unloads/reloads the store dynamically with the BT state.
#ifndef CPR_ENABLE_READING_STATS
#define CPR_ENABLE_READING_STATS 1
#endif
