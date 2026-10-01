#pragma once

#include <windows.h>

namespace quit {

// Alt+F4 posts WM_CLOSE to the game window; the game's window procedure swallows WM_SYSKEYDOWN.
void Install(HMODULE game);

}  // namespace quit
