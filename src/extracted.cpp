#include "extracted.h"

#include <cstring>

#include "hook.h"

namespace extracted {
namespace {

// Startup probes the default file device for __DISC__ and takes the crashing disc build path if it exists, which only happens once the DATs are gone and the probe reaches the disk.
constexpr char kMarker[] = "__DISC__";

decltype(&CreateFileA) gameCreateFileA;
decltype(&GetFileAttributesA) gameGetFileAttributesA;

bool IsMarker(LPCSTR path) {
  if (!path) return false;
  const char* name = path;
  for (const char* p = path; *p; ++p)
    if (*p == '\\' || *p == '/') name = p + 1;
  return _stricmp(name, kMarker) == 0;
}

HANDLE WINAPI OnCreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                            DWORD disposition, DWORD flags, HANDLE templateFile) {
  if (IsMarker(path)) {
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  return gameCreateFileA(path, access, share, security, disposition, flags, templateFile);
}

DWORD WINAPI OnGetFileAttributesA(LPCSTR path) {
  if (IsMarker(path)) {
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_FILE_ATTRIBUTES;
  }
  return gameGetFileAttributesA(path);
}

}  // namespace

void Install(HMODULE game) {
  hook::Import(game, "kernel32.dll", "CreateFileA", OnCreateFileA, gameCreateFileA);
  hook::Import(game, "kernel32.dll", "GetFileAttributesA", OnGetFileAttributesA, gameGetFileAttributesA);
}

}  // namespace extracted
