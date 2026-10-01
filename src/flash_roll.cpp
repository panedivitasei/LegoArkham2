#include "flash_roll.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "glide_anim.h"
#include "hook.h"
#include "input_frame.h"
#include "player_character.h"
#include "surface_run.h"
#include "water_run.h"

namespace flashroll {
namespace {
constexpr uintptr_t kParty = 0x138F4E0;
constexpr uintptr_t kJumpState = 0x11A2E34, kRollState = 0x11A2EB0;
constexpr float kPi = 3.14159265359f, kAngleScale = 32768.0f / kPi;
constexpr float kLoopFirst = 124.0f, kLoopFrames = 44.0f;
constexpr int kAirAnim = 261, kLandAnim = 263;
using HeldFn = bool(__cdecl*)(int);
using TimeFn = double(__cdecl*)();
using StateFn = int(__fastcall*)(int, void*, int);
using RequestFn = bool(__fastcall*)(int, void*, int, int);
using SyncFn = void(__cdecl*)(int, int, int, float, float, int, char, char);
using DoneFn = int(__cdecl*)(int, float*);
using SeekFn = void(__fastcall*)(int, void*, float);
using MovementFn = int(__cdecl*)(int);
using EaseFn = double(__cdecl*)(float,float,float);
using ClipFlagsFn = bool(__thiscall*)(int,uint8_t);
const auto jumpHeld = reinterpret_cast<HeldFn>(0x886D50);
const auto setState = reinterpret_cast<StateFn>(0x403720);
const auto request = reinterpret_cast<RequestFn>(0x4D7B30);
const auto sync = reinterpret_cast<SyncFn>(0x9ABC30);
const auto done = reinterpret_cast<DoneFn>(0x8EFA30);
const auto seek = reinterpret_cast<SeekFn>(0x877310);
bool enabled;
bool rollOffLedges=true;

template<class T> T& At(int p, int offset) { return *reinterpret_cast<T*>(p + offset); }
struct Held {
  int character = 0, resource = 0, player = 0;
  bool session = false, looping = false;
  float phase = 0, speed = 0;
};
Held held[2];

bool InState(int c, uintptr_t id) { return At<int16_t>(c,1270) == *reinterpret_cast<int*>(id); }
bool IsFlash(int c) {
  const char* name = c ? playercharacter::ResourceName(c) : nullptr;
  return name && !_stricmp(name,"Flash") && playercharacter::HumanSlot(c) >= 0;
}
float Seconds() { return std::clamp(static_cast<float>(reinterpret_cast<TimeFn>(0x8B4120)()),0.0f,0.1f); }
float HorizontalSpeed(int c) { return std::hypot(At<float>(c,4112),At<float>(c,4120)); }
Held* Find(int c) {
  if (!enabled || !glideanim::FlashRollLoaded() || !IsFlash(c)) return nullptr;
  int slot = playercharacter::HumanSlot(c);
  auto& h = held[slot];
  if (h.character != c || h.resource != At<int>(c,4104) || h.player != At<int>(c,424)) {
    h = {};
    h.character = c; h.resource = At<int>(c,4104); h.player = At<int>(c,424);
  }
  return &h;
}

void Release(int c, Held& h) {
  if (h.looping) {
    if (InState(c,kRollState)) {
      At<uint8_t>(c,1265) = 0;
      At<uint8_t>(c,1267) = 0;
      At<float>(c,1188) = 0;
      setState(c,nullptr,-1);
      request(c,nullptr,At<int16_t>(c,540),0);
    } else if (InState(c,kJumpState)) {
      request(c,nullptr,At<int16_t>(c,610),0);
    }
  }
  h.session = false; h.looping = false;
}

void Observe(int c, Held& h) {
  bool air = InState(c,kJumpState);
  bool land = InState(c,kRollState);
  bool ordinary = At<int16_t>(c,1270) == -1;
  bool surface = surfacerun::Active(c);
  if (!jumpHeld(c) || (!air && !land && !ordinary && !surfacerun::Active(c))) {
    Release(c,h);
    return;
  }
  if (!h.session && (land || surface || (air && At<uint8_t>(c,1289) == 8))) {
    h.session = true;
    h.speed = surface ? std::hypot(HorizontalSpeed(c),At<float>(c,4116)) : HorizontalSpeed(c);
    h.phase = 0;
  }
}

void Accelerate(int c,Held& h) {
  int input=At<int>(c,1992);
  int definition=At<int>(At<int>(c,4104),44);
  float target=input ? At<float>(input,48) : 0;
  if (!h.session || !definition || !std::isfinite(target) || target<=.1f) return;
  // B02FD0 writes the native walk/run/sprint target; AFB5D0 uses definition+88 to approach it.
  h.speed=static_cast<float>(reinterpret_cast<EaseFn>(0x8E5B90)(h.speed,target,At<float>(definition,88)));
}

void Carry(int c, Held& h, float dt) {
  if (!h.session || surfacerun::Active(c)) return;
  float yaw = At<float>(c,436);
  int input = At<int>(c,1992);
  if (input && At<float>(input,48) > 0.1f) {
    // AFB5D0 stores 97B610's camera-resolved running direction here.
    float target = At<int16_t>(c,2568) / kAngleScale;
    yaw += std::clamp(std::remainder(target-yaw,2*kPi),-7.0f*dt,7.0f*dt);
    yaw = std::remainder(yaw,2*kPi);
    At<float>(c,436) = yaw;
    auto angle = static_cast<int16_t>(static_cast<int>(yaw*kAngleScale));
    At<int16_t>(c,164) = angle; At<int16_t>(c,4108) = angle;
  }
  float x = std::sin(yaw)*h.speed, z = std::cos(yaw)*h.speed;
  At<float>(c,3760) = x; At<float>(c,3768) = z;
  At<float>(c,4112) = x; At<float>(c,4120) = z;
}

void HoldState(int c, Held& h) {
  if (!h.session) return;
  bool ground = At<uint8_t>(c,140) || waterrun::Supported(c);
  if (ground && At<int16_t>(c,1270) == -1) setState(c,nullptr,*reinterpret_cast<int*>(kRollState));
  if (InState(c,kRollState)) {
    // B185F0 tests both the completion marker and this independent countdown.
    At<uint8_t>(c,1265) = 0; At<uint8_t>(c,1267) = 0;
    At<float>(c,1188) = 1.0f;
  }
}

void Update() {
  for (int slot=0; slot<2; ++slot) {
    int c = reinterpret_cast<const int*>(kParty)[slot];
    Held* h = Find(c);
    if (!h) continue;
    Observe(c,*h);
    HoldState(c,*h);
  }
}

float PhaseFor(float frame) {
  // Source times of the authored cycle, derived from Flash's native hips rotation.
  constexpr float times[] = {24,24.781003f,25.555835f,26.325403f,27.090571f,27.851801f,
      28.611870f,29.371659f,30.131765f,30.893393f,31.659031f,32.428934f,33.204012f,
      33.985542f,35.037049f,35.499590f,35.962130f,36.401789f,36.839406f,37.316671f,
      37.816918f,38.317179f,38.817447f,39.311898f,39.803006f};
  float t = frame-1;
  for (int i=0; i<24; ++i)
    if (t < times[i+1]) return (i+std::clamp((t-times[i])/(times[i+1]-times[i]),0.0f,1.0f))/24.0f;
  return 0;
}

void Scrub(int c, int player, float frame) {
  int tail = At<int>(player,3676);
  if (!tail || At<int>(tail,20) != 1) return;
  int entry = At<int>(tail,28);
  if (!entry || At<int16_t>(entry,268) != kLandAnim) return;
  seek(tail,nullptr,frame);
  // 9ABC30 uses +4 for a single clip, +16 for the incoming clip in a crossfade.
  At<float>(c,4056) = frame;
  if (At<uint8_t>(c,4077)&0x10) At<float>(c,4068) = frame;
  At<float>(c,4060) = frame;
}

void __cdecl OnSync(int player,int holder,int block,float frames,float speed,int seconds,char reverse,char extra) {
  int c = block-4052;
  Held* h = Find(c);
  if (!h || !h->session) { sync(player,holder,block,frames,speed,seconds,reverse,extra); return; }
  if (!h->looping) {
    int tail = player ? At<int>(player,3676) : 0;
    int entry = tail && At<int>(tail,20)==1 ? At<int>(tail,28) : 0;
    int anim = entry ? At<int16_t>(entry,268) : -1;
    float frame = tail ? At<float>(tail,144) : 0;
    if ((anim==kAirAnim && frame>=25) || anim==kLandAnim || surfacerun::Active(c)) {
      h->phase = PhaseFor(anim==kAirAnim || anim==kLandAnim ? frame : 25);
      h->looping = true;
    }
  }
  if (!h->looping) { sync(player,holder,block,frames,speed,seconds,reverse,extra); return; }
  At<int16_t>(block,34) = kLandAnim;
  At<int16_t>(c,1258) = kLandAnim;
  // Zero advancement suppresses root-motion/events; the actual skeletal pose advances below.
  sync(player,holder,block,0,speed,seconds,reverse,extra);
  Scrub(c,player,kLoopFirst+h->phase*kLoopFrames);
  int definition=At<int>(At<int>(c,4104),44);
  float run=definition ? At<float>(definition,36) : 0;
  float rate=run>0 ? 2.5f*h->speed/run : 2.5f;
  h->phase = std::fmod(h->phase+Seconds()*rate,1.0f);
}

int __cdecl OnRollDone(int c,float* length) {
  Held* h = Find(c);
  if (h && h->session && jumpHeld(c)) { if (length) *length=0; return 0; }
  return done(c,length);
}

int __cdecl OnMovement(int c) {
  Held* before = Find(c);
  if (before) { Observe(c,*before); Accelerate(c,*before); }
  int result = reinterpret_cast<MovementFn>(0xAFB5D0)(c);
  Held* h = Find(c);
  if (!h) return result;
  Observe(c,*h);
  if (!h->session || surfacerun::Active(c)) return result;
  // B23180 calculates velocity before B1F6C0 performs collision movement.
  Carry(c,*h,Seconds());
  At<float>(c,3772) = 1.0f;
  At<float>(c,4124) = 1.0f;
  return result;
}

bool __fastcall OnLedgeClipFlags(int player,void*,uint8_t flags) {
  for (int slot=0;slot<2;++slot) {
    int c=reinterpret_cast<const int*>(kParty)[slot];
    Held* h=Find(c);
    if (rollOffLedges && h && h->session && At<int>(c,424)==player && InState(c,kRollState) && jumpHeld(c))
      return true;
  }
  return reinterpret_cast<ClipFlagsFn>(0x64F630)(player,flags);
}

bool __fastcall OnLedgeProbe(int manager,void*,int c,char check) {
  Held* h=Find(c);
  if (rollOffLedges && h && h->session && jumpHeld(c)) return false;
  using ProbeFn=bool(__thiscall*)(int,int,char);
  return reinterpret_cast<ProbeFn>(At<int>(At<int>(manager,0),604))(manager,c,check);
}
}

bool Active(int c) {
  Held* h=Find(c);
  if (h && !h->session && surfacerun::Active(c) && jumpHeld(c)) Observe(c,*h);
  return h && h->session && jumpHeld(c);
}
float Speed(int c,float fallback) { Held* h=Find(c); return h && h->session ? h->speed : fallback; }
int WallAnim(int c,int runAnim) { return Active(c) ? kLandAnim : runAnim; }
void BeforeSync(int c) {
  Held* h=Find(c);
  if (!h) return;
  Observe(c,*h); HoldState(c,*h);
}
void AfterSync(int) {
}
void Install(const std::string& clipPath,bool allowLedges) {
  rollOffLedges=allowLedges;
  glideanim::FlashRollClip(clipPath);
  bool pose=hook::Call(0xAAFD46,reinterpret_cast<void*>(sync),reinterpret_cast<void*>(&OnSync));
  bool marker=hook::Call(0xB18A79,reinterpret_cast<void*>(done),reinterpret_cast<void*>(&OnRollDone));
  bool movement=hook::Call(0xB04601,reinterpret_cast<void*>(0xAFB5D0),reinterpret_cast<void*>(&OnMovement));
  // B1F6C0 has a late rollback and 9D3620 has an earlier floor-ahead velocity guard.
  bool ledge=hook::Call(0xB20B39,reinterpret_cast<void*>(0x64F630),reinterpret_cast<void*>(&OnLedgeClipFlags));
  uintptr_t probe=reinterpret_cast<uintptr_t>(&OnLedgeProbe);
  bool early=hook::Patch(0x9D3804,{0x8B,0x92,0x5C,0x02,0,0},
    {0xBA,static_cast<BYTE>(probe),static_cast<BYTE>(probe>>8),static_cast<BYTE>(probe>>16),static_cast<BYTE>(probe>>24),0x90});
  enabled=pose && marker && movement && ledge && early;
  if (enabled) inputframe::OnFrame(Update);
  glideanim::Note(enabled ? "flash roll: held skeletal loop installed" : "flash roll: hook mismatch; disabled");
}
}
