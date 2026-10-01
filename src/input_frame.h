#pragma once

namespace inputframe {

// Registers `fn` to run once per game frame on the game thread, right after the input manager
// starts the frame and before any player reads a device.
void OnFrame(void (*fn)());

}  // namespace inputframe
