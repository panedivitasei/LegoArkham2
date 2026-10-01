#pragma once

#include <cstdint>

namespace abilitycontrols {
struct Binding {
  uint8_t kind, reserved;
  int16_t code;
};
static_assert(sizeof(Binding) == 4);

inline Binding DefaultBinding(int action, bool keyboard) {
  return keyboard ? Binding{7, 0, static_cast<int16_t>(action ? 42 : 33)}
                  : Binding{static_cast<uint8_t>(action ? 2 : 1), 0, static_cast<int16_t>(action ? 7 : 5)};
}

inline bool Valid(Binding b) {
  if (b.code < 0) return false;
  switch (b.kind) {
    case 0: return b.code == 0;
    case 1: return b.code < 132;
    case 2: case 3: return b.code < 32;
    case 4: return b.code < 8;
    case 7: return b.code < 256;
    default: return false;
  }
}

// Matches 636F20's button and positive-half-axis evaluation for a type-3 descriptor.
inline bool ReadBinding(Binding b, bool connected, const uint8_t* pad, const uint8_t* mouse,
                        const uint8_t* keys, int threshold, bool blockEscape) {
  if (!Valid(b)) return false;
  switch (b.kind) {
    case 1: return connected && pad && pad[64 + b.code] != 0;
    case 2: case 3: {
      if (!connected || !pad) return false;
      int value = reinterpret_cast<const int16_t*>(pad)[b.code];
      if (b.kind == 3) value = -value;
      if (value > 32767) value = 32767;
      return value > threshold;
    }
    case 4: return mouse && mouse[56 + b.code] != 0;
    case 7: return keys && !(blockEscape && b.code == 1) && keys[b.code] != 0;
    default: return false;
  }
}
}
