#pragma once

#include <string>

#include "../Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  // quickTransition: skip the "Entering sleep" popup — used when cycling
  // wallpapers via double-click from sleep, where the popup would just be a
  // flash between two wallpapers.
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool quickTransition = false)
      : Activity("Sleep", renderer, mappedInput), quickTransition_(quickTransition) {}
  void onEnter() override;

 private:
  const bool quickTransition_;
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderReadingDashboardSleepScreen() const;
  void renderCoverStatsSleepScreen(bool footerOnly = false) const;
  void renderCustomStatsSleepScreen(bool footerOnly = false) const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, const std::string& sourcePath = "") const;
  bool renderPngSleepScreen(const std::string& sourcePath) const;
  void renderBlankSleepScreen() const;
  bool resolveLastBookCoverPath(std::string& coverBmpPath) const;
};
