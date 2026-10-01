#include "cape_sound.h"

#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstring>

namespace capesound {
namespace {

// The game's own sound banks cannot take new events without repacking them, so the cape sounds
// are plain files played through the Windows media layer. Each one is opened once under an alias;
// the mpegvideo device plays wav and mp3 alike and takes a volume and a repeat flag.
const char* const kAliases[Kinds] = {"la2dive", "la2open", "la2loop"};
bool loaded[Kinds];
bool openPlaying;
bool loopWanted;

bool Command(const char* text) { return mciSendStringA(text, nullptr, 0, nullptr) == 0; }

void Load(Kind kind, const std::string& path, int volume) {
  if (path.empty()) return;
  char text[MAX_PATH + 96];
  snprintf(text, sizeof(text), "open \"%s\" type mpegvideo alias %s", path.c_str(), kAliases[kind]);
  loaded[kind] = Command(text);
  if (!loaded[kind]) return;
  snprintf(text, sizeof(text), "setaudio %s volume to %d", kAliases[kind], volume);
  Command(text);
}

void Play(Kind kind, bool repeat) {
  if (!loaded[kind]) return;
  char text[96];
  snprintf(text, sizeof(text), "play %s from 0%s", kAliases[kind], repeat ? " repeat" : "");
  Command(text);
}

void Stop(Kind kind) {
  if (!loaded[kind]) return;
  char text[64];
  snprintf(text, sizeof(text), "stop %s", kAliases[kind]);
  Command(text);
}

bool Playing(Kind kind) {
  if (!loaded[kind]) return false;
  char text[64], mode[32] = {};
  snprintf(text, sizeof(text), "status %s mode", kAliases[kind]);
  if (mciSendStringA(text, mode, sizeof(mode), nullptr) != 0) return false;
  return strcmp(mode, "playing") == 0;
}

}  // namespace

void Install(const std::string& dive, const std::string& open, const std::string& loop, int volume) {
  if (volume < 0) volume = 0;
  if (volume > 1000) volume = 1000;
  Load(Dive, dive, volume);
  Load(Open, open, volume);
  Load(Loop, loop, volume);
}

void StartDive() {
  Stop(Open);
  Stop(Loop);
  openPlaying = false;
  loopWanted = false;
  Play(Dive, true);
}

void StartGlide() {
  Stop(Dive);
  Stop(Loop);
  if (loaded[Open]) {
    Play(Open, false);
    openPlaying = true;
    loopWanted = true;
  } else {
    Play(Loop, true);
  }
}

void Silence() {
  for (int kind = 0; kind < Kinds; ++kind) Stop(static_cast<Kind>(kind));
  openPlaying = false;
  loopWanted = false;
}

void Tick() {
  if (!openPlaying) return;
  if (Playing(Open)) return;
  openPlaying = false;
  if (loopWanted) {
    loopWanted = false;
    Play(Loop, true);
  }
}

}  // namespace capesound
