#include "cutscene.h"

#include <cstdint>

#include "frame.h"

namespace cutscene {
namespace {

// LEGOBatman2.exe addresses (Steam build, no ASLR).
// The cutscene player asks one flag on the game-settings object before it accepts the skip
// input: `allowcutskip` from stuff\game.txt, which the shipped file leaves off, or the N0CUT5
// cheat. Kept on here, so the skip prompt shows on every scene.
constexpr uintptr_t kSettingsPtr = 0x11BDF70;
constexpr int kAllowSkipOffset = 220;

void AllowSkip() {
  auto settings = *reinterpret_cast<uint8_t**>(kSettingsPtr);
  if (settings) settings[kAllowSkipOffset] = 1;
}

}  // namespace

void Install() { frame::OnTick(AllowSkip); }

}  // namespace cutscene
