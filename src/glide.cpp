#include "glide.h"

#include <windows.h>

#include <cstdint>
#include <vector>

#include <algorithm>
#include <cmath>

#include "cape_sound.h"
#include "glide_anim.h"
#include "hook.h"
#include "ability_controls.h"
#include "player_character.h"

namespace glide {
namespace {

// LEGOBatman2.exe addresses (Steam build, no ASLR).
// The glide is a state of the game's own, entered from the character update's glide logic when
// jump is pressed or held while falling. That logic has no ability check; what keeps it out of
// story levels are two flags on the level definition: it wants super_bonus_level (Gotham City)
// and refuses levelplay_level. Both tests are taken out of the code and put back, per character,
// in front of the call that enters the glide.
constexpr uintptr_t kBonusLevelTest = 0xAC7196;  // `and edi, 1` on levelDef+220 bit 25
constexpr uintptr_t kLevelplayMask = 0xAC71F4;   // imm32 of `test [ecx+0DCh], 4000000h`
constexpr uintptr_t kEnterCallSite = 0xAC731D;   // call Character_EnterGlide(character)
constexpr uintptr_t kEnterGlide = 0xAC6A10;
constexpr int kCharacterInstance = 4104;   // -> +44 level definition
constexpr int kController = 5804;          // the player's input object, null on AI characters
constexpr int kLevelFlagsOffset = 220;
constexpr int kStateOffset = 1270;             // u16 current state id on the character
constexpr uintptr_t kGlideStateId = 0x11A2E10;  // dword the glide state registers under
constexpr uint32_t kSuperBonusLevel = 0x2000000;
constexpr uint32_t kLevelplayLevel = 0x4000000;

using EnterFn = int(__cdecl*)(int character);

std::vector<std::string> allowed;

// A comma separated list into its trimmed, non-empty items.
std::vector<std::string> Split(const std::string& list) {
  std::vector<std::string> items;
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) end = list.size();
    size_t a = start, b = end;
    while (a < b && list[a] == ' ') ++a;
    while (b > a && list[b - 1] == ' ') --b;
    if (b > a) items.push_back(list.substr(a, b - a));
    start = end + 1;
  }
  return items;
}

bool GameAllows(int character) {
  int instance = *reinterpret_cast<int*>(character + kCharacterInstance);
  int definition = instance ? *reinterpret_cast<int*>(instance + 44) : 0;
  if (!definition) return false;
  uint32_t flags = *reinterpret_cast<uint32_t*>(definition + kLevelFlagsOffset);
  return (flags & kSuperBonusLevel) && !(flags & kLevelplayLevel);
}

const char* Name(int character) {
  return playercharacter::ResourceName(character);
}

bool Listed(int character) {
  const char* name = Name(character);
  if (!name) return false;
  for (const std::string& entry : allowed)
    if (_stricmp(entry.c_str(), name) == 0) return true;
  return false;
}

// Anim entry records carry a blend-in and a blend-out time in 50 ms steps, and the player fades
// between two clips for the smaller of the outgoing clip's blend-out and the incoming clip's
// blend-in; a zero on either side is a cut. The glide, the fall and whatever clip the glide
// interrupts get an ease of their own on the listed characters' anim sets.
constexpr uintptr_t kFindEntry = 0x651E40;  // thiscall(set, index, 0) -> entry record
constexpr uintptr_t kFallAnimId = 0x11A1A98;
constexpr uintptr_t kGlideAnimId = 0x11A2230;
constexpr int kCharacterAnimSet = 4100;  // -> +4 set object
constexpr int kEntryBlendIn = 274;
constexpr int kEntryBlendOut = 275;
constexpr int kRequestedAnim = 1258;

using FindEntryFn = int(__fastcall*)(int set, void* edx, int index, int flag);

int8_t easeSteps;

void EaseEntry(int character, int index, int8_t steps) {
  if (!steps || index < 0) return;
  int set = *reinterpret_cast<int*>(character + kCharacterAnimSet);
  set = set ? *reinterpret_cast<int*>(set + 4) : 0;
  if (!set) return;
  int entry = reinterpret_cast<FindEntryFn>(kFindEntry)(set, nullptr, index, 0);
  if (!entry) return;
  auto in = reinterpret_cast<int8_t*>(entry + kEntryBlendIn);
  auto out = reinterpret_cast<int8_t*>(entry + kEntryBlendOut);
  *in = steps;
  *out = steps;
}

// The glide logic ends a glide after the level's time limit, read from the level definition at
// +208 every frame; a limit of zero there means none.
constexpr int kLevelGlideTime = 208;
bool unlimited;

// Dive and Robin glide use private entries; locomotion keeps its stock clips.
constexpr uintptr_t kAnimIdOf = 0x945980;  // cdecl(name) -> anim id
constexpr uintptr_t kRequestAnim = 0x4D7B30;  // thiscall(character, id, 0) -> requests it for the sync
constexpr int kEntryLoopFields = 270;  // flags, speed, blend, frame range, through +279
constexpr int kEntryLoopLength = 10;
constexpr int kEntryTriggers = 428;  // list head, tail and count of the entry's trigger records
constexpr int kEntryTriggersLength = 12;

using AnimIdFn = int(__cdecl*)(const char* name);
using RequestFn = bool(__fastcall*)(int character, void* edx, int id, int flag);

std::vector<std::string> diveCharacters;  // characters allowed to request the private dive
int diveAnim = -1;
std::vector<int> preparedSets;  // sets prepared for dive transitions
int8_t diveEaseSteps;

bool Prepared(int character) {
  int set = *reinterpret_cast<int*>(character + kCharacterAnimSet);
  set = set ? *reinterpret_cast<int*>(set + 4) : 0;
  for (int prepared : preparedSets)
    if (prepared == set) return true;
  return false;
}

// The two one-shot transitions, each over a donor of its own: the tuck plays when the dive starts
// and the pull-out when it ends, and each hands over to its loop when its frames run out. Their
// endpoints match the loops, so they get a 50 ms blend on both sides.
constexpr int kEntryFirstFrame = 276;  // int16 frame range of the entry record
constexpr int kEntryLastFrame = 278;
std::string pulloutDonor, tuckDonor;
int pulloutAnim = -1, tuckAnim = -1;
int pulloutFrames, tuckFrames;

void PrepareTransition(int character, int set, const std::string& donor, int& anim, int frames) {
  if (donor.empty() || frames <= 0) return;
  if (anim < 0) anim = reinterpret_cast<AnimIdFn>(kAnimIdOf)(donor.c_str());
  int entry = reinterpret_cast<FindEntryFn>(kFindEntry)(set, nullptr, anim, 0);
  if (!entry) return;
  *reinterpret_cast<int16_t*>(entry + kEntryFirstFrame) = 0;
  *reinterpret_cast<int16_t*>(entry + kEntryLastFrame) = static_cast<int16_t>(frames - 1);
  memset(reinterpret_cast<void*>(entry + kEntryTriggers), 0, kEntryTriggersLength);
  EaseEntry(character, anim, 1);
}

bool Named(int character, const std::vector<std::string>& names) {
  const char* name = Name(character);
  if (!name) return false;
  for (const std::string& entry : names)
    if (_stricmp(entry.c_str(), name) == 0) return true;
  return false;
}

bool HasDiveClip(int character) { return Named(character, diveCharacters); }

// Robin's cape uses its own glide pose.
std::vector<std::string> glideCharacters;
int glideOverrideAnim = -1;

bool HasGlideOverride(int character) { return glideOverrideAnim >= 0 && Named(character, glideCharacters); }

// Copy loop settings into a private entry.
bool DressAsGlide(int set, int glide, int anim) {
  int donor = reinterpret_cast<FindEntryFn>(kFindEntry)(set, nullptr, anim, 0);
  if (!donor) return false;
  memcpy(reinterpret_cast<void*>(donor + kEntryLoopFields), reinterpret_cast<const void*>(glide + kEntryLoopFields),
         kEntryLoopLength);
  memset(reinterpret_cast<void*>(donor + kEntryTriggers), 0, kEntryTriggersLength);
  return true;
}

// The glide's own blend-in and blend-out take the dive's longer ease too: the glide fades into
// and out of the dive over that time, while its fade into the fall stays the fall's shorter one,
// since the player always takes the shorter of the two sides.
void PrepareDive(int character) {
  bool dive = HasDiveClip(character);
  bool override = Named(character, glideCharacters);
  if (!dive && !override) return;
  if (dive) diveAnim = glideanim::DiveId();
  if (override) glideOverrideAnim = glideanim::GlideOverrideId();
  int set = *reinterpret_cast<int*>(character + kCharacterAnimSet);
  set = set ? *reinterpret_cast<int*>(set + 4) : 0;
  if (!set) return;
  int glideId = *reinterpret_cast<int16_t*>(kGlideAnimId);
  int glide = reinterpret_cast<FindEntryFn>(kFindEntry)(set, nullptr, glideId, 0);
  if (!glide) return;
  if (!Prepared(character)) preparedSets.push_back(set);
  int8_t steps = diveEaseSteps ? diveEaseSteps : easeSteps;
  EaseEntry(character, glideId, steps);
  if (override && DressAsGlide(set, glide, glideOverrideAnim)) EaseEntry(character, glideOverrideAnim, steps);
  if (dive && DressAsGlide(set, glide, diveAnim)) {
    EaseEntry(character, diveAnim, steps);
    PrepareTransition(character, set, pulloutDonor, pulloutAnim, pulloutFrames);
    PrepareTransition(character, set, tuckDonor, tuckAnim, tuckFrames);
  }
}

void ResetDive(int character);

int __cdecl OnEnterGlide(int character) {
  if (!GameAllows(character) && !Listed(character)) return 0;
  if (Listed(character)) {
    // A glide that ended mid-dive never saw the release; the next one starts clean instead of
    // finishing that transition into the ground and then lifting on the old banked speed.
    ResetDive(character);
    if (*reinterpret_cast<int*>(character + kController)) capesound::StartGlide();
    if (unlimited) {
      int instance = *reinterpret_cast<int*>(character + kCharacterInstance);
      int definition = instance ? *reinterpret_cast<int*>(instance + 44) : 0;
      if (definition) *reinterpret_cast<float*>(definition + kLevelGlideTime) = 0.0f;
    }
    EaseEntry(character, *reinterpret_cast<int16_t*>(kGlideAnimId), easeSteps);
    EaseEntry(character, *reinterpret_cast<int16_t*>(kFallAnimId), easeSteps);
    EaseEntry(character, *reinterpret_cast<int16_t*>(character + kRequestedAnim), easeSteps);
    PrepareDive(character);
  }
  return reinterpret_cast<EnterFn>(kEnterGlide)(character);
}

// Airborne fields on the character record: the air mode byte, the vertical velocity the integrator
// reads, the vertical move the glide handler writes and the character's own gravity.
constexpr int kAirMode = 1289;
constexpr int kVelocityY = 4116;
constexpr int kMoveY = 3764;
constexpr int kGravity = 3844;

void PatchPointer(uintptr_t slot, uintptr_t expected, uintptr_t value) {
  hook::Patch(slot,
              {static_cast<BYTE>(expected), static_cast<BYTE>(expected >> 8), static_cast<BYTE>(expected >> 16),
               static_cast<BYTE>(expected >> 24)},
              {static_cast<BYTE>(value), static_cast<BYTE>(value >> 8), static_cast<BYTE>(value >> 16),
               static_cast<BYTE>(value >> 24)});
}

// The Gotham hub's falling-with-style code dresses every long fall as a glide. Each frame a cape
// character has no ground within four units below, a hub routine points the character's fall
// slot, the pending anim request and the fall-to-glide swap pair at the glide clip, and the anim
// sync then plays the glide whenever the fall is asked for. A hub event does the same to the anim
// request on its own, and another hub event pins a falling cape character's velocity to a fixed
// dive (4 units a second down plus a forward push) instead of letting gravity work. All sit behind
// vtable entries, so the entries are retargeted and the writes undone or skipped for the listed
// characters, which leaves them the plain fall the Batcave plays, at the Batcave's speed.
constexpr uintptr_t kHighFallSlot = 0xD02B14;    // vtable entry holding sub_46B1A0
constexpr uintptr_t kHighFall = 0x46B1A0;
constexpr uintptr_t kStyleEventSlot = 0xD0A61C;  // FallingWithStyle secondary vtable, slot 0
constexpr uintptr_t kStyleEvent = 0x493FC0;
constexpr int kFallSlot = 610;      // i16 anim index the fall state plays
constexpr int kSwapFrom = 812;      // i16 pair the anim sync swaps, from -> to
constexpr int kSwapTo = 814;
constexpr int kRequestNow = 4082;   // i16 anim request block indices the player reads
constexpr int kRequestNext = 4086;
constexpr int kStyleEventGlide = 23;
constexpr int kStyleEventDive = 28;
constexpr int kStyleCharacter = 52;  // the event's this is the object + 16, character at object + 68

bool InGlideState(int character) {
  return *reinterpret_cast<uint16_t*>(character + kStateOffset) ==
         static_cast<uint16_t>(*reinterpret_cast<int*>(kGlideStateId));
}

using HighFallFn = bool(__stdcall*)(int character, int component);
using StyleEventFn = bool(__fastcall*)(void* self, void* edx, int event, void* argument);

int16_t& Field(int character, int offset) { return *reinterpret_cast<int16_t*>(character + offset); }

bool __stdcall OnHighFall(int character, int component) {
  const int offsets[] = {kFallSlot, kSwapFrom, kSwapTo, kRequestNow, kRequestNext};
  int16_t saved[5];
  for (int i = 0; i < 5; ++i) saved[i] = Field(character, offsets[i]);
  bool result = reinterpret_cast<HighFallFn>(kHighFall)(character, component);
  if (!Listed(character)) return result;
  for (int i = 0; i < 5; ++i) Field(character, offsets[i]) = saved[i];
  return result;
}

bool __fastcall OnStyleEvent(void* self, void* edx, int event, void* argument) {
  int character = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(self) + kStyleCharacter);
  int kind = event & 0xFF;
  if (character && Listed(character)) {
    if (kind == kStyleEventGlide) return true;
    if (kind == kStyleEventDive && !InGlideState(character)) return true;
  }
  return reinterpret_cast<StyleEventFn>(kStyleEvent)(self, edx, event, argument);
}

// The glide move handler writes the horizontal velocity as sin/cos of the heading times the glide
// speed. It is reached by one direct call and by a handler table filled from a push of its address.
constexpr uintptr_t kGlideMove = 0x8A4AE0;
constexpr uintptr_t kGlideMoveCallSite = 0xACD7BB;
constexpr uintptr_t kGlideMovePushImm = 0xAC0F88;  // imm32 of `push offset sub_8A4AE0`
constexpr int kVelocityX = 3760;
constexpr int kVelocityZ = 3768;

// The same handler also moves a cape character's plain fall, so the glide proper is the glide state
// with jump held or just pressed, the two tests the state's own update uses.
constexpr uintptr_t kJumpHeld = 0x886D00;
constexpr uintptr_t kJumpPressed = 0x886D50;

using MoveFn = int(__cdecl*)(int character, int a2, int a3, int a4, float* a5);
using TestFn = bool(__cdecl*)(int character);

float speedScale = 1.0f;

bool GlidingProper(int character) {
  if (*reinterpret_cast<uint16_t*>(character + kStateOffset) != static_cast<uint16_t>(*reinterpret_cast<int*>(kGlideStateId)))
    return false;
  return JumpDown(character);
}

// The dive: while the dive key is held in the glide, the descent steepens and speed banks up; on
// release the banked speed carries the glide faster and further and lifts it a little, bleeding
// off over time. All of it is written over the handler's own velocity, which it sets every frame.
constexpr uintptr_t kFrameTime = 0x8B4120;  // cdecl() -> seconds of the current frame
constexpr float kBaseDescent = 0.375f;      // the handler's own glide descent, units a second

using FrameTimeFn = double(__cdecl*)();

struct Dive {
  int character;
  float banked;  // extra speed, as a fraction of the glide speed
  float lift;    // upward speed left from the last dive, units a second
  float liftRate;  // how fast that lift fades, units a second per second
  float pull;    // 0 = the dive's velocity, 1 = the glide's; the shaped transition progress
  float phase;   // the running transition's raw progress, 0 to 1 over its time
  bool into;     // the running transition leads into the dive
  int clip;      // the transition clip being scrubbed along the same progress, -1 = none
  int clipFrames;
  bool diving;
  bool landed;     // the hard landing has been started for this glide
  bool hot;        // the dive was live on the last glide tick
  bool wasGliding;
  float rollHold;  // seconds jump has been held during an airborne roll
};
Dive dives[2];
float diveGain, diveMax, diveDecay, diveDescent, diveLift, diveForward, diveLiftTime, divePullOut, diveTuckTime;
float diveCurve = 3.0f, diveAttack = 0.5f;

// One curve shapes every transition: the velocity, the body pitch and the clip frame all read the
// same value, so the motion and the pose commit together. It is a bias/gain S whose knee sharpens
// with the exponent, pulled towards an ease-out of the same exponent by the attack so the change
// can land early and settle instead of rounding in and out.
float Shape(float p) {
  p = std::min(std::max(p, 0.0f), 1.0f);
  float a = powf(p, diveCurve), b = powf(1.0f - p, diveCurve);
  float s = a + b > 0.0f ? a / (a + b) : p;
  float e = 1.0f - b;
  return s + (e - s) * diveAttack;
}

// Shape is monotone, so its inverse is a bisection; a flip mid-transition picks up from the current
// blend value and nothing jumps.
float Unshape(float value) {
  float lo = 0.0f, hi = 1.0f;
  for (int i = 0; i < 24; ++i) {
    float mid = (lo + hi) * 0.5f;
    (Shape(mid) < value ? lo : hi) = mid;
  }
  return (lo + hi) * 0.5f;
}

void Reset(Dive& dive, int character) {
  dive = {character};
  dive.pull = 1.0f;
  dive.phase = 1.0f;
  dive.clip = -1;
}

Dive& DiveFor(int character) {
  for (Dive& dive : dives)
    if (dive.character == character) return dive;
  for (Dive& dive : dives)
    if (!dive.character) {
      Reset(dive, character);
      return dive;
    }
  Reset(dives[0], character);
  return dives[0];
}

void ResetDive(int character) { Reset(DiveFor(character), character); }

int __cdecl OnGlideMove(int character, int a2, int a3, int a4, float* a5) {
  int result = reinterpret_cast<MoveFn>(kGlideMove)(character, a2, a3, a4, a5);
  bool proper = GlidingProper(character);
  Dive& dive = DiveFor(character);
  auto dt = static_cast<float>(reinterpret_cast<FrameTimeFn>(kFrameTime)());
  bool diving = proper && *reinterpret_cast<int*>(character + kController) &&
                abilitycontrols::Down(character, abilitycontrols::Dive);
  bool startedDive = diving && !dive.diving;
  bool released = !diving && dive.diving;
  if (*reinterpret_cast<int*>(character + kController)) {
    if (startedDive) capesound::StartDive();
    if (released) capesound::StartGlide();
  }
  if (startedDive || released) {
    dive.into = diving;
    dive.phase = Unshape(diving ? 1.0f - dive.pull : dive.pull);
  }
  float span = dive.into ? diveTuckTime : divePullOut;
  dive.phase = span > 0.0f ? std::min(dive.phase + dt / span, 1.0f) : 1.0f;
  float shaped = Shape(dive.phase);
  dive.pull = dive.into ? 1.0f - shaped : shaped;
  if (diving) {
    dive.banked = std::min(dive.banked + diveGain * dt, diveMax);
    dive.lift = 0.0f;
  } else {
    // Letting go turns the banked speed into a climb that fades over its own time.
    if (dive.diving) {
      dive.lift = diveLift * dive.banked;
      dive.liftRate = diveLiftTime > 0.0f ? dive.lift / diveLiftTime : dive.lift;
    }
    dive.banked = std::max(dive.banked - diveDecay * dt, 0.0f);
    dive.lift = std::max(dive.lift - dive.liftRate * dt, 0.0f);
  }
  dive.diving = diving;
  if (!proper) {
    dive.clip = -1;
    return result;
  }
  if (diveAnim >= 0 && HasDiveClip(character) && Prepared(character)) {
    auto request = [&](int id) { reinterpret_cast<RequestFn>(kRequestAnim)(character, nullptr, id, 0); };
    int glideId = *reinterpret_cast<int16_t*>(kGlideAnimId);
    // The game asks for the shared glide clip on its own; a character with a glide of its own is
    // steered to that whenever the shared one comes up.
    int glideLoop = HasGlideOverride(character) ? glideOverrideAnim : glideId;
    int loop = diving ? diveAnim : glideLoop;
    int transition = diving ? tuckAnim : pulloutAnim;
    int holder = *reinterpret_cast<int*>(character + kCharacterAnimSet);
    int set = holder ? *reinterpret_cast<int*>(holder + 4) : 0;
    if (transition >= 0 && (!set || !reinterpret_cast<FindEntryFn>(kFindEntry)(set, nullptr, transition, 0)))
      transition = -1;
    auto requested = *reinterpret_cast<int16_t*>(character + kRequestedAnim);
    // The transition clip is not played but scrubbed: ApplyPitch sets its frame from the same
    // shaped progress every tick, and the loop takes over when the progress runs out.
    if (startedDive || released) {
      dive.clip = -1;
      if (transition >= 0 && dive.phase < 1.0f) {
        request(transition);
        dive.clip = transition;
        dive.clipFrames = diving ? tuckFrames : pulloutFrames;
      } else {
        request(loop);
      }
    } else if (dive.clip >= 0) {
      if (dive.phase >= 1.0f) {
        request(loop);
        dive.clip = -1;
      }
    } else if (diving ? requested != diveAnim : requested == diveAnim || (glideLoop != glideId && requested == glideId)) {
      request(loop);
    }
  }
  // The dive is a drop that steepens as speed banks; the glide after it carries all of that speed.
  // Both velocities are always computed and the pull value sweeps between them, so a release
  // curves from straight down through forward into the climb instead of switching in a frame.
  if (dive.pull >= 1.0f && dive.lift <= 0.0f && dive.banked <= 0.0f) {
    *reinterpret_cast<float*>(character + kVelocityX) *= speedScale;
    *reinterpret_cast<float*>(character + kVelocityZ) *= speedScale;
    return result;
  }
  float ramp = diveMax > 0.0f ? std::min(dive.banked / diveMax, 1.0f) : 1.0f;
  float diveVertical = -(kBaseDescent + (diveDescent - kBaseDescent) * ramp);
  float glideVertical = -kBaseDescent + dive.lift;
  float diveScale = speedScale * diveForward;
  float glideScale = speedScale * (1.0f + 2.0f * dive.banked);
  float blend = dive.pull;
  float scale = diveScale + (glideScale - diveScale) * blend;
  *reinterpret_cast<float*>(character + kVelocityX) *= scale;
  *reinterpret_cast<float*>(character + kVelocityZ) *= scale;
  *reinterpret_cast<float*>(character + kMoveY) = diveVertical + (glideVertical - diveVertical) * blend;
  return result;
}

}  // namespace

bool JumpDown(int character) {
  return reinterpret_cast<TestFn>(kJumpHeld)(character) || reinterpret_cast<TestFn>(kJumpPressed)(character);
}

bool IsListed(int character) { return Listed(character); }

void DiveSetup(int gainPercent, int maxPercent, int decayPercent, int descentTenths, int liftTenths,
               int forwardPercent, int liftTimeTenths, int easeMilliseconds, int pullOutTenths, int tuckTenths,
               int curveTenths, int attackPercent) {
  divePullOut = pullOutTenths / 10.0f;
  diveTuckTime = tuckTenths / 10.0f;
  diveCurve = std::max(curveTenths, 10) / 10.0f;
  diveAttack = std::min(std::max(attackPercent, 0), 100) / 100.0f;
  diveGain = gainPercent / 100.0f;
  diveMax = maxPercent / 100.0f;
  diveDecay = decayPercent / 100.0f;
  diveDescent = descentTenths / 10.0f;
  diveLift = liftTenths / 10.0f;
  diveForward = forwardPercent / 100.0f;
  diveLiftTime = liftTimeTenths / 10.0f;
  int steps = easeMilliseconds / 50;
  diveEaseSteps = static_cast<int8_t>(steps < 0 ? 0 : steps > 127 ? 127 : steps);
}

void Unlimited() { unlimited = true; }

// The engine only ever yaws a character, so the dive's pitch has to be written over the rotation
// rows of its transform after the game has built them: nose down as the dive takes hold, back to
// level through the pull-out, a touch of nose up in the climb. The character record's +44 is not
// that transform (its rows read zero); the visual object at +3500 is the next candidate.
constexpr int kHeading = 436;  // float radians
constexpr int kLiftPitchDivisor = 4;  // climb pitch is DiveClimbPitch at a lift of 4 units a second
float divePitch, diveClimbPitch;

// The character's transform lives on its physics body at +3500, twice: a 4x4 at +64 and another
// at +128, rows right, up, forward, translation (surveyed 2026-09-21: right = (cos, 0, -sin),
// forward = (sin, 0, cos) of the heading).
constexpr int kBody = 3500;
constexpr int kBodyMatrices[] = {64, 128};

void Tilt(int character, float pitch) {
  int body = *reinterpret_cast<int*>(character + kBody);
  if (!body) return;
  float yaw = *reinterpret_cast<float*>(character + kHeading);
  float sy = sinf(yaw), cy = cosf(yaw), sp = sinf(pitch), cp = cosf(pitch);
  for (int offset : kBodyMatrices) {
    auto m = reinterpret_cast<float*>(body + offset);
    m[0] = cy;
    m[1] = 0.0f;
    m[2] = -sy;
    m[4] = sy * sp;
    m[5] = cp;
    m[6] = cy * sp;
    m[8] = sy * cp;
    m[9] = -sp;
    m[10] = cy * cp;
  }
}

bool glideWasActive;  // the played character's glide state last frame, for the cape sounds

// The double-tap roll is the air state with air mode 8, and the glide logic only enters the glide
// from air mode 0, so a roll off a roof rides the roll clip to the ground. Holding jump through
// the roll for long enough clears the mode, and the glide entry takes it from there.
constexpr uintptr_t kAirStateId = 0x11A2E34;
constexpr int kGroundFlag = 140;   // byte, nonzero on the ground
constexpr int kAirModeRoll = 8;
float rollGlideHold = 1.0f;

// The glide's own landing is the glide_land clip, played with the glide state in mode 2. A dive
// hitting the ground goes through the fall state's landing routine instead, which picks the
// character's fall-land clip and runs the landing state (sub_AC6190, as the fall state uses it).
constexpr uintptr_t kLand = 0xAC6190;  // cdecl(character, anim) -> started; anim -1 = the fall-land clip
constexpr int kGlideMode = 1268;       // byte: 2 = the landing clip is playing
constexpr int kGlideModeLanding = 2;
bool diveLanding = true;

using LandFn = int(__cdecl*)(int character, int anim);

void RollCancel(int character, Dive& dive) {
  bool rolling = *reinterpret_cast<uint16_t*>(character + kStateOffset) ==
                     static_cast<uint16_t>(*reinterpret_cast<int*>(kAirStateId)) &&
                 *reinterpret_cast<uint8_t*>(character + kAirMode) == kAirModeRoll &&
                 !*reinterpret_cast<uint8_t*>(character + kGroundFlag);
  if (!rolling || rollGlideHold <= 0.0f || !JumpDown(character)) {
    dive.rollHold = 0.0f;
    return;
  }
  dive.rollHold += static_cast<float>(reinterpret_cast<FrameTimeFn>(kFrameTime)());
  if (dive.rollHold < rollGlideHold) return;
  dive.rollHold = 0.0f;
  *reinterpret_cast<uint8_t*>(character + kAirMode) = 0;
}

// The anim player at +424 keeps its playing clips in a list from +3672 (next at +4); each holds
// its entry record at +28, its frame at +144 and its rate at +152, and the player rebuilds the
// pose when +3864 is set (all from sub_677520, sub_677860 and sub_8F1020). A transition clip's
// rate is zeroed and its frame written outright, so the pose sits exactly where the motion is.
constexpr int kAnimPlayer = 424;
constexpr int kPlayerClips = 3672;
constexpr int kPlayerDirty = 3864;
constexpr int kClipNext = 4;
constexpr int kClipEntry = 28;
constexpr int kClipFrame = 144;
constexpr int kClipRate = 152;
constexpr int kEntryIndex = 268;
// The character's request block at +4052 mirrors the tail clip's frame (+4, +8, and +16 while a
// blend runs); the driver sub_9ABC30 writes the clip's frame back from it every tick before the
// update and copies it out again after, so the block has to carry the scrubbed frame too.
constexpr int kPlayerTail = 3676;
constexpr int kRequestBlock = 4052;
constexpr int kBlockFrames[] = {4, 8, 16};

void Scrub(int character, int anim, float frame) {
  int player = *reinterpret_cast<int*>(character + kAnimPlayer);
  if (!player) return;
  for (int clip = *reinterpret_cast<int*>(player + kPlayerClips); clip; clip = *reinterpret_cast<int*>(clip + kClipNext)) {
    int entry = *reinterpret_cast<int*>(clip + kClipEntry);
    if (!entry || *reinterpret_cast<int16_t*>(entry + kEntryIndex) != anim) continue;
    *reinterpret_cast<float*>(clip + kClipRate) = 0.0f;
    *reinterpret_cast<float*>(clip + kClipFrame) = frame;
    *reinterpret_cast<uint8_t*>(player + kPlayerDirty) = 1;
    if (*reinterpret_cast<int*>(player + kPlayerTail) == clip)
      for (int offset : kBlockFrames) *reinterpret_cast<float*>(character + kRequestBlock + offset) = frame;
  }
}

void ApplyPitch(int character) {
  bool inGlide = *reinterpret_cast<uint16_t*>(character + kStateOffset) ==
                 static_cast<uint16_t>(*reinterpret_cast<int*>(kGlideStateId));
  bool player = *reinterpret_cast<int*>(character + kController) != 0;
  if (player) {
    if (glideWasActive && !inGlide) capesound::Silence();
    glideWasActive = inGlide;
    capesound::Tick();
  }
  if (!Listed(character)) return;
  if (player) RollCancel(character, DiveFor(character));
  Dive* dive = nullptr;
  for (Dive& d : dives)
    if (d.character == character) dive = &d;
  if (!dive) return;
  dive->wasGliding = inGlide;
  if (!inGlide) return;
  dive->hot = dive->diving || dive->pull < 1.0f;
  float blend = dive->pull;
  if (dive->clip >= 0 && dive->clipFrames > 1) {
    float progress = dive->into ? 1.0f - blend : blend;
    Scrub(character, dive->clip, progress * (dive->clipFrames - 1));
  }
  float pitch = divePitch * (1.0f - blend) - diveClimbPitch * std::min(dive->lift / kLiftPitchDivisor, 1.0f) * blend;
  if (pitch != 0.0f) Tilt(character, pitch);
}

// A dive that meets the ground drops the glide state on the spot, with the dive clip still playing
// and no landing of any kind (traced 2026-09-21), so the tick after a live dive's last glide tick is
// where the fall landing is started. It runs before the anim sync, the phase the game's own
// landing runs in, since a state change from inside the sync is not something the game ever does.
void BeforeSync(int character) {
  if (!diveLanding || !Listed(character)) return;
  Dive* dive = nullptr;
  for (Dive& d : dives)
    if (d.character == character) dive = &d;
  if (!dive || !dive->wasGliding || !dive->hot || dive->landed) return;
  bool inGlide = *reinterpret_cast<uint16_t*>(character + kStateOffset) ==
                 static_cast<uint16_t>(*reinterpret_cast<int*>(kGlideStateId));
  bool grounded = *reinterpret_cast<uint8_t*>(character + kGroundFlag) != 0;
  if (inGlide || !grounded) return;
  dive->landed = true;
  reinterpret_cast<LandFn>(kLand)(character, -1);
}

void DivePitch(int degrees, int climbDegrees) {
  divePitch = degrees * 3.14159265f / 180.0f;
  diveClimbPitch = climbDegrees * 3.14159265f / 180.0f;
}

void DiveClip(const std::string& characters) {
  diveCharacters = Split(characters);
}

void GlideOverride(const std::string& characters) {
  glideCharacters = Split(characters);
}

void TransitionClips(const std::string& pullout, int pulloutFrameCount, const std::string& tuck, int tuckFrameCount) {
  pulloutDonor = pullout;
  pulloutFrames = pulloutFrameCount;
  tuckDonor = tuck;
  tuckFrames = tuckFrameCount;
}

void RollGlide(int holdTenths) { rollGlideHold = holdTenths / 10.0f; }

void Landing(bool hard) { diveLanding = hard; }

void Ease(int milliseconds) {
  int steps = milliseconds / 50;
  easeSteps = static_cast<int8_t>(steps < 0 ? 0 : steps > 127 ? 127 : steps);
}

// The vertical velocity integrator reads the character's own gravity each call. Scaling that
// value around the call, only while a listed character drops outside the glide state, slows the
// fall without touching jumps on the way up or the glide's own descent.
constexpr uintptr_t kFallPhysics = 0x9E1280;
constexpr uintptr_t kFallPhysicsCallSites[] = {0xA799A1, 0xB1B8A3, 0xB1D2C8, 0xB1DAE3, 0xB20E66};

using FallPhysicsFn = void(__cdecl*)(int character, float* a2, float a3, float a4, float* a5);

float fallScale = 1.0f;

void __cdecl OnFallPhysics(int character, float* a2, float a3, float a4, float* a5) {
  auto& gravity = *reinterpret_cast<float*>(character + kGravity);
  float saved = gravity;
  bool scaled = *reinterpret_cast<float*>(character + kVelocityY) < 0.0f && !InGlideState(character) && Listed(character);
  if (scaled) gravity = saved * fallScale;
  reinterpret_cast<FallPhysicsFn>(kFallPhysics)(character, a2, a3, a4, a5);
  if (scaled) gravity = saved;
}

void FallSpeed(int percent) {
  if (percent <= 0 || percent == 100) return;
  fallScale = percent / 100.0f;
  for (uintptr_t site : kFallPhysicsCallSites)
    hook::Call(site, reinterpret_cast<void*>(kFallPhysics), reinterpret_cast<void*>(OnFallPhysics));
}

void Speed(int percent) {
  if (percent > 0) speedScale = percent / 100.0f;
  hook::Call(kGlideMoveCallSite, reinterpret_cast<void*>(kGlideMove), reinterpret_cast<void*>(OnGlideMove));
  PatchPointer(kGlideMovePushImm, kGlideMove, reinterpret_cast<uintptr_t>(&OnGlideMove));
}

void Install(const std::string& characters) {
  allowed = Split(characters);
  hook::Patch(kBonusLevelTest, {0x83, 0xE7, 0x01}, {0x83, 0xCF, 0x01});  // or edi, 1
  hook::Patch(kLevelplayMask, {0x00, 0x00, 0x00, 0x04}, {0x00, 0x00, 0x00, 0x00});
  hook::Call(kEnterCallSite, reinterpret_cast<void*>(kEnterGlide), reinterpret_cast<void*>(OnEnterGlide));
  PatchPointer(kHighFallSlot, kHighFall, reinterpret_cast<uintptr_t>(&OnHighFall));
  PatchPointer(kStyleEventSlot, kStyleEvent, reinterpret_cast<uintptr_t>(&OnStyleEvent));
  glideanim::Note("glide: installed");
}

}  // namespace glide
