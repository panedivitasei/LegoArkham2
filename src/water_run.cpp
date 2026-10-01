#include "water_run.h"

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <chrono>
#include "glide_anim.h"
#include "grapple.h"
#include "hook.h"
#include "player_character.h"
#include "surface_run.h"
#include "flash_roll.h"

namespace waterrun {
namespace {
constexpr float kNoHeight=2000000.0f;
constexpr float kMinimumSpeed=0.5f;
constexpr float kContactGap=0.015f;
using GroundFn=void(__cdecl*)(int,int,float);
using PositionFn=int(__fastcall*)(int,void*,const float*,int);
GroundFn query=reinterpret_cast<GroundFn>(0x95CDC0);
PositionFn position=reinterpret_cast<PositionFn>(0x87DF50);
bool enabled=false;
// FlyingWaterSkim as spawned by the flying ability's surface skim (sub_4C5C20): every 0.1 s at the water under
// the character, rotated to its heading. The effect id resolves through the particle registry.
constexpr uintptr_t kSkimEffectId=0x1023038;
constexpr uintptr_t kEffectRegistry=0x11BDF00;
constexpr uintptr_t kIdentity=0x102B510;
constexpr float kSkimInterval=0.1f;
using ResolveFn=int(__thiscall*)(int,const int16_t*);
using ManagerFn=int(__cdecl*)(int);
using SpawnFn=void*(__thiscall*)(int,void*,int,float*,int,int);
using ReleaseFn=void(__thiscall*)(void*);
using RotateFn=float*(__cdecl*)(float*,int16_t);
ResolveFn resolve=reinterpret_cast<ResolveFn>(0x8CCF50);
ManagerFn manager=reinterpret_cast<ManagerFn>(0x430380);
SpawnFn spawn=reinterpret_cast<SpawnFn>(0x7A7760);
ReleaseFn release=reinterpret_cast<ReleaseFn>(0x4A5680);
RotateFn rotate=reinterpret_cast<RotateFn>(0x51D450);
bool skim=false;
std::chrono::steady_clock::time_point lastSkim[2];
// Footsteps (sub_997A30, from the anim trigger sub_A2C190) play footstep_water while +752 carries the
// wading flag, which the game only sets with water above the body; the flag is lent to Flash's steps on water.
constexpr uintptr_t kFootstepCallSite=0xA2C1C1;
constexpr uintptr_t kFootstep=0x997A30;
constexpr uint32_t kWading=0x4000000;
constexpr float kSupportGrace=0.1f;
using FootstepFn=int(__cdecl*)(int,float);
int supported[2];
std::chrono::steady_clock::time_point lastSupport[2];
template<class T> T& At(int c,int offset) { return *reinterpret_cast<T*>(c+offset); }

bool CanSupport(int c) {
  const char* name=playercharacter::ResourceName(c);
  if (!name || _stricmp(name,"Flash") || playercharacter::HumanSlot(c)<0) return false;
  if (surfacerun::Active(c) || grapple::Occupied(c) || At<uint8_t>(c,5992)
      || (At<uint8_t>(c,136)&0x22) || (At<uint8_t>(c,740)&1)) return false;
  if (At<uint16_t>(c,1270)==*reinterpret_cast<const uint16_t*>(0x11A2E24)) return false;
  if (At<uint8_t>(c,4608)!=1) return false;
  int input=At<int>(c,1992);
  float amount=At<float>(input,48);
  float vx=At<float>(c,4112), vy=At<float>(c,4116), vz=At<float>(c,4120);
  if (!std::isfinite(amount) || (amount<=0.1f && !flashroll::Active(c)) || !std::isfinite(vx)
      || !std::isfinite(vy) || !std::isfinite(vz)
      || vx*vx+vz*vz<kMinimumSpeed*kMinimumSpeed || vy>0.1f) return false;
  float water=At<float>(c,4560), ground=At<float>(c,4556);
  float bottom=At<float>(At<int>(c,4104),56)*At<float>(c,4192);
  float feet=At<float>(c,96)+bottom;
  if (!std::isfinite(water) || water==kNoHeight || !std::isfinite(ground)
      || !std::isfinite(feet) || !std::isfinite(bottom)) return false;
  if (ground!=kNoHeight && ground>=water-kContactGap) return false;
  // Catch near-surface contact without lifting submerged or airborne characters onto the water.
  return feet>=water-0.25f && feet<=water+0.12f && bottom<=kContactGap;
}

void Support(int c) {
  float water=At<float>(c,4560);
  float bottom=At<float>(At<int>(c,4104),56)*At<float>(c,4192);
  float next[3]={At<float>(c,92),water-bottom+kContactGap,At<float>(c,100)};
  position(c,nullptr,next,1);
  At<float>(c,4556)=water;
  At<float>(c,3776)=0; At<float>(c,3780)=1; At<float>(c,3784)=0;
  At<uint8_t>(c,4611)=0;
  At<float>(c,4116)=0; At<float>(c,3764)=0;
}

// Owner object as the flying ability resolves it: the game object behind the character's +16 component.
int Owner(int c) {
  int link=At<int>(c,316);
  return link?At<int>(link,12):0;
}

void Skim(int c,int slot) {
  auto now=std::chrono::steady_clock::now();
  if (std::chrono::duration<float>(now-lastSkim[slot]).count()<kSkimInterval) return;
  lastSkim[slot]=now;
  int registry=*reinterpret_cast<int*>(kEffectRegistry);
  if (!registry) return;
  int effect=resolve(registry,reinterpret_cast<const int16_t*>(kSkimEffectId));
  int owner=Owner(c);
  int particles=owner?manager(owner):0;
  if (effect==-1 || !particles) return;
  float matrix[16];
  memcpy(matrix,reinterpret_cast<const void*>(kIdentity),sizeof(matrix));
  matrix[12]=At<float>(c,92); matrix[13]=At<float>(c,4560); matrix[14]=At<float>(c,100);
  float heading=std::atan2(At<float>(c,4112),At<float>(c,4120));
  rotate(matrix,static_cast<int16_t>(static_cast<int>(heading*(32768.0f/3.14159265f))));
  uint32_t result[5]={};
  spawn(particles,result,effect,matrix,-1,0);
  release(result);
}

int __cdecl OnFootstep(int c,float volume) {
  int slot=c?playercharacter::HumanSlot(c):-1;
  bool wet=slot>=0 && supported[slot]==c
      && std::chrono::duration<float>(std::chrono::steady_clock::now()-lastSupport[slot]).count()<kSupportGrace;
  uint32_t flags=wet?At<uint32_t>(c,752):0;
  if (wet) At<uint32_t>(c,752)=flags|kWading;
  int result=reinterpret_cast<FootstepFn>(kFootstep)(c,volume);
  if (wet) At<uint32_t>(c,752)=flags;
  return result;
}

void __cdecl OnGround(int c,int hasGround,float height) {
  query(c,hasGround,height);
  if (!enabled || !c) return;
  if (playercharacter::HumanSlot(c)<0) return;
  if (!CanSupport(c)) return;
  Support(c);
  int slot=playercharacter::HumanSlot(c);
  supported[slot]=c;
  lastSupport[slot]=std::chrono::steady_clock::now();
  if (skim) Skim(c,slot);
}
}

bool Supported(int c) {
  int slot=c?playercharacter::HumanSlot(c):-1;
  return slot>=0 && supported[slot]==c
      && std::chrono::duration<float>(std::chrono::steady_clock::now()-lastSupport[slot]).count()<kSupportGrace;
}

void Install(bool splash) {
  skim=splash;
  // B1F6C0 refreshes this query after collision movement and liquid-state checks.
  enabled=hook::Call(0xB20BD8,reinterpret_cast<void*>(query),reinterpret_cast<void*>(&OnGround));
  if (enabled) hook::Call(kFootstepCallSite,reinterpret_cast<void*>(kFootstep),reinterpret_cast<void*>(&OnFootstep));
  if (!enabled) glideanim::Note("water run: hook mismatch; disabled");
}
}
