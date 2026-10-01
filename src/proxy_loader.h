#pragma once
#include <windows.h>
#include <string>
#include <cstring>

namespace proxyloader {
inline FARPROC Resolve(const std::string& folder, const std::string& chain, FARPROC self) {
  // Chaining is explicit; never search for another mod or reload our proxy.
  if (chain.size() > 4 && chain.find_first_of("\\/: ") == std::string::npos &&
      _stricmp(chain.c_str(), "dinput8.dll") && _stricmp(chain.c_str() + chain.size() - 4, ".dll") == 0) {
    HMODULE module = LoadLibraryA((folder + chain).c_str());
    if (module) {
      FARPROC entry = GetProcAddress(module, "DirectInput8Create");
      if (entry && entry != self) return entry;
      FreeLibrary(module);
    }
  }
  char path[MAX_PATH];
  if (!GetSystemDirectoryA(path, MAX_PATH)) return nullptr;
  strcat_s(path, "\\dinput8.dll");
  HMODULE system = LoadLibraryA(path);
  return system ? GetProcAddress(system, "DirectInput8Create") : nullptr;
}
}
