#pragma once

namespace keybinds {

// Gives the keyboard-and-mouse profiles an Arkham layout and adds a "Camera toggle" row to the
// game's Control Configuration screen. Needs the exe unpacked: call once its code runs.
void Install(bool arkhamKeys);

// Hooks the layout installer, same requirement.
void Attach();

// W, A, S and D navigate menus alongside the arrow keys, through the game's own menu input.
void WasdMenus();

// Whether the key with this DirectInput scancode is down, from the game's own keyboard state.
bool KeyDown(unsigned char scancode);

// Automatic reassignment must preserve Control Setup's pending edits and cached device data.
bool MayFollowDevice(int player, int slot);
void FollowDevice(int player, int slot);

}  // namespace keybinds
