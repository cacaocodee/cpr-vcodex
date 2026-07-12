#pragma once

#include <InflateReader.h>

#include <vector>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_PAGE_SLOTS = 4;  // One per font style (R/B/I/BI)
  // Merge cap per slot: consecutive pages accumulate glyphs; above this a
  // slot is rebuilt from the current page only (bounds steady-state heap).
  static constexpr uint32_t MAX_SLOT_BYTES = 12u * 1024u;

  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // Checks the page buffer (from prewarm) first, then falls back to the hot group slot.
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex);

  // Free all cached data (page buffer + hot group).
  void clearCache();

  // Free only the transient hot-group buffers. Page slots persist across
  // page turns (cross-page glyph reuse); this keeps the 4-64KB hot group
  // from pinning heap between pages.
  void trimTransient();

  // Evict page slots whose font data is not in keep[] — called when the
  // reader font changes so stale slots can't linger.
  void releaseSlotsNotIn(const EpdFontData* const* keep, uint8_t keepCount);

  // Pre-scan UTF-8 text and ensure all needed glyph bitmaps are in this
  // font's page slot. Reuses the previous page's slot when it already covers
  // the text (zero decompression), merges in just the missing glyphs when it
  // mostly does, and rebuilds from scratch otherwise.
  // Returns the number of glyphs that couldn't be loaded (0 on full success).
  int prewarmCache(const EpdFontData* fontData, const char* utf8Text);

  struct Stats {
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t decompressTimeMs = 0;
    uint16_t uniqueGroupsAccessed = 0;
    uint32_t pageBufferBytes = 0;  // pageBuffer allocation
    uint32_t pageGlyphsBytes = 0;  // pageGlyphs lookup table allocation
    uint32_t hotGroupBytes = 0;    // current hot group allocation
    uint32_t peakTempBytes = 0;    // largest temp buffer in prewarm
    uint32_t getBitmapTimeUs = 0;  // cumulative getBitmap time (micros)
    uint32_t getBitmapCalls = 0;   // number of getBitmap calls
  };
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

 private:
  Stats stats;
  InflateReader inflateReader;

  // Page buffer slots: each style gets its own flat glyph buffer with sorted lookup.
  // Up to MAX_PAGE_SLOTS (4) styles can be prewarmed simultaneously.
  struct PageGlyphEntry {
    uint32_t glyphIndex;
    uint32_t bufferOffset;
    uint32_t alignedOffset;  // byte-aligned offset within its decompressed group (set during prewarm pre-scan)
  };
  struct PageSlot {
    uint8_t* buffer = nullptr;
    const EpdFontData* fontData = nullptr;
    PageGlyphEntry* glyphs = nullptr;
    uint16_t glyphCount = 0;
    uint32_t bufferUsed = 0;      // bytes of extracted glyph data in buffer
    uint32_t bufferCapacity = 0;  // allocated size of buffer
  };
  PageSlot pageSlots[MAX_PAGE_SLOTS] = {};

  PageSlot* findSlot(const EpdFontData* fontData);
  PageSlot* allocateSlot();
  void freeSlot(PageSlot& slot);
  // Merge `missing` (sorted, not present in slot) into the slot, compacting
  // retained bitmap data into a new buffer. False = caps/OOM, caller rebuilds.
  bool mergeIntoSlot(PageSlot& slot, const EpdFontData* fontData, const uint32_t* missing, uint16_t missingCount);
  // Decompress the groups covering entries with bufferOffset==UINT32_MAX and
  // extract them into the slot buffer. Returns the number of glyphs missed.
  int extractPending(PageSlot& slot, const EpdFontData* fontData);

  // Hot group: last decompressed group (byte-aligned) for non-prewarmed fallback path.
  // Kept in byte-aligned format; individual glyphs are compacted on demand into hotGlyphBuf.
  const EpdFontData* hotGroupFont = nullptr;
  uint16_t hotGroupIndex = UINT16_MAX;
  std::vector<uint8_t> hotGroup;

  // Scratch buffer for compacting a single glyph from the hot group.
  // Valid until the next getBitmap() call.
  std::vector<uint8_t> hotGlyphBuf;

  void freePageBuffer();
  void freeHotGroup();
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);
};
