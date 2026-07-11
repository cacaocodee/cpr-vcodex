#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits (ignore UNDERLINE bit for font selection)
  const bool hasBold = (style & BOLD) != 0;
  const bool hasItalic = (style & ITALIC) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (bold) return bold;
    if (italic) return italic;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  }

  return regular;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
  getFont(style)->getTextDimensions(string, w, h);
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  const EpdGlyph* glyph = getFont(style)->getGlyph(cp);
  if (!glyph && fallback_) {
    return fallback_->getGlyph(cp, style);
  }
  return glyph;
}

bool EpdFontFamily::resolveGlyph(const uint32_t cp, const Style style, const EpdGlyph** glyph,
                                 const EpdFontData** data) const {
  const EpdGlyph* found = getFont(style)->getGlyph(cp);
  if (found) {
    *glyph = found;
    *data = getData(style);
    return true;
  }
  if (fallback_) {
    return fallback_->resolveGlyph(cp, style, glyph, data);
  }
  *glyph = nullptr;
  *data = nullptr;
  return false;
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  return getFont(style)->getKerning(leftCp, rightCp);
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  return getFont(style)->applyLigatures(cp, text);
}
