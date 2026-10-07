#include "surface_run.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "flash_roll.h"
#include "glide_anim.h"
#include "chase_camera.h"
#include <cstring>
#include "grapple.h"
#include "hook.h"
#include "player_character.h"
#include "climb.h"
#include "ledge_route.h"
#include "flash_obstacle.h"

namespace surfacerun {
namespace {
constexpr uintptr_t kClimbMove = 0x8F5F60;
// The hang logic (state 76, "Hang"): while in the climb or jump state it takes any downward-facing
// surface the game's own probe finds (normal y < -0.92 at +1092) and switches to the hang, which
// is what pulled Flash off flat walls near ledges. One call site, skipped while he runs.
constexpr uintptr_t kHangLogic = 0xAACEF0;
constexpr uintptr_t kHangLogicSite = 0xB1B9B5;
constexpr uintptr_t kClimbState = 0x11A2E2C;
constexpr uintptr_t kFallState = 0x11A2E34;
constexpr uintptr_t kChangeState = 0x403720;
constexpr uintptr_t kMayClimb = 0x8F5DA0;
constexpr uintptr_t kFrameTime = 0x8B4120;
constexpr uintptr_t kJumpPressed = 0x886D00;
constexpr uintptr_t kFindEntry = 0x651E40;
constexpr uintptr_t kAnimId = 0x945980;
constexpr uintptr_t kRequest = 0x4D7B30;
constexpr uint32_t kOwnTransform=0x800000;
using LogicFn = void(__cdecl*)(int);
using MoveFn = int(__cdecl*)(int, int16_t, int, int, float*);
using StateFn = int(__fastcall*)(int, void*, int);
using TestFn = bool(__cdecl*)(int);
using TimeFn = double(__cdecl*)();
using IdFn = int(__cdecl*)(const char*);
using FindFn = int(__fastcall*)(int, void*, int, int);
using RequestFn = bool(__fastcall*)(int, void*, int, int);

template<class T> T& At(int p, int off) { return *reinterpret_cast<T*>(p + off); }
struct Vec { float x, y, z; };
Vec operator+(Vec a, Vec b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec operator*(Vec a, float b) { return {a.x*b,a.y*b,a.z*b}; }
float Dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec Cross(Vec a, Vec b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
float Length(Vec a) { return sqrtf(Dot(a,a)); }
Vec Unit(Vec a) { float n=Length(a); return n>0.0001f ? a*(1.0f/n) : Vec{0,0,0}; }
Vec Plane(Vec a, Vec n) { return a-n*Dot(a,n); }
struct Runner {
  ledgeroute::Route ledge;
  float ledgeProbe=0;
  float attachHeight=0,entrySpeed=0;
  int character=0, instance=0, renderComponent=0, anim=-1;
  bool active=false, ownsTransform=false, previousOwnTransform=false;
  float cooldown=0, idle=0, unsupported=0, bypass=0, clearance=0.12f;
  Vec normal{0,1,0}, forward{0,0,1}, facing{0,0,1};
};
std::vector<Runner> runners;
bool enabled=false;
float speed=7.5f;

// The resource name precedes GameObject's duplicate-name suffixing.
bool IsFlashName(int c) {
  if (!enabled) return false;
  const char* name=playercharacter::ResourceName(c);
  return name && _stricmp(name,"Flash")==0;
}
bool IsFlash(int c) { return IsFlashName(c) && playercharacter::HumanSlot(c)>=0; }
void ReleaseTransform(int c,Runner& r) {
  if (r.ownsTransform && !r.previousOwnTransform) At<uint32_t>(c,132)&=~kOwnTransform;
  r.ownsTransform=false;
}
Runner& Get(int c) {
  for (auto& r:runners) if (r.character==c) {
    if (r.instance!=At<int>(c,4104) || r.renderComponent!=At<int>(c,424)) {
      ReleaseTransform(c,r); r=Runner{};
    }
    r.character=c; r.instance=At<int>(c,4104); r.renderComponent=At<int>(c,424);
    return r;
  }
  runners.push_back(Runner{});
  auto& r=runners.back(); r.character=c; r.instance=At<int>(c,4104); r.renderComponent=At<int>(c,424);
  return r;
}
Runner* Find(int c) {
  for (auto& r:runners) if (r.character==c && r.instance==At<int>(c,4104) && r.renderComponent==At<int>(c,424)) return &r;
  return nullptr;
}
bool InClimb(int c) { return At<uint16_t>(c,1270)==*reinterpret_cast<uint16_t*>(kClimbState); }

bool Ray(int c, Vec start, Vec extent, Vec& hit, Vec& normal) {
  if (!grapple::CastClimbRay(c,&start.x,&extent.x,&hit.x,&normal.x)) return false;
  float length=Length(normal);
  if (!std::isfinite(length) || length<0.5f || length>1.5f) return false;
  normal=normal*(1.0f/length);
  if (Dot(normal,extent)>0) normal=normal*-1.0f;
  return true;
}

void Orient(int c, Vec up, Vec forward) {
  // The body faces down the negative of its third row (v9 ran up walls facing the ground), so the
  // travel direction goes in negated; the right axis follows so the frame stays right-handed.
  forward=forward*-1.0f;
  Vec right=Unit(Cross(up,forward));
  forward=Unit(Cross(right,up));
  Vec axes[3]={right,up,forward};
  // The dive tilt visibly pitches the body through the two matrices on the object at +3500 (rows
  // right, up, forward), so the frame goes there first, before anything that could bail out.
  int body=At<int>(c,3500);
  if (body) {
    for (int offset : {64,128}) {
      float* m=reinterpret_cast<float*>(body+offset);
      for (int row=0;row<3;++row) { m[row*4]=axes[row].x; m[row*4+1]=axes[row].y; m[row*4+2]=axes[row].z; }
    }
  }
  // The per-frame pass sub_A2BFA0 copies the character's own matrix at +4224 into the GameObject
  // after every update, so that matrix is the one the renderer ends up with; rows keep their scale.
  {
    float* source=reinterpret_cast<float*>(c+4224);
    for (int row=0;row<3;++row) {
      float* v=source+row*4;
      float scale=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
      if (!std::isfinite(scale) || scale<0.001f || scale>100.0f) scale=1.0f;
      v[0]=axes[row].x*scale; v[1]=axes[row].y*scale; v[2]=axes[row].z*scale;
    }
    // The skipped rebuild also carried the position, so the translation row is kept current here.
    Vec position=At<Vec>(c,92);
    source[12]=position.x; source[13]=position.y; source[14]=position.z; source[15]=1.0f;
  }
  alignas(16) float matrix[16];
  std::memcpy(matrix,reinterpret_cast<const void*>(c+336),sizeof(matrix));
  for (int row=0;row<3;++row) {
    float* v=matrix+row*4;
    float scale=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    if (!std::isfinite(scale) || scale<0.001f || scale>100.0f) return;
    v[0]=axes[row].x*scale; v[1]=axes[row].y*scale; v[2]=axes[row].z*scale;
  }
  // MagnetWalkAddon uses the GameObject setter to update the render hierarchy.
  using MatrixFn=int(__fastcall*)(int,void*,const float*);
  reinterpret_cast<MatrixFn>(0x692A90)(c+16,nullptr,matrix);
}

void Detach(int c, Runner& r, bool jump) {
  r.ledge.next=3;
  r.active=false; r.cooldown=0.35f;
  ReleaseTransform(c,r);
  At<uint16_t>(c,3692)=0; At<float>(c,3696)=0;
  reinterpret_cast<StateFn>(kChangeState)(c,nullptr,*reinterpret_cast<int*>(kFallState));
  At<uint8_t>(c,140)=0;
  At<float>(c,1188)=0;
  Vec velocity=r.facing*(speed*0.5f)+r.normal*(jump?3.0f:0.5f);
  At<Vec>(c,4112)=velocity; At<Vec>(c,3760)=velocity;
  int fall=At<int16_t>(c,610);
  if (fall>=0) reinterpret_cast<RequestFn>(kRequest)(c,nullptr,fall,0);
  float yaw=At<float>(c,436);
  Orient(c,{0,1,0},{sinf(yaw),0,cosf(yaw)});
}

float RoofClearance(int c) {
  int resource=At<int>(c,4104);
  return (At<float>(resource,60)-At<float>(resource,56))*At<float>(c,4192)*.5f+.02f;
}

bool RoofReady(int c,Vec normal,Vec surfaceNormal,Vec hit) {
  float bottom=At<float>(c,96)+At<float>(At<int>(c,4104),56)*At<float>(c,4192);
  return surfaceNormal.y>.8f && normal.y>.95f && bottom>=hit.y-.01f;
}

void FinishRoof(int c,Runner& r,Vec direction,float amount) {
  Vec forward=Unit(Plane(direction,{0,1,0}));
  if (Dot(forward,forward)<.5f) forward=Unit(Plane(r.forward,{0,1,0}));
  At<float>(c,436)=atan2f(forward.x,forward.z);
  Detach(c,r,false);
  At<Vec>(c,4112)=forward*(amount*flashroll::Speed(c,speed));At<Vec>(c,3760)=At<Vec>(c,4112);
}

Vec ObstacleLaunch(Vec direction,Vec normal,float momentum) {
  return Unit(direction*.8f-normal*.6f)*momentum;
}

void LaunchObstacle(int c,Runner& r,Vec direction,float amount) {
  Vec velocity=ObstacleLaunch(direction,r.normal,std::max(speed*amount,r.entrySpeed));
  Detach(c,r,false);
  At<Vec>(c,4112)=velocity;At<Vec>(c,3760)=velocity;
}

int RunAnim(int c) {
  int holder=At<int>(c,4100), set=holder?At<int>(holder,4):0;
  if (!set) return -1;
  for (const char* name:{"sprint","run"}) {
    int id=reinterpret_cast<IdFn>(kAnimId)(name);
    if (id>=0 && reinterpret_cast<FindFn>(kFindEntry)(set,nullptr,id,0)) return id;
  }
  return -1;
}

void ChangeNormal(Runner& r, Vec n) {
  bool wallToWall=std::fabs(r.normal.y)<.65f && std::fabs(n.y)<.65f;
  Vec axis=Cross(r.normal,n);
  float cosine=std::clamp(Dot(r.normal,n),-1.0f,1.0f);
  auto transport=[&](Vec v) {
    return cosine>-0.95f ? Unit(v+Cross(axis,v)+Cross(axis,Cross(axis,v))*(1.0f/(1.0f+cosine))) : v;
  };
  r.forward=transport(r.forward); r.facing=transport(r.facing); r.normal=n;
  r.forward=Unit(Plane(r.forward,n)); r.facing=Unit(Plane(r.facing,n));
  if (wallToWall) {
    Vec up=Unit(Plane({0,1,0},n));
    r.forward=Dot(r.forward,up)>=0?up:up*-1;
  }
}

void __cdecl OnLogic(int c) {
  if (!IsFlashName(c)) {
    for (auto& r:runners) if (r.character==c) { ReleaseTransform(c,r); r.active=false; }
    if (!climb::LedgeActive(c) && !(playercharacter::HumanSlot(c)>=0
        && (grapple::DoorContact(c) || grapple::DoorApproach(c))))
      climb::NativeLogic(c);
    return;
  }
  auto& r=Get(c);
  if (!IsFlash(c)) {
    if (r.active && InClimb(c)) Detach(c,r,false);
    else { ReleaseTransform(c,r); r.active=false; }
    return;
  }
  float dt=std::clamp(static_cast<float>(reinterpret_cast<TimeFn>(kFrameTime)()),0.0f,0.1f);
  r.cooldown=std::max(0.0f,r.cooldown-dt);
  if (r.active && !InClimb(c)) {
    r.active=false; r.cooldown=0.35f;
    ReleaseTransform(c,r);
  }
  if (grapple::DoorContact(c) || grapple::DoorApproach(c)) {
    if (r.active) Detach(c,r,false);
    return;
  }
  if (r.active) {
    if (!flashroll::Active(c) && reinterpret_cast<TestFn>(kJumpPressed)(c)) { Detach(c,r,true); return; }
    At<uint16_t>(c,3692)=20; At<float>(c,3696)=0.3f;
    return;
  }
  if (r.cooldown>0 || grapple::Occupied(c)) return;
  int input=At<int>(c,1992);
  bool pushing=input && At<float>(input,48)>0.1f;
  if (!InClimb(c) && !flashroll::Active(c) && !reinterpret_cast<TestFn>(kMayClimb)(c)) return;
  if (!pushing) return;
  float yaw=At<float>(c,436);
  Vec ahead{sinf(yaw),0,cosf(yaw)}, hit{}, normal{};
  Vec position=At<Vec>(c,4140);
  if (!Ray(c,position+Vec{0,0.25f,0},ahead*1.2f,hit,normal)) return;
  if (fabsf(normal.y)>0.2f || Dot(ahead,normal)>-0.6f) return;
  // Require a tall wall before attaching; low props must never enter the launch path.
  Vec upperHit{},upperNormal{};
  float height=2.0f*(RoofClearance(c)-.02f);
  if (!Ray(c,position+Vec{0,std::max(1.2f,2.0f*height),0},ahead*1.2f,upperHit,upperNormal)
      || fabsf(upperNormal.y)>.2f || Dot(normal,upperNormal)<.95f
      || std::fabs(Dot(upperHit-hit,normal))>.15f) return;
  r.anim=RunAnim(c);
  if (r.anim<0) return;
  r.attachHeight=position.y;
  r.entrySpeed=std::clamp(Length(At<Vec>(c,4112)),speed,speed*2);
  reinterpret_cast<StateFn>(kChangeState)(c,nullptr,*reinterpret_cast<int*>(kClimbState));
  if (!InClimb(c)) return;
  r.normal=normal; r.forward=Unit(Plane({0,1,0},normal)); r.facing=r.forward;
  r.idle=0; r.unsupported=0; r.bypass=0; r.active=true; r.ledge={}; r.ledgeProbe=0;
  At<uint16_t>(c,3692)=20; At<float>(c,3696)=0.3f;
  At<Vec>(c,1124)=normal; At<Vec>(c,1136)=hit;
}

// Flash never hangs: the hang logic is skipped for him whether or not a run is active.
void __cdecl OnHang(int c) {
  if (IsFlashName(c)) return;
  if (climb::TryLedge(c)) return;
  reinterpret_cast<LogicFn>(kHangLogic)(c);
}

int __cdecl OnMove(int c,int16_t angle,int a3,int a4,float* limits) {
  auto* r=Find(c);
  if (!r || !r->active || !IsFlash(c)) {
    int result=reinterpret_cast<MoveFn>(kClimbMove)(c,angle,a3,a4,limits);
    climb::ScaleNativeMove(c);
    climb::MoveLedge(c);
    return result;
  }
  float moveSpeed=flashroll::Speed(c,speed);
  bool rolling=flashroll::Active(c);
  float dt=std::clamp(static_cast<float>(reinterpret_cast<TimeFn>(kFrameTime)()),0.0f,0.1f);
  int input=At<int>(c,1992);
  float amount=input?std::clamp(At<float>(input,48),0.0f,1.0f):0;
  r->idle=amount>0.1f || rolling?0:r->idle+dt;
  if (r->idle>0.2f) { Detach(c,*r,false); return 7; }
  int16_t rawAngle=input?At<int16_t>(input,42):0;
  float relative=rawAngle*(6.283185307f/65536.0f);
  Vec right=Unit(Cross(r->normal,r->forward));
  Vec direction=Unit(r->forward*cosf(relative)+right*sinf(relative));
  Vec cameraDirection{};
  if (chasecam::SurfaceDirection(c,rawAngle,&r->normal.x,&cameraDirection.x)) {
    direction=cameraDirection;
    relative=atan2f(Dot(direction,right),Dot(direction,r->forward));
  }
  if (rolling) {
    if (amount<=.1f) direction=r->facing;
    relative=atan2f(Dot(direction,right),Dot(direction,r->forward));
    amount=1.0f;
  }
  Vec position=At<Vec>(c,4140),hit{},normal{};
  if (amount>.1f && At<uint8_t>(c,3968) && std::fabs(r->normal.y)<.65f && direction.y>.4f) {
    int resource=At<int>(c,4104);
    float centerHeight=(At<float>(resource,60)+At<float>(resource,56))*At<float>(c,4192)*.5f;
    Vec center=At<Vec>(c,92)+Vec{0,centerHeight,0};
    Vec contact=At<Vec>(c,1076),underside=Unit(At<Vec>(c,1088));
    if (flashobstacle::BlockingUnderside({center.x,center.y,center.z},{direction.x,direction.y,direction.z},
        {contact.x,contact.y,contact.z},{underside.x,underside.y,underside.z},RoofClearance(c)-.02f)) {
      ChangeNormal(*r,underside);r->bypass=0;r->ledge.next=3;
      right=Unit(Cross(r->normal,r->forward));
      direction=Unit(r->forward*cosf(relative)+right*sinf(relative));
    }
  }
  auto convert=[](Vec v) { return ledgeroute::V{v.x,v.y,v.z}; };
  auto cast=[&](ledgeroute::V a,ledgeroute::V b,ledgeroute::V& h,ledgeroute::V& n) {
    Vec hit{},normal{};
    bool found=Ray(c,{a.x,a.y,a.z},{b.x,b.y,b.z},hit,normal);
    h=convert(hit); n=convert(normal); return found;
  };
  if (!rolling && amount>.1f && direction.y>.4f && std::fabs(r->normal.y)<.65f
      && position.y-r->attachHeight<1.2f) {
    ledgeroute::V top{};
    if (flashobstacle::LowTop(cast,convert(position),convert(r->normal),RoofClearance(c)-.02f,top)) {
      LaunchObstacle(c,*r,direction,amount);return 7;
    }
  }
  r->ledgeProbe=std::max(0.0f,r->ledgeProbe-dt);
  bool straightAscent=std::cos(relative)>.8f && std::fabs(std::sin(relative))<.2f;
  if (!straightAscent) r->ledge.next=3;
  if (!r->ledge.Active() && r->ledgeProbe<=0 && amount>.1f && straightAscent && fabsf(r->normal.y)<.2f && direction.y>.4f) {
    r->ledgeProbe=.08f;
    if (ledgeroute::Find(cast,convert(position),convert(r->normal),1.4f,r->ledge) && r->ledge.roof) {
      // Include the route's 0.08 completion tolerance above the physical bottom.
      float lift=RoofClearance(c);
      r->ledge.points[1].y+=lift;r->ledge.points[2].y+=lift;
    }
  }
  if (r->ledge.Active()) {
    auto v=ledgeroute::Velocity(r->ledge,convert(position),amount*moveSpeed,dt);
    At<Vec>(c,3760)={v.x,v.y,v.z}; At<Vec>(c,4112)={v.x,v.y,v.z};
    At<float>(c,3772)=1; At<float>(c,4124)=1;
    r->unsupported=0;
    if (!r->ledge.Active()) {
      if (r->ledge.failed) { Detach(c,*r,false); return 7; }
      auto n=r->ledge.normal; ChangeNormal(*r,{n.x,n.y,n.z});
      auto h=r->ledge.contact; At<Vec>(c,1124)=r->normal; At<Vec>(c,1136)={h.x,h.y,h.z};
      if (r->ledge.roof) FinishRoof(c,*r,r->forward,amount);
    }
    return 7;
  }
  float look=std::max(0.3f,moveSpeed*dt+0.15f);
  r->bypass=std::max(0.0f,r->bypass-dt);
  // Clear shallow trim outward before committing to a ceiling normal.
  if (amount>0.1f && Ray(c,position+r->normal*0.2f,direction*look,hit,normal)
      && Dot(normal,r->normal)<0.8f && Dot(normal,r->normal)>-0.9f) {
    bool trim=r->bypass>0;
    if (!trim && fabsf(r->normal.y)<0.65f && direction.y>0.4f) {
      for (float clearance : {0.45f,0.75f,1.05f}) {
        Vec start=position+r->normal*clearance, blocked{}, obstacleNormal{};
        Vec upper=start+direction*(look+0.8f), wall{}, wallNormal{};
        if (!Ray(c,start,direction*(look+0.8f),blocked,obstacleNormal)
            && Ray(c,upper,r->normal*-2.0f,wall,wallNormal)
            && Dot(wallNormal,r->normal)>0.9f) {
          r->clearance=flashobstacle::TrimDistance(convert(start),convert(wall),convert(r->normal));
          r->bypass=0.35f; trim=true;
          break;
        }
      }
    }
    if (!trim) {
      Runner candidate=*r;
      ChangeNormal(candidate,normal);
      Vec along=Unit(candidate.forward*cosf(relative)+Cross(candidate.normal,candidate.forward)*sinf(relative));
      Vec farHit{},farNormal{};
      if (Ray(c,hit+along*1.2f+normal*0.25f,normal*-0.8f,farHit,farNormal)
          && Dot(farNormal,normal)>0.9f) {
        ChangeNormal(*r,normal);
        right=Unit(Cross(r->normal,r->forward));
        direction=Unit(r->forward*cosf(relative)+right*sinf(relative));
      }
    }
  }
  Vec probe=position+direction*(amount*moveSpeed*dt)+r->normal*0.25f;
  bool supported=Ray(c,probe,r->normal*-2.0f,hit,normal) && Dot(normal,r->normal)>0.6f;
  // Search around an outside corner before falling back to the surface behind the feet.
  ledgeroute::V cornerHit{},cornerNormal{};
  if (!supported && amount>.1f
      && ledgeroute::Wrap(cast,convert(probe),convert(r->normal),convert(direction),look,cornerHit,cornerNormal)) {
    hit={cornerHit.x,cornerHit.y,cornerHit.z}; normal={cornerNormal.x,cornerNormal.y,cornerNormal.z};
    ChangeNormal(*r,normal);
    right=Unit(Cross(r->normal,r->forward));
    direction=Unit(r->forward*cosf(relative)+right*sinf(relative));
    supported=true;
  }
  if (!supported) {
    supported=Ray(c,position+r->normal*0.25f,r->normal*-2.0f,hit,normal)
        && Dot(normal,r->normal)>0.6f;
  }
  r->unsupported=supported?0:r->unsupported+dt;
  if (r->unsupported>0.15f) { Detach(c,*r,false); return 7; }
  if (supported && Dot(normal,r->normal)<0.995f) ChangeNormal(*r,Unit(r->normal*0.8f+normal*0.2f));
  right=Unit(Cross(r->normal,r->forward));
  direction=Unit(r->forward*cosf(relative)+right*sinf(relative));
  if (amount>0.1f) r->facing=direction;
  if (supported && RoofReady(c,r->normal,normal,hit)) {
    FinishRoof(c,*r,direction,amount);return 7;
  }
  float distance=supported?Dot(position-hit,r->normal):0.12f;
  float desired=r->bypass>0?r->clearance:0.12f;
  if (supported && (normal.y>.8f || normal.y<-.6f)) desired=std::max(desired,RoofClearance(c));
  float correction=supported?std::clamp((desired-distance)*12.0f,-3.0f,5.0f):0;
  Vec velocity=direction*(amount*moveSpeed)+r->normal*correction;
  At<Vec>(c,3760)=velocity; At<Vec>(c,4112)=velocity;
  At<float>(c,3772)=1; At<float>(c,4124)=1;
  At<Vec>(c,1124)=r->normal; if (supported) At<Vec>(c,1136)=hit;
  return 7;
}
}

bool Active(int c) { auto* r=Find(c); return enabled && r && r->active && InClimb(c) && IsFlash(c); }
bool Frame(int c,float* normal,float* forward) {
  if (!Active(c)) return false;
  auto* r=Find(c);
  std::memcpy(normal,&r->normal,sizeof(Vec));
  std::memcpy(forward,&r->forward,sizeof(Vec));
  return true;
}
void BeforeSync(int c) {
  if (!Active(c)) return;
  auto* r=Find(c);
  int anim=flashroll::WallAnim(c,r->anim);
  if (At<int16_t>(c,1258)!=anim) reinterpret_cast<RequestFn>(kRequest)(c,nullptr,anim,0);
}
void AfterSync(int c) {
  if (!Active(c)) return;
  auto* r=Find(c);
  if (!r->ownsTransform) {
    r->previousOwnTransform=(At<uint32_t>(c,132)&kOwnTransform)!=0;
    r->ownsTransform=true;
  }
  At<uint32_t>(c,132)|=kOwnTransform;
  Orient(c,r->normal,r->facing);
}
void Install(int speedTenths) {
  speed=std::clamp(speedTenths/10.0f,1.0f,15.0f);
  uintptr_t target=reinterpret_cast<uintptr_t>(&OnMove);
  bool move=hook::Patch(0xAC07BB,{0x60,0x5F,0x8F,0},
      {static_cast<BYTE>(target),static_cast<BYTE>(target>>8),static_cast<BYTE>(target>>16),static_cast<BYTE>(target>>24)});
  bool first=hook::Call(0xB1BA5A,climb::LogicTarget(),reinterpret_cast<void*>(&OnLogic));
  bool second=hook::Call(0xB1DA1A,climb::LogicTarget(),reinterpret_cast<void*>(&OnLogic));
  bool hang=hook::Call(kHangLogicSite,reinterpret_cast<void*>(kHangLogic),reinterpret_cast<void*>(&OnHang));
  enabled=move && first && second && hang;
  if (!enabled) glideanim::Note("surface run: hook mismatch; disabled");
}
}
