#pragma once
#include "surface_frame.h"

namespace climbsurface {
using surfaceframe::V;
using surfaceframe::Dot;
using surfaceframe::Cross;
using surfaceframe::Unit;
using surfaceframe::Plane;
struct Frame { V normal,forward; };
inline void Turn(Frame& frame,V normal) {
  V axis=Cross(frame.normal,normal);
  float cosine=std::clamp(Dot(frame.normal,normal),-1.0f,1.0f);
  if (cosine<-.95f) return;
  frame.forward=Unit(Plane(frame.forward+Cross(axis,frame.forward)
      +Cross(axis,Cross(axis,frame.forward))*(1/(1+cosine)),normal));
  frame.normal=normal;
}
inline V Direction(const Frame& frame,float ascent,float sideways) {
  return frame.forward*ascent+Cross(frame.forward,frame.normal)*sideways;
}
struct Motion { V velocity{},contact{},surfaceNormal{}; bool supported=false; };
template<class Cast> Motion Step(Cast cast,Frame& frame,V center,float ascent,float sideways,float dt,float clearance) {
  Motion result;
  V velocity=Direction(frame,ascent,sideways);
  float speed=std::sqrt(Dot(velocity,velocity));
  V direction=speed>.01f?Unit(velocity):frame.forward,hit{},normal{};
  V probe=center+velocity*dt;
  float closest=clearance+.15f;
  bool compatibleFound=false;
  // Search from the collision center so thin bevels cannot fall between fixed-depth probes.
  for (int degrees=-100;degrees<=100;degrees+=5) {
    float angle=degrees*.01745329252f;
    V ray=(frame.normal*-std::cos(angle)+direction*std::sin(angle))*(clearance+.15f);
    V h{},n{};
    if (!cast(probe,ray,h,n) || Dot(n,frame.normal)<-.2f) continue;
    // Preserve current-surface contacts around an edge even when the straight ray misses.
    bool compatible=Dot(n,frame.normal)>.65f;
    if (compatibleFound && !compatible) continue;
    float distance=std::sqrt(Dot(probe-h,probe-h));
    if (distance<.001f || (distance>=closest && !(compatible && !compatibleFound))) continue;
    compatibleFound=compatible;
    closest=distance;hit=h;normal=n;result.supported=true;
  }
  if (result.supported) {
    V h{},n{};
    if (cast(probe,normal*-(clearance+.15f),h,n) && Dot(n,normal)>.999f) hit=h;
    V radial=Unit(probe-hit);
    if (Dot(radial,normal)>.995f) radial=normal;
    Turn(frame,radial);
    velocity=Direction(frame,ascent,sideways);
    float distance=Dot(center-hit,frame.normal);
    velocity=velocity+frame.normal*std::clamp((clearance-distance)*10.0f,-.5f,.5f);
    result.contact=hit;
    result.surfaceNormal=normal;
  }
  result.velocity=velocity;
  return result;
}
}
