#pragma once

#include <string>

namespace abilitycontrols {
enum Action { Grapple, Dive, Count };
constexpr int kFirstEntry = 24;
void Install(const std::string& path);
bool Down(int character, Action action);
bool GrapplePressed(int character);
bool CameraToggleDown();
}
