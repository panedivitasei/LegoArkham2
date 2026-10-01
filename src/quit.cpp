#include "quit.h"

#include <cstdint>

#include "hook.h"

namespace quit {
namespace {

// The main window's procedure answers WM_SYSKEYDOWN itself, so DefWindowProc never turns Alt+F4
// into SC_CLOSE. Its WM_CLOSE case is the game's own shutdown (waits for a pending save, then exits).
constexpr uintptr_t kGameWindowProc = 0x640A20;

decltype(&CreateWindowExA) gameCreateWindowExA;
WNDPROC gameWindowProc;

LRESULT CALLBACK OnWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_SYSKEYDOWN && wParam == VK_F4) {
    PostMessageA(window, WM_CLOSE, 0, 0);
    return 0;
  }
  return CallWindowProcA(gameWindowProc, window, message, wParam, lParam);
}

HWND WINAPI OnCreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y, int width,
                              int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
  HWND window =
      gameCreateWindowExA(exStyle, className, title, style, x, y, width, height, parent, menu, instance, param);
  if (window && !gameWindowProc &&
      static_cast<uintptr_t>(GetWindowLongPtrA(window, GWLP_WNDPROC)) == kGameWindowProc)
    gameWindowProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(OnWindowMessage)));
  return window;
}

}  // namespace

void Install(HMODULE game) {
  hook::Import(game, "user32.dll", "CreateWindowExA", OnCreateWindowExA, gameCreateWindowExA);
}

}  // namespace quit
