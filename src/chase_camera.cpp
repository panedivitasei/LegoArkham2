#include "chase_camera.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "frame.h"
#include "hook.h"
#include "ability_controls.h"
#include "devices.h"
#include "reticle.h"
#include "surface_run.h"
#include "surface_frame.h"
#include "player_character.h"

namespace chasecam {
namespace {

// LEGOBatman2.exe addresses (Steam build, no ASLR).
// Two things keep the chase camera out of a story level. The per-frame chase update skips levels
// whose levels.txt entry says no_chase_camera, and the camera it does submit carries priority
// -1000, below the level's rail cameras at 490. Both are immediates in the code.
constexpr uintptr_t kLevelFlagMask = 0x9AA3BF;    // imm32 of `and ecx, 200000h` in ChaseCameras_Update
constexpr uintptr_t kDefaultPriority = 0x9614E8;  // imm32 of `mov ebx, -1000` in ChaseCamera::Update
constexpr uintptr_t kUpdateCallSite = 0xA4C33B;   // call ChaseCameras_Update(area) in the level tick
constexpr uintptr_t kChaseUpdate = 0x9AA340;
// The raised priority is 1000: above the rails, below cutscene shots at 2000.

// The on-foot chase camera's config from stuff\chase_camera_foot.txt, 29 floats; the first three
// are stick_length_min/mid/max, the distance from the character at the three zoom levels.
constexpr uintptr_t kFootConfig = 0x11A5020;
constexpr int kStickLengths = 3;

// The game's chase camera swings itself back behind the character and uses the stick's vertical
// axis for zoom, so its output is replaced: the call at the end of ChaseCamera::Update that hands
// the camera system a position and look-at point gets an orbit instead, built from the character's
// position, the right stick and the mouse. Everything downstream (blending, priorities, cutscene
// hand-over) remains original. 
constexpr uintptr_t kSubmitCallSite = 0x96157E;  // call CameraSystem_Submit(...) in ChaseCamera::Update
constexpr uintptr_t kSubmit = 0x8431A0;
constexpr uintptr_t kReadPan = 0x90F010;  // thiscall(chase, player, &x, &y): the right stick, -1..1
constexpr uintptr_t kFrameSeconds = 0x8B4120;
constexpr int kChasePlayerOffset = 528;
constexpr int kChaseTargetOffset = 112;   // the character followed
constexpr int kChasePositionOffset = 416;  // the game's own smoothed camera position and look-at, for the hand-over
constexpr int kChaseLookAtOffset = 444;
constexpr int kCharacterPositionOffset = 92;
constexpr float kTargetHeight = 0.5f;  // above the character's origin, about chest height on a minifigure
constexpr float kPi = 3.14159265f;
constexpr float kYawSpeed = 3.0f;    // radians per second at full stick
constexpr float kPitchSpeed = 2.0f;
constexpr float kMinPitch = -0.6f;
constexpr float kMaxPitch = 1.2f;
constexpr float kRadiansPerCount = 0.0025f;  // at sensitivity 1, a full turn is about 2500 mouse counts

// Collision uses the level's own line-of-sight query, the way the game's chase camera does: a
// ray query of a start point and an offset, a FirstPointCollector, and the level's collision
// world found through the chase camera's area link. The nearest hit lands at collector+144.
constexpr uintptr_t kRayInit = 0x402150;          // thiscall(query, start, offset)
constexpr uintptr_t kCollectorBase = 0xB8F200;    // thiscall(collector): base constructor
constexpr uintptr_t kCollectorReady = 0xB80D00;   // thiscall(collector)
constexpr uintptr_t kCollectorVtable = 0xCFB550;  // FirstPointCollector
constexpr uintptr_t kRayCast = 0xB99610;          // thiscall(world, query, collector) -> hit
constexpr uintptr_t kAreaObject = 0x42F210;       // cdecl(areaLink)
constexpr uintptr_t kAreaWorld = 0x6A34B0;        // thiscall(areaObject)
constexpr int kChaseAreaOffset = 116;             // chase -> area link, +216 -> the collision link
constexpr int kQueryMaskA = 32, kQueryMaskB = 36; // the chase camera's own values: 2 and -2
constexpr int kCollectorLimit = 16;               // float 1.0 before the cast
constexpr int kCollectorHit = 144;                // float[3]
constexpr int kCollectorNormal = 160;             // float[3]
constexpr float kWallMargin = 0.2f;   // how far the camera keeps from any surface, measured square to it
constexpr float kCameraRadius = 0.12f;  // the side rays run this far off the centre line
constexpr float kMinGraze = 0.25f;    // the back-off along the ray stops growing below this cosine
constexpr float kWaterMargin = 0.25f;  // how far above the water at the character the camera stays
constexpr float kNoWater = 2000000.0f;
constexpr int kCharacterWater = 4560;  // float, water height under the character
constexpr float kMinDistance = 0.3f;
constexpr float kCharacterRadius = 0.45f;  // the ray starts outside the character's own collision
constexpr float kPushOutRate = 4.0f;  // per second, easing back out once the way is clear

using UpdateFn = int(__cdecl*)(int area);
using RayInitFn = void(__fastcall*)(void* query, int, const float* start, const float* offset);
using CollectorFn = int(__fastcall*)(void* collector, int);
using RayCastFn = bool(__fastcall*)(int world, int, void* query, void* collector);
using AreaObjectFn = int(__cdecl*)(int link);
using AreaWorldFn = int(__fastcall*)(int area, int);
using SubmitFn = int(__cdecl*)(int character, int name, int chase, int priority, float weight, float* position,
                               float* lookAt, float* rotation, int flags, float a10, float a11, float a12, float a13);
using ReadPanFn = int(__fastcall*)(int self, int unused, int player, float* x, float* y);
using SecondsFn = double(__cdecl*)();

bool enabled;
bool applied;
bool toggleHeld;
bool mouseLook;
bool invertMouse;
float mouseSensitivity = 1.0f;
float gamepadSensitivity = 1.0f;
float distance = 1.0f;         // multiplier on the stick lengths
float scaled[kStickLengths];   // what the config held after the last scaling, to notice a reload
bool collide = true;
struct OrbitState {
  bool orbiting=false, surfaceSteering=false;
  int orbitCharacter=0, instance=0, input=0;
  float yaw=0,pitch=0,clearDistance=0;
  float position[4]{},lookAt[4]{};
  surfaceframe::V surfaceUp{0,1,0},surfaceForward{0,0,1},viewForward{0,0,1};
  float surfaceRoll=0,contactAge=1.0f,steeringYaw=0;
  surfaceframe::V retainedNormal{0,1,0},targetOffset{0,kTargetHeight,0};
};
OrbitState orbits[2];
bool Matches(const OrbitState& state,int character) {
  return state.orbiting && state.orbitCharacter==character
      && state.instance==playercharacter::Read<int>(character,4104)
      && state.input==playercharacter::Read<int>(character,1992);
}

// True with the hit point in `hit` when the level blocks the ray from `start` along `offset`.
bool RayHit(int chase, const float* start, const float* offset, float* hit, float* normal = nullptr) {
  int link = *reinterpret_cast<int*>(*reinterpret_cast<int*>(chase + kChaseAreaOffset) + 216);
  if (!link) return false;
  int area = reinterpret_cast<AreaObjectFn>(kAreaObject)(link);
  int world = area ? reinterpret_cast<AreaWorldFn>(kAreaWorld)(area, 0) : 0;
  if (!world) return false;
  alignas(16) uint8_t query[64] = {};  // the init writes 52 bytes
  reinterpret_cast<RayInitFn>(kRayInit)(query, 0, start, offset);
  *reinterpret_cast<int*>(query + kQueryMaskA) = 2;
  *reinterpret_cast<int*>(query + kQueryMaskB) = -2;
  alignas(16) uint8_t collector[176] = {};
  reinterpret_cast<CollectorFn>(kCollectorBase)(collector, 0);
  *reinterpret_cast<float*>(collector + kCollectorLimit) = 1.0f;
  *reinterpret_cast<uintptr_t*>(collector) = kCollectorVtable;
  reinterpret_cast<CollectorFn>(kCollectorReady)(collector, 0);
  if (!reinterpret_cast<RayCastFn>(kRayCast)(world, 0, query, collector)) return false;
  auto point = reinterpret_cast<const float*>(collector + kCollectorHit);
  hit[0] = point[0];
  hit[1] = point[1];
  hit[2] = point[2];
  if (normal) {
    auto n = reinterpret_cast<const float*>(collector + kCollectorNormal);
    float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!std::isfinite(length) || length < 0.5f) length = 0.0f;
    for (int i = 0; i < 3; ++i) normal[i] = length ? n[i] / length : 0.0f;
  }
  return true;
}

// Pulls the camera in to the first surface between the character and where the orbit wants it;
// in at once, back out eased. Five parallel rays stand in for the camera's width, and each hit backs
// the camera off far enough to keep the margin square to that surface, so glancing hits don't clip.
float Clear(OrbitState& state,int chase, const float* forward, float reach, float dt) {
  float span = reach - kCharacterRadius;
  using surfaceframe::V;
  V f{forward[0], forward[1], forward[2]};
  V right = surfaceframe::Unit(surfaceframe::Cross(f, V{0, 1, 0}));
  if (!std::isfinite(right.x)) right = V{1, 0, 0};
  V up = surfaceframe::Cross(right, f);
  const V sides[5] = {V{0, 0, 0}, right, right * -1.0f, up, up * -1.0f};
  float allowed = reach;
  for (const V& side : sides) {
    if (span <= 0.0f) break;
    float start[4] = {state.lookAt[0] - forward[0] * kCharacterRadius + side.x * kCameraRadius,
                      state.lookAt[1] - forward[1] * kCharacterRadius + side.y * kCameraRadius,
                      state.lookAt[2] - forward[2] * kCharacterRadius + side.z * kCameraRadius, 1.0f};
    float offset[4] = {-forward[0] * span, -forward[1] * span, -forward[2] * span, 0.0f};
    float hit[3], normal[3];
    if (!RayHit(chase, start, offset, hit, normal)) continue;
    float d[3] = {hit[0] - start[0], hit[1] - start[1], hit[2] - start[2]};
    float along = d[0] * -forward[0] + d[1] * -forward[1] + d[2] * -forward[2];
    float graze = std::fabs(normal[0] * forward[0] + normal[1] * forward[1] + normal[2] * forward[2]);
    float backoff = kWallMargin / std::max(graze, kMinGraze);
    allowed = std::min(allowed, std::clamp(kCharacterRadius + along - backoff, kMinDistance, reach));
  }
  if (allowed < state.clearDistance) state.clearDistance = allowed;
  else state.clearDistance += (allowed - state.clearDistance) * std::min(kPushOutRate * dt, 1.0f);
  return state.clearDistance;
}
// Surfaces beside the final spot that the rays along the view missed, and water, which the camera's
// collision query doesn't see: the camera is pushed off both by the margin.
void KeepClear(OrbitState& state, int chase, int character) {
  const float probes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (const auto& probe : probes) {
    float offset[4] = {probe[0] * kWallMargin, probe[1] * kWallMargin, probe[2] * kWallMargin, 0.0f};
    float hit[3];
    if (!RayHit(chase, state.position, offset, hit)) continue;
    float d[3] = {hit[0] - state.position[0], hit[1] - state.position[1], hit[2] - state.position[2]};
    float gap = kWallMargin - std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; ++i) state.position[i] -= probe[i] * gap;
  }
  float water = *reinterpret_cast<const float*>(character + kCharacterWater);
  if (std::isfinite(water) && water != kNoWater && state.lookAt[1] > water + kWaterMargin)
    state.position[1] = std::max(state.position[1], water + kWaterMargin);
}

bool wantCursorHidden;  // set by the level tick, read by the frame tick, so menus get the cursor back
bool cursorHidden;

// The game re-shows the Windows cursor from its message loop, so the hide runs on that thread,
// once per frame, and pins the cursor to the middle of the window so it can't wander off-screen.
void UpdateCursor() {
  HWND window = GetActiveWindow();
  DWORD owner = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &owner);
  bool hide = wantCursorHidden && window && owner == GetCurrentProcessId();
  wantCursorHidden = false;
  if (hide) {
    RECT r;
    if (GetClientRect(window, &r)) {
      POINT centre{(r.right - r.left) / 2, (r.bottom - r.top) / 2};
      ClientToScreen(window, &centre);
      RECT pin{centre.x, centre.y, centre.x + 1, centre.y + 1};
      ClipCursor(&pin);
    }
    if (!cursorHidden) {
      while (ShowCursor(FALSE) >= 0) {}
      cursorHidden = true;
    }
  } else if (cursorHidden) {
    ClipCursor(nullptr);
    while (ShowCursor(TRUE) < 0) {}
    cursorHidden = false;
  }
}

// The config is parsed once at startup, later than our install, so it's scaled when it shows
// up and again if the game ever re-reads the file.
void ScaleDistance() {
  auto lengths = reinterpret_cast<float*>(kFootConfig);
  if (distance == 1.0f || lengths[kStickLengths - 1] == 0.0f) return;
  bool current = true;
  for (int i = 0; i < kStickLengths; ++i) current &= lengths[i] == scaled[i];
  if (current) return;
  for (int i = 0; i < kStickLengths; ++i) scaled[i] = lengths[i] = lengths[i] * distance;
}

void Apply(bool on) {
  if (on == applied) return;
  applied = on;
  if (on) {
    hook::Patch(kLevelFlagMask, {0x00, 0x00, 0x20, 0x00}, {0x00, 0x00, 0x00, 0x00});
    hook::Patch(kDefaultPriority, {0x18, 0xFC, 0xFF, 0xFF}, {0xE8, 0x03, 0x00, 0x00});
  } else {
    hook::Patch(kLevelFlagMask, {0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x20, 0x00});
    hook::Patch(kDefaultPriority, {0xE8, 0x03, 0x00, 0x00}, {0x18, 0xFC, 0xFF, 0xFF});
  }
}

// Runs once per frame while a level is playing; the toggle is read here so it can't fire in menus.
int __cdecl OnUpdate(int area) {
  bool down = abilitycontrols::CameraToggleDown();
  if (down && !toggleHeld) enabled = !enabled;
  toggleHeld = down;
  Apply(enabled);
  ScaleDistance();
  if (!enabled) for (auto& state:orbits) state.orbiting=false;
  wantCursorHidden = enabled && mouseLook;
  return reinterpret_cast<UpdateFn>(kChaseUpdate)(area);
}

// Picks up the angles from wherever the game's camera is, so the hand-over doesn't jump.
void StartOrbit(OrbitState& state,int chase) {
  state=OrbitState{};
  auto from = reinterpret_cast<const float*>(chase + kChasePositionOffset);
  auto to = reinterpret_cast<const float*>(chase + kChaseLookAtOffset);
  float f[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
  float flat = std::sqrt(f[0] * f[0] + f[2] * f[2]);
  state.yaw = std::atan2(f[0], f[2]);
  state.pitch = std::clamp(-std::atan2(f[1], std::max(flat, 0.001f)), kMinPitch, kMaxPitch);
  state.clearDistance = reinterpret_cast<const float*>(kFootConfig)[1];
  state.surfaceUp={0,1,0}; state.surfaceForward={0,0,1}; state.surfaceRoll=0;
  state.contactAge=1; state.surfaceSteering=false; state.retainedNormal={0,1,0}; state.targetOffset={0,kTargetHeight,0};
  state.orbiting = true;
}

void Orbit(OrbitState& state,int chase,int character,int player) {
  if (!Matches(state,character)) StartOrbit(state,chase);
  state.orbitCharacter=character;
  state.instance=playercharacter::Read<int>(character,4104);
  state.input=playercharacter::Read<int>(character,1992);
  float dt = static_cast<float>(reinterpret_cast<SecondsFn>(kFrameSeconds)());
  float stickX = 0.0f, stickY = 0.0f;
  reinterpret_cast<ReadPanFn>(kReadPan)(chase, 0, *reinterpret_cast<int*>(chase + kChasePlayerOffset), &stickX,
                                        &stickY);
  float mouseX = 0.0f, mouseY = 0.0f;
  if (mouseLook && player==0 && !reticle::MouseActive(character)) devices::TakeMouse(mouseX, mouseY);
  float counts = kRadiansPerCount * mouseSensitivity;
  state.yaw += stickX * kYawSpeed * gamepadSensitivity * dt + mouseX * counts;
  state.pitch += stickY * kPitchSpeed * gamepadSensitivity * dt + mouseY * counts * (invertMouse ? -1.0f : 1.0f);
  state.pitch = std::clamp(state.pitch, kMinPitch, kMaxPitch);
  state.yaw = std::remainder(state.yaw, 2.0f * kPi);

  using namespace surfaceframe;
  V targetUp{0,1,0}, tangent{};
  dt=std::clamp(dt,0.0f,0.1f);
  bool attached=surfacerun::Frame(character,&targetUp.x,&tangent.x);
  if (attached) {
    if (!state.surfaceSteering) state.steeringYaw=state.yaw;
    state.surfaceSteering=true; state.retainedNormal=targetUp; state.contactAge=0;
  } else {
    state.contactAge+=dt;
    if (state.contactAge>0.45f) { state.surfaceSteering=false; state.retainedNormal={0,1,0}; }
  }
  Approach(state.surfaceUp,state.surfaceForward,CameraUp(state.retainedNormal),dt,2.0f,0.2617994f);
  V wantedOffset=state.retainedNormal*kTargetHeight;
  state.targetOffset=state.targetOffset+(wantedOffset-state.targetOffset)*(1.0f-std::exp(-4.0f*dt));
  auto origin = reinterpret_cast<const float*>(character + kCharacterPositionOffset);
  state.lookAt[0] = origin[0] + state.targetOffset.x;
  state.lookAt[1] = origin[1] + state.targetOffset.y;
  state.lookAt[2] = origin[2] + state.targetOffset.z;
  state.lookAt[3] = 1.0f;
  float reach = reinterpret_cast<const float*>(kFootConfig)[1];
  // 8B71B0 selects chase_camera_vehicle.txt with this resource flag.
  if (state.instance && (playercharacter::Read<uint32_t>(state.instance,20) & 0x2000)) reach *= 1.5f;
  state.viewForward=View(state.surfaceUp,state.surfaceForward,state.yaw,state.pitch);
  // Keep the native look-at state.yaw defined at a vertical view.
  if (state.viewForward.x*state.viewForward.x+state.viewForward.z*state.viewForward.z<0.000001f) {
    V offset=Unit(Plane(state.surfaceUp,{0,1,0}));
    state.viewForward=Unit(state.viewForward+offset*0.001f);
  }
  state.surfaceRoll=EaseRoll(state.surfaceRoll,Roll(state.viewForward,state.surfaceUp)*180.0f/kPi,dt);
  float forward[3]={state.viewForward.x,state.viewForward.y,state.viewForward.z};
  if (collide) reach = Clear(state,chase, forward, reach, dt);
  for (int i = 0; i < 3; ++i) state.position[i] = state.lookAt[i] - forward[i] * reach;
  state.position[3] = 1.0f;
  if (collide) KeepClear(state, chase, character);
}

int __cdecl OnSubmit(int character, int name, int chase, int priority, float weight, float* pos, float* look,
                     float* rotation, int flags, float a10, float a11, float a12, float a13) {
  float rolled[3]={rotation[0],rotation[1],rotation[2]};
  int player=*reinterpret_cast<int*>(chase+kChasePlayerOffset);
  if (enabled && character && player>=0 && player<2 && playercharacter::HumanSlot(character)==player) {
    auto& state=orbits[player];
    Orbit(state,chase,character,player);
    pos = state.position;
    look = state.lookAt;
    rolled[2]+=state.surfaceRoll;
    rotation=rolled;
  }
  return reinterpret_cast<SubmitFn>(kSubmit)(character, name, chase, priority, weight, pos, look, rotation, flags, a10,
                                             a11, a12, a13);
}

}  // namespace

bool SurfaceDirection(int character, short angle, const float* normal, float* result) {
  int player=playercharacter::HumanSlot(character);
  if (!enabled || player<0) return false;
  const auto& state=orbits[player];
  if (!Matches(state,character)) return false;
  using namespace surfaceframe;
  V n{normal[0],normal[1],normal[2]};
  V forward{},unused{};
  if (!surfacerun::Frame(character,&unused.x,&forward.x)) return false;
  float relative=angle*(2*kPi/65536.0f)+(state.surfaceSteering?state.yaw-state.steeringYaw:0.0f);
  V direction=forward*std::cos(relative)+Unit(Cross(n,forward))*std::sin(relative);
  result[0]=direction.x; result[1]=direction.y; result[2]=direction.z;
  return true;
}

void SetGamepadSensitivity(int percent) { gamepadSensitivity = std::clamp(percent,10,500)/100.0f; }
int MouseSensitivity() { return static_cast<int>(std::lround(mouseSensitivity*100.0f)); }
void SetMouseSensitivity(int percent) { mouseSensitivity = std::clamp(percent,10,500)/100.0f; }

void Install(bool startEnabled, int distancePercent, bool mouse, int mouseSensitivityPercent, bool invertY,
             bool wallCollision) {
  enabled = startEnabled;
  collide = wallCollision;
  distance = std::clamp(distancePercent, 25, 400) / 100.0f;
  mouseLook = mouse;
  mouseSensitivity = std::clamp(mouseSensitivityPercent, 10, 500) / 100.0f;
  invertMouse = invertY;
  hook::Call(kUpdateCallSite, reinterpret_cast<void*>(kChaseUpdate), reinterpret_cast<void*>(OnUpdate));
  hook::Call(kSubmitCallSite, reinterpret_cast<void*>(kSubmit), reinterpret_cast<void*>(OnSubmit));
  if (mouseLook) frame::OnTick(UpdateCursor);
}

}  // namespace chasecam
