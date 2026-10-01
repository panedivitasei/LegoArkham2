#include "device_follow.h"

#include <windows.h>
#include <xinput.h>

#include <cstdint>
#include <cstring>

#include "devices.h"
#include "input_frame.h"
#include "keybinds.h"

namespace devicefollow {
namespace {

// Per player {device slot, flags}, low flag byte = has a device. Slots 0-3 are the XInput pads,
// 10-13 the keyboards.
constexpr uintptr_t kPlayerDevices = 0x11CE670;
constexpr uintptr_t kDeviceIsPlayer = 0x11CC18C;  // when set the game ignores the table above
constexpr uintptr_t kInputPtr = 0x13640E4;
constexpr int kSlotStride = 380;
constexpr int kSlotPadType = 20032;  // nonzero while the pad answers XInputGetState
constexpr int kPads = 4;
constexpr uintptr_t kKeyStates = 0x1364778;  // two 256-byte scancode arrays
constexpr uintptr_t kKeyStateIndex = 0x13655A4;

// Reassignment as done by the Control Setup screen: bind the device profile, then map the player.
constexpr uintptr_t kBindProfile = 0x63A560;     // cdecl(slot, player, player)
constexpr uintptr_t kAssignDevice = 0x53F2F0;    // cdecl(player, slot)
constexpr uintptr_t kKeyboardSlotFor = 0x6386C0;  // cdecl(slot, player): which keyboard slot a player uses

using BindProfileFn = int(__cdecl*)(int, int, int);
using AssignDeviceFn = int(__cdecl*)(int, int);
using KeyboardSlotForFn = int(__cdecl*)(int, int);
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

GetStateFn getState;
bool padHeld[kPads];
uint8_t keysBefore[256];

template <class T>
T& At(uintptr_t address) {
  return *reinterpret_cast<T*>(address);
}

bool HasDevice(int player) { return At<uint8_t>(kPlayerDevices + 8 * player + 4) != 0; }
int Device(int player) { return At<int>(kPlayerDevices + 8 * player); }
bool IsPad(int slot) { return slot >= 0 && slot < kPads; }
bool IsKeyboard(int slot) { return slot >= 10 && slot <= 13; }

bool PadConnected(uintptr_t input, int pad) { return At<int>(input + kSlotStride * pad + kSlotPadType) != 0; }

bool Assign(int player, int slot) {
  if (!keybinds::MayFollowDevice(player, slot)) return false;
  reinterpret_cast<BindProfileFn>(kBindProfile)(slot, player, player);
  reinterpret_cast<AssignDeviceFn>(kAssignDevice)(player, slot);
  keybinds::FollowDevice(player, slot);
  return true;
}

// Any button, a trigger, or a stick past half deflection; smaller stick values are drift.
bool PadInUse(const XINPUT_STATE& state) {
  const XINPUT_GAMEPAD& pad = state.Gamepad;
  static constexpr SHORT kStick = 16000;
  constexpr BYTE kTrigger = 100;
  auto away = [](SHORT v) { return v > kStick || v < -kStick; };
  return pad.wButtons || pad.bLeftTrigger > kTrigger || pad.bRightTrigger > kTrigger || away(pad.sThumbLX) ||
         away(pad.sThumbLY) || away(pad.sThumbRX) || away(pad.sThumbRY);
}

// Device whose use began this frame, or -1. Onsets only, so input held on the previous device is
// ignored.
int FreshDevice(uintptr_t input, int keyboard, int skip) {
  int fresh = -1;
  auto keys = reinterpret_cast<const uint8_t*>(kKeyStates + 256 * At<uint8_t>(kKeyStateIndex));
  bool keyOnset = false;
  for (int i = 0; i < 256; ++i) keyOnset |= keys[i] && !keysBefore[i];
  memcpy(keysBefore, keys, sizeof(keysBefore));
  if ((devices::TakeActivity() || keyOnset) && keyboard != skip) fresh = keyboard;
  for (int pad = 0; pad < kPads; ++pad) {
    XINPUT_STATE state{};
    bool held = PadConnected(input, pad) && getState(pad, &state) == ERROR_SUCCESS && PadInUse(state);
    if (held && !padHeld[pad] && pad != skip && fresh < 0) fresh = pad;
    padHeld[pad] = held;
  }
  return fresh;
}

void Update() {
  uintptr_t input = At<uintptr_t>(kInputPtr);
  if (!input || !getState || At<int>(kDeviceIsPlayer)) return;
  int keyboard = reinterpret_cast<KeyboardSlotForFn>(kKeyboardSlotFor)(10, 0);
  bool second = HasDevice(1) && Device(1) >= 0;
  int fresh = FreshDevice(input, keyboard, second ? Device(1) : -1);
  if (!HasDevice(0) || Device(0) < 0) return;

  // Player 2 on a keyboard with a pad connected: move player 2 to a spare pad, or swap with player 1's.
  // Player 1 is reassigned first because reassignment clears the previous slot's owner.
  if (second && IsKeyboard(Device(1))) {
    int spare = -1;
    for (int pad = 0; pad < kPads && spare < 0; ++pad)
      if (PadConnected(input, pad) && pad != Device(0)) spare = pad;
    if (spare >= 0) {
      Assign(1, spare);
    } else if (IsPad(Device(0))) {
      int pad = Device(0);
      if (Assign(0, keyboard)) Assign(1, pad);
    }
    return;
  }
  if (fresh >= 0 && fresh != Device(0)) Assign(0, fresh);
}

}  // namespace

void Install() {
  // Same XInput DLL the game imports.
  if (HMODULE xinput = LoadLibraryA("xinput1_3.dll"))
    getState = reinterpret_cast<GetStateFn>(GetProcAddress(xinput, "XInputGetState"));
  inputframe::OnFrame(Update);
}

}  // namespace devicefollow
