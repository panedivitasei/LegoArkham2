#pragma once

#include <windows.h>

// Hides __DISC__ from the disk probes so a DAT-less game takes the Steam path.
namespace extracted {

void Install(HMODULE game);

}  // namespace extracted
