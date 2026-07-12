#pragma once

#include <string>

class GfxRenderer;

namespace HeaderDateUtils {

struct DisplayDateInfo {
  uint32_t timestamp = 0;
  bool usedFallback = false;
};

DisplayDateInfo getDisplayDateInfo();
std::string getDisplayDateText();
std::string getSyncDayReminderText();
// Free RAM, e.g. "RAM 84K" (verbose, home top bar) or "84K" (compact,
// reader status bar).
std::string getSystemInfoText(bool compact);
void drawTopLine(GfxRenderer& renderer, const std::string& dateText);
void drawHeaderWithDate(GfxRenderer& renderer, const char* title, const char* subtitle = nullptr);

}  // namespace HeaderDateUtils
