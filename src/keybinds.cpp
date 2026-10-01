#include "keybinds.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "ability_controls.h"
#include "frame.h"
#include "hook.h"
#include "input_frame.h"

namespace keybinds {
namespace {

// Steam LB2: 156-byte profiles with 16 {kind, code} buttons at +124 and 380-byte device slots.
constexpr uintptr_t kDefaultRecords = 0x1151BB0;  // 13 built-in profiles, name at +0, type at +64
constexpr int kRecordSize = 156;
constexpr int kRecordButtons = 124;
constexpr uintptr_t kInputPtr = 0x13640E4;
constexpr int kSlotRecords = 20032;
constexpr int kSlotStride = 380;
constexpr int kSlotBindings = 112;  // {kind, pad, code16} per output entry
constexpr int kMouseRecords[] = {7, 8};       // KeyboardMouse1/2
constexpr int kKeyboardRecords[] = {7, 8, 9, 10};
// Engine bits, as the record's button index. Entries 12-15 are bits 4-7, 18 is bit 3, 23 bit 11;
// bit 9 is entry 20, which the game binds but never shows or reads.
constexpr int kToggleUp = 3;
constexpr int kTag = 4;
constexpr int kSpecial = 5;
constexpr int kJump = 6;
constexpr int kAction = 7;
constexpr int kCameraToggle = 9;
constexpr int kStart = 11;
// The device update centres the stick axes of the slot whose player owns the mouse, and that
// player then moves on the four d-pad bits, which the game binds to the arrow keys. WASD go
// there, and come off the movement axes at +100 (three bytes per axis: kind, negative key,
// positive key), or a key on both paths leaves the character facing sideways when it stops.
constexpr int kRecordAxisKeys = 100;
constexpr int kMoveAxisBytes = 6;  // x then y
constexpr int kPadUp = 12;
constexpr int kPadRight = 13;
constexpr int kPadDown = 14;
constexpr int kPadLeft = 15;
constexpr int kCameraToggleEntry = 20;
constexpr uint8_t kKind_Key = 7;
constexpr uint8_t kKind_Mouse = 4;
constexpr uint8_t kDikEscape = 0x01;
constexpr uint8_t kDik2 = 0x03;
constexpr uint8_t kDikW = 0x11;
constexpr uint8_t kDikE = 0x12;
constexpr uint8_t kDikA = 0x1E;
constexpr uint8_t kDikS = 0x1F;
constexpr uint8_t kDikD = 0x20;
constexpr uint8_t kDikSpace = 0x39;
constexpr uint8_t kDikF1 = 0x3B;

// The PC init installs the plain keyboard layouts from tables after the profiles were first
// read, so ours go in again behind it.
constexpr uintptr_t kLayoutInstallCallSite = 0x96A205;
constexpr uintptr_t kLayoutInstall = 0x63A490;

// The Control Configuration screen: a null-terminated array of row lists, each row 12 bytes
// {type, output entry, label, flags, int, float} ending in label 0xFF, with labels indexed into
// a 47-entry text table the PC init resolves. Label 27 ("press Escape to cancel") is in the table
// but used by neither a row nor the screen's code, so its text becomes ours.
constexpr uintptr_t kRowListTable = 0x11AC078;
constexpr uintptr_t kGameRows = 0x11ABF40;
constexpr uintptr_t kLabels = 0x1396BD8;
constexpr int kCameraToggleLabel = 27;
constexpr int kStartEntry = 23;
constexpr int kMaxRows = 40;
constexpr char kCameraToggleName[] = "Camera toggle";

struct Row {
  uint8_t type, entry, label, axis;  // axis doubles as the flags byte: low 3 bits axis kind, rest flags
  int32_t value;
  float weight;
};
Row rows[kMaxRows];
const char* menuLabels[49];
bool arkhamLayout;

void RefreshLabels() {
  memcpy(menuLabels, reinterpret_cast<const void*>(kLabels), 47 * sizeof(const char*));
  menuLabels[kCameraToggleLabel] = kCameraToggleName;
  menuLabels[47] = "Grapple";
  menuLabels[48] = "Dive";
}

using LayoutInstallFn = char*(__cdecl*)(char*, char*, char*, char*, char*);

template <class T>
T& At(uintptr_t address) {
  return *reinterpret_cast<T*>(address);
}

uint8_t* Record(int index) { return reinterpret_cast<uint8_t*>(kDefaultRecords + kRecordSize * index); }

void Bind(uint8_t* record, int button, uint8_t kind, uint8_t code) {
  record[kRecordButtons + 2 * button] = kind;
  record[kRecordButtons + 2 * button + 1] = code;
}

// The mouse profiles get the whole layout, every keyboard the toggle and Escape on the Start row.
void ApplyDefaults() {
  // XInput maps right-stick click to button 9 in 63B6F0.
  for (int index : {1, 2}) {
    Bind(Record(index), kToggleUp, 1, 9);
    Bind(Record(index), 10, 0, 0);
  }
  if (!arkhamLayout) return;
  for (int index : kMouseRecords) {
    uint8_t* record = Record(index);
    Bind(record, kJump, kKind_Key, kDikSpace);
    Bind(record, kTag, kKind_Key, kDikE);
    Bind(record, kAction, kKind_Mouse, 0);
    Bind(record, kSpecial, kKind_Mouse, 1);
    Bind(record, kToggleUp, kKind_Key, kDik2);
    Bind(record, kPadUp, kKind_Key, kDikW);
    Bind(record, kPadRight, kKind_Key, kDikD);
    Bind(record, kPadDown, kKind_Key, kDikS);
    Bind(record, kPadLeft, kKind_Key, kDikA);
    for (int i = 0; i < kMoveAxisBytes; ++i) record[kRecordAxisKeys + i] = 0;
  }
  for (int index : kKeyboardRecords) {
    Bind(Record(index), kCameraToggle, kKind_Key, kDikF1);
    Bind(Record(index), kStart, kKind_Key, kDikEscape);
  }
}

void BuildRows() {
  auto game = reinterpret_cast<const Row*>(kGameRows);
  int n = 0;
  for (int i = 0; n < kMaxRows - 4; ++i) {
    rows[n++] = game[i];
    if (game[i].label == 0xFF) break;
    if (game[i].type == 0 && game[i].entry == kStartEntry) {
      rows[n++] = Row{0, kCameraToggleEntry, kCameraToggleLabel, 0, 0, 0.0f};
      rows[n++] = Row{0, abilitycontrols::kFirstEntry, 47, 0, 0, 0.0f};
      rows[n++] = Row{0, abilitycontrols::kFirstEntry + 1, 48, 0, 0, 0.0f};
    }
  }
  rows[kMaxRows - 1].label = 0xFF;
}

char* __cdecl OnLayoutInstall(char* a, char* b, char* c, char* d, char* reserved) {
  char* result = reinterpret_cast<LayoutInstallFn>(kLayoutInstall)(a, b, c, d, reserved);
  ApplyDefaults();
  return result;
}

// The game starts with mouse control off, so the plain keyboard slots are the active ones and
// players land on those. Its startup call that picks the active slots is made with the mouse
// switched on instead, and player 1 gets the mouse. The Control Setup screen can still change
// both afterwards.
constexpr uintptr_t kSelectKeyboardsCallSite = 0x6419F5;  // in PC startup, right after the input manager is made
constexpr uintptr_t kSelectKeyboards = 0x63A4F0;          // cdecl(twoKeyboards, mouseEnabled)
constexpr int kMouseOwner = 26264;                        // player index on the input manager, -1 for none

using SelectKeyboardsFn = int(__cdecl*)(char twoKeyboards, char mouseEnabled);

// The mouse read accumulates movement into totals only binding capture resets; past 32 they log a press
// on the keyboard-and-mouse slot every frame, keeping it busy so the title screen drops all keys. Cleared per frame.
constexpr uintptr_t kClearMouseTotals = 0x633AD0;

void ClearMouseTotals() {
  if (At<uintptr_t>(kInputPtr)) reinterpret_cast<void(__cdecl*)()>(kClearMouseTotals)();
}

int __cdecl OnSelectKeyboards(char twoKeyboards, char) {
  int result = reinterpret_cast<SelectKeyboardsFn>(kSelectKeyboards)(twoKeyboards, 1);
  uintptr_t input = At<uintptr_t>(kInputPtr);
  if (input) At<int>(input + kMouseOwner) = 0;
  return result;
}

// Each time the screen (re)reads its devices it recomputes the rows' flags: on a keyboard-and-
// mouse slot it hides the left/right movement rows, renames up/down to Walk/Run for a mouse-steer
// scheme the game doesn't run, and hides the d-pad rows on every keyboard. The d-pad entries are
// where a mouse player's movement is read, so on those slots they show up as movement rows and the
// Walk/Run pair goes away.
constexpr uintptr_t kRecomputeRows = 0x91A960;  // thiscall(screen)
constexpr uintptr_t kRecomputeRowsCallSites[] = {0x91B088, 0x91B382, 0x91B3B8, 0x91B687, 0x96A4CD};
constexpr int kScreenPages = 24;         // int16
constexpr int kScreenPlayer = 48;        // char: whose column is selected
constexpr int kScreenSlots = 50;         // int16[2]: each player's device slot, -1 for none
constexpr int kScreenPageList = 140;     // {int count, int cursor, Row* rows} per page
constexpr uintptr_t kControlScreen = 0x11AE560;
constexpr uintptr_t kMenuChecksum = 0x8C2520;
using MenuChecksumFn = int(__thiscall*)(uintptr_t);
constexpr int kRowPad = 0x08;            // row flag: a d-pad row
constexpr int kRowAxisMask = 0x07;       // 3 and 4 are the up/down movement rows
constexpr int kRowHidden = 0x20;         // per player: bit 5 for player 1, bit 6 for player 2
constexpr int kRowDim = 0x80;            // hidden for the selected player
constexpr int kPadEntryFirst = 8;        // up, right, down, left
constexpr uint8_t kMoveLabels[] = {9, 8, 10, 7};  // Move up, Move right, Move down, Move left

using RecomputeRowsFn = int(__fastcall*)(int self, int);

bool KeyboardMouseSlot(int slot) { return slot == 10 || slot == 12; }

int __cdecl OnValidateBinding(int player, int players, int kind, int code) {
  using ValidateFn = int(__cdecl*)(int, int, int, int);
  auto native = reinterpret_cast<ValidateFn>(0x638780);
  uintptr_t input = At<uintptr_t>(kInputPtr);
  int page = At<int16_t>(kControlScreen + 26);
  if (!input || kind != kKind_Key || (code != 0x3B && code != 0x3C) ||
      page < 0 || page >= At<int16_t>(kControlScreen + kScreenPages))
    return native(player, players, kind, code);
  uintptr_t list = kControlScreen + kScreenPageList + 12 * page;
  int cursor = At<int>(list + 4);
  auto rows = At<const Row*>(list + 8);
  if (!rows || cursor < 0 || cursor >= At<int>(list) || rows[cursor].type != 0 ||
      rows[cursor].entry != kCameraToggleEntry) return native(player, players, kind, code);
  // 636510 rejects reserved F1/F2 before checking other players' bindings.
  auto reserved = reinterpret_cast<uint8_t*>(input + 19999);
  uint8_t saved[33];
  memcpy(saved, reserved, sizeof(saved));
  int n = 0;
  for (int i = 0; i < 32 && saved[i]; ++i)
    if (saved[i] != code) reserved[n++] = saved[i];
  reserved[n] = 0;
  int result = native(player, players, kind, code);
  memcpy(reserved, saved, sizeof(saved));
  return result;
}

int __cdecl OnRequiredBinding(int slot, int entry, int isPlayer, int player) {
  // 8C26D0 validates hidden movement axes on join; our mouse layout uses WASD entries 8-11.
  if (arkhamLayout && isPlayer && player >= 0 && player < 2 && entry >= 0 && entry < 4) {
    uintptr_t input = At<uintptr_t>(kInputPtr);
    if (input && KeyboardMouseSlot(At<int16_t>(input + 25956 + 2 * player))) return 1;
  }
  using RequiredBindingFn = int(__cdecl*)(int, int, int, int);
  return reinterpret_cast<RequiredBindingFn>(0x6340D0)(slot, entry, isPlayer, player);
}

int __fastcall OnRecomputeRows(int self, int unused) {
  RefreshLabels();
  int result = reinterpret_cast<RecomputeRowsFn>(kRecomputeRows)(self, unused);
  int player = At<int8_t>(self + kScreenPlayer);
  int16_t slots[2] = {At<int16_t>(self + kScreenSlots), At<int16_t>(self + kScreenSlots + 2)};
  for (int page = 0; page < At<int16_t>(self + kScreenPages); ++page) {
    int count = At<int>(self + kScreenPageList + 12 * page);
    auto row = At<Row*>(self + kScreenPageList + 12 * page + 8);
    for (int i = 0; i < count; ++i, ++row) {
      if (row->type != 0) continue;
      if (row->entry >= abilitycontrols::kFirstEntry && row->entry < abilitycontrols::kFirstEntry + 2) {
        row->axis &= 0x1F;
        for (int p = 0; p < 2; ++p) if (slots[p] < 0) row->axis |= kRowHidden << p;
        if (slots[player] < 0) row->axis |= kRowDim;
        continue;
      }
      if (!arkhamLayout) continue;
      bool pad = (row->axis & kRowPad) != 0;
      bool walkRun = (row->axis & kRowAxisMask) == 3 || (row->axis & kRowAxisMask) == 4;
      if (!pad && !walkRun) continue;
      for (int p = 0; p < 2; ++p) {
        if (!KeyboardMouseSlot(slots[p])) continue;
        if (pad) row->axis &= ~(kRowHidden << p);
        else row->axis |= kRowHidden << p;
      }
      if (pad && KeyboardMouseSlot(slots[player]) && row->entry >= kPadEntryFirst && row->entry < kPadEntryFirst + 4)
        row->label = kMoveLabels[row->entry - kPadEntryFirst];
      bool hidden = (row->axis & (kRowHidden << player)) != 0;
      row->axis = (row->axis & ~kRowDim) | (hidden ? kRowDim : 0);
    }
  }
  return result;
}

// Menu navigation on a keyboard never goes through the bindings: the device update builds the
// menu button word from the keyboard state array itself, arrows for the directions, Enter and
// Space to accept, Escape and Backspace to go back. W, A, S and D join the arrows there.
constexpr uintptr_t kMenuButtonsCallSite = 0x63D9E9;  // in the device update, for every slot
constexpr uintptr_t kMenuButtons = 0x637530;          // thiscall(input, slot, buttons) -> buttons
constexpr uintptr_t kKeyStates = 0x1364778;           // two 256-byte scancode arrays
constexpr uintptr_t kKeyStateIndex = 0x13655A4;       // which of the two is current
constexpr int kMenuUp = 0x1, kMenuRight = 0x2, kMenuDown = 0x4, kMenuLeft = 0x8;

using MenuButtonsFn = int(__fastcall*)(int self, int, int slot, int buttons);

int __fastcall OnMenuButtons(int self, int unused, int slot, int buttons) {
  int result = reinterpret_cast<MenuButtonsFn>(kMenuButtons)(self, unused, slot, buttons);
  auto keys = reinterpret_cast<const uint8_t*>(kKeyStates + 256 * At<uint8_t>(kKeyStateIndex));
  if (keys[kDikW]) result |= kMenuUp;
  if (keys[kDikD]) result |= kMenuRight;
  if (keys[kDikS]) result |= kMenuDown;
  if (keys[kDikA]) result |= kMenuLeft;
  return result;
}

// The label table is filled at game init, so our text follows it in.
void Tick() {
  RefreshLabels();
}

}  // namespace

void Install(bool arkhamKeys) {
  arkhamLayout = arkhamKeys;
  ApplyDefaults();
  BuildRows();
  RefreshLabels();
  uintptr_t table = reinterpret_cast<uintptr_t>(menuLabels);
  for (uintptr_t site : {0x91C5D8, 0x91C65B, 0x91C6F7, 0x91C875})
    hook::Patch(site, {0xD8, 0x6B, 0x39, 0x01},
                {static_cast<BYTE>(table), static_cast<BYTE>(table >> 8),
                 static_cast<BYTE>(table >> 16), static_cast<BYTE>(table >> 24)});
  At<Row*>(kRowListTable) = rows;  // the table's first (and only) list
  frame::OnTick(Tick);
}

void WasdMenus() {
  hook::Call(kMenuButtonsCallSite, reinterpret_cast<void*>(kMenuButtons), reinterpret_cast<void*>(OnMenuButtons));
}

void Attach() {
  hook::Call(0x91B11F, reinterpret_cast<void*>(0x638780), reinterpret_cast<void*>(OnValidateBinding));
  hook::Call(0x8C2739, reinterpret_cast<void*>(0x6340D0), reinterpret_cast<void*>(OnRequiredBinding));
  if (arkhamLayout) {
    hook::Call(kLayoutInstallCallSite, reinterpret_cast<void*>(kLayoutInstall), reinterpret_cast<void*>(OnLayoutInstall));
    hook::Call(kSelectKeyboardsCallSite, reinterpret_cast<void*>(kSelectKeyboards),
               reinterpret_cast<void*>(OnSelectKeyboards));
  }
  inputframe::OnFrame(ClearMouseTotals);
  for (uintptr_t site : kRecomputeRowsCallSites)
    hook::Call(site, reinterpret_cast<void*>(kRecomputeRows), reinterpret_cast<void*>(OnRecomputeRows));
}

bool KeyDown(uint8_t scancode) {
  auto keys = reinterpret_cast<const uint8_t*>(kKeyStates + 256 * At<uint8_t>(kKeyStateIndex));
  return keys[scancode] != 0;
}

bool MayFollowDevice(int player, int slot) {
  if (!At<uint8_t>(kControlScreen + 6)) return true;
  if (player < 0 || player >= At<int8_t>(kControlScreen + 4) || At<uint8_t>(kControlScreen + 5) ||
      At<int>(kControlScreen + 20) != 2 || At<int>(kControlScreen + 28) != 0) return false;
  if (reinterpret_cast<MenuChecksumFn>(kMenuChecksum)(kControlScreen) != At<int>(kControlScreen + 76))
    return false;
  using ValidateFn = bool(__thiscall*)(uintptr_t, int, int, int);
  return reinterpret_cast<ValidateFn>(0x8C2640)(kControlScreen, player, slot, 1);
}

void FollowDevice(int player, int slot) {
  if (!At<uint8_t>(kControlScreen + 6)) return;
  // Match 91ACB0's Change Device refresh; 91BC80 formats bindings using these cached slots.
  At<int16_t>(kControlScreen + kScreenSlots + 2 * player) = static_cast<int16_t>(slot);
  using SlotValueFn = int(__cdecl*)(int);
  At<float>(kControlScreen + 88 + 4 * player) = static_cast<float>(reinterpret_cast<SlotValueFn>(0x6385E0)(slot));
  At<uint8_t>(kControlScreen + 128 + player) = reinterpret_cast<SlotValueFn>(0x633B00)(slot) != 0;
  if (slot >= 10 && slot <= 13)
    At<float>(kControlScreen + 120) = static_cast<float>(reinterpret_cast<SlotValueFn>(0x638610)(slot));
  OnRecomputeRows(static_cast<int>(kControlScreen), 0);
  using CheckFn = char(__thiscall*)(uintptr_t, int);
  At<uint8_t>(kControlScreen + 80) = reinterpret_cast<CheckFn>(0x8C26D0)(kControlScreen, -1);
  At<int>(kControlScreen + 32) = At<int>(kControlScreen + 36) = 3;
  reinterpret_cast<void(__cdecl*)()>(kClearMouseTotals)();
  At<int>(kControlScreen + 76) = reinterpret_cast<MenuChecksumFn>(kMenuChecksum)(kControlScreen);
}

}  // namespace keybinds
