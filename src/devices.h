#pragma once

#include <windows.h>

namespace devices {

// Watches the game's own DirectInput mouse. `input` is the IDirectInput8 the game just created
// through the proxy; the mouse device it makes from it is the one read.
void Attach(void* input);

// Once a frame: drains the mouse buffer, since the game acquires the device but never reads it.
void Tick();

// Hands back the mouse movement gathered since the last call, in counts, and clears it.
void TakeMouse(float& dx, float& dy);
void TakeAimMouse(float& dx, float& dy);

// True if the mouse was clicked or moved since the last call; resets on read.
bool TakeActivity();

}  // namespace devices
