#pragma once

namespace grapple {

// Lets the glide-listed characters grapple onto the top edge of any wall in front of them. `range`
// is how far ahead a wall is looked for and `height` how far above the wall hit the roof edge may
// be, both in world units. Needs the exe unpacked.
void Install(int range, int height);

// A collision ray from `start` along `extent` in the character's level, the gizmo floor-probe
// recipe; `hit` gets the first surface point.
bool CastRay(int character, const float* start, const float* extent, float* hit, float* normal = nullptr);

// Door transition collision boxes cannot support a climb or wall run.
bool CastClimbRay(int character, const float* start, const float* extent, float* hit, float* normal = nullptr);
bool DoorContact(int character);
bool DoorApproach(int character, float reach = 1.2f);

// Whether the character is in a grapple, hang, ledge or climb-onto-the-top state.
bool Occupied(int character);

}  // namespace grapple
