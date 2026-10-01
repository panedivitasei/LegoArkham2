#pragma once

namespace waterrun {
// `splash` adds the flying skim effect on the water under Flash while he runs on it.
void Install(bool splash);

// True while the water run is holding this character on the surface.
bool Supported(int character);
}
