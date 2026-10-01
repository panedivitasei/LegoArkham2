#include "flash_jump.h"

#include <windows.h>

#include <cmath>
#include <cstdint>

#include "input_frame.h"
#include "player_character.h"

namespace flashjump {
namespace {

// Flash's jump launches through a path that isn't a direct call site, so the takeoff is caught on the
// next frame instead: vertical velocity +4116 still holds exactly the definition's jump_speed there.
constexpr uintptr_t kParty = 0x138F4E0;  // one character per human slot
constexpr int kResource = 4104;
constexpr int kDefinition = 44;  // on the resource
constexpr int kRunSpeed = 36;    // on the definition; sprint_speed is +40
constexpr int kJumpSpeed = 64;   // on the definition
constexpr int kVelocity = 4112;  // x, y, z
// Ground speed passes run_speed only while the sprint ramp (sub_B02FD0) targets sprint_speed.
constexpr float kSprintMargin = 1.05f;

float launchScale = 1.0f;
float lastVy[2];

template <class T>
T& At(int base, int offset) {
  return *reinterpret_cast<T*>(base + offset);
}

void Update() {
  auto party = reinterpret_cast<const int*>(kParty);
  for (int slot = 0; slot < 2; ++slot) {
    int c = party[slot];
    const char* name = c ? playercharacter::ResourceName(c) : nullptr;
    if (!name || _stricmp(name, "Flash") || playercharacter::HumanSlot(c) != slot) continue;
    float& vy = At<float>(c, kVelocity + 4);
    float before = lastVy[slot];
    lastVy[slot] = vy;
    int definition = At<int>(At<int>(c, kResource), kDefinition);
    if (!definition || before > 0.5f || vy != At<float>(definition, kJumpSpeed)) continue;
    float vx = At<float>(c, kVelocity), vz = At<float>(c, kVelocity + 8);
    float run = At<float>(definition, kRunSpeed) * kSprintMargin;
    if (vx * vx + vz * vz > run * run) lastVy[slot] = vy *= launchScale;
  }
}

}  // namespace

void Install(int heightPercent) {
  if (heightPercent == 100) return;
  // Jump height grows with the square of launch speed.
  launchScale = std::sqrt(heightPercent / 100.0f);
  inputframe::OnFrame(Update);
}

}  // namespace flashjump
