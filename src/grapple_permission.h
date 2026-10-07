#pragma once
#include <cstdint>

namespace grapplepermission {
inline bool Flying(int character) {
  using Find = int(__thiscall*)(int,const void*);
  auto find = reinterpret_cast<Find>(0x865FC0);
  // 4EC990 installs these add-ons only on entry to hub or level flight.
  return find(character+4664,reinterpret_cast<const void*>(0x11BDB2C)) ||
      find(character+4664,reinterpret_cast<const void*>(0x11BDB60));
}
inline void Start(int character, int gizmo) {
  if (Flying(character)) return;
  int resource = *reinterpret_cast<int*>(character + 4104);
  int definition = resource ? *reinterpret_cast<int*>(resource + 44) : 0;
  if (!definition) return;
  auto& flags = *reinterpret_cast<uint8_t*>(definition + 250);
  uint8_t saved = flags;
  // 429D80 requires the grapple bit; AEF2B0 repeats that check through the manager.
  flags |= 1;
  using Check = bool(__stdcall*)(int, int);
  using Begin = char(__cdecl*)(int, int);
  if (reinterpret_cast<Check>(0x429D80)(character, gizmo))
    reinterpret_cast<Begin>(0xAEF2B0)(character, gizmo);
  flags = saved;
}
}
