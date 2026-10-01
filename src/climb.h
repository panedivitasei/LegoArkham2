#pragma once

namespace climb {

constexpr int kModeHop = 1;    // the state is entered but never held: a jump at the wall each frame
constexpr int kModeClimb = 2;  // the state is held with the climb-wall contact mark: the real climb

// Makes every wall a climb-wall for the listed glide characters being played, by marking every
// surface type climbable in the game's surface table so its own wall-climb takes over. `mode` is
// kModeHop or kModeClimb, `reachTenths` how close a wall has to be for the climb mode's contact
// mark, `key` an optional DirectInput scancode that has to be held (0 = always).
void Install(int mode, int reachTenths, int key);

// Climb travel speed as a percentage of the game's own, 25 to 400; 100 leaves it alone. Applies to
// every character the game's wall climb moves, whether or not ClimbAnywhere is on.
void Speed(int percent);

// Scales the requested velocity the game's climb move handler just wrote; called from the move
// detour before the mod's own ceiling climb reads it.
void ScaleNativeMove(int character);

// Per-character, per-frame; called from the anim-sync detour after the game's own update.
void Tick(int character);

bool LedgeActive(int character);
bool TryLedge(int character);
bool MoveLedge(int character);

}  // namespace climb
