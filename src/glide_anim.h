#pragma once
#include <string>

namespace glideanim {
// Private animation resources do not depend on packed or loose game assets.
void Install(const std::string& clipPath, const std::string& logPath);
void FallClip(const std::string& clipPath);
void DiveClip(const std::string& clipPath);
void PulloutClip(const std::string& clipPath);
void TuckClip(const std::string& clipPath);
void GlideOverrideClip(const std::string& clipPath);
void FlashRollClip(const std::string& clipPath);
int GlideId();
int FallId();
int DiveId();
int GlideOverrideId();
int PulloutId();
int TuckId();
int FlashRollId();
bool FlashRollLoaded();
void Note(const char* text);
}
