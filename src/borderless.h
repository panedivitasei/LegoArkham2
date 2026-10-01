#pragma once

#include <windows.h>

namespace borderless {

// Puts the game in a frameless window the size of its monitor instead of exclusive fullscreen.
void Install(HMODULE game);

}  // namespace borderless
