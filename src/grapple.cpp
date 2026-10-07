#include "grapple.h"
#include "character_list.h"
#include "grapple_permission.h"
#include "lantern_grapple.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "climb.h"
#include "flash_roll.h"
#include "surface_run.h"
#include "glide.h"
#include "glide_anim.h"
#include "hook.h"
#include "ability_controls.h"
#include "player_character.h"

namespace grapple {
namespace {
CharacterList allowed;
bool enabled;

bool ListedLantern(int character) {
  return enabled && allowed.Contains(character) &&
      *reinterpret_cast<uint16_t*>(character + 3944) == *reinterpret_cast<uint16_t*>(0x101FE60);
}

// LEGOBatman2.exe addresses (Steam build, no ASLR).
// The game's grapple starts from one routine that takes the character and a grapple gizmo and
// does the rest itself: the state, the anims, the rope, the pull, the hang and the climb onto the
// top. The game only calls it for its own points, so the grapple key is read here instead: on a
// press, a ray ahead finds the wall in front, a ray down from above it finds the roof edge, a
// private copy of one of the level's grapple gizmos is placed at that edge, and the start routine
// is given the copy. The copy sits in no list, so nothing draws or targets it and no real point
// goes missing. The check runs from the per-frame anim sync, a plain call made for every character.
constexpr uintptr_t kCharacterTick = 0xAAF6C0;  // cdecl(character), the anim sync
constexpr uintptr_t kCharacterTickSites[] = {0xB244A6, 0xB24859};
constexpr uintptr_t kSceneOfEntity = 0x403510;  // cdecl(entity) -> the level scene the systems hang off
constexpr uintptr_t kGrappleManager = 0x11BDFB0;  // dword, the grapple manager object
constexpr uintptr_t kSystemTable = 0x11BDEEC;     // dword, object holding the system count at +100
constexpr int kManagerSystemIndex = 116;
constexpr int kManagerAttachOffset = 132;  // float x, y, z: the attach point relative to a point, in its frame
constexpr int kSceneSystems = 88;          // -> +8 array of system objects
constexpr uintptr_t kIterateSystems = 0xA5A170;  // thiscall(system, callback(system, params) -> stop, params, 1)
constexpr int kSystemFirstGizmo = 104;  // list of grapple gizmos, next at gizmo + 12
constexpr int kGizmoSize = 688;
constexpr int kGizmoNext = 12;
constexpr int kGizmoPosition = 72;    // float x, y, z
constexpr int kGizmoPitch = 84;       // u16 angles the attach offset is rotated by
constexpr int kGizmoYaw = 86;
constexpr int kGizmoFlags = 90;
constexpr uint16_t kGizmoActive = 2;
constexpr int kGizmoSelf = 176;       // the targeting interface's pointer back to the gizmo
constexpr int kGizmoKind = 182;       // bit 0 = pull-type point
constexpr int kGizmoMarkX = 192;      // the attach point's x and z again
constexpr int kGizmoFloor = 196;      // height of the floor under the attach point
constexpr int kGizmoMarkZ = 200;
constexpr int kGizmoAttach = 204;     // float x, y, z: where the rope goes
constexpr int kGizmoLanding = 228;    // float x, y, z: where the character hangs
constexpr int kGizmoFloorFacing = 240;  // two u16 angles from the floor probe
constexpr int kGizmoMatrixFlag = 592;   // nonzero: the attach point comes from the matrix at +596 instead

// At the hang a jump press goes through one manager method: with the flip clip in the set it
// starts the climb-onto-the-top context, otherwise it does a plain jump out of the grapple, the
// same jump the pull allows at any time. Our copies take the plain jump every time.
constexpr uintptr_t kJumpTopSlot = 0xCFEEE4 + 240;  // manager vtable entry holding sub_4F0F30
constexpr uintptr_t kJumpTop = 0x4F0F30;            // thiscall(manager, character, gizmo)
constexpr uintptr_t kJumpOut = 0xB0DA20;            // (character, gizmo)
// Two more manager checks the start routine leans on: whether the character is on a given point in
// any of the grapple, hang or climb states (sub_46B510), and whether a grapple may start at all
// (sub_429D80: not while climbing onto a top, and only where the level allows it). When the second
// says no, the start routine plays the shrug, which drops a hanging character, so it is asked first.
constexpr uintptr_t kUsingPoint = 0x46B510;   // stdcall(character, gizmo)
constexpr uintptr_t kClimbTopState = 0x102135C;  // dword, the climb-onto-the-top state id

// Character.
constexpr int kEntity = 16;
constexpr int kPosition = 4140;  // float x, y, z
constexpr int kHeading = 436;    // float radians
constexpr int kState = 1270;
constexpr int kGrapplePhase = 1267;  // 1 once the pull has finished and the character hangs
constexpr uintptr_t kBusyStates[] = {0x11A2E7C, 0x11A2E80, 0x11A2E84, 0x11A2E30, 0x11A2E28};  // grapple x3, hang, ledge
constexpr float kRadiansToTurns = 10430.378f;  // 65536 / 2 pi

// Collision query, the recipe the grapple gizmo's own floor probe uses.
constexpr uintptr_t kEntityOf = 0x67F070;      // thiscall(character + 16)
constexpr uintptr_t kSceneOf = 0x42F210;       // cdecl(entity)
constexpr uintptr_t kWorldOf = 0x6A34B0;       // fastcall(scene)
constexpr uintptr_t kRayInit = 0x402150;       // thiscall(ray, start, extent)
constexpr uintptr_t kCollectorBase = 0xB8F200;  // thiscall(collector)
constexpr uintptr_t kCollectorInit = 0xB80D00;  // thiscall(collector)
constexpr uintptr_t kRayCast = 0xB99610;       // thiscall(world, ray, collector) -> hit
constexpr uintptr_t kFirstPointCollector = 0xCFB550;  // its vtable
constexpr int kHitPoint = 36;  // float x, y, z in the collector

// Vector helpers.
constexpr uintptr_t kRotateX = 0x51F4F0;  // cdecl(out, in, angle16)
constexpr uintptr_t kRotateY = 0x51F560;  // cdecl(out, in, angle16)

using TickFn = void(__cdecl*)(int character);
using JumpTopFn = void(__fastcall*)(int manager, void* edx, int character, int gizmo);
using JumpOutFn = void(__stdcall*)(int character, int gizmo);
using EntityFn = int(__fastcall*)(int self, void* edx);
using SceneFn = int(__cdecl*)(int entity);
using WorldFn = int(__fastcall*)(int scene);
using RayInitFn = float*(__fastcall*)(float* ray, void* edx, const float* start, const float* extent);
using CollectorFn = int(__fastcall*)(void* collector, void* edx);
using RayCastFn = bool(__fastcall*)(int world, void* edx, float* ray, void* collector);
using IterateFn = int(__fastcall*)(int system, void* edx, void* callback, void* params, int flag);
using RotateFn = float*(__cdecl*)(float* out, const float* in, int angle);

float range = 14.0f;
float height = 30.0f;
constexpr float kEyeHeight = 1.0f;  // the wall ray starts this far above the feet
constexpr float kInset = 0.35f;     // the roof ray starts this far past the wall face
constexpr float kMinRise = 1.5f;    // the edge must be at least this far above the feet
constexpr float kLipDepth = 0.1f;   // the face ray runs this far under the roof edge
constexpr float kFaceReach = 2.0f;  // and starts this far out from the eye-level hit
constexpr float kRopeGap = 0.03f;   // the rope end sits this far out from the face

struct Proxy {
  int scene;
  int character;
  uint8_t* gizmo;
};
Proxy proxies[2];

int World(int character) {
  int entity = reinterpret_cast<EntityFn>(kEntityOf)(character + kEntity, nullptr);
  int scene = entity ? reinterpret_cast<SceneFn>(kSceneOf)(entity) : 0;
  return scene ? reinterpret_cast<WorldFn>(kWorldOf)(scene) : 0;
}

bool DoorBody(uintptr_t body) {
  auto id = *reinterpret_cast<uint32_t*>(0x139DDF8);
  return body && id && *reinterpret_cast<uint32_t*>(body+32)==id;
}

struct DoorQuery {
  float point[3];
  float radius, reach;
  bool found = false;
};

bool InDoorOpening(int door, const DoorQuery& query) {
  // 8CC420 builds bottom-left, top-left, top-right and bottom-right at +168..+212.
  auto corner = reinterpret_cast<const float*>(door+168);
  float right[3], up[3], delta[3], normal[3];
  float width2=0, height2=0;
  for (int i=0;i<3;++i) {
    right[i]=corner[9+i]-corner[i];up[i]=corner[3+i]-corner[i];
    delta[i]=query.point[i]-corner[i];
    width2+=right[i]*right[i];height2+=up[i]*up[i];
  }
  if (width2<0.0001f || height2<0.0001f) return false;
  float width=std::sqrt(width2),height=std::sqrt(height2),x=0,y=0,z=0;
  for (int i=0;i<3;++i) {right[i]/=width;up[i]/=height;}
  for (int i=0;i<3;++i) normal[i]=right[(i+1)%3]*up[(i+2)%3]-right[(i+2)%3]*up[(i+1)%3];
  for (int i=0;i<3;++i) {x+=delta[i]*right[i];y+=delta[i]*up[i];z+=delta[i]*normal[i];}
  return x>=-query.radius && x<=width+query.radius && y>=-query.radius && y<=height+query.radius
    && std::fabs(z)<=query.reach+query.radius;
}

char __cdecl DoorCallback(int system, DoorQuery* query) {
  for (int door=*reinterpret_cast<int*>(system+104);door;door=*reinterpret_cast<int*>(door+12)) {
    if ((*reinterpret_cast<uint16_t*>(door+90)&2) && InDoorOpening(door,*query)) {
      query->found=true;return 1;
    }
  }
  return 0;
}

bool Cast(int world, const float* start, const float* extent, float* hit, float* normal = nullptr, bool climb = false);

}  // namespace

bool CastRay(int character, const float* start, const float* extent, float* hit, float* normal) {
  int world = World(character);
  return world && Cast(world, start, extent, hit, normal);
}

bool CastClimbRay(int character, const float* start, const float* extent, float* hit, float* normal) {
  int world = World(character);
  return world && Cast(world,start,extent,hit,normal,true);
}

bool DoorContact(int character) {
  // A2AD10 copies the selected collision's body into the contact record at +1104.
  return *reinterpret_cast<uint8_t*>(character+3968) && DoorBody(*reinterpret_cast<uintptr_t*>(character+1104));
}

bool DoorApproach(int character, float reach) {
  int entity=reinterpret_cast<EntityFn>(kEntityOf)(character+kEntity,nullptr);
  int scene=entity ? reinterpret_cast<SceneFn>(kSceneOfEntity)(entity) : 0;
  int manager=*reinterpret_cast<int*>(0x11BDFCC),table=*reinterpret_cast<int*>(kSystemTable);
  int systems=scene ? *reinterpret_cast<int*>(scene+kSceneSystems) : 0;
  if (!manager || !table || !systems) return false;
  int index=*reinterpret_cast<int*>(manager+kManagerSystemIndex);
  if (index<0 || index>=*reinterpret_cast<int*>(table+100)) return false;
  int system=*reinterpret_cast<int*>(*reinterpret_cast<int*>(systems+8)+4*index);
  if (!system) return false;
  int resource=*reinterpret_cast<int*>(character+4104);
  if (!resource) return false;
  DoorQuery query{};
  memcpy(query.point,reinterpret_cast<void*>(character+kPosition),sizeof(query.point));
  query.radius=*reinterpret_cast<float*>(resource+52)* *reinterpret_cast<float*>(character+4192);
  query.reach=reach;
  reinterpret_cast<IterateFn>(kIterateSystems)(system,nullptr,reinterpret_cast<void*>(DoorCallback),&query,1);
  return query.found;
}

namespace {

bool Cast(int world, const float* start, const float* extent, float* hit, float* normal, bool climb) {
  float ray[24] = {};
  uint32_t collector[44] = {};
  float from[4] = {start[0], start[1], start[2], 1.0f};
  float to[4] = {extent[0], extent[1], extent[2], 1.0f};
  reinterpret_cast<RayInitFn>(kRayInit)(ray, nullptr, from, to);
  auto words = reinterpret_cast<int*>(ray);
  words[8] = 18;
  words[9] &= 0xFFFFEFFB;
  *reinterpret_cast<int16_t*>(ray + 10) = -1025;
  words[11] = 1;
  words[12] = 1;
  collector[28] = collector[29] = 0;
  reinterpret_cast<CollectorFn>(kCollectorBase)(collector, nullptr);
  reinterpret_cast<float*>(collector)[4] = 1.0f;
  collector[0] = kFirstPointCollector;
  collector[30] = collector[31] = 0;
  reinterpret_cast<CollectorFn>(kCollectorInit)(collector, nullptr);
  if (!reinterpret_cast<RayCastFn>(kRayCast)(world, nullptr, ray, collector)) return false;
  // 402040 stores the hit body at +120; 8CC260 identifies GizmoDoorBox through body+32.
  if (climb && DoorBody(collector[30])) return false;
  memcpy(hit, collector + kHitPoint, 3 * sizeof(float));
  if (normal) memcpy(normal, collector + 40, 3 * sizeof(float));
  return true;
}

// The wall in front at eye height, then the first surface a ray meets coming down from far above
// that wall, just past its face; that surface is the roof edge when it sits well above the feet. A
// last ray just under the edge finds the face there, since setbacks and cornices move it from the eye-level hit.
bool FindLedge(int character, float* edge, float* outward) {
  int world = World(character);
  if (!world) return false;
  auto position = reinterpret_cast<const float*>(character + kPosition);
  float yaw = *reinterpret_cast<const float*>(character + kHeading);
  float fx = sinf(yaw), fz = cosf(yaw);
  float eye[3] = {position[0], position[1] + kEyeHeight, position[2]};
  float ahead[3] = {fx * range, 0.0f, fz * range};
  float wall[3], normal[3];
  if (!Cast(world, eye, ahead, wall, normal)) return false;
  float nx = normal[0], nz = normal[2], length = std::sqrt(nx * nx + nz * nz);
  if (!std::isfinite(length) || length < 0.3f) {
    nx = -fx;
    nz = -fz;
  } else {
    nx /= length;
    nz /= length;
    if (nx * fx + nz * fz > 0) nx = -nx, nz = -nz;
  }
  float above[3] = {wall[0] - nx * kInset, wall[1] + height, wall[2] - nz * kInset};
  float drop[3] = {0.0f, -(height + (wall[1] - position[1]) + 1.0f), 0.0f};
  float top[3];
  if (!Cast(world, above, drop, top)) return false;
  if (top[1] < position[1] + kMinRise || top[1] <= wall[1]) return false;
  float lip[3] = {wall[0] + nx * kFaceReach, top[1] - kLipDepth, wall[2] + nz * kFaceReach};
  float across[3] = {-nx * (kFaceReach + kInset), 0.0f, -nz * (kFaceReach + kInset)};
  float face[3] = {wall[0], top[1], wall[2]};
  if (Cast(world, lip, across, face)) face[1] = top[1];
  memcpy(edge, face, 3 * sizeof(float));
  outward[0] = nx;
  outward[1] = 0.0f;
  outward[2] = nz;
  return true;
}

int GrappleSystem(int scene) {
  int manager = *reinterpret_cast<int*>(kGrappleManager);
  int table = *reinterpret_cast<int*>(kSystemTable);
  int systems = scene ? *reinterpret_cast<int*>(scene + kSceneSystems) : 0;
  if (!manager || !table || !systems) return 0;
  int index = *reinterpret_cast<int*>(manager + kManagerSystemIndex);
  if (index < 0 || index >= *reinterpret_cast<int*>(table + 100)) return 0;
  return *reinterpret_cast<int*>(*reinterpret_cast<int*>(systems + 8) + 4 * index);
}

// Gotham streams in chunks and every chunk's scene keeps its own grapple system and gizmo list, so
// the game's target search walks all of them through an iterator when the character's own is
// empty. The same iterator, with a callback of ours, finds a plain grapple point to copy.
char __cdecl PickCallback(int system, int* found) {
  for (int gizmo = *reinterpret_cast<int*>(system + kSystemFirstGizmo); gizmo;
       gizmo = *reinterpret_cast<int*>(gizmo + kGizmoNext)) {
    if (*reinterpret_cast<uint8_t*>(gizmo + kGizmoKind) & 1) continue;
    *found = gizmo;
    return 1;
  }
  return 0;
}

void InitializeClone(uint8_t* gizmo) {
  *reinterpret_cast<int*>(gizmo + kGizmoNext) = 0;
  *reinterpret_cast<uint8_t**>(gizmo + kGizmoSelf) = gizmo;
  *reinterpret_cast<int*>(gizmo + kGizmoMatrixFlag) = 0;
  // Private copies never receive the scene activation update; sub_B0DF80 cancels inactive targets.
  *reinterpret_cast<uint16_t*>(gizmo+kGizmoFlags) |= kGizmoActive;
}

uint8_t* CloneGizmo(int scene) {
  int system = GrappleSystem(scene);
  if (!system) return nullptr;
  int model = 0;
  reinterpret_cast<IterateFn>(kIterateSystems)(system, nullptr, reinterpret_cast<void*>(PickCallback), &model, 1);
  if (!model) return nullptr;
  auto gizmo = new uint8_t[kGizmoSize];
  memcpy(gizmo, reinterpret_cast<const void*>(model), kGizmoSize);
  InitializeClone(gizmo);
  return gizmo;
}

// One copy per character and level. A copy from an earlier level is abandoned, not freed: the
// game may still hold its address in a character record for a moment.
Proxy* ProxyFor(int scene, int character) {
  for (Proxy& proxy : proxies)
    if (proxy.scene != scene) proxy = {};
  for (Proxy& proxy : proxies)
    if (proxy.character == character && proxy.gizmo) return &proxy;
  for (Proxy& proxy : proxies) {
    if (proxy.gizmo) continue;
    proxy = {};
    proxy.scene = scene;
    proxy.character = character;
    proxy.gizmo = CloneGizmo(scene);
    return proxy.gizmo ? &proxy : nullptr;
  }
  return nullptr;
}

bool Busy(int character) {
  auto state = *reinterpret_cast<uint16_t*>(character + kState);
  for (uintptr_t id : kBusyStates) {
    int value = *reinterpret_cast<int*>(id);
    if (value != -1 && state == static_cast<uint16_t>(value)) return true;
  }
  return false;
}

// The point faces out of the wall like the level's points. The level's rope end sits at the manager's
// designed offset in front of a visible marker; ours has none, so the point is sunk into the wall until
// that offset lands on the face, and the hang keeps the designed clearance.
void Place(uint8_t* gizmo, const float* edge, const float* outward) {
  auto facing = static_cast<uint16_t>(static_cast<int>(std::atan2(outward[0], outward[2]) * kRadiansToTurns));
  *reinterpret_cast<uint16_t*>(gizmo + kGizmoPitch) = 0;
  *reinterpret_cast<uint16_t*>(gizmo + kGizmoYaw) = facing;
  int manager = *reinterpret_cast<int*>(kGrappleManager);
  float offset[3], turned[3], attach[3], position[3], landing[3];
  reinterpret_cast<RotateFn>(kRotateX)(offset, reinterpret_cast<const float*>(manager + kManagerAttachOffset), 0);
  reinterpret_cast<RotateFn>(kRotateY)(turned, offset, facing);
  for (int i = 0; i < 3; ++i) {
    attach[i] = edge[i] + outward[i] * kRopeGap + (i == 1 ? turned[1] : 0.0f);
    position[i] = attach[i] - turned[i];
    landing[i] = edge[i] + turned[i];
  }
  landing[1] -= 0.1f;
  memcpy(gizmo + kGizmoPosition, position, 3 * sizeof(float));
  memcpy(gizmo + kGizmoAttach, attach, 3 * sizeof(float));
  memcpy(gizmo + kGizmoLanding, landing, 3 * sizeof(float));
  *reinterpret_cast<float*>(gizmo + kGizmoMarkX) = attach[0];
  *reinterpret_cast<float*>(gizmo + kGizmoMarkZ) = attach[2];
  *reinterpret_cast<float*>(gizmo + kGizmoFloor) = edge[1] - 50.0f;
  *reinterpret_cast<uint32_t*>(gizmo + kGizmoFloorFacing) = 0;
}

bool IsClone(int gizmo) {
  for (const Proxy& proxy : proxies)
    if (proxy.gizmo && reinterpret_cast<int>(proxy.gizmo) == gizmo) return true;
  return false;
}

float* __fastcall OnFallingDive(int self, void*, int argument) {
  int character = *reinterpret_cast<int*>(self+68);
  // 41E2F0 overwrites both velocity vectors with a fixed descent.
  if (enabled && character && allowed.Contains(character) && playercharacter::HumanSlot(character)>=0 &&
      Busy(character) && IsClone(*reinterpret_cast<int*>(character+1228)))
    return reinterpret_cast<float*>(character);
  return reinterpret_cast<float*(__thiscall*)(int,int)>(0x41E2F0)(self,argument);
}

void __fastcall OnJumpTop(int manager, void* edx, int character, int gizmo) {
  if (IsClone(gizmo)) {
    reinterpret_cast<JumpOutFn>(kJumpOut)(character, gizmo);
    return;
  }
  reinterpret_cast<JumpTopFn>(kJumpTop)(manager, edx, character, gizmo);
}

void PatchPointer(uintptr_t slot, uintptr_t expected, uintptr_t value) {
  hook::Patch(slot,
              {static_cast<BYTE>(expected), static_cast<BYTE>(expected >> 8), static_cast<BYTE>(expected >> 16),
               static_cast<BYTE>(expected >> 24)},
              {static_cast<BYTE>(value), static_cast<BYTE>(value >> 8), static_cast<BYTE>(value >> 16),
               static_cast<BYTE>(value >> 24)});
}

void __cdecl OnCharacterTick(int character) {
  lanterngrapple::Tick(character, Occupied(character));
  glide::BeforeSync(character);
  surfacerun::BeforeSync(character);
  flashroll::BeforeSync(character);
  reinterpret_cast<TickFn>(kCharacterTick)(character);
  glide::ApplyPitch(character);
  surfacerun::AfterSync(character);
  flashroll::AfterSync(character);
  climb::Tick(character);
  bool pressed=abilitycontrols::GrapplePressed(character);
  if (!enabled || !pressed || !allowed.Contains(character)) return;
  // On one of our points the only way off is the plain jump; there is no climb onto the top.
  if (Occupied(character) || grapplepermission::Flying(character)) return;
  int entity = reinterpret_cast<EntityFn>(kEntityOf)(character + kEntity, nullptr);
  int scene = entity ? reinterpret_cast<SceneFn>(kSceneOfEntity)(entity) : 0;
  if (!scene) return;
  Proxy* proxy = ProxyFor(scene, character);
  if (!proxy) return;
  float edge[3], outward[3];
  if (!FindLedge(character, edge, outward)) return;
  Place(proxy->gizmo, edge, outward);
  lanterngrapple::Prepare(character);
  grapplepermission::Start(character, reinterpret_cast<int>(proxy->gizmo));
  lanterngrapple::FinishStart(character, reinterpret_cast<int>(proxy->gizmo));
}

}  // namespace

bool Occupied(int character) {
  auto state = *reinterpret_cast<uint16_t*>(character + kState);
  return Busy(character) || state == static_cast<uint16_t>(*reinterpret_cast<int*>(kClimbTopState));
}

void InstallTicks() {
  for (uintptr_t site : kCharacterTickSites)
    hook::Call(site, reinterpret_cast<void*>(kCharacterTick), reinterpret_cast<void*>(OnCharacterTick));
}

void Characters(const std::string& names) { allowed.Set(names); }

void Install(int rangeUnits, int heightUnits, const std::string& folder) {
  enabled = true;
  hook::Call(0x493FFD,reinterpret_cast<void*>(0x41E2F0),reinterpret_cast<void*>(OnFallingDive));
  lanterngrapple::Install(ListedLantern);
  glideanim::GrappleClips(folder,lanterngrapple::AnimSet);
  if (rangeUnits > 0) range = static_cast<float>(rangeUnits);
  if (heightUnits > 0) height = static_cast<float>(heightUnits);
  PatchPointer(kJumpTopSlot, kJumpTop, reinterpret_cast<uintptr_t>(&OnJumpTop));
}

}  // namespace grapple
