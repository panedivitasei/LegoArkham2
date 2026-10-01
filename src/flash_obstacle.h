#pragma once
#include "ledge_route.h"

namespace flashobstacle {
using ledgeroute::V;
inline bool BlockingUnderside(V center,V direction,V hit,V normal,float halfHeight) {
  float distance=ledgeroute::Dot(center-hit,normal);
  return normal.y<-.6f && ledgeroute::Dot(direction,normal)<-.25f
      && distance>=0 && distance<=halfHeight+.1f
      && ledgeroute::Length(center-hit)<=halfHeight+.2f;
}
template<class Cast> bool LowTop(Cast cast,V center,V outward,float halfHeight,V& top) {
  V hit{},normal{};
  float rise=halfHeight+.15f;
  if (cast(center+outward*.2f+V{0,rise,0},outward*-.8f,hit,normal)
      && ledgeroute::Dot(normal,outward)>.6f) return false;
  // Sample narrow parapets before the body reaches the corner's collision limit.
  for (float inset:{.14f,.20f,.28f,.40f,.55f}) {
    if (cast(center-outward*inset+V{0,rise,0},{0,-(rise+.1f),0},hit,normal)
        && normal.y>.8f && hit.y>=center.y-.1f && hit.y<=center.y+halfHeight+.1f) {
      top=hit;return true;
    }
  }
  return false;
}
inline float TrimDistance(V testedStart,V upperWall,V normal) {
  return ledgeroute::Dot(testedStart-upperWall,normal);
}
}
