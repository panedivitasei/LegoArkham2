#pragma once

#include <string>

namespace glide {

// Lets the listed characters use the game's own glide in every level. `characters` is a comma
// separated list of character names as the game names them, matched whole. Needs the exe unpacked.
void Install(const std::string& characters);

// Hooks the glide move handler and scales its horizontal travel speed by `percent`.
void Speed(int percent);

// The game's own jump-held-or-pressed test for a character.
bool JumpDown(int character);

// Whether the character resource name is on the list given to Install.
bool IsListed(int character);

// The dive binding is held with jump during the glide. Gain, max and
// decay are percentages of the glide speed (per second, total, per second); descent and lift are
// tenths of a unit a second, lift per 100% of banked speed; forward is the glide speed kept while
// diving, in percent; the lift fades over liftTime (tenths of a second); the dive's own crossfade
// with the glide is easeMilliseconds; the transition out of the dive takes pullOut and the one
// into it tuck (tenths of a second), both along a curve of the given sharpness (tenths, 10 =
// straight) and attack (percent front-loaded); the transition clips are scrubbed along it too.
void DiveSetup(int gainPercent, int maxPercent, int decayPercent, int descentTenths, int liftTenths,
               int forwardPercent, int liftTimeTenths, int easeMilliseconds, int pullOutTenths, int tuckTenths,
               int curveTenths, int attackPercent);

// Removes the level's glide time limit for the listed characters.
void Unlimited();

// Nose-down pitch at a full dive and nose-up pitch in the climb, in degrees.
void DivePitch(int degrees, int climbDegrees);

// Tilts a listed character's world matrix for the dive; call once per frame after the game's
// own update of it.
void ApplyPitch(int character);

// Starts the fall landing for a dive that met the ground; call once per frame before the game's
// anim sync.
void BeforeSync(int character);

// Enable private dive and glide clips for the named characters.
void DiveClip(const std::string& characters);
void GlideOverride(const std::string& characters);

// The one-shot pull-out (dive to glide) and tuck (glide to dive) clips, each over the pack entry
// named, with their frame counts at 30 fps; empty names skip them.
void TransitionClips(const std::string& pullout, int pulloutFrameCount, const std::string& tuck, int tuckFrameCount);

// Holding jump through an airborne double-tap roll for this long (tenths of a second) ends the
// roll and lets the glide start; 0 = off.
void RollGlide(int holdTenths);

// Whether a dive that hits the ground plays the character's fall landing instead of the glide's.
void Landing(bool hard);

// Scales the listed characters' downward acceleration outside the glide to `percent` of the game's own.
void FallSpeed(int percent);

// Crossfade length, in milliseconds, between the glide, the fall and the clip the glide interrupts.
void Ease(int milliseconds);

}  // namespace glide
