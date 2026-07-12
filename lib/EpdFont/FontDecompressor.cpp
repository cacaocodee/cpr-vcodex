#include "FontDecompressor.h"

#include <Arduino.h>
#include <Logging.h>
#include <Utf8.h>

#include <cstdlib>
#include <new>

// Grow a byte vector without risking an uncatchable std::bad_alloc — this
// project builds with -fno-exceptions, so a failed resize() aborts the whole
// firmware (seen live on the X3 with the BLE HID host resident). Growth only
// proceeds when the heap has a large-enough contiguous block plus margin;
// returns false (leaving the vector usable) when it doesn't.
static bool safeGrow(std::vector<uint8_t>& buf, size_t newSize) {
  if (newSize <= buf.size()) {
    return true;
  }
  if (newSize > buf.capacity()) {
    // Vector growth allocates the new block while the old one is still held
    constexpr size_t HEADROOM = 1024;
    if (ESP.getMaxAllocHeap() < newSize + HEADROOM) {
      return false;
    }
  }
  buf.resize(newSize);
  return true;
}

FontDecompressor::~FontDecompressor() { deinit(); }

bool FontDecompressor::init() {
  clearCache();
  return true;
}

void FontDecompressor::deinit() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::clearCache() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::freePageBuffer() {
  for (uint8_t s = 0; s < MAX_PAGE_SLOTS; s++) {
    freeSlot(pageSlots[s]);
  }
}

void FontDecompressor::freeSlot(PageSlot& slot) {
  free(slot.buffer);
  free(slot.glyphs);
  slot = {};
}

FontDecompressor::PageSlot* FontDecompressor::findSlot(const EpdFontData* fontData) {
  for (uint8_t s = 0; s < MAX_PAGE_SLOTS; s++) {
    if (pageSlots[s].fontData == fontData && pageSlots[s].glyphs != nullptr) {
      return &pageSlots[s];
    }
  }
  return nullptr;
}

FontDecompressor::PageSlot* FontDecompressor::allocateSlot() {
  for (uint8_t s = 0; s < MAX_PAGE_SLOTS; s++) {
    if (pageSlots[s].glyphs == nullptr && pageSlots[s].buffer == nullptr) {
      return &pageSlots[s];
    }
  }
  return nullptr;
}

void FontDecompressor::trimTransient() { freeHotGroup(); }

void FontDecompressor::releaseSlotsNotIn(const EpdFontData* const* keep, uint8_t keepCount) {
  for (uint8_t s = 0; s < MAX_PAGE_SLOTS; s++) {
    if (pageSlots[s].fontData == nullptr) continue;
    bool retain = false;
    for (uint8_t k = 0; k < keepCount; k++) {
      if (pageSlots[s].fontData == keep[k]) {
        retain = true;
        break;
      }
    }
    if (!retain) {
      freeSlot(pageSlots[s]);
    }
  }
}

void FontDecompressor::freeHotGroup() {
  hotGroup.clear();
  hotGroup.shrink_to_fit();
  hotGroupFont = nullptr;
  hotGroupIndex = UINT16_MAX;
  hotGlyphBuf.clear();
  hotGlyphBuf.shrink_to_fit();
}

uint16_t FontDecompressor::getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex) {
  // O(1) path for frequency-grouped fonts with glyphToGroup mapping
  if (fontData->glyphToGroup != nullptr) {
    return fontData->glyphToGroup[glyphIndex];
  }

  // Contiguous-group fonts: linear scan
  for (uint16_t i = 0; i < fontData->groupCount; i++) {
    uint32_t first = fontData->groups[i].firstGlyphIndex;
    if (glyphIndex >= first && glyphIndex < first + fontData->groups[i].glyphCount) {
      return i;
    }
  }
  return fontData->groupCount;  // sentinel = not found
}

bool FontDecompressor::decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf,
                                       uint32_t outSize) {
  const EpdFontGroup& group = fontData->groups[groupIndex];

  const uint32_t tDecomp = millis();
  inflateReader.init(false);
  inflateReader.setSource(&fontData->bitmap[group.compressedOffset], group.compressedSize);
  if (!inflateReader.read(outBuf, outSize)) {
    stats.decompressTimeMs += millis() - tDecomp;
    LOG_ERR("FDC", "Decompression failed for group %u", groupIndex);
    return false;
  }
  stats.decompressTimeMs += millis() - tDecomp;
  return true;
}

// --- Byte-aligned helpers ---

uint32_t FontDecompressor::getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex) {
  uint32_t offset = 0;

  auto accumGlyph = [&](const EpdGlyph& g) {
    if (g.width > 0 && g.height > 0) {
      offset += ((g.width + 3) / 4) * g.height;
    }
  };

  if (fontData->glyphToGroup) {
    // Frequency-grouped: scan glyphs before glyphIndex that belong to this group
    for (uint32_t i = 0; i < glyphIndex; i++) {
      if (fontData->glyphToGroup[i] == groupIndex) {
        accumGlyph(fontData->glyph[i]);
      }
    }
  } else {
    // Contiguous-group: sum aligned sizes of preceding glyphs in the group
    const EpdFontGroup& group = fontData->groups[groupIndex];
    for (uint32_t i = group.firstGlyphIndex; i < glyphIndex; i++) {
      accumGlyph(fontData->glyph[i]);
    }
  }

  return offset;
}

void FontDecompressor::compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width,
                                          uint8_t height) {
  if (width == 0 || height == 0) return;
  const uint32_t rowStride = (width + 3) / 4;
  if (width % 4 == 0) {
    memcpy(packedDst, alignedSrc, rowStride * height);
    return;
  }
  uint8_t outByte = 0, outBits = 0;
  uint32_t writeIdx = 0;
  for (uint8_t y = 0; y < height; y++) {
    for (uint8_t x = 0; x < width; x++) {
      outByte = (outByte << 2) | ((alignedSrc[y * rowStride + x / 4] >> ((3 - (x % 4)) * 2)) & 0x3);
      outBits += 2;
      if (outBits == 8) {
        packedDst[writeIdx++] = outByte;
        outByte = 0;
        outBits = 0;
      }
    }
  }
  if (outBits > 0) packedDst[writeIdx] = outByte << (8 - outBits);
}

// --- getBitmap: page buffer → hot group → decompress ---

const uint8_t* FontDecompressor::getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex) {
  const uint32_t tStart = micros();
  stats.getBitmapCalls++;

  if (!fontData->groups || fontData->groupCount == 0) {
    stats.getBitmapTimeUs += micros() - tStart;
    return &fontData->bitmap[glyph->dataOffset];
  }

  // Check page buffer slots (populated by prewarmCache — one slot per font style)
  for (uint8_t s = 0; s < MAX_PAGE_SLOTS; s++) {
    const auto& slot = pageSlots[s];
    if (slot.fontData != fontData || slot.glyphCount == 0) continue;

    int left = 0, right = slot.glyphCount - 1;
    while (left <= right) {
      int mid = left + (right - left) / 2;
      if (slot.glyphs[mid].glyphIndex == glyphIndex) {
        if (slot.glyphs[mid].bufferOffset != UINT32_MAX) {
          stats.cacheHits++;
          stats.getBitmapTimeUs += micros() - tStart;
          return &slot.buffer[slot.glyphs[mid].bufferOffset];
        }
        break;  // Not extracted during prewarm; fall through to hot-group path
      }
      if (slot.glyphs[mid].glyphIndex < glyphIndex)
        left = mid + 1;
      else
        right = mid - 1;
    }
    break;  // Found the right slot but glyph wasn't in it; don't check other slots
  }

  // Fallback: hot group slot
  uint16_t groupIndex = getGroupIndex(fontData, glyphIndex);
  if (groupIndex >= fontData->groupCount) {
    LOG_ERR("FDC", "Glyph %u not found in any group", glyphIndex);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  // Check if hot group already has this group decompressed — if not, decompress it
  if (!(!hotGroup.empty() && hotGroupFont == fontData && hotGroupIndex == groupIndex)) {
    stats.cacheMisses++;
    const EpdFontGroup& group = fontData->groups[groupIndex];

    if (!safeGrow(hotGroup, group.uncompressedSize)) {
      LOG_ERR("FDC", "Failed to allocate %u bytes for hot group %u", group.uncompressedSize, groupIndex);
      hotGroupFont = nullptr;
      hotGroupIndex = UINT16_MAX;
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    if (!decompressGroup(fontData, groupIndex, hotGroup.data(), group.uncompressedSize)) {
      hotGroup.clear();
      hotGroup.shrink_to_fit();
      hotGroupFont = nullptr;
      hotGroupIndex = UINT16_MAX;
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    hotGroupFont = fontData;
    hotGroupIndex = groupIndex;
    stats.hotGroupBytes = group.uncompressedSize;
  } else {
    stats.cacheHits++;
  }

  // Compact just the requested glyph from byte-aligned data into scratch buffer
  if (!safeGrow(hotGlyphBuf, glyph->dataLength)) {
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  uint32_t alignedOff = getAlignedOffset(fontData, groupIndex, glyphIndex);
  compactSingleGlyph(&hotGroup[alignedOff], hotGlyphBuf.data(), glyph->width, glyph->height);
  stats.getBitmapTimeUs += micros() - tStart;
  return hotGlyphBuf.data();
}

// --- Prewarm: pre-decompress glyph bitmaps for a page of text ---

int32_t FontDecompressor::findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint) {
  const EpdUnicodeInterval* intervals = fontData->intervals;
  const int count = fontData->intervalCount;

  if (count == 0) return -1;

  // Binary search
  int left = 0;
  int right = count - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const EpdUnicodeInterval* interval = &intervals[mid];

    if (codepoint < interval->first) {
      right = mid - 1;
    } else if (codepoint > interval->last) {
      left = mid + 1;
    } else {
      return static_cast<int32_t>(interval->offset + (codepoint - interval->first));
    }
  }

  return -1;
}

int FontDecompressor::prewarmCache(const EpdFontData* fontData, const char* utf8Text) {
  if (!fontData || !fontData->groups || !utf8Text) return 0;

  // Step 1: Collect unique glyph indices needed for this page
  uint32_t neededGlyphs[MAX_PAGE_GLYPHS];
  uint16_t glyphCount = 0;
  bool glyphCapWarned = false;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    int32_t glyphIdx = findGlyphIndex(fontData, cp);
    if (glyphIdx < 0) continue;

    // Deduplicate
    bool found = false;
    for (uint16_t i = 0; i < glyphCount; i++) {
      if (neededGlyphs[i] == static_cast<uint32_t>(glyphIdx)) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (glyphCount < MAX_PAGE_GLYPHS) {
        neededGlyphs[glyphCount++] = static_cast<uint32_t>(glyphIdx);
      } else if (!glyphCapWarned) {
        LOG_DBG("FDC", "Glyph cap (%u) reached during prewarm; excess glyphs will use hot-group fallback",
                MAX_PAGE_GLYPHS);
        glyphCapWarned = true;
      }
    }
  }

  // Add ligature output glyphs: if both input codepoints of a ligature pair are
  // in the needed set, the output glyph will be queried during rendering.
  if (fontData->ligaturePairs && fontData->ligaturePairCount > 0) {
    for (uint32_t li = 0; li < fontData->ligaturePairCount && glyphCount < MAX_PAGE_GLYPHS; li++) {
      uint32_t leftCp = fontData->ligaturePairs[li].pair >> 16;
      uint32_t rightCp = fontData->ligaturePairs[li].pair & 0xFFFF;

      int32_t leftIdx = findGlyphIndex(fontData, leftCp);
      int32_t rightIdx = findGlyphIndex(fontData, rightCp);
      if (leftIdx < 0 || rightIdx < 0) continue;

      // Check if both inputs are in neededGlyphs
      bool hasLeft = false, hasRight = false;
      for (uint16_t i = 0; i < glyphCount; i++) {
        if (neededGlyphs[i] == static_cast<uint32_t>(leftIdx)) hasLeft = true;
        if (neededGlyphs[i] == static_cast<uint32_t>(rightIdx)) hasRight = true;
        if (hasLeft && hasRight) break;
      }
      if (!hasLeft || !hasRight) continue;

      int32_t outIdx = findGlyphIndex(fontData, fontData->ligaturePairs[li].ligatureCp);
      if (outIdx < 0) continue;

      // Deduplicate
      bool found = false;
      for (uint16_t i = 0; i < glyphCount; i++) {
        if (neededGlyphs[i] == static_cast<uint32_t>(outIdx)) {
          found = true;
          break;
        }
      }
      if (!found) {
        neededGlyphs[glyphCount++] = static_cast<uint32_t>(outIdx);
      }
    }
  }

  if (glyphCount == 0) return 0;

  // Sort needed glyph indices for the containment check / merge and so the
  // slot lookup table ends up sorted for getBitmap()'s binary search.
  for (uint16_t i = 1; i < glyphCount; i++) {
    const uint32_t key = neededGlyphs[i];
    int j = i - 1;
    while (j >= 0 && neededGlyphs[j] > key) {
      neededGlyphs[j + 1] = neededGlyphs[j];
      j--;
    }
    neededGlyphs[j + 1] = key;
  }

  // Cross-page reuse: if this font already has a slot, keep everything it
  // holds and only fetch what this page adds.
  if (PageSlot* existing = findSlot(fontData)) {
    uint32_t missing[MAX_PAGE_GLYPHS];
    uint16_t missingCount = 0;
    uint16_t si = 0;
    for (uint16_t i = 0; i < glyphCount; i++) {
      while (si < existing->glyphCount && existing->glyphs[si].glyphIndex < neededGlyphs[i]) si++;
      if (si >= existing->glyphCount || existing->glyphs[si].glyphIndex != neededGlyphs[i]) {
        missing[missingCount++] = neededGlyphs[i];
      }
    }
    if (missingCount == 0) {
      stats.cacheHits += glyphCount;  // full reuse — zero decompression
      return 0;
    }
    if (mergeIntoSlot(*existing, fontData, missing, missingCount)) {
      return extractPending(*existing, fontData);
    }
    // Merge refused (caps or allocation) — rebuild from this page only
    freeSlot(*existing);
  }

  PageSlot* slotPtr = allocateSlot();
  if (!slotPtr) {
    LOG_ERR("FDC", "All %u page buffer slots full, cannot prewarm fontData=%p", MAX_PAGE_SLOTS, (void*)fontData);
    return -1;
  }
  PageSlot& slot = *slotPtr;

  // Compute total buffer size for a fresh build
  uint32_t totalBytes = 0;
  for (uint16_t i = 0; i < glyphCount; i++) {
    totalBytes += fontData->glyph[neededGlyphs[i]].dataLength;
  }

  // Allocate page buffer and lookup table for this slot
  slot.buffer = static_cast<uint8_t*>(malloc(totalBytes));
  slot.glyphs = static_cast<PageGlyphEntry*>(malloc(glyphCount * sizeof(PageGlyphEntry)));
  if (!slot.buffer || !slot.glyphs) {
    LOG_ERR("FDC", "Failed to allocate page buffer (%u bytes, %u glyphs)", totalBytes, glyphCount);
    free(slot.buffer);
    free(slot.glyphs);
    slot = {};
    return glyphCount;
  }
  stats.pageBufferBytes += totalBytes;
  stats.pageGlyphsBytes += glyphCount * sizeof(PageGlyphEntry);

  slot.fontData = fontData;
  slot.glyphCount = glyphCount;
  slot.bufferCapacity = totalBytes;
  slot.bufferUsed = 0;

  // Initialize lookup entries, already sorted (bufferOffset = UINT32_MAX
  // means not yet extracted)
  for (uint16_t i = 0; i < glyphCount; i++) {
    slot.glyphs[i] = {neededGlyphs[i], UINT32_MAX, 0};
  }

  return extractPending(slot, fontData);
}

bool FontDecompressor::mergeIntoSlot(PageSlot& slot, const EpdFontData* fontData, const uint32_t* missing,
                                     uint16_t missingCount) {
  const uint32_t unionCount = static_cast<uint32_t>(slot.glyphCount) + missingCount;
  if (unionCount > MAX_PAGE_GLYPHS) {
    return false;
  }

  uint32_t newBytes = 0;
  for (uint16_t i = 0; i < missingCount; i++) {
    newBytes += fontData->glyph[missing[i]].dataLength;
  }
  // Count every existing entry — including still-pending ones (missed under
  // an earlier OOM) — so they have buffer capacity to extract on this pass.
  uint32_t retainedBytes = 0;
  for (uint16_t i = 0; i < slot.glyphCount; i++) {
    retainedBytes += fontData->glyph[slot.glyphs[i].glyphIndex].dataLength;
  }

  const uint32_t totalBytes = retainedBytes + newBytes;
  if (totalBytes > MAX_SLOT_BYTES) {
    return false;
  }
  const uint32_t glyphsBytes = unionCount * sizeof(PageGlyphEntry);
  if (ESP.getMaxAllocHeap() < totalBytes + glyphsBytes + 4096) {
    return false;
  }

  auto* newBuffer = static_cast<uint8_t*>(malloc(totalBytes));
  auto* newGlyphs = static_cast<PageGlyphEntry*>(malloc(glyphsBytes));
  if (!newBuffer || !newGlyphs) {
    free(newBuffer);
    free(newGlyphs);
    return false;
  }

  // Two-pointer merge of the sorted old entries and sorted missing indices,
  // compacting retained bitmap data into the new buffer as we go. A missing
  // index can never equal an old entry's index by construction.
  uint32_t writeOffset = 0;
  uint16_t oi = 0, mi = 0, outCount = 0;
  while (oi < slot.glyphCount || mi < missingCount) {
    const bool takeOld =
        mi >= missingCount || (oi < slot.glyphCount && slot.glyphs[oi].glyphIndex < missing[mi]);
    if (takeOld) {
      PageGlyphEntry e = slot.glyphs[oi++];
      if (e.bufferOffset != UINT32_MAX) {
        const uint32_t len = fontData->glyph[e.glyphIndex].dataLength;
        memcpy(newBuffer + writeOffset, slot.buffer + e.bufferOffset, len);
        e.bufferOffset = writeOffset;
        writeOffset += len;
      }
      newGlyphs[outCount++] = e;
    } else {
      newGlyphs[outCount++] = {missing[mi++], UINT32_MAX, 0};
    }
  }

  free(slot.buffer);
  free(slot.glyphs);
  slot.buffer = newBuffer;
  slot.glyphs = newGlyphs;
  slot.glyphCount = outCount;
  slot.bufferCapacity = totalBytes;
  slot.bufferUsed = writeOffset;
  stats.cacheHits += slot.glyphCount - missingCount;
  stats.pageBufferBytes += newBytes;
  return true;
}

int FontDecompressor::extractPending(PageSlot& slot, const EpdFontData* fontData) {
  // Collect the groups covering entries that still need extraction
  uint16_t neededGroups[128];
  uint8_t groupCount = 0;
  bool groupCapWarned = false;
  for (uint16_t i = 0; i < slot.glyphCount; i++) {
    if (slot.glyphs[i].bufferOffset != UINT32_MAX) continue;
    const uint16_t gi = getGroupIndex(fontData, slot.glyphs[i].glyphIndex);
    bool found = false;
    for (uint8_t j = 0; j < groupCount; j++) {
      if (neededGroups[j] == gi) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (groupCount < 128) {
        neededGroups[groupCount++] = gi;
      } else if (!groupCapWarned) {
        LOG_DBG("FDC", "Group cap (128) reached during prewarm; some groups will use hot-group fallback");
        groupCapWarned = true;
      }
    }
  }
  if (groupCount == 0) return 0;
  stats.uniqueGroupsAccessed += groupCount;

  // Pre-scan to compute each pending glyph's byte-aligned offset within its group.
  // This avoids recomputing aligned offsets per group during extraction in step 4.
  uint32_t groupAlignedTracker[128] = {};  // running byte-aligned offset for each needed group

  if (fontData->glyphToGroup) {
    // Frequency-grouped: single O(totalGlyphs) pass through glyphToGroup
    const auto& lastInterval = fontData->intervals[fontData->intervalCount - 1];
    const uint32_t totalGlyphs = lastInterval.offset + (lastInterval.last - lastInterval.first + 1);

    for (uint32_t i = 0; i < totalGlyphs; i++) {
      const uint16_t gi = fontData->glyphToGroup[i];
      // Find this glyph's group position in neededGroups
      uint8_t gpPos = groupCount;
      for (uint8_t j = 0; j < groupCount; j++) {
        if (neededGroups[j] == gi) {
          gpPos = j;
          break;
        }
      }
      if (gpPos == groupCount) continue;  // not a needed group

      const EpdGlyph& glyph = fontData->glyph[i];

      // Binary search in sorted slot.glyphs to find if glyph i is needed
      int left = 0, right = (int)slot.glyphCount - 1;
      while (left <= right) {
        const int mid = left + (right - left) / 2;
        if (slot.glyphs[mid].glyphIndex == i) {
          slot.glyphs[mid].alignedOffset = groupAlignedTracker[gpPos];
          break;
        }
        if (slot.glyphs[mid].glyphIndex < i)
          left = mid + 1;
        else
          right = mid - 1;
      }

      if (glyph.width > 0 && glyph.height > 0) {
        groupAlignedTracker[gpPos] += ((glyph.width + 3) / 4) * glyph.height;
      }
    }
  } else {
    // Contiguous-group: iterate each needed group's glyphs directly
    for (uint8_t g = 0; g < groupCount; g++) {
      const EpdFontGroup& group = fontData->groups[neededGroups[g]];
      uint32_t alignedOff = 0;
      for (uint16_t j = 0; j < group.glyphCount; j++) {
        const uint32_t glyphI = group.firstGlyphIndex + j;
        const EpdGlyph& glyph = fontData->glyph[glyphI];

        int left = 0, right = (int)slot.glyphCount - 1;
        while (left <= right) {
          const int mid = left + (right - left) / 2;
          if (slot.glyphs[mid].glyphIndex == glyphI) {
            slot.glyphs[mid].alignedOffset = alignedOff;
            break;
          }
          if (slot.glyphs[mid].glyphIndex < glyphI)
            left = mid + 1;
          else
            right = mid - 1;
        }

        if (glyph.width > 0 && glyph.height > 0) {
          alignedOff += ((glyph.width + 3) / 4) * glyph.height;
        }
      }
    }
  }

  // For each unique group, decompress to temp buffer and extract the pending
  // glyphs, appending after any bytes retained from the previous page.
  uint32_t writeOffset = slot.bufferUsed;
  int missed = 0;

  for (uint8_t g = 0; g < groupCount; g++) {
    uint16_t groupIdx = neededGroups[g];
    const EpdFontGroup& group = fontData->groups[groupIdx];

    auto* tempBuf = static_cast<uint8_t*>(malloc(group.uncompressedSize));
    if (!tempBuf) {
      LOG_ERR("FDC", "Failed to allocate temp buffer (%u bytes) for group %u", group.uncompressedSize, groupIdx);
      missed++;
      continue;
    }
    if (group.uncompressedSize > stats.peakTempBytes) {
      stats.peakTempBytes = group.uncompressedSize;
    }

    if (!decompressGroup(fontData, groupIdx, tempBuf, group.uncompressedSize)) {
      free(tempBuf);
      missed++;
      continue;
    }

    // Extract pending glyphs directly from the byte-aligned temp buffer,
    // compacting on the fly. alignedOffset was pre-computed above.
    for (uint16_t i = 0; i < slot.glyphCount; i++) {
      if (slot.glyphs[i].bufferOffset != UINT32_MAX) continue;  // already extracted
      if (getGroupIndex(fontData, slot.glyphs[i].glyphIndex) != groupIdx) continue;

      const EpdGlyph& glyph = fontData->glyph[slot.glyphs[i].glyphIndex];
      if (writeOffset + glyph.dataLength > slot.bufferCapacity) {
        missed++;  // capacity accounting mismatch — glyph falls to hot group
        continue;
      }
      compactSingleGlyph(&tempBuf[slot.glyphs[i].alignedOffset], &slot.buffer[writeOffset], glyph.width, glyph.height);
      slot.glyphs[i].bufferOffset = writeOffset;
      writeOffset += glyph.dataLength;
    }

    free(tempBuf);
  }
  slot.bufferUsed = writeOffset;

  LOG_DBG("FDC", "Prewarm: %u glyphs, %u bytes used, %u groups inflated (%d missed)", slot.glyphCount, writeOffset,
          groupCount, missed);

  return missed;
}

// --- Stats ---

void FontDecompressor::resetStats() { stats = Stats{}; }

void FontDecompressor::logStats(const char* label) {
  const uint32_t total = stats.cacheHits + stats.cacheMisses;
  LOG_DBG("FDC", "[%s] hits=%lu misses=%lu (%.1f%% hit rate)", label, stats.cacheHits, stats.cacheMisses,
          total > 0 ? 100.0f * stats.cacheHits / total : 0.0f);
  LOG_DBG("FDC", "[%s] decompress=%lums groups_accessed=%u", label, stats.decompressTimeMs, stats.uniqueGroupsAccessed);
  LOG_DBG("FDC", "[%s] mem: pageBuf=%lu pageGlyphs=%lu hotGroup=%lu peakTemp=%lu", label, stats.pageBufferBytes,
          stats.pageGlyphsBytes, stats.hotGroupBytes, stats.peakTempBytes);
  if (stats.getBitmapCalls > 0) {
    LOG_DBG("FDC", "[%s] getBitmap: %lu calls, %luus total, %luus/call avg", label, stats.getBitmapCalls,
            stats.getBitmapTimeUs, stats.getBitmapTimeUs / stats.getBitmapCalls);
  }
  resetStats();
}
