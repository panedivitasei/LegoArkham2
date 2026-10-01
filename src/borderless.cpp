#include "borderless.h"

#include <d3d9.h>

#include "frame.h"
#include "glide_anim.h"
#include "hook.h"

namespace borderless {
namespace {

// The game creates "TTalesWindow" at the saved window size with a caption and thick frame, then
// CreateDevice with its own present parameters; a device reset centres the window again with
// SetWindowPos. Both go through the import table, so no code is touched.
constexpr int kSlot_CreateDevice = 16;  // IDirect3D9
constexpr int kSlot_Reset = 16;         // IDirect3DDevice9
constexpr int kSlot_Present = 17;
constexpr DWORD kFrameStyles = WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
constexpr DWORD kFrameExStyles = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;

using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

decltype(&Direct3DCreate9) gameDirect3DCreate9;
decltype(&CreateWindowExA) gameCreateWindowExA;
decltype(&SetWindowPos) gameSetWindowPos;
CreateDeviceFn gameCreateDevice;
ResetFn gameReset;
PresentFn gamePresent;

HWND window;

RECT Screen(HMONITOR monitor) {
  MONITORINFO info{sizeof(info)};
  GetMonitorInfoA(monitor, &info);
  return info.rcMonitor;
}

// The parameters are the game's own copy, so its windowed flag reads as set from here on.
void Windowed(D3DPRESENT_PARAMETERS* parameters) {
  if (!parameters) return;
  parameters->Windowed = TRUE;
  parameters->FullScreen_RefreshRateInHz = 0;
}

HRESULT STDMETHODCALLTYPE OnReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters) {
  Windowed(parameters);
  return gameReset(device, parameters);
}

// The one call the game makes every frame whatever it's showing, so the frame ticks hang off it.
HRESULT STDMETHODCALLTYPE OnPresent(IDirect3DDevice9* device, const RECT* source, const RECT* dest, HWND window,
                                    const RGNDATA* dirty) {
  frame::Tick();
  return gamePresent(device, source, dest, window, dirty);
}

// Keyboard and mouse are foreground-only DirectInput devices, while the game's active flag starts set
// and tracks WM_ACTIVATE only, so a window shown without the foreground reads pads but no keys.
// Attaching to the foreground thread's input queue lifts the focus-steal lock for the show.
void TakeForeground() {
  HWND front = GetForegroundWindow();
  if (front == window) return;
  DWORD frontThread = front ? GetWindowThreadProcessId(front, nullptr) : 0;
  DWORD self = GetCurrentThreadId();
  bool attached = frontThread && frontThread != self && AttachThreadInput(self, frontThread, TRUE);
  BringWindowToTop(window);
  SetForegroundWindow(window);
  SetFocus(window);
  if (attached) AttachThreadInput(self, frontThread, FALSE);
  if (GetForegroundWindow() != window) glideanim::Note("window: could not take the foreground");
}

HRESULT STDMETHODCALLTYPE OnCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behaviour,
                                         D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device) {
  Windowed(parameters);
  HRESULT result = gameCreateDevice(d3d, adapter, type, focus, behaviour, parameters, device);
  if (FAILED(result)) return result;
  hook::Vtable(*device, kSlot_Reset, OnReset, gameReset);
  hook::Vtable(*device, kSlot_Present, OnPresent, gamePresent);
  // Exclusive fullscreen brings the window up by itself; windowed needs the show.
  if (window && !IsWindowVisible(window)) {
    ShowWindow(window, SW_SHOW);
    TakeForeground();
  }
  return result;
}

IDirect3D9* WINAPI OnDirect3DCreate9(UINT version) {
  IDirect3D9* d3d = gameDirect3DCreate9(version);
  if (d3d) hook::Vtable(d3d, kSlot_CreateDevice, OnCreateDevice, gameCreateDevice);
  return d3d;
}

HWND WINAPI OnCreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y, int width,
                              int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
  bool topLevel = !parent && !(style & WS_CHILD);
  if (window || !topLevel)
    return gameCreateWindowExA(exStyle, className, title, style, x, y, width, height, parent, menu, instance, param);
  SetProcessDPIAware();  // or a scaled desktop stretches the window past the monitor
  RECT screen = Screen(MonitorFromPoint({x, y}, MONITOR_DEFAULTTOPRIMARY));
  style = (style & ~kFrameStyles) | WS_POPUP;
  window = gameCreateWindowExA(exStyle & ~kFrameExStyles, className, title, style, screen.left, screen.top,
                               screen.right - screen.left, screen.bottom - screen.top, parent, menu, instance, param);
  return window;
}

BOOL WINAPI OnSetWindowPos(HWND target, HWND after, int x, int y, int width, int height, UINT flags) {
  if (target != window || (flags & SWP_NOSIZE)) return gameSetWindowPos(target, after, x, y, width, height, flags);
  RECT screen = Screen(MonitorFromWindow(target, MONITOR_DEFAULTTOPRIMARY));
  return gameSetWindowPos(target, after, screen.left, screen.top, screen.right - screen.left,
                          screen.bottom - screen.top, flags & ~SWP_NOMOVE);
}

}  // namespace

void Install(HMODULE game) {
  hook::Import(game, "d3d9.dll", "Direct3DCreate9", OnDirect3DCreate9, gameDirect3DCreate9);
  hook::Import(game, "user32.dll", "CreateWindowExA", OnCreateWindowExA, gameCreateWindowExA);
  hook::Import(game, "user32.dll", "SetWindowPos", OnSetWindowPos, gameSetWindowPos);
}

}  // namespace borderless
