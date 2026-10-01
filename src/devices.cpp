#include "devices.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <cstdlib>

#include "hook.h"

namespace devices {
namespace {

// The game creates one GUID_SysMouse device, exclusive and foreground, absolute axes, 16-event
// buffer, and acquires it whenever its window is in front, but never reads it. Exclusive
// DirectInput sits on Raw Input underneath, so nothing else in the process can listen; the
// buffer is drained here instead. The cursor is hidden by the exclusive acquire itself.
constexpr int kSlot_CreateDevice = 3;
constexpr DWORD kEventBuffer = 16;

using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A*, REFGUID, IDirectInputDevice8A**, LPUNKNOWN);

CreateDeviceFn gameCreateDevice;
IDirectInputDevice8A* mouse;
float pendingX, pendingY;
float travel;    // movement since the last TakeActivity, in counts
bool clicked;    // a button went down since the last TakeActivity
float aimX, aimY;
LONG lastX, lastY;  // the absolute axis values last seen, deltas come from the change
bool seen;

HRESULT STDMETHODCALLTYPE OnCreateDevice(IDirectInput8A* self, REFGUID guid, IDirectInputDevice8A** out,
                                         LPUNKNOWN outer) {
  HRESULT result = gameCreateDevice(self, guid, out, outer);
  if (SUCCEEDED(result) && out && *out && IsEqualGUID(guid, GUID_SysMouse)) mouse = *out;
  return result;
}

}  // namespace

void Attach(void* input) {
  hook::Vtable(input, kSlot_CreateDevice, OnCreateDevice, gameCreateDevice);
}

void Tick() {
  aimX = aimY = 0.0f;
  if (!mouse) return;
  DIDEVICEOBJECTDATA events[kEventBuffer];
  DWORD count = kEventBuffer;
  HRESULT result = mouse->GetDeviceData(sizeof(events[0]), events, &count, 0);
  if (result == DIERR_INPUTLOST || result == DIERR_NOTACQUIRED) {
    mouse->Acquire();  // the game's own acquire lapses when the window loses focus
    seen = false;
    return;
  }
  if (FAILED(result)) return;
  for (DWORD i = 0; i < count; ++i) {
    auto value = static_cast<LONG>(events[i].dwData);
    if (events[i].dwOfs == DIMOFS_X) {
      if (seen) {
        pendingX += static_cast<float>(value - lastX);
        aimX += static_cast<float>(value - lastX);
        travel += static_cast<float>(abs(value - lastX));
      }
      lastX = value;
    } else if (events[i].dwOfs == DIMOFS_Y) {
      if (seen) {
        pendingY += static_cast<float>(value - lastY);
        aimY += static_cast<float>(value - lastY);
        travel += static_cast<float>(abs(value - lastY));
      }
      lastY = value;
    } else {
      if (events[i].dwOfs >= DIMOFS_BUTTON0 && events[i].dwOfs <= DIMOFS_BUTTON7 && (value & 0x80)) clicked = true;
      continue;
    }
    seen = true;
  }
}

void TakeMouse(float& dx, float& dy) {
  dx = pendingX;
  dy = pendingY;
  pendingX = pendingY = 0.0f;
}

void TakeAimMouse(float& dx, float& dy) {
  dx = aimX; dy = aimY;
  aimX = aimY = 0.0f;
}

bool TakeActivity() {
  // Threshold filters sensor drift on a resting mouse.
  constexpr float kTravel = 12.0f;
  bool active = clicked || travel >= kTravel;
  clicked = false;
  travel = 0.0f;
  return active;
}

}  // namespace devices
