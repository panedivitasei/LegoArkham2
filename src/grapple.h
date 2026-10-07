#pragma once
#include <string>

namespace grapple {

void Characters(const std::string& names);
void InstallTicks();

// Enable roof-edge grappling for listed characters; range and height are world units.
void Install(int range, int height, const std::string& folder);

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
