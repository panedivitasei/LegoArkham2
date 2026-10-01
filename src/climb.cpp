#include "climb.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "glide.h"
#include "surface_run.h"
#include "glide_anim.h"
#include "grapple.h"
#include "keybinds.h"
#include "player_character.h"
#include "climb_surface.h"

namespace climb {
namespace {

// LEGOBatman2.exe addresses (Steam build, no ASLR).
// The puzzle climb-walls are a surface type. Every collision surface carries a type index, the
// surface table (36-byte entries, up to 33 of them, filled from data at load) holds a flags word
// per type, and bit 0x10000 makes a type climbable (sub_89E510). The wall-climb logic
// (sub_B17030) casts its own ray along the heading each frame, and when the hit surface's type is
// climbable and a second cast from the feet agrees (sub_9497F0), it enters the wall-climb state
// (dword_11A2E2C) with that type in +1272; the move handler (sub_8F5F60) then runs the stick along
// the wall plane. Setting the bit on every type makes every wall a climb-wall.
// A played character keeps climbing only while it carries the "touching a climb wall" contact mark
// (+3692 = 20, held for a few seconds) that the climb-wall surfaces' contact handler writes; without
// it the state falls back to a jump at the wall every frame. The mark is written here whenever a
// wall is ahead or a climb is running. Surface types 5 and 7 want a different mark (11), so the
// type is left as the wall's own; the state uses the climb_wall idle and move clips either way.
constexpr uintptr_t kSurfaceTable = 0x11AD594;
constexpr int kSurfaceTypes = 33;
constexpr int kSurfaceEntry = 36;  // bytes
constexpr uint32_t kClimbable = 0x10000;
constexpr uintptr_t kWallClimbState = 0x11A2E2C;  // dword
constexpr int kClimbProbe = 3968;   // byte, the game's own "climbable surface ahead" mark
constexpr int kContactKind = 3692;  // u16, what the character last touched
constexpr int kContactTime = 3696;  // float, seconds the mark holds
constexpr int kContactWall = 20;
constexpr int kPosition = 4140;     // Predicted collision center; sub_97A060.
constexpr int kHeading = 436;       // float radians
constexpr int kState = 1270;
constexpr int kController = 5804;
constexpr int kRequested = 3760;    // the velocity the move handler asks for this frame
constexpr int kMoveFlags = 140;
constexpr float kMantleBoost = 2.0f;  // sub_8F5F60 adds this to a rising climb at a ledge
constexpr float kHold = 5.0f;
constexpr float kProbeHeight = 1.0f;   // the wall rays start this far above the feet
constexpr float kProbeSpread = 0.35f;  // the side rays are this far left and right
// Upward-facing surfaces end the climb.
constexpr float kFloorFacing = 0.5f;
constexpr int kClimbNormal = 1124;     // V, the climb surface normal; sub_B17030 writes it from the probe
constexpr float kTurnRate = 5.0f;      // radians per second the climbing body turns toward its travel
// AFB5D0 stores 97B610's camera-resolved input before invoking the move handler.
constexpr int kMoveAngle = 2568;
constexpr float kTurnsPerRadian = 10430.378f;
constexpr uintptr_t kJumpState = 0x11A2E34;

using LedgeStateFn=int(__fastcall*)(int,void*,int);
using LedgeAnimFn=bool(__fastcall*)(int,void*,int,int);
LedgeStateFn ledgeState=reinterpret_cast<LedgeStateFn>(0x403720);
LedgeAnimFn ledgeAnim=reinterpret_cast<LedgeAnimFn>(0x4D7B30);

bool enabled;
int mode = kModeClimb;
float speedScale = 1.0f;
float reach = 1.2f;
uint8_t key;
bool surveyed;
uint32_t original[kSurfaceTypes];  // the table's own flags, put back while the character is occupied

template<class T> T& At(int c,int offset) { return *reinterpret_cast<T*>(c+offset); }
using V=surfaceframe::V;
struct Ledge {
  int character=0,instance=0;
  bool active=false,ownsTransform=false,previousOwnTransform=false;
  bool jumpHeld=false;
  bool ceilingReached=false;
  climbsurface::Frame frame{},visual{};
  V wallRight{};
  float unsupported=0;
  float clearance=.225f;
};
Ledge ledges[2];
using LedgeTimeFn=double(__cdecl*)();
using LedgeTestFn=bool(__cdecl*)(int);
using HasAnimFn=bool(__cdecl*)(int,int);
LedgeTimeFn ledgeTime=reinterpret_cast<LedgeTimeFn>(0x8B4120);
LedgeTestFn ledgeJump=reinterpret_cast<LedgeTestFn>(0x886D50);
HasAnimFn hasAnim=reinterpret_cast<HasAnimFn>(0x895090);
void Release(Ledge& entry) {
  if (entry.ownsTransform && !entry.previousOwnTransform) At<uint32_t>(entry.character,132)&=~0x800000u;
  entry.ownsTransform=false;entry.active=false;
}
Ledge* CurrentLedge(int c) {
  int slot=playercharacter::HumanSlot(c);
  if (slot<0) return nullptr;
  auto& entry=ledges[slot];
  if (entry.character!=c || entry.instance!=At<int>(c,4104)) {
    auto* party=reinterpret_cast<const int*>(0x138F4E0);
    for (int index=0;index<3;++index)
      if (entry.character && party[index]==entry.character && entry.instance==At<int>(entry.character,4104)) {
        Release(entry);break;
      }
    entry={};entry.character=c;entry.instance=At<int>(c,4104);
  }
  return &entry;
}
bool LedgeRay(int c,V start,V extent,V& hit,V& normal) {
  if (!grapple::CastClimbRay(c,&start.x,&extent.x,&hit.x,&normal.x)) return false;
  float length=std::sqrt(surfaceframe::Dot(normal,normal));
  if (!std::isfinite(length) || length<.5f) return false;
  normal=normal*(1/length);
  if (surfaceframe::Dot(normal,extent)>0) normal=normal*-1;
  return true;
}

void StopSurfaceClimb(int c,Ledge& entry) {
  Release(entry);
  At<uint16_t>(c,kContactKind)=0;At<float>(c,kContactTime)=0;
  ledgeState(c,nullptr,*reinterpret_cast<int*>(0x11A2E34));
}
void SurfacePose(int c,Ledge& entry,float dt) {
  surfaceframe::Approach(entry.visual.normal,entry.visual.forward,entry.frame.normal,dt,12.0f,6.0f);
  V facing=surfaceframe::Plane(entry.frame.forward,entry.visual.normal);
  if (surfaceframe::Dot(facing,facing)>.0001f) entry.visual.forward=surfaceframe::Unit(facing);
  V axes[3]={surfaceframe::Unit(surfaceframe::Cross(entry.visual.forward,entry.visual.normal)),
      entry.visual.forward,entry.visual.normal};
  float* matrix=reinterpret_cast<float*>(c+4224);
  for (int row=0;row<3;++row) {
    float* v=matrix+row*4;
    float scale=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    if (!std::isfinite(scale) || scale<.001f || scale>100) scale=1;
    v[0]=axes[row].x*scale;v[1]=axes[row].y*scale;v[2]=axes[row].z*scale;
  }
  int resource=At<int>(c,4104);
  float centerHeight=(At<float>(resource,56)+At<float>(resource,60))*At<float>(c,4192)*.5f;
  // Preserve the model center while rotating; +4140 includes predicted displacement.
  auto position=At<V>(c,92)+V{0,centerHeight,0}-entry.visual.forward*centerHeight;
  // Keep the upright collision envelope while seating the horizontal model against the underside or floor.
  float bodyDepth=At<float>(resource,52)*At<float>(c,4192)+.015f;
  float seating=std::max(0.0f,entry.clearance-bodyDepth)*std::fabs(entry.visual.normal.y);
  position=position-entry.visual.normal*seating;
  matrix[12]=position.x;matrix[13]=position.y;matrix[14]=position.z;matrix[15]=1;
}
void SurfaceAnimation(int c,float ascent,float sideways) {
  bool moving=std::fabs(ascent)+std::fabs(sideways)>.01f;
  int anim=At<int16_t>(c,moving?616:614);
  if (moving && anim>=0 && hasAnim(At<int>(c,4100),anim)) {
    float angle=std::atan2(sideways,ascent)/6.283185307f;
    At<float>(c,1184)=angle<0?angle+1:angle;
  } else {
    uintptr_t address=0x11A1B44;
    if (moving) address=std::fabs(ascent)>=std::fabs(sideways)?(ascent>=0?0x11A1B5C:0x11A1B60)
        :(sideways>=0?0x11A1B68:0x11A1B64);
    if (moving || anim<0) anim=*reinterpret_cast<int16_t*>(address);
  }
  if (anim>=0 && hasAnim(At<int>(c,4100),anim) && At<int16_t>(c,1258)!=anim) ledgeAnim(c,nullptr,anim,0);
}

uint32_t& Flags(int type) { return *reinterpret_cast<uint32_t*>(kSurfaceTable + kSurfaceEntry * type); }

void Survey() {
  surveyed = true;
  for (int type = 0; type < kSurfaceTypes; ++type) original[type] = Flags(type);
}

// The grapple's climb onto a roof and the hangs run the same wall-climb logic every frame, and
// with every surface climbable it would take over mid-move, so the patch is lifted for them.
void Patch(bool on) {
  for (int type = 0; type < kSurfaceTypes; ++type) Flags(type) = on ? original[type] | kClimbable : original[type];
}

// Three rays ahead at chest height; the wall counts when the centre one hits.
bool WallAhead(int character) {
  auto position = reinterpret_cast<const float*>(character + kPosition);
  float heading = *reinterpret_cast<const float*>(character + kHeading);
  float fx = sinf(heading), fz = cosf(heading);
  float rx = fz, rz = -fx;
  for (int i = 0; i < 3; ++i) {
    float side = (i - 1) * kProbeSpread;
    float start[3] = {position[0] + rx * side, position[1] + kProbeHeight, position[2] + rz * side};
    float extent[3] = {fx * reach, 0.0f, fz * reach};
    float hit[3];
    if (grapple::CastClimbRay(character, start, extent, hit)) return true;
  }
  return false;
}

}  // namespace

// sub_8F5F60 sizes the whole requested velocity from one speed scalar (the climb clip's own motion
// speed, sub_895430), so scaling the vector it wrote is the same as scaling that scalar. The mantle
// boost it adds on top of a rising climb goes back unscaled, so ledge pull-ups land where they did.
void ScaleNativeMove(int c) {
  V velocity = At<V>(c, kRequested);
  bool mantle = (At<uint8_t>(c, kMoveFlags) & 2) && velocity.y > kMantleBoost;
  if (mantle) velocity.y -= kMantleBoost;
  velocity = velocity * speedScale;
  auto* ledge = CurrentLedge(c);
  V normal = ledge && ledge->active ? ledge->frame.normal : At<V>(c, kClimbNormal);
  if (std::fabs(normal.y) <= kFloorFacing && velocity.y > 0.0f) velocity.y *= 2.0f;
  if (mantle) velocity.y += kMantleBoost;
  At<V>(c, kRequested) = velocity;
}

bool LedgeActive(int c) {
  for (auto& entry:ledges) if (entry.character==c && entry.active) {
    const char* name=playercharacter::ResourceName(c);
    int slot=playercharacter::HumanSlot(c);
    if (!enabled || entry.instance!=At<int>(c,4104) || slot<0 || &entry!=&ledges[slot]
        || !name || !_stricmp(name,"Flash") || At<uint16_t>(c,kState)!=*reinterpret_cast<uint16_t*>(kWallClimbState)) {
      Release(entry);return false;
    }
    return true;
  }
  return false;
}

// Sets up the surface climb on `normal` from an upright pose facing the current heading.
void Attach(int c,Ledge& entry,V normal) {
  float yaw=At<float>(c,kHeading);
  entry.visual={{-sinf(yaw),0,-cosf(yaw)},{0,1,0}};
  entry.frame=entry.visual;
  entry.wallRight=surfaceframe::Cross(entry.frame.forward,entry.frame.normal);
  climbsurface::Turn(entry.frame,normal);
  entry.unsupported=0;entry.active=true;
  entry.ceilingReached=false;
  entry.jumpHeld=ledgeJump(c);
  int resource=At<int>(c,4104);
  float halfHeight=(At<float>(resource,60)-At<float>(resource,56))*At<float>(c,4192)*.5f;
  entry.clearance=std::max(halfHeight,At<float>(c,4212))+.015f;
  entry.previousOwnTransform=(At<uint32_t>(c,132)&0x800000)!=0;
  entry.ownsTransform=true;At<uint32_t>(c,132)|=0x800000;
  At<V>(c,kClimbNormal)=entry.frame.normal;
  At<uint16_t>(c,kContactKind)=kContactWall;At<float>(c,kContactTime)=kHold;
}

bool TryLedge(int c) {
  if (LedgeActive(c)) return true;
  if (!enabled || mode!=kModeClimb || !glide::IsListed(c)) return false;
  const char* name=playercharacter::ResourceName(c);
  if (!name || !_stricmp(name,"Flash")) return false;
  auto* entry=CurrentLedge(c);
  if (!entry || (key && !keybinds::KeyDown(key))) return false;
  uint16_t state=At<uint16_t>(c,kState);
  bool hanging=state==*reinterpret_cast<uint16_t*>(0x11A2E30);
  if (!hanging && state!=*reinterpret_cast<uint16_t*>(kWallClimbState)) return false;
  int input=At<int>(c,1992);
  if (At<float>(input,48)<=.1f || (!hanging && At<float>(c,3764)<=.05f)) return false;
  V normal{};
  if (At<uint8_t>(c,kClimbProbe) && At<float>(c,1092)<-.6f) normal=surfaceframe::Unit(At<V>(c,1088));
  else if (hanging && At<float>(c,1184)>=0 && At<float>(c,1184)<=.2f) normal={0,-1,0};
  else return false;
  if (hanging) {
    ledgeState(c,nullptr,*reinterpret_cast<int*>(kWallClimbState));
    if (At<uint16_t>(c,kState)!=*reinterpret_cast<uint16_t*>(kWallClimbState)) return false;
  }
  Attach(c,*entry,normal);
  return true;
}

bool MoveLedge(int c) {
  if (!LedgeActive(c)) return false;
  auto& entry=*CurrentLedge(c);
  bool jump=ledgeJump(c),pressed=jump && !entry.jumpHeld;
  entry.jumpHeld=jump;
  if (pressed || (key && !keybinds::KeyDown(key))) {
    StopSurfaceClimb(c,entry);return false;
  }
  float dt=std::clamp(static_cast<float>(ledgeTime()),0.0f,0.1f);
  V requested=At<V>(c,3760);
  int input=At<int>(c,1992);
  float ascent=requested.y,sideways=surfaceframe::Dot(requested,entry.wallRight);
  if (At<float>(input,48)<=.1f) ascent=sideways=0;
  float speed=std::sqrt(ascent*ascent+sideways*sideways);
  // The body turns toward its travel and moves head first: on walls W stays up the wall, on floors and
  // ceilings the stick reads against the camera like on foot.
  V n=entry.frame.normal;
  bool flat=std::fabs(n.y)>kFloorFacing;
  V want{};
  if (flat) {
    float yaw=At<int16_t>(c,kMoveAngle)/kTurnsPerRadian;
    // 97B610 adds the stick angle to the camera basis; mirror only its lateral component on undersides.
    if (n.y<-kFloorFacing)
      yaw+=3.14159265f-2.0f*At<int16_t>(input,42)/kTurnsPerRadian;
    want=surfaceframe::Plane(V{std::sin(yaw),0,std::cos(yaw)},n);
    // 8F5F60 adds lateral input perpendicular to this base velocity; its projection recovers the native speed.
    float heading=At<float>(c,kHeading);
    speed=At<float>(input,48)>.1f
        ? std::fabs(surfaceframe::Dot(requested,V{std::sin(heading),0,std::cos(heading)})) : 0;
  } else {
    want=surfaceframe::Unit(surfaceframe::Plane(V{0,1,0},n))*ascent
        +surfaceframe::Unit(surfaceframe::Plane(entry.wallRight,n))*sideways;
  }
  if (speed>.01f && surfaceframe::Dot(want,want)>.0001f) {
    V f=entry.frame.forward,w=surfaceframe::Unit(want),side=surfaceframe::Cross(n,f);
    float angle=std::atan2(surfaceframe::Dot(w,side),surfaceframe::Dot(w,f));
    float step=std::clamp(angle,-kTurnRate*dt,kTurnRate*dt);
    entry.frame.forward=surfaceframe::Unit(f*std::cos(step)+side*std::sin(step));
  }
  ascent=speed;sideways=0;
  auto cast=[&](V a,V b,V& h,V& n) { return LedgeRay(c,a,b,h,n); };
  int resource=At<int>(c,4104);
  float wallClearance=At<float>(resource,52)*At<float>(c,4192)+.02f;
  float clearance=wallClearance+(entry.clearance-wallClearance)*std::fabs(entry.frame.normal.y);
  auto motion=climbsurface::Step(cast,entry.frame,At<V>(c,kPosition),ascent,sideways,dt,clearance);
  entry.unsupported=motion.supported?0:entry.unsupported+dt;
  if (entry.unsupported>.2f) {
    StopSurfaceClimb(c,entry);return false;
  }
  At<V>(c,3760)=motion.velocity;At<V>(c,4112)=motion.velocity;
  At<float>(c,3772)=1;At<float>(c,4124)=1;
  At<V>(c,1124)=entry.frame.normal;
  if (motion.supported) At<V>(c,1136)=motion.contact;
  At<uint16_t>(c,kContactKind)=kContactWall;At<float>(c,kContactTime)=kHold;
  SurfaceAnimation(c,ascent,sideways);
  if (motion.supported && motion.surfaceNormal.y<-.6f && entry.visual.normal.y<-.6f) entry.ceilingReached=true;
  if (entry.ceilingReached && motion.supported && std::fabs(motion.surfaceNormal.y)<.1f && entry.frame.forward.y>.95f
      && surfaceframe::Dot(entry.visual.normal,motion.surfaceNormal)>.995f
      && surfaceframe::Dot(At<V>(c,kPosition)-motion.contact,motion.surfaceNormal)<=wallClearance+.025f) {
    At<float>(c,kHeading)=std::atan2(-motion.surfaceNormal.x,-motion.surfaceNormal.z);
    At<V>(c,1124)=motion.surfaceNormal;
    Release(entry);
    return true;
  }
  if (motion.supported && motion.surfaceNormal.y>kFloorFacing && entry.frame.normal.y>kFloorFacing)
    StopSurfaceClimb(c,entry);
  return true;
}

void Tick(int character) {
  if (LedgeActive(character)) {
    SurfacePose(character,*CurrentLedge(character),std::clamp(static_cast<float>(ledgeTime()),0.0f,0.1f));
  }
  if (surfacerun::Active(character)) return;
  if (!enabled || playercharacter::HumanSlot(character)<0 || !glide::IsListed(character)) return;
  if (!surveyed) Survey();
  bool wanted = (!key || keybinds::KeyDown(key)) && !grapple::Occupied(character);
  auto state = *reinterpret_cast<uint16_t*>(character + kState);
  bool climbing = state == static_cast<uint16_t>(*reinterpret_cast<int*>(kWallClimbState));
  if (grapple::DoorContact(character) || grapple::DoorApproach(character,reach)) {
    Patch(false);
    At<uint16_t>(character,kContactKind)=0;At<float>(character,kContactTime)=0;
    if (climbing) {
      if (auto* entry=CurrentLedge(character)) Release(*entry);
      ledgeState(character,nullptr,*reinterpret_cast<int*>(kJumpState));
    }
    return;
  }
  // The game's own climb latches onto any climbable surface ahead, sloped roofs included.
  if (climbing && !LedgeActive(character) && At<V>(character,kClimbNormal).y>kFloorFacing) {
    At<uint16_t>(character,kContactKind)=0;At<float>(character,kContactTime)=0;
    ledgeState(character,nullptr,*reinterpret_cast<int*>(kJumpState));
    return;
  }
  Patch(wanted || climbing);
  if (mode == kModeClimb && (climbing || wanted) && (climbing || WallAhead(character))) {
    *reinterpret_cast<uint16_t*>(character + kContactKind) = kContactWall;
    *reinterpret_cast<float*>(character + kContactTime) = kHold;
  }
}

void Speed(int percent) {
  speedScale = std::clamp(percent, 25, 400) / 100.0f;
  if (speedScale == 1.0f) return;
  char line[80];
  snprintf(line, sizeof(line), "climb: travel speed at %d%% of the game's own", static_cast<int>(speedScale * 100));
  glideanim::Note(line);
}

void Install(int climbMode, int reachTenths, int scancode) {
  enabled = true;
  mode = climbMode == kModeHop ? kModeHop : kModeClimb;
  if (reachTenths > 0) reach = reachTenths / 10.0f;
  key = static_cast<uint8_t>(scancode);
  glideanim::Note("climb: installed");
}

}  // namespace climb
