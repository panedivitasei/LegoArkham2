#include "ability_controls.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

#include "ability_binding.h"
#include "hook.h"
#include "input_frame.h"
#include "player_character.h"

namespace abilitycontrols {
namespace {
constexpr uintptr_t kInputPtr = 0x13640E4;
constexpr uintptr_t kLoadProfile = 0x636980;
constexpr uintptr_t kLoadSites[] = {0x6387BF, 0x63A207, 0x63A481, 0x63B4DB, 0x63B566, 0x63B5EC,
                                   0x63B663, 0x63B6E5, 0x63B83C, 0x63C097, 0x63D662, 0x63D6B6};
constexpr uintptr_t kBind = 0x638570;
constexpr uintptr_t kPoll = 0x636F20;
constexpr uintptr_t kReset = 0x6387A0;
constexpr uintptr_t kFormat = 0x6387F0;
constexpr int kPlayerBindings = 25444, kPlayerSlots = 25956;
constexpr const char* kSections[2][2] = {{"Player1Gamepad", "Player1Keyboard"},
                                        {"Player2Gamepad", "Player2Keyboard"}};
constexpr const char* kNames[Count] = {"Grapple", "Dive"};

using LoadFn = char(__thiscall*)(uintptr_t, const void*, int, int, int, int);
using BindFn = int(__cdecl*)(int, int, int, int, int, int, int, int);
using InstanceBindFn = int(__thiscall*)(uintptr_t, int, int, int, int, int, int, int, int);
using PollFn = int(__thiscall*)(uintptr_t, int, uint8_t*, int, const uint8_t*, const uint8_t*, const uint8_t**);
using ResetFn = char(__cdecl*)(const void*, int, int, int, int);
using DeviceFn = int(__cdecl*)(unsigned, char);
using FormatFn = int(__cdecl*)(int, int, char*, int, int, int, int, const uint8_t*);

std::string settingsPath;
Binding saved[2][2][Count];
bool held[2][Count], previous[2][Count], consumed[2];
bool cameraHeld[2];
bool pending;

template<class T> T& At(uintptr_t address) { return *reinterpret_cast<T*>(address); }
bool Keyboard(int slot) { return slot >= 10 && slot <= 13; }
bool ValidSlot(int slot) { return slot >= 0 && slot < 14; }

void Store(int player, int keyboard) {
  for (int a = 0; a < Count; ++a) {
    const Binding b = saved[player][keyboard][a];
    std::string value = std::to_string(b.kind) + "," + std::to_string(b.code);
    WritePrivateProfileStringA(kSections[player][keyboard], kNames[a], value.c_str(), settingsPath.c_str());
  }
}

void SavePending() {
  uintptr_t input = At<uintptr_t>(kInputPtr);
  if (!pending || !input) return;
  pending = false;
  for (int p = 0; p < 2; ++p) {
    int slot = At<int16_t>(input + kPlayerSlots + 2 * p);
    if (!ValidSlot(slot)) continue;
    int keyboard = Keyboard(slot);
    for (int a = 0; a < Count; ++a) {
      Binding b = At<Binding>(input + kPlayerBindings + 128 * p + 4 * (kFirstEntry + a));
      saved[p][keyboard][a] = Valid(b) ? b : Binding{};
    }
    Store(p, keyboard);
  }
}

// The native profile has room for 32 entries but serializes only its original axes and bits.
void Descriptors(uintptr_t input) {
  for (int a = 0; a < Count; ++a) {
    auto d = reinterpret_cast<uint8_t*>(input + 26612 + 6 * (kFirstEntry + a));
    d[0] = 3;
    d[1] = d[2] = 0xFF;
  }
}

char __fastcall OnLoad(uintptr_t input, void*, const void* profile, int slot, int profilePlayer,
                       int isPlayer, int player) {
  SavePending();
  char result = reinterpret_cast<LoadFn>(kLoadProfile)(input, profile, slot, profilePlayer, isPlayer, player);
  if (!ValidSlot(slot)) return result;
  Descriptors(input);
  int owner = isPlayer ? player : profilePlayer;
  if (owner < 0 || owner >= 2 || !profile) return result;
  for (int a = 0; a < Count; ++a) {
    Binding b = saved[owner][Keyboard(slot)][a];
    // Profile loading runs inside input-manager construction, before 13640E4 is assigned.
    if (b.kind) reinterpret_cast<InstanceBindFn>(0x637E70)(input, kFirstEntry + a, slot, b.kind, b.code,
                                                         1, isPlayer, player, 0);
  }
  return result;
}

int __cdecl OnBind(int entry, int slot, int kind, int code, int replace, int isPlayer, int player, int mask) {
  int result = reinterpret_cast<BindFn>(kBind)(entry, slot, kind, code, replace, isPlayer, player, mask);
  pending = true;
  return result;
}

char __cdecl OnReset(const void* profile, int slot, int profilePlayer, int isPlayer, int player) {
  SavePending();
  if (player >= 0 && player < 2 && ValidSlot(slot)) {
    for (int a = 0; a < Count; ++a) saved[player][Keyboard(slot)][a] = DefaultBinding(a, Keyboard(slot));
    Store(player, Keyboard(slot));
  }
  char result = reinterpret_cast<ResetFn>(kReset)(profile, slot, profilePlayer, isPlayer, player);
  pending = true;
  return result;
}

int __fastcall OnPoll(uintptr_t input, void*, int slot, uint8_t* output, int connected,
                      const uint8_t* pad, const uint8_t* mouse, const uint8_t** keys) {
  int result = reinterpret_cast<PollFn>(kPoll)(input, slot, output, connected, pad, mouse, keys);
  for (int p = 0; p < 2; ++p) {
    if (reinterpret_cast<DeviceFn>(0x533380)(p, 1) != slot) continue;
    bool blocked = (At<uint8_t>(0x136555C) & 4) || At<uint8_t>(input + 26596) ||
                   (Keyboard(slot) && At<uint8_t>(input + 2));
    // Entry 20 uses descriptor bit 9; 632F70 maps it to output bit 0x400.
    cameraHeld[p] = !blocked && (At<uint32_t>(reinterpret_cast<uintptr_t>(output) + 8) & 0x400) != 0;
    for (int a = 0; a < Count; ++a)
      held[p][a] = !blocked && ReadBinding(saved[p][Keyboard(slot)][a], connected != 0, pad, mouse,
                                          keys ? *keys : nullptr, At<int>(input + 25964),
                                          At<uint8_t>(input + 19998) != 0);
  }
  return result;
}

void BeginFrame() {
  SavePending();
  memcpy(previous, held, sizeof(held));
  memset(held, 0, sizeof(held));
  memset(consumed, 0, sizeof(consumed));
  memset(cameraHeld, 0, sizeof(cameraHeld));
}

int __cdecl OnFormat(int slot, int entry, char* text, int isPlayer, int player, int glyphs, int alternate,
                     const uint8_t* decoration) {
  uintptr_t input = At<uintptr_t>(kInputPtr);
  if (entry < kFirstEntry || entry >= kFirstEntry + Count || !input)
    return reinterpret_cast<FormatFn>(kFormat)(slot, entry, text, isPlayer, player, glyphs, alternate, decoration);
  // 637930's forced menu-key table has only 24 entries; extension rows use their actual bindings.
  int menuOverrides = At<int>(input + 25440);
  At<int>(input + 25440) = 1;
  int result = reinterpret_cast<FormatFn>(kFormat)(slot, entry, text, isPlayer, player, glyphs, alternate, decoration);
  At<int>(input + 25440) = menuOverrides;
  return result;
}
}

void Install(const std::string& path) {
  settingsPath = path;
  for (int p = 0; p < 2; ++p) {
    for (int k = 0; k < 2; ++k) {
      for (int a = 0; a < Count; ++a) {
        saved[p][k][a] = DefaultBinding(a, k != 0);
        char text[64];
        GetPrivateProfileStringA(kSections[p][k], kNames[a], "", text, sizeof(text), path.c_str());
        int kind, code;
        char extra;
        if (sscanf_s(text, "%d,%d%c", &kind, &code, &extra, 1u) == 2 && kind >= 0 && kind <= 7 &&
            code >= 0 && code <= 255) {
          Binding b{static_cast<uint8_t>(kind), 0, static_cast<int16_t>(code)};
          if (Valid(b)) saved[p][k][a] = b;
        }
      }
    }
  }
  for (uintptr_t site : kLoadSites)
    hook::Call(site, reinterpret_cast<void*>(kLoadProfile), reinterpret_cast<void*>(OnLoad));
  hook::Call(0x91B150, reinterpret_cast<void*>(kBind), reinterpret_cast<void*>(OnBind));
  hook::Call(0x91B6DE, reinterpret_cast<void*>(kReset), reinterpret_cast<void*>(OnReset));
  hook::Call(0x63D927, reinterpret_cast<void*>(kPoll), reinterpret_cast<void*>(OnPoll));
  for (uintptr_t site : {0x91C3E0, 0x91C4D6})
    hook::Call(site, reinterpret_cast<void*>(kFormat), reinterpret_cast<void*>(OnFormat));
  inputframe::OnFrame(BeginFrame);
}

bool Down(int character, Action action) {
  int player = playercharacter::HumanSlot(character);
  return player >= 0 && action >= 0 && action < Count && held[player][action];
}

bool CameraToggleDown() { return cameraHeld[0] || cameraHeld[1]; }

bool GrapplePressed(int character) {
  int player = playercharacter::HumanSlot(character);
  if (player < 0 || consumed[player] || !held[player][Grapple] || previous[player][Grapple]) return false;
  consumed[player] = true;
  return true;
}
}
