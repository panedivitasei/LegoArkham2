#pragma once

namespace chasecam {

// Each human player has an independent orbit; mouse look belongs to player one.
bool SurfaceDirection(int character, short angle, const float* normal, float* result);
int MouseSensitivity();
void SetMouseSensitivity(int percent);
void SetGamepadSensitivity(int percent);

void Install(bool startEnabled, int distancePercent, bool mouse, int mouseSensitivityPercent, bool invertY,
             bool wallCollision);

}  // namespace chasecam
