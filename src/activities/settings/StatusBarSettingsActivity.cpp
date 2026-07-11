#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <cstring>
#include <memory>

#include "ClockOffsetActivity.h"
#include "ClockSyncActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// The last four entries are the DS3231 clock settings; they only show when
// the RTC is present (X3).
constexpr int MENU_ITEMS = 11;
constexpr int BASE_MENU_ITEMS = 7;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_CHAPTER_PAGE_COUNT,
                                     StrId::STR_BOOK_PROGRESS_PERCENTAGE,
                                     StrId::STR_PROGRESS_BAR,
                                     StrId::STR_PROGRESS_BAR_THICKNESS,
                                     StrId::STR_TITLE,
                                     StrId::STR_BATTERY,
                                     StrId::STR_XTC_STATUS_BAR,
                                     StrId::STR_CLOCK,
                                     StrId::STR_CLOCK_FORMAT,
                                     StrId::STR_CLOCK_UTC_OFFSET,
                                     StrId::STR_CLOCK_SYNC};

int menuItemCount() { return halClock.isAvailable() ? MENU_ITEMS : BASE_MENU_ITEMS; }

constexpr int STATUS_BAR_CLOCK_ITEMS = 3;
const StrId statusBarClockNames[STATUS_BAR_CLOCK_ITEMS] = {StrId::STR_HIDE, StrId::STR_DIR_RIGHT, StrId::STR_DIR_LEFT};
constexpr int PROGRESS_BAR_ITEMS = 3;
const StrId progressBarNames[PROGRESS_BAR_ITEMS] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};

constexpr int PROGRESS_BAR_THICKNESS_ITEMS = 3;
const StrId progressBarThicknessNames[PROGRESS_BAR_THICKNESS_ITEMS] = {
    StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK};

constexpr int TITLE_ITEMS = 3;
const StrId titleNames[TITLE_ITEMS] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};

constexpr int XTC_STATUS_BAR_ITEMS = 3;
const StrId xtcStatusBarNames[XTC_STATUS_BAR_ITEMS] = {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP};

const int widthMargin = 10;
const int verticalPreviewPadding = 50;
const int verticalPreviewTextPadding = 40;
}  // namespace

void StatusBarSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;

  // Clamp statusBarProgressBar and statusBarTitle in case of corrupt/migrated data
  if (SETTINGS.statusBarProgressBar >= PROGRESS_BAR_ITEMS) {
    SETTINGS.statusBarProgressBar = CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  }

  if (SETTINGS.statusBarTitle >= PROGRESS_BAR_THICKNESS_ITEMS) {
    SETTINGS.statusBarTitle = CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS::PROGRESS_BAR_NORMAL;
  }

  if (SETTINGS.statusBarTitle >= TITLE_ITEMS) {
    SETTINGS.statusBarTitle = CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE;
  }

  if (SETTINGS.xtcStatusBarMode >= XTC_STATUS_BAR_ITEMS) {
    SETTINGS.xtcStatusBarMode = CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_HIDE;
  }

  if (SETTINGS.statusBarClock >= STATUS_BAR_CLOCK_ITEMS) {
    SETTINGS.statusBarClock = CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_HIDE;
  }

  requestUpdate();
}

void StatusBarSettingsActivity::onExit() { Activity::onExit(); }

void StatusBarSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, menuItemCount());
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, menuItemCount());
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, menuItemCount());
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, menuItemCount());
    requestUpdate();
  });
}

void StatusBarSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Chapter Page Count
    SETTINGS.statusBarChapterPageCount = (SETTINGS.statusBarChapterPageCount + 1) % 2;
  } else if (selectedIndex == 1) {
    // Book Progress %
    SETTINGS.statusBarBookProgressPercentage = (SETTINGS.statusBarBookProgressPercentage + 1) % 2;
  } else if (selectedIndex == 2) {
    // Progress Bar
    SETTINGS.statusBarProgressBar = (SETTINGS.statusBarProgressBar + 1) % PROGRESS_BAR_ITEMS;
  } else if (selectedIndex == 3) {
    // Progress Bar Thickness
    SETTINGS.statusBarProgressBarThickness =
        (SETTINGS.statusBarProgressBarThickness + 1) % PROGRESS_BAR_THICKNESS_ITEMS;
  } else if (selectedIndex == 4) {
    // Chapter Title
    SETTINGS.statusBarTitle = (SETTINGS.statusBarTitle + 1) % TITLE_ITEMS;
  } else if (selectedIndex == 5) {
    // Show Battery
    SETTINGS.statusBarBattery = (SETTINGS.statusBarBattery + 1) % 2;
  } else if (selectedIndex == 6) {
    // XTC Status Bar
    SETTINGS.xtcStatusBarMode = (SETTINGS.xtcStatusBarMode + 1) % XTC_STATUS_BAR_ITEMS;
  } else if (selectedIndex == 7) {
    // Clock (Hide / Right / Left)
    SETTINGS.statusBarClock = (SETTINGS.statusBarClock + 1) % STATUS_BAR_CLOCK_ITEMS;
  } else if (selectedIndex == 8) {
    // Clock Format (24h / 12h)
    SETTINGS.clockFormat = (SETTINGS.clockFormat + 1) % 2;
  } else if (selectedIndex == 9) {
    // Clock UTC Offset — dedicated picker
    startActivityForResult(std::make_unique<ClockOffsetActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { requestUpdate(); });
    return;
  } else if (selectedIndex == 10) {
    // Sync Clock over NTP
    startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { requestUpdate(); });
    return;
  }
  SETTINGS.saveToFile();
}

void StatusBarSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CUSTOMISE_STATUS_BAR));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, menuItemCount(),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [this](int index) {
        // Draw status for each setting
        if (index == 0) {
          return SETTINGS.statusBarChapterPageCount ? tr(STR_SHOW) : tr(STR_HIDE);
        } else if (index == 1) {
          return SETTINGS.statusBarBookProgressPercentage ? tr(STR_SHOW) : tr(STR_HIDE);
        } else if (index == 2) {
          return I18N.get(progressBarNames[SETTINGS.statusBarProgressBar]);
        } else if (index == 3) {
          return I18N.get(progressBarThicknessNames[SETTINGS.statusBarProgressBarThickness]);
        } else if (index == 4) {
          return I18N.get(titleNames[SETTINGS.statusBarTitle]);
        } else if (index == 5) {
          return SETTINGS.statusBarBattery ? tr(STR_SHOW) : tr(STR_HIDE);
        } else if (index == 6) {
          return I18N.get(xtcStatusBarNames[SETTINGS.xtcStatusBarMode]);
        } else if (index == 7) {
          return I18N.get(statusBarClockNames[SETTINGS.statusBarClock]);
        } else if (index == 8) {
          return SETTINGS.clockFormat == 1 ? tr(STR_CLOCK_FORMAT_12H) : tr(STR_CLOCK_FORMAT_24H);
        } else if (index == 9) {
          // Show the current offset as e.g. "+7:00"
          static char offsetBuf[8];
          const int q = static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48;
          const int aq = q < 0 ? -q : q;
          snprintf(offsetBuf, sizeof(offsetBuf), "%c%d:%02d", q < 0 ? '-' : '+', aq / 4, (aq % 4) * 15);
          return static_cast<const char*>(offsetBuf);
        } else if (index == 10) {
          return SETTINGS.clockHasBeenSynced ? tr(STR_CLOCK_SYNC_OK) : "";
        } else {
          return tr(STR_HIDE);
        }
      },
      true);

  // Draw button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  std::string title;
  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = tr(STR_EXAMPLE_BOOK);
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_EXAMPLE_CHAPTER);
  }

  GUI.drawStatusBar(renderer, 75, 8, 32, title, verticalPreviewPadding, 0, false);

  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding,
                    renderer.getScreenHeight() - UITheme::getInstance().getStatusBarHeight() - verticalPreviewPadding -
                        verticalPreviewTextPadding,
                    tr(STR_PREVIEW));

  renderer.displayBuffer();
}
