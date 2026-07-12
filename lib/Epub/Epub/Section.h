#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub.h"

class Page;
class GfxRenderer;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;

  // RAM copies of the section file's lookup tables plus a persistent read
  // handle: a page turn becomes one seek + one contiguous read instead of a
  // re-open, header parse, and per-field reads. Empty vectors = fall back to
  // the legacy streaming path.
  struct ParaLutEntry {
    uint32_t xhtmlByteOffset;
    uint16_t paragraphIndex;
    uint16_t listItemIndex;
  };
  std::vector<uint32_t> pageLut_;
  std::vector<ParaLutEntry> paraLut_;
  uint32_t lutOffset_ = 0;
  uint32_t anchorMapOffset_ = 0;
  uint32_t paragraphLutOffset_ = 0;
  mutable FsFile readFile_;
  mutable bool readFileOpen_ = false;

  bool loadLutsFromFile();
  bool ensureReadFile() const;
  void closeReadFile() const;

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing,
                              bool forceParagraphIndents, uint8_t paragraphAlignment, uint16_t viewportWidth,
                              uint16_t viewportHeight, bool hyphenationEnabled, bool focusReadingEnabled,
                              bool embeddedStyle, uint8_t imageRendering);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;
  // One-line diagnosis of the last createSectionFile failure (shown on the
  // section-load-failure screen so field reports carry the cause).
  std::string lastBuildDiag;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}
  ~Section() { closeReadFile(); }
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, bool forceParagraphIndents,
                       uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                       bool hyphenationEnabled, bool focusReadingEnabled, bool embeddedStyle, uint8_t imageRendering);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, bool forceParagraphIndents,
                         uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                         bool hyphenationEnabled, bool focusReadingEnabled, bool embeddedStyle, uint8_t imageRendering,
                         const std::function<void()>& popupFn = nullptr);
  std::unique_ptr<Page> loadPageFromSectionFile();

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from a KOReader li XPath.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;

  // Look up the XHTML byte offset recorded at the page boundary for the given page.
  std::optional<uint32_t> getXhtmlByteOffsetForPage(uint16_t page) const;
};
