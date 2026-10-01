#include "frame.h"

namespace frame {
namespace {

constexpr int kMaxCallbacks = 8;
void (*callbacks[kMaxCallbacks])();
int count;

}  // namespace

void OnTick(void (*fn)()) {
  if (count < kMaxCallbacks) callbacks[count++] = fn;
}

void Tick() {
  for (int i = 0; i < count; ++i) callbacks[i]();
}

}  // namespace frame
