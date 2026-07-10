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
