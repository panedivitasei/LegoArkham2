#pragma once

#include <cstdint>
#include <string>

namespace glideanim {

// Replaces the shared glide pak entry, recognised by its stored and decompressed sizes, with the
// clip file at `clipPath` (a 'Deflate_v1.0' container). `srcLens` and `sizes` are comma lists
// pairing up by position, one pair per pack that carries the entry. `logPath` gets a line per swap.
void Install(const std::string& clipPath, const std::string& srcLens, const std::string& sizes, const std::string& logPath);

// The same for the characters' own fall entries.
void FallClip(const std::string& clipPath, const std::string& srcLens, const std::string& sizes);

// Transition clips replace their configured pack entries.
void PulloutClip(const std::string& clipPath, const std::string& srcLens, const std::string& sizes);
void TuckClip(const std::string& clipPath, const std::string& srcLens, const std::string& sizes);

// Private clips leave native locomotion entries intact.
void DiveClip(const std::string& clipPath);
void GlideOverrideClip(const std::string& clipPath);

int DiveId();
int GlideOverrideId();

// Appends a line to the clip log.
void Note(const char* text);

void FlashRollClip(const std::string& clipPath);
bool FlashRollLoaded();

}  // namespace glideanim
