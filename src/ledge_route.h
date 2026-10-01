#pragma once
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace ledgeroute {
struct V { float x=0,y=0,z=0; };
inline V operator+(V a,V b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline V operator-(V a,V b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline V operator*(V a,float s) { return {a.x*s,a.y*s,a.z*s}; }
inline float Dot(V a,V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline float Length(V a) { return std::sqrt(Dot(a,a)); }
inline V Unit(V a) { float n=Length(a); return n>0.0001f?a*(1/n):V{}; }
struct Route {
  V points[3]{},normal{},contact{};
  int next=3;
  float elapsed=0,stalled=0,lastDistance=1000;
  bool roof=false,failed=false;
  bool Active() const { return next<3; }
};

template<class Cast> bool Wrap(Cast cast,V probe,V up,V forward,float look,V& hit,V& normal) {
  for (float depth:{0.45f,0.65f,0.9f,1.3f}) {
    if (cast(probe-up*depth,forward*-(look+1.0f),hit,normal)
        && Dot(normal,up)>-.1f && Dot(normal,up)<.6f && Dot(normal,forward)>.5f) return true;
  }
  return false;
}

// Sweep a body envelope along each segment; offsets leave the supporting wall outside the sweep.
template<class Cast> bool Clear(Cast cast,V from,V to,V outward,float height) {
  V side{-outward.z,0,outward.x},hit{},normal{};
  for (float y:{0.08f,height*0.5f,std::max(0.08f,height-0.08f)}) {
    for (float width:{-0.3f,0.0f,0.3f}) {
      V offset=V{0,y,0}+side*width+outward*0.08f;
      if (cast(from+offset,to-from,hit,normal)) return false;
    }
  }
  return true;
}

template<class Cast> bool FindFromContact(Cast cast,V feet,V outward,float height,V lip,Route& route) {
  if (std::fabs(outward.y)>0.2f || height<0.3f || height>3.0f) return false;
  outward=Unit(outward);
  float firstRise=std::max(0.5f,lip.y-feet.y+0.25f);
  for (float rise=firstRise;rise<=firstRise+2.0f;rise+=0.4f) {
    for (float clearance:{0.65f,1.0f,1.4f,1.9f,2.5f}) {
      V outside=feet+outward*clearance,above=outside+V{0,rise,0};
      V wall{},wn{},target{}; bool roof=false;
      if (cast(above,outward*-(clearance+0.8f),wall,wn) && Dot(wn,outward)>.8f) {
        if (Dot(wall-feet,outward)>0.0f) continue;
        target=wall+outward*0.16f;
      } else {
        V top=feet-outward*0.55f+V{0,rise,0};
        if (!cast(top,V{0,-(rise+0.1f),0},wall,wn) || wn.y<.8f || wall.y<lip.y) continue;
        target=wall+V{0,0.08f,0}; roof=true;
        above.y=target.y;
      }
      if (!Clear(cast,feet,outside,outward,height) || !Clear(cast,outside,above,outward,height)
          || !Clear(cast,above,target,outward,height)) continue;
      route={}; route.points[0]=outside; route.points[1]=above; route.points[2]=target;
      route.normal=wn; route.contact=wall; route.roof=roof; route.next=0;
      return true;
    }
  }
  return false;
}

template<class Cast> bool Find(Cast cast,V feet,V outward,float height,Route& route) {
  if (std::fabs(outward.y)>0.2f || height<0.3f || height>3.0f) return false;
  outward=Unit(outward);
  V lip{},normal{};
  if (!cast(feet+outward*0.08f+V{0,0.1f,0},V{0,height+0.7f,0},lip,normal)
      || normal.y>-.6f) return false;
  return FindFromContact(cast,feet,outward,height,lip,route);
}

inline V Velocity(Route& route,V feet,float speed,float dt) {
  if (!route.Active() || dt<=0) return {};
  route.elapsed+=dt;
  float distance=Length(route.points[route.next]-feet);
  if (distance<0.08f) {
    ++route.next; route.stalled=0; route.lastDistance=1000;
    if (!route.Active()) return {};
    distance=Length(route.points[route.next]-feet);
  }
  route.stalled=distance<route.lastDistance-0.002f?0:route.stalled+dt;
  route.lastDistance=distance;
  if (route.elapsed>5 || route.stalled>.6f) { route.failed=true; route.next=3; return {}; }
  return Unit(route.points[route.next]-feet)*std::min(speed,distance/dt);
}
}
