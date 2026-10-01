#pragma once
#include <algorithm>
#include <cmath>

namespace surfaceframe {
struct V { float x,y,z; };
inline V operator+(V a,V b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline V operator-(V a,V b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline V operator*(V a,float b) { return {a.x*b,a.y*b,a.z*b}; }
inline float Dot(V a,V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline V Cross(V a,V b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline V Unit(V v) { float n=std::sqrt(Dot(v,v)); return n>1e-6f?v*(1/n):V{0,0,0}; }
inline V Plane(V v,V n) { return v-n*Dot(v,n); }
inline V Rotate(V v,V axis,float angle) {
  float c=std::cos(angle),s=std::sin(angle);
  return v*c+Cross(axis,v)*s+axis*(Dot(axis,v)*(1-c));
}
inline void Approach(V& up,V& forward,V target,float dt,float response=8.0f,float maxRate=100.0f) {
  float cosine=std::clamp(Dot(up,target),-1.0f,1.0f);
  if (cosine>0.999999f) return;
  V axis=Unit(Cross(up,target));
  if (Dot(axis,axis)<0.5f) axis=Unit(Cross(up,forward));
  float angle=std::min(std::acos(cosine)*(1.0f-std::exp(-response*dt)),maxRate*dt);
  up=Unit(Rotate(up,axis,angle));
  forward=Unit(Plane(Rotate(forward,axis,angle),up));
}
inline V CameraUp(V normal) {
  // Limit camera lean to 20 degrees; the character retains full surface alignment.
  constexpr float lean=0.34906585f;
  V horizontal=Unit({normal.x,0,normal.z});
  if (Dot(horizontal,horizontal)<0.5f || normal.y<-0.7f) return {0,1,0};
  float angle=std::min(std::acos(std::clamp(normal.y,-1.0f,1.0f)),lean);
  return horizontal*std::sin(angle)+V{0,std::cos(angle),0};
}
inline float EaseRoll(float current,float target,float dt) {
  target=std::clamp(target,-10.0f,10.0f);
  float step=(target-current)*(1.0f-std::exp(-2.0f*dt));
  return current+std::clamp(step,-12.0f*dt,12.0f*dt);
}
inline V View(V up,V base,float yaw,float pitch) {
  V right=Unit(Cross(up,base));
  return (base*std::cos(yaw)+right*std::sin(yaw))*std::cos(pitch)-up*std::sin(pitch);
}
inline float Roll(V forward,V up) {
  V right=Unit(Cross({0,1,0},forward));
  V worldUp=Unit(Cross(forward,right));
  V desired=Unit(Plane(up,forward));
  return std::atan2(-Dot(desired,right),Dot(desired,worldUp));
}
}
