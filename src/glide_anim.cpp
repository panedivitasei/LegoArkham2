#include "glide_anim.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>
#include <list>

#include "hook.h"
#include "grapple_clips.h"

namespace glideanim {
namespace {

FILE* log;

// Private entries stay outside native banks; their AN4 buffers live for the process.
struct PrivateClip {
  std::vector<uint8_t> blob, decoded;
  const char* name;
  int id = -1;
  bool attempted = false;
  int templateId = -1;
};
PrivateClip privateDive{{}, {}, "LegoArkham2_PrivateDive"};
PrivateClip privateGlide{{}, {}, "LegoArkham2_PrivateRobinGlide"};
PrivateClip privateBaseGlide{{}, {}, "LegoArkham2_PrivateGlide"};
PrivateClip privateFall{{}, {}, "LegoArkham2_PrivateFall"};
PrivateClip privatePullout{{}, {}, "LegoArkham2_PrivatePullout"};
PrivateClip privateTuck{{}, {}, "LegoArkham2_PrivateTuck"};
PrivateClip privateFlash{{}, {}, "LegoArkham2_PrivateFlashRoll"};
std::vector<PrivateClip*> clips = {&privateDive,&privateGlide,&privateBaseGlide,&privateFall,
                            &privatePullout,&privateTuck,&privateFlash};
const char* grappleNames[] = {"grappleaim", "grapplecatch", "grapplethrow", "grapplewait",
    "grapple_down", "grapple_gunout", "grapple_hang", "grapple_hang_aim", "grapple_hang_catch",
    "grapple_hang_throw", "grapple_hang_wait", "grapple_idle", "grapple_up", "grapple_hang_jump"};
PrivateClip grappleClips[14];
std::string grapplePrivateNames[14];
bool (*grappleSet)(int);
struct PrivateEntry {
  void* bank;
  PrivateClip* clip;
  const void* source;
  alignas(4) std::array<uint8_t, 508> bytes;
  std::array<uint8_t, 508> snapshot;
};
std::list<PrivateEntry> privateEntries;
bool privateHooks;
using BankFn = void*(__fastcall*)(void*, void*, int);
using ResourceFn = void*(__fastcall*)(uintptr_t*, void*);
using ReleaseFn = void(__fastcall*)(uintptr_t*, void*);

PrivateClip* PrivateResource(uintptr_t* ref) {
  for (auto* clip : clips)
    if (*ref == reinterpret_cast<uintptr_t>(clip)) return clip;
  return nullptr;
}

void* __fastcall OnPrivateResource(uintptr_t* ref, void* edx) {
  if (auto* clip = PrivateResource(ref)) return clip->decoded.data();
  return reinterpret_cast<ResourceFn>(0x7EDE80)(ref, edx);
}

void __fastcall OnPrivateRelease(uintptr_t* ref, void* edx) {
  if (!PrivateResource(ref)) reinterpret_cast<ReleaseFn>(0x7EDEC0)(ref, edx);
}

void* __fastcall OnPrivateFind(void* bank, void* edx, int id) {
  auto native = reinterpret_cast<BankFn>(0x650910);
  PrivateClip* clip = nullptr;
  for (auto* candidate : clips)
    if (id >= 0 && candidate->id == id && !candidate->decoded.empty()) clip = candidate;
  if (!clip) return native(bank, edx, id);
  int templateId = clip->templateId;
  if (templateId < 0) templateId = *reinterpret_cast<int16_t*>(0x11A2230);
  void* source = native(bank, edx, templateId);
  if (!source) return nullptr;
  PrivateEntry* entry = nullptr;
  for (auto& item : privateEntries)
    if (item.bank == bank && item.clip == clip && item.source == source &&
        *reinterpret_cast<const int16_t*>(item.bytes.data() + 268) == id &&
        memcmp(item.snapshot.data(), source, item.snapshot.size()) == 0) entry = &item;
  if (entry) return entry->bytes.data();
  privateEntries.push_back({bank, clip, source, {}, {}});
  auto* bytes = privateEntries.back().bytes.data();
  memcpy(bytes, source, 508);
  memcpy(privateEntries.back().snapshot.data(), source, 508);
  memset(bytes, 0, 8);
  memset(bytes + 124, 0, 144);
  // Flash's appended loop belongs to combatroll_land, not the first subclip.
  if (clip == &privateFlash) {
    *reinterpret_cast<const char**>(bytes + 124) = "Flash";
    *reinterpret_cast<const char**>(bytes + 196) = "combatroll_land";
  }
  *reinterpret_cast<int16_t*>(bytes + 268) = static_cast<int16_t>(id);
  bytes[280] = 1;
  memset(bytes + 284, 0, 144);
  *reinterpret_cast<uintptr_t*>(bytes + 356) = reinterpret_cast<uintptr_t>(clip);
  memset(bytes + 428, 0, 24);
  reinterpret_cast<void(__thiscall*)(void*)>(0x64C0E0)(bytes);
  return bytes;
}

bool InstallPrivateHooks() {
  if (privateHooks) return true;
  struct Site { uintptr_t at, target; void* replacement; };
  std::vector<Site> sites{{0x651E6D, 0x650910, reinterpret_cast<void*>(OnPrivateFind)}};
  for (uintptr_t at : {0x64BB6B, 0x64BD12, 0x64C116, 0x64C281, 0x64C3BB, 0x64FF22, 0x6500EE, 0x650512})
    sites.push_back({at, 0x7EDE80, reinterpret_cast<void*>(OnPrivateResource)});
  for (uintptr_t at : {0x64C20D, 0x64C38E, 0x64C59F, 0x65061C, 0x650635,
                       0x65A5DC, 0x65AA77, 0x65AC00, 0x65AD4A, 0x65ADF2})
    sites.push_back({at, 0x7EDEC0, reinterpret_cast<void*>(OnPrivateRelease)});
  for (auto site : sites)
    if (*reinterpret_cast<uint8_t*>(site.at) != 0xE8 ||
        site.at + 5 + *reinterpret_cast<int32_t*>(site.at + 1) != site.target) {
      Note("private clips: hook mismatch; disabled");
      return false;
    }
  for (auto site : sites)
    hook::Call(site.at, reinterpret_cast<void*>(site.target), site.replacement);
  privateHooks = true;
  return true;
}

int PrivateId(PrivateClip& clip) {
  if (!privateHooks || clip.blob.empty()) return -1;
  if (!clip.attempted) {
    clip.attempted = true;
    int size = *reinterpret_cast<const int*>(clip.blob.data() + 32);
    if (size <= 0 || size > 0x1000000) return -1;
    std::vector<uint8_t> raw(size);
    using UnpackFn = bool(__cdecl*)(int, const void*, int, void*, int, int*);
    int used = 0;
    if (!reinterpret_cast<UnpackFn>(0x558370)(2, clip.blob.data(), static_cast<int>(clip.blob.size()),
                                            raw.data(), size, &used) || used != size) return -1;
    using SizeFn = int(__cdecl*)(void*);
    using ConvertFn = int(__cdecl*)(void*, void**, int);
    int converted = reinterpret_cast<SizeFn>(0x56B320)(raw.data());
    if (converted < 0 || converted > 0x1000000) return -1;
    if (converted) {
      clip.decoded.resize(converted);
      void* destination = clip.decoded.data();
      int written = reinterpret_cast<ConvertFn>(0x56B3C0)(raw.data(), &destination, converted);
      if (written <= 0 || written > converted || destination != clip.decoded.data()) {
        clip.decoded.clear();
        return -1;
      }
    } else clip.decoded = std::move(raw);
    using FixFn = char(__fastcall*)(void*, void*, int, void*, int, int, int);
    reinterpret_cast<FixFn>(0x56C460)(clip.decoded.data(), nullptr, 0, clip.decoded.data(), 0, 0, 1);
  }
  if (clip.decoded.empty()) return -1;
  using IdFn = int(__cdecl*)(const char*);
  clip.id = reinterpret_cast<IdFn>(0x945980)(clip.name);
  return clip.id;
}

// 651E40's displaced instructions have no relative operands.
__declspec(naked) void FindOriginal() {
  __asm {
    push esi
    mov esi, [ecx + 2Ch]
    xor eax, eax
    push 0651E46h
    ret
  }
}

struct GrappleEntry {
  int set, id;
  const void* source;
  std::array<uint8_t, 508> bytes;
  std::array<uint32_t, 7> gameData;
};
std::list<GrappleEntry> grappleEntries;
void* __fastcall FindGrapple(int set, void*, int id, int flagged) {
  auto native = reinterpret_cast<void*(__thiscall*)(int, int, int)>(FindOriginal);
  if (grappleSet && grappleSet(set)) {
    auto action = reinterpret_cast<int(__cdecl*)(const char*)>(0x945980);
    for (int i = 0; i < 14; ++i) {
      if (id != action(grappleNames[i])) continue;
      grappleClips[i].templateId = action("idle");
      int replacement = PrivateId(grappleClips[i]);
      auto source = replacement >= 0 ? native(set, replacement, 0) : nullptr;
      if (!source) break;
      if (flagged && !(grappleclips::metadata[i].playback[1] & 0x20)) return nullptr;
      for (auto& entry : grappleEntries)
        if (entry.set == set && entry.id == id && entry.source == source)
          return entry.bytes.data();
      grappleEntries.push_back({set, id, source, {}, {}});
      auto& entry = grappleEntries.back();
      memcpy(entry.bytes.data(), source, entry.bytes.size());
      *reinterpret_cast<int16_t*>(entry.bytes.data() + 268) = static_cast<int16_t>(id);
      const auto& metadata = grappleclips::metadata[i];
      memcpy(entry.bytes.data()+270,metadata.playback,sizeof metadata.playback);
      entry.bytes[488] = metadata.root;
      memcpy(entry.bytes.data()+496,metadata.bounds,sizeof metadata.bounds);
      // 894330 constructs GAMEANIMDATA; 9F8370 uses its gun-out marker before drawing the rope.
      entry.gameData[0] = 0xF39C5C;
      memcpy(entry.gameData.data()+1,metadata.gameData,sizeof metadata.gameData);
      *reinterpret_cast<uint32_t**>(entry.bytes.data()+460) = entry.gameData.data();
      return entry.bytes.data();
    }
  }
  return native(set, id, flagged);
}

std::vector<uint8_t> ReadBlob(const std::string& path) {
  std::vector<uint8_t> data;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    fseek(f, 0, SEEK_END);
    data.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    fclose(f);
  }
  if (data.size() < 36 || memcmp(data.data(), "Deflate_v1.0", 12) != 0) data.clear();
  return data;
}

}  // namespace

void Install(const std::string& clipPath, const std::string& logPath) {
  if (!logPath.empty()) log = fopen(logPath.c_str(), "w");
  privateBaseGlide.blob = ReadBlob(clipPath);
  InstallPrivateHooks();
}

void FallClip(const std::string& clipPath) {
  privateFall.blob = ReadBlob(clipPath);
}
void DiveClip(const std::string& clipPath) { privateDive.blob = ReadBlob(clipPath); }
void PulloutClip(const std::string& clipPath) { privatePullout.blob = ReadBlob(clipPath); }
void TuckClip(const std::string& clipPath) { privateTuck.blob = ReadBlob(clipPath); }
void GlideOverrideClip(const std::string& clipPath) { privateGlide.blob = ReadBlob(clipPath); }
int GlideId() { return PrivateId(privateBaseGlide); }
int FallId() {
  privateFall.templateId = *reinterpret_cast<int16_t*>(0x11A1A98);
  return PrivateId(privateFall);
}
int DiveId() { return PrivateId(privateDive); }
int GlideOverrideId() { return PrivateId(privateGlide); }
int PulloutId() { return PrivateId(privatePullout); }
int TuckId() { return PrivateId(privateTuck); }
int FlashRollId() { return PrivateId(privateFlash); }

void Note(const char* text) {
  if (!log) return;
  fprintf(log, "%s\n", text);
  fflush(log);
}

void FlashRollClip(const std::string& clipPath) {
  privateFlash.blob = ReadBlob(clipPath);
  privateFlash.templateId = 263;
  if (privateFlash.blob.empty()) Note("flash roll: missing clip; disabled");
}

void GrappleClips(const std::string& folder, bool (*eligibleSet)(int)) {
  grappleSet = eligibleSet;
  for (int i = 0; i < 14; ++i) {
    grapplePrivateNames[i] = std::string("LegoArkham2_Private_") + grappleNames[i];
    grappleClips[i].name = grapplePrivateNames[i].c_str();
    grappleClips[i].blob = ReadBlob(folder + "LegoArkham2_" + grappleNames[i] + ".an4");
    clips.push_back(&grappleClips[i]);
  }
  uintptr_t offset = reinterpret_cast<uintptr_t>(FindGrapple) - 0x651E45;
  hook::Patch(0x651E40, {0x56,0x8B,0x71,0x2C,0x33,0xC0},
      {0xE9,static_cast<BYTE>(offset),static_cast<BYTE>(offset >> 8),
       static_cast<BYTE>(offset >> 16),static_cast<BYTE>(offset >> 24),0x90});
}

bool FlashRollLoaded() { return FlashRollId() >= 0; }

}  // namespace glideanim
