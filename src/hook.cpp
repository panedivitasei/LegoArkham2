#include "hook.h"

#include <cstdint>
#include <cstring>

namespace hook {
namespace {

void* Exchange(void** slot, void* value) {
  DWORD protect;
  VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protect);
  void* previous = *slot;
  *slot = value;
  VirtualProtect(slot, sizeof(*slot), protect, &protect);
  return previous;
}

void WriteCode(void* address, const void* bytes, size_t size) {
  DWORD protect;
  VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &protect);
  memcpy(address, bytes, size);
  VirtualProtect(address, size, protect, &protect);
  FlushInstructionCache(GetCurrentProcess(), address, size);
}

}  // namespace

void* Import(HMODULE module, const char* dll, const char* function, void* detour) {
  auto base = reinterpret_cast<BYTE*>(module);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!imports.VirtualAddress) return nullptr;

  auto descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
  for (; descriptor->Name; ++descriptor) {
    if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), dll) != 0) continue;
    if (!descriptor->OriginalFirstThunk) return nullptr;  // bound imports carry no names
    auto lookup = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
    auto address = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
    for (; lookup->u1.AddressOfData; ++lookup, ++address) {
      if (IMAGE_SNAP_BY_ORDINAL(lookup->u1.Ordinal)) continue;
      auto entry = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + lookup->u1.AddressOfData);
      if (strcmp(entry->Name, function) == 0) return Exchange(reinterpret_cast<void**>(&address->u1.Function), detour);
    }
  }
  return nullptr;
}

void* Vtable(void* object, int index, void* detour) {
  void** table = *static_cast<void***>(object);
  return Exchange(&table[index], detour);
}

bool Patch(uintptr_t address, std::initializer_list<BYTE> expected, std::initializer_list<BYTE> replacement) {
  auto at = reinterpret_cast<BYTE*>(address);
  if (expected.size() != replacement.size() || memcmp(at, expected.begin(), expected.size()) != 0) return false;
  WriteCode(at, replacement.begin(), replacement.size());
  return true;
}

bool Call(uintptr_t site, void* expected, void* detour) {
  auto at = reinterpret_cast<BYTE*>(site);
  if (at[0] != 0xE8 && at[0] != 0xE9) return false;
  auto next = reinterpret_cast<intptr_t>(at + 5);
  int32_t rel;
  memcpy(&rel, at + 1, sizeof(rel));
  if (next + rel != reinterpret_cast<intptr_t>(expected)) return false;
  rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(detour) - next);
  WriteCode(at + 1, &rel, sizeof(rel));
  return true;
}

}  // namespace hook
