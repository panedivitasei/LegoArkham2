#include "glide_anim.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>
#include <list>

#include "hook.h"

namespace glideanim {
namespace {

// LEGOBatman2.exe addresses (Steam build, no ASLR). Character clips arrive through the animation pak
// loader as 'Deflate_v1.0' entries; the call below hands one entry to the decompress-and-convert step.
// The glide state plays the shared "glide" entry and the fall state plays the character's own "Fall"
// entry, so replacing those two entries at the source gives each state its own clip, and the
// player's usual crossfade eases between them.
constexpr uintptr_t kLoadEntryCallSite = 0x7ECCE4;
constexpr uintptr_t kLoadEntry = 0x7EC6A0;
constexpr int kTypeAn4 = 14;
constexpr int kDeflateSizeOffset = 32;  // u32 decompressed size inside the 36-byte container header

using LoadFn = void*(__fastcall*)(void* self, void* edx, uint32_t handle, int mode, const uint8_t* src, uint32_t srclen,
                                  int type, int decompressed, int a8);

// One clip can stand in for the same-named entry of several packs (Batman's and Robin's own packs
// carry their own copies of the donor), so a swap holds a list of the stored and decompressed
// sizes that identify those entries.
struct Swap {
  std::vector<uint8_t> blob;  // ready-made 'Deflate_v1.0' container
  std::vector<std::pair<uint32_t, uint32_t>> entries;  // {stored size, decompressed size} of each pak entry
  const char* name;
};

Swap glideSwap;
Swap fallSwap;
Swap pulloutSwap;
Swap tuckSwap;
Swap flashRollSwap;
bool flashRollLoaded;
FILE* log;

// Private entries stay outside native banks; their AN4 buffers live for the process.
struct PrivateClip {
  std::vector<uint8_t> blob, decoded;
  const char* name;
  int id = -1;
  bool attempted = false;
};
PrivateClip privateDive{{}, {}, "LegoArkham2_PrivateDive"};
PrivateClip privateGlide{{}, {}, "LegoArkham2_PrivateRobinGlide"};
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
  for (auto* clip : {&privateDive, &privateGlide})
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
  for (auto* candidate : {&privateDive, &privateGlide})
    if (id >= 0 && candidate->id == id && !candidate->decoded.empty()) clip = candidate;
  if (!clip) return native(bank, edx, id);
  void* source = native(bank, edx, *reinterpret_cast<int16_t*>(0x11A2230));
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
  *reinterpret_cast<int16_t*>(bytes + 268) = static_cast<int16_t>(id);
  bytes[280] = 1;
  memset(bytes + 284, 0, 144);
  *reinterpret_cast<uintptr_t*>(bytes + 356) = reinterpret_cast<uintptr_t>(clip);
  memset(bytes + 428, 0, 24);
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
    Note(clip.name);
  }
  if (clip.decoded.empty()) return -1;
  using IdFn = int(__cdecl*)(const char*);
  clip.id = reinterpret_cast<IdFn>(0x945980)(clip.name);
  return clip.id;
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

bool Matches(const Swap& swap, int type, uint32_t srclen, int decompressed) {
  if (type != kTypeAn4 || swap.blob.empty()) return false;
  for (const auto& entry : swap.entries)
    if (srclen == entry.first && static_cast<uint32_t>(decompressed) == entry.second) return true;
  return false;
}

// "8897,11429" and "22817,16471" pair up by position; a lone value is one entry.
std::vector<std::pair<uint32_t, uint32_t>> Pairs(const std::string& srcLens, const std::string& sizes) {
  std::vector<uint32_t> a, b;
  for (auto* list : {&srcLens, &sizes}) {
    auto& out = list == &srcLens ? a : b;
    size_t start = 0;
    while (start <= list->size()) {
      size_t end = list->find(',', start);
      if (end == std::string::npos) end = list->size();
      std::string item = list->substr(start, end - start);
      if (!item.empty()) out.push_back(static_cast<uint32_t>(strtoul(item.c_str(), nullptr, 10)));
      start = end + 1;
    }
  }
  std::vector<std::pair<uint32_t, uint32_t>> pairs;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) pairs.emplace_back(a[i], b[i]);
  return pairs;
}

void* __fastcall OnLoadEntry(void* self, void* edx, uint32_t handle, int mode, const uint8_t* src, uint32_t srclen,
                             int type, int decompressed, int a8) {
  // Match Flash's stock combatroll pack entry, including its compressed payload prefix.
  if (type == kTypeAn4 && decompressed == 35833 && srclen >= 128 && !flashRollSwap.blob.empty()) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 128; ++i) hash = (hash ^ src[i]) * 16777619u;
    if (hash == 0xCC71EB53u) {
      const auto& blob = flashRollSwap.blob;
      int size = *reinterpret_cast<const int*>(blob.data() + kDeflateSizeOffset);
      auto cache = static_cast<uint32_t*>(self);
      // 7ECC10 checked the donor size; repeat its capacity check for the replacement.
      using SpaceFn = void(__fastcall*)(void*, void*, int);
      if (size > 0 && static_cast<uint32_t>(size) > cache[5])
        reinterpret_cast<SpaceFn>(0x7EB550)(self, nullptr, size);
      if (size <= 0 || static_cast<uint32_t>(size) > cache[5]) {
        Note("flash roll: insufficient animation cache space; using stock clip");
        return reinterpret_cast<LoadFn>(kLoadEntry)(self, edx, handle, mode, src, srclen, type, decompressed, a8);
      }
      void* result = reinterpret_cast<LoadFn>(kLoadEntry)(self, edx, handle, mode, blob.data(),
          static_cast<uint32_t>(blob.size()), type, size, a8);
      flashRollLoaded = result && static_cast<uint32_t*>(result)[4];
      Note(flashRollLoaded ? "flash roll: extended stock clip loaded" : "flash roll: clip load failed");
      return result;
    }
  }
  for (const Swap* swap : {&glideSwap, &fallSwap, &pulloutSwap, &tuckSwap}) {
    if (!Matches(*swap, type, srclen, decompressed)) continue;
    src = swap->blob.data();
    srclen = static_cast<uint32_t>(swap->blob.size());
    decompressed = *reinterpret_cast<const int*>(swap->blob.data() + kDeflateSizeOffset);
    Note(swap->name);
    break;
  }
  return reinterpret_cast<LoadFn>(kLoadEntry)(self, edx, handle, mode, src, srclen, type, decompressed, a8);
}

}  // namespace

void Install(const std::string& clipPath, const std::string& srcLens, const std::string& sizes, const std::string& logPath) {
  if (!logPath.empty()) log = fopen(logPath.c_str(), "w");
  glideSwap = {ReadBlob(clipPath), Pairs(srcLens, sizes), "glide"};
  hook::Call(kLoadEntryCallSite, reinterpret_cast<void*>(kLoadEntry), reinterpret_cast<void*>(OnLoadEntry));
}

void FallClip(const std::string& clipPath, const std::string& srcLens, const std::string& sizes) {
  fallSwap = {ReadBlob(clipPath), Pairs(srcLens, sizes), "fall"};
}

void DiveClip(const std::string& clipPath) {
  privateDive.blob = ReadBlob(clipPath);
  InstallPrivateHooks();
}

void PulloutClip(const std::string& clipPath, const std::string& srcLens, const std::string& sizes) {
  pulloutSwap = {ReadBlob(clipPath), Pairs(srcLens, sizes), "pullout"};
}

void TuckClip(const std::string& clipPath, const std::string& srcLens, const std::string& sizes) {
  tuckSwap = {ReadBlob(clipPath), Pairs(srcLens, sizes), "tuck"};
}

void GlideOverrideClip(const std::string& clipPath) {
  privateGlide.blob = ReadBlob(clipPath);
  InstallPrivateHooks();
}

int DiveId() { return PrivateId(privateDive); }
int GlideOverrideId() { return PrivateId(privateGlide); }

void Note(const char* text) {
  if (!log) return;
  fprintf(log, "%s\n", text);
  fflush(log);
}

void FlashRollClip(const std::string& clipPath) {
  flashRollSwap.blob = ReadBlob(clipPath);
  if (flashRollSwap.blob.empty()) Note("flash roll: missing clip; disabled");
}

bool FlashRollLoaded() { return flashRollLoaded; }

}  // namespace glideanim
