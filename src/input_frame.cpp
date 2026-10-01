#include "input_frame.h"

#include <cstdint>

#include "hook.h"

namespace inputframe {
namespace {

// First call in the game's per-frame update: the input manager's frame start.
constexpr uintptr_t kFrameInputCallSite = 0x5BC173;
constexpr uintptr_t kFrameInput = 0x63C580;
constexpr int kMaxCallbacks = 8;

using FrameInputFn = int(__cdecl*)();

void (*callbacks[kMaxCallbacks])();
int count;

int __cdecl OnFrameInput() {
  int result = reinterpret_cast<FrameInputFn>(kFrameInput)();
  for (int i = 0; i < count; ++i) callbacks[i]();
  return result;
}

}  // namespace

void OnFrame(void (*fn)()) {
  if (count == 0)
    hook::Call(kFrameInputCallSite, reinterpret_cast<void*>(kFrameInput), reinterpret_cast<void*>(OnFrameInput));
  if (count < kMaxCallbacks) callbacks[count++] = fn;
}

}  // namespace inputframe
