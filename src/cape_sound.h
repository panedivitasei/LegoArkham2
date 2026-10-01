#pragma once

#include <string>

namespace capesound {

enum Kind { Dive, Open, Loop, Kinds };

// Loads the three cape sounds (wav or mp3, empty paths skipped) at `volume`, 0 to 1000.
void Install(const std::string& dive, const std::string& open, const std::string& loop, int volume);

// The dive loop starts; the open one-shot plays and the glide loop follows once it has finished;
// Silence stops everything.
void StartDive();
void StartGlide();
void Silence();

// Once per frame: starts the glide loop after the open sound ends.
void Tick();

}  // namespace capesound
