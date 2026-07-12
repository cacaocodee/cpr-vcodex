#include "Section.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <Serialization.h>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 34;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                                 sizeof(bool) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t);

struct PageLutEntry {
  uint32_t fileOffset;
  uint32_t xhtmlByteOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
};

constexpr uint32_t PARAGRAPH_LUT_ENTRY_SIZE = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t);

// RAM LUT limits: a 2048-page section costs 8KB (page LUT) + 16KB (paragraph
// LUT); larger sections fall back to the legacy on-disk lookups.
constexpr uint16_t MAX_RAM_LUT_PAGES = 2048;
// A page blob is typically 1-6KB; tables can be larger. Above this the
// legacy streaming path handles the page.
constexpr size_t MAX_PAGE_SPAN_BYTES = 24u * 1024u;
// Keep this much contiguous heap available after transient read buffers
constexpr size_t SPAN_ALLOC_HEADROOM = 8u * 1024u;
}  // namespace

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }
  if (!page) {
    LOG_ERR("SCT", "Null page for page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                                     const uint16_t viewportWidth, const uint16_t viewportHeight,
                                     const bool hyphenationEnabled, const bool focusReadingEnabled,
                                     const bool embeddedStyle,
                                     const uint8_t imageRendering) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(forceParagraphIndents) +
                                   sizeof(paragraphAlignment) + sizeof(viewportWidth) + sizeof(viewportHeight) +
                                   sizeof(pageCount) + sizeof(hyphenationEnabled) + sizeof(embeddedStyle) +
                                   sizeof(focusReadingEnabled) + sizeof(imageRendering) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, forceParagraphIndents);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, focusReadingEnabled);
  serialization::writePod(file, embeddedStyle);
  serialization::writePod(file, imageRendering);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                              const uint16_t viewportWidth, const uint16_t viewportHeight,
                              const bool hyphenationEnabled, const bool focusReadingEnabled,
                              const bool embeddedStyle, const uint8_t imageRendering) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    bool fileForceParagraphIndents;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileFocusReadingEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileForceParagraphIndents);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileFocusReadingEnabled);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, fileImageRendering);

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || forceParagraphIndents != fileForceParagraphIndents ||
        paragraphAlignment != fileParagraphAlignment || viewportWidth != fileViewportWidth ||
        viewportHeight != fileViewportHeight || hyphenationEnabled != fileHyphenationEnabled ||
        focusReadingEnabled != fileFocusReadingEnabled || embeddedStyle != fileEmbeddedStyle ||
        imageRendering != fileImageRendering) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  serialization::readPod(file, pageCount);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  loadLutsFromFile();  // best effort — empty LUTs just mean the legacy paths run
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

bool Section::loadLutsFromFile() {
  pageLut_.clear();
  pageLut_.shrink_to_fit();
  paraLut_.clear();
  paraLut_.shrink_to_fit();
  closeReadFile();

  if (pageCount == 0 || pageCount > MAX_RAM_LUT_PAGES) {
    return false;
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return false;
  }
  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  serialization::readPod(f, lutOffset_);
  serialization::readPod(f, anchorMapOffset_);
  serialization::readPod(f, paragraphLutOffset_);

  const uint32_t pageLutBytes = static_cast<uint32_t>(pageCount) * sizeof(uint32_t);
  if (lutOffset_ < HEADER_SIZE || lutOffset_ + pageLutBytes > fileSize || anchorMapOffset_ < lutOffset_ ||
      paragraphLutOffset_ < anchorMapOffset_ || paragraphLutOffset_ >= fileSize) {
    LOG_ERR("SCT", "Invalid section offsets (lut=%u anchors=%u para=%u size=%u)", lutOffset_, anchorMapOffset_,
            paragraphLutOffset_, fileSize);
    lutOffset_ = anchorMapOffset_ = paragraphLutOffset_ = 0;
    f.close();
    return false;
  }

  // The LUTs are small (see MAX_RAM_LUT_PAGES) but don't gamble under
  // pressure — the legacy paths work without them.
  if (ESP.getMaxAllocHeap() < pageLutBytes + SPAN_ALLOC_HEADROOM) {
    f.close();
    return false;
  }
  pageLut_.resize(pageCount);
  f.seek(lutOffset_);
  if (f.read(reinterpret_cast<uint8_t*>(pageLut_.data()), pageLutBytes) != static_cast<int>(pageLutBytes)) {
    LOG_ERR("SCT", "Failed to read page LUT");
    pageLut_.clear();
    pageLut_.shrink_to_fit();
    f.close();
    return false;
  }

  // Paragraph LUT: entry layout on disk matches ParaLutEntry exactly
  static_assert(sizeof(ParaLutEntry) == PARAGRAPH_LUT_ENTRY_SIZE, "ParaLutEntry must match on-disk layout");
  f.seek(paragraphLutOffset_);
  uint16_t paraCount = 0;
  serialization::readPod(f, paraCount);
  const uint32_t paraBytes = static_cast<uint32_t>(paraCount) * PARAGRAPH_LUT_ENTRY_SIZE;
  if (paraCount > 0 && paraCount <= MAX_RAM_LUT_PAGES &&
      paragraphLutOffset_ + sizeof(uint16_t) + paraBytes <= fileSize &&
      ESP.getMaxAllocHeap() >= paraBytes + SPAN_ALLOC_HEADROOM) {
    paraLut_.resize(paraCount);
    if (f.read(reinterpret_cast<uint8_t*>(paraLut_.data()), paraBytes) != static_cast<int>(paraBytes)) {
      paraLut_.clear();
      paraLut_.shrink_to_fit();
    }
  }
  f.close();
  return true;
}

bool Section::ensureReadFile() const {
  if (readFileOpen_) {
    return true;
  }
  readFileOpen_ = Storage.openFileForRead("SCT", filePath, readFile_);
  return readFileOpen_;
}

void Section::closeReadFile() const {
  if (readFileOpen_) {
    readFile_.close();
    readFileOpen_ = false;
  }
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  closeReadFile();  // about to remove the file the persistent handle points at
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                                const uint16_t viewportWidth, const uint16_t viewportHeight,
                                const bool hyphenationEnabled, const bool focusReadingEnabled,
                                const bool embeddedStyle, const uint8_t imageRendering,
                                const std::function<void()>& popupFn) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // The build rewrites filePath — drop the persistent read handle and any
  // RAM LUTs pointing into the old file.
  closeReadFile();
  pageLut_.clear();
  pageLut_.shrink_to_fit();
  paraLut_.clear();
  paraLut_.shrink_to_fit();

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Retry logic for SD card timing issues
  bool success = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);  // Brief delay before retry
    }

    // Remove any incomplete file from previous attempt before retrying
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    FsFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    // Explicitly close() file before calling Storage.remove()
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    lastBuildDiag = "sd: stream chapter failed";
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    lastBuildDiag = "sd: open cache failed";
    return false;
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents, paragraphAlignment,
                         viewportWidth, viewportHeight, hyphenationEnabled, focusReadingEnabled, embeddedStyle,
                         imageRendering);
  std::vector<PageLutEntry> lut = {};

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      if (!cssParser->loadFromCache()) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents,
      paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled, focusReadingEnabled,
      [this, &lut](std::unique_ptr<Page> page, const ChapterHtmlSlimParser::ParagraphLutEntry syncEntry) {
        lut.push_back({this->onPageComplete(std::move(page)), syncEntry.xhtmlByteOffset, syncEntry.paragraphIndex,
                       syncEntry.listItemIndex});
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, std::move(tocAnchors), popupFn, cssParser);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    const auto heap = MemoryBudget::snapshot();
    LOG_ERR("SCT", "Failed to parse XML and build pages (lowMemoryAbort=%u imageFallback=%u free=%u maxAlloc=%u)",
            visitor.wasLowMemoryAbortTriggered() ? 1U : 0U, visitor.wasLowMemoryFallbackTriggered() ? 1U : 0U,
            heap.freeHeap, heap.maxAllocHeap);
    char diag[96];
    if (visitor.wasLowMemoryAbortTriggered()) {
      snprintf(diag, sizeof(diag), "oom @%s: %uK free / %uK max",
               visitor.getAbortStage() ? visitor.getAbortStage() : "?", visitor.getAbortFreeHeap() / 1024,
               visitor.getAbortMaxAlloc() / 1024);
    } else {
      snprintf(diag, sizeof(diag), "parse failed (not oom), %uK free", heap.freeHeap / 1024);
    }
    lastBuildDiag = diag;
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  if (visitor.wasLowMemoryFallbackTriggered()) {
    const auto heap = MemoryBudget::snapshot();
    LOG_DBG("SCT", "Section built with low-memory image fallback (free=%u maxAlloc=%u)", heap.freeHeap,
            heap.maxAllocHeap);
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (const auto& entry : lut) {
    if (entry.fileOffset == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, entry.fileOffset);
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = visitor.getAnchors();
  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  const uint32_t paragraphLutOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(lut.size()));
  for (const auto& entry : lut) {
    serialization::writePod(file, entry.xhtmlByteOffset);
    serialization::writePod(file, entry.paragraphIndex);
    serialization::writePod(file, entry.listItemIndex);
  }

  // Patch header with final pageCount, lutOffset, anchorMapOffset, and paragraphLutOffset
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 3 - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  if (cssParser) {
    cssParser->clear();
  }
  loadLutsFromFile();  // freshly built file — pick up the LUTs for fast page loads
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (currentPage < 0 || currentPage >= pageCount) {
    LOG_ERR("SCT", "Page load out of bounds: %d/%u", currentPage, pageCount);
    return nullptr;
  }

  // Fast path: RAM LUT + persistent handle + one contiguous read of the
  // page's byte span, then deserialize from memory.
  if (static_cast<size_t>(currentPage) < pageLut_.size() && lutOffset_ != 0 && ensureReadFile()) {
    const uint32_t start = pageLut_[currentPage];
    const uint32_t end = (currentPage + 1 < pageCount) ? pageLut_[currentPage + 1] : lutOffset_;
    if (start >= HEADER_SIZE && end > start && end <= lutOffset_) {
      const size_t span = end - start;
      if (span <= MAX_PAGE_SPAN_BYTES && ESP.getMaxAllocHeap() >= span + SPAN_ALLOC_HEADROOM) {
        std::vector<uint8_t> blob(span);
        if (readFile_.seek(start) &&
            readFile_.read(blob.data(), span) == static_cast<int>(span)) {
          serialization::MemReader reader(blob.data(), span);
          auto page = Page::deserialize(reader);
          if (page && !reader.overrun()) {
            return page;
          }
          LOG_ERR("SCT", "Page %d RAM deserialize failed (overrun=%d) — falling back to streaming", currentPage,
                  reader.overrun() ? 1 : 0);
        } else {
          LOG_ERR("SCT", "Page %d span read failed — reopening", currentPage);
          closeReadFile();
        }
      }
    } else {
      LOG_ERR("SCT", "Invalid RAM LUT span %u..%u (lut=%u) — dropping RAM LUT", start, end, lutOffset_);
      pageLut_.clear();
      pageLut_.shrink_to_fit();
    }
  }

  // Legacy streaming path — also the low-memory and corrupt-LUT fallback
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }

  const uint32_t fileSize = file.size();
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  uint32_t lutOffset;
  serialization::readPod(file, lutOffset);
  if (lutOffset == 0 || lutOffset >= fileSize) {
    LOG_ERR("SCT", "Invalid LUT offset: %u size=%u", lutOffset, fileSize);
    file.close();
    return nullptr;
  }

  const uint32_t lutEntryOffset = lutOffset + sizeof(uint32_t) * static_cast<uint32_t>(currentPage);
  if (lutEntryOffset + sizeof(uint32_t) > fileSize) {
    LOG_ERR("SCT", "Invalid LUT entry offset: %u size=%u", lutEntryOffset, fileSize);
    file.close();
    return nullptr;
  }

  file.seek(lutEntryOffset);
  uint32_t pagePos;
  serialization::readPod(file, pagePos);
  if (pagePos < HEADER_SIZE || pagePos >= lutOffset || pagePos >= fileSize) {
    LOG_ERR("SCT", "Invalid page offset: %u lut=%u size=%u", pagePos, lutOffset, fileSize);
    file.close();
    return nullptr;
  }

  file.seek(pagePos);

  serialization::FileReader reader(file);
  auto page = Page::deserialize(reader);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  return page;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    serialization::readString(f, key);
    serialization::readPod(f, page);
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  if (!paraLut_.empty()) {
    for (uint16_t i = 0; i < paraLut_.size(); i++) {
      if (paraLut_[i].paragraphIndex >= pIndex) {
        return i;
      }
    }
    return static_cast<uint16_t>(paraLut_.size() - 1);
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * PARAGRAPH_LUT_ENTRY_SIZE;
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint32_t xhtmlByteOffset;
    uint16_t pagePIdx;
    uint16_t listItemIndex;
    serialization::readPod(f, xhtmlByteOffset);
    serialization::readPod(f, pagePIdx);
    serialization::readPod(f, listItemIndex);
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  if (!paraLut_.empty()) {
    for (uint16_t i = 0; i < paraLut_.size(); i++) {
      if (paraLut_[i].listItemIndex >= liIndex) {
        return i;
      }
    }
    return static_cast<uint16_t>(paraLut_.size() - 1);
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * PARAGRAPH_LUT_ENTRY_SIZE;
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint32_t xhtmlByteOffset;
    uint16_t paragraphIndex;
    uint16_t pageLiIdx;
    serialization::readPod(f, xhtmlByteOffset);
    serialization::readPod(f, paragraphIndex);
    serialization::readPod(f, pageLiIdx);
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  if (!paraLut_.empty()) {
    if (page >= paraLut_.size()) {
      return std::nullopt;
    }
    return paraLut_[page].paragraphIndex;
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * PARAGRAPH_LUT_ENTRY_SIZE;
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset + sizeof(uint16_t) + page * PARAGRAPH_LUT_ENTRY_SIZE + sizeof(uint32_t));
  uint16_t pIdx;
  serialization::readPod(f, pIdx);
  return pIdx;
}

std::optional<uint32_t> Section::getXhtmlByteOffsetForPage(const uint16_t page) const {
  if (!paraLut_.empty()) {
    if (page >= paraLut_.size() || paraLut_[page].xhtmlByteOffset == 0) {
      return std::nullopt;
    }
    return paraLut_[page].xhtmlByteOffset;
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * PARAGRAPH_LUT_ENTRY_SIZE;
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset + sizeof(uint16_t) + page * PARAGRAPH_LUT_ENTRY_SIZE);
  uint32_t xhtmlByteOffset;
  serialization::readPod(f, xhtmlByteOffset);
  if (xhtmlByteOffset == 0) {
    return std::nullopt;
  }
  return xhtmlByteOffset;
}
