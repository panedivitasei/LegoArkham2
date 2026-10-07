#pragma once
#include <string>

namespace climb {

void Characters(const std::string& names);

constexpr int kModeHop = 1;    // the state is entered but never held: a jump at the wall each frame
constexpr int kModeClimb = 2;  // the state is held with the climb-wall contact mark: the real climb

// Enable surface climbing for listed players; reach is in tenths of a world unit.
// key is a DirectInput scancode, or zero for automatic attachment.
void Install(int mode, int reachTenths, int key);

// Native climb speed percentage, 25 to 400, for listed players with climbing enabled.
void Speed(int percent);

// Scales the requested velocity the game's climb move handler just wrote; called from the move
// detour before the mod's own ceiling climb reads it.
void ScaleNativeMove(int character);

// Per-character, per-frame; called from the anim-sync detour after the game's own update.
void Tick(int character);
void __cdecl NativeLogic(int character);
void* LogicTarget();

bool LedgeActive(int character);
bool TryLedge(int character);
bool MoveLedge(int character);

}  // namespace climb
