#pragma once

#include <windows.h>

#include <initializer_list>

namespace hook {

// Swaps the exe's import slot for `function` in `dll` and returns what it held, or nullptr if the
// import isn't there by that name.
void* Import(HMODULE module, const char* dll, const char* function, void* detour);

// Swaps entry `index` of a COM object's vtable and returns the previous entry.
void* Vtable(void* object, int index, void* detour);

// Overwrites code bytes, but only while they still read as `expected`; false otherwise.
bool Patch(uintptr_t address, std::initializer_list<BYTE> expected, std::initializer_list<BYTE> replacement);

// Retargets a rel32 call or jump at `site`, but only while it still goes to `expected`.
bool Call(uintptr_t site, void* expected, void* detour);

template <class Fn>
void Import(HMODULE module, const char* dll, const char* function, Fn detour, Fn& original) {
  original = reinterpret_cast<Fn>(Import(module, dll, function, reinterpret_cast<void*>(detour)));
}

// Hooks the slot once; later objects of the same class share the vtable and are already covered.
template <class Fn>
void Vtable(void* object, int index, Fn detour, Fn& original) {
  if (!original) original = reinterpret_cast<Fn>(Vtable(object, index, reinterpret_cast<void*>(detour)));
}

}  // namespace hook
