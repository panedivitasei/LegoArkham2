#include "reticle.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include "devices.h"
#include "chase_camera.h"
#include "glide_anim.h"
#include "hook.h"
#include "input_frame.h"
#include "player_character.h"

namespace reticle {
namespace {
template<class T> T& At(uintptr_t p, int offset = 0) { return *reinterpret_cast<T*>(p + offset); }
using CursorFn = void(__cdecl*)(int, float*, float*, float);
using PauseFn = void(__cdecl*)(int);
using RowFn = void(__cdecl*)(int, const char*, uint8_t);
using DeviceFn = int(__cdecl*)(int, int);
std::string settings;
constexpr int kOptions = 7;
constexpr std::array<int,kOptions> kDefaults = {40,100,250,100,500,1,0};
auto percentages = kDefaults;
const char* keys[] = {"MouseReticleSensitivity", "MouseSensitivity", "GamepadReticleSensitivity", "GamepadLookSensitivity", "FlyingReticleSensitivity", "HideReticleWhenFlying", "AlwaysHideReticle"};
const char* sections[] = {"Reticle", "Camera", "Reticle", "Camera", "Reticle", "Reticle", "Reticle"};
const char* labels[] = {"Mouse Reticle Sensitivity", "Mouse Look Sensitivity", "Gamepad Reticle Sensitivity", "Gamepad Look Sensitivity", "Flying Reticle Sensitivity", "Hide Reticle When Flying", "Always Hide Reticle"};
int drawSelection;
float mouseX, mouseY;
int mouseCharacter;
bool mouseTaken;
void Save(int which, int delta) {
  int next = (which < 5) ? std::clamp(percentages[which]+delta,10,500) : !percentages[which];
  if (next == percentages[which]) return;
  percentages[which] = next;
  if (which == 1) chasecam::SetMouseSensitivity(next);
  if (which == 3) chasecam::SetGamepadSensitivity(next);
  char value[16];
  snprintf(value,sizeof(value),"%d",next);
  WritePrivateProfileStringA(sections[which],keys[which],value,settings.c_str());
  using SoundFn = int(__thiscall*)(void*, const int16_t*);
  At<int>(0x1190B28) = static_cast<int16_t>(reinterpret_cast<SoundFn>(0x8DF350)(
    At<void*>(0x11BDF28),reinterpret_cast<const int16_t*>(0x11AC9B8)));
}

// Pause menu 4 counts rows through its draw callback, including the initialization pass.
void __cdecl OnBackRow(int menu, const char* back, uint8_t alpha) {
  At<int16_t>(menu,10) = static_cast<int16_t>(drawSelection);
  auto draw = reinterpret_cast<RowFn>(0x71ECA0);
  for (int i=0;i<kOptions;++i) {
    char value[96];
    if (i < 5) snprintf(value,sizeof(value),"%s: %d%%",labels[i],percentages[i]);
    else snprintf(value,sizeof(value),"%s: %s",labels[i],percentages[i] ? "On" : "Off");
    draw(menu,value,alpha);
  }
  draw(menu,back,alpha);
}

void __cdecl OnPauseDraw(int menu) {
  drawSelection = At<int16_t>(menu,10);
  bool counting = At<int>(0x1379984) != 0;
  int last = At<int16_t>(menu,18);
  if (!counting && drawSelection >= last-kOptions)
    At<int16_t>(menu,10) = static_cast<int16_t>(drawSelection == last ? last-kOptions : -1);
  float font = At<float>(menu,220), spacing = At<float>(menu,224);
  At<float>(menu,220) = At<float>(0x1190B5C)*0.8f;
  At<float>(menu,224) = At<float>(0x1190B64)*0.8f;
  At<float>(menu,164) *= 0.8f;
  reinterpret_cast<PauseFn>(0x917040)(menu);
  At<int16_t>(menu,10) = static_cast<int16_t>(drawSelection);
  At<float>(menu,220) = font; At<float>(menu,224) = spacing;
}

void __cdecl OnPauseUpdate(int menu) {
  int selected = At<int>(menu,64);
  int first = At<int16_t>(menu,18)-kOptions;
  bool custom = selected >= first && selected < first+kOptions;
  if (custom) {
    if (!At<int>(menu,116)) {
      if (At<int>(menu,104)) Save(selected-first,-10);
      else if (At<int>(menu,108)) Save(selected-first,10);
    }
    At<int>(menu,64) = -1;
  } else if (selected >= first+kOptions) {
    At<int>(menu,64) = selected-kOptions;
  }
  reinterpret_cast<PauseFn>(0x966D50)(menu);
  At<int>(menu,64) = selected;
  if (custom) At<uint8_t>(0x137997E) = 1;
}

bool HideForCharacter(int character) {
  if (percentages[6]) return true;
  if (!percentages[5] || !character) return false;
  // 4EC990 queries the active HubFlying and LevelFlying add-ons at character+4664.
  using FindFn = int(__thiscall*)(void*, const void*);
  auto find = reinterpret_cast<FindFn>(0x865FC0);
  return find(reinterpret_cast<void*>(character+4664),reinterpret_cast<void*>(0x11BDB2C))
    || find(reinterpret_cast<void*>(character+4664),reinterpret_cast<void*>(0x11BDB60));
}

void __cdecl OnReticleDraw(int context, int sprite, float* position, float scale) {
  // 93CF50 resolves sprite+198 through the character table before drawing.
  int slot = At<int8_t>(sprite,198);
  int character = slot >= 0 && slot < 20 ? At<int>(0x138F4E0+4*slot) : 0;
  if (HideForCharacter(character)) return;
  using DrawFn = void(__cdecl*)(int,int,float*,float);
  reinterpret_cast<DrawFn>(0x93CF50)(context,sprite,position,scale);
}

template<int Offset>
void __fastcall OnFlyingInput(float* cursor, void*, int position, int camera) {
  int character = At<int>(reinterpret_cast<uintptr_t>(cursor)-Offset,68);
  int player = playercharacter::HumanSlot(character);
  int device = player >= 0 ? reinterpret_cast<DeviceFn>(0x533380)(player,1) : -1;
  int input = player >= 0 ? At<int>(character,1992) : 0;
  bool scale = input && device >= 0 && device <= 13;
  float amount = scale ? At<float>(input,48) : 0.0f;
  // 45C0B0 derives the flying cursor from movement magnitude, not the shared aim axes.
  if (scale) At<float>(input,48) = amount*percentages[4]/100.0f;
  using InputFn = void(__thiscall*)(float*,int,int);
  reinterpret_cast<InputFn>(0x45C0B0)(cursor,position,camera);
  if (scale) At<float>(input,48) = amount;
}

void __fastcall OnFlyingCursor(float* cursor, void*, int position, int camera) {
  // 45C300 updates flight targeting before testing cursor+8 for the 3D cursor draw.
  float alpha = cursor[2];
  if (percentages[5] || percentages[6]) cursor[2] = 0.0f;
  using FlyingFn = void(__thiscall*)(float*,int,int);
  reinterpret_cast<FlyingFn>(0x45C300)(cursor,position,camera);
  cursor[2] = alpha;
}

constexpr uintptr_t drawPointers[] = {
  0x44A063,0x45266B,0x46CD7D,0x48AE11,0x93D2BC,0x93D4CB,
  0x93D6ED,0x93D82E,0x93DBFB,0x93DC93,0x93DE82,0x93E266,
  0x94659B,0x946A49,0x99FFDB,0xA13E4E,0xA140A7,0xA4240C
};

bool PatchPointer(uintptr_t address, uintptr_t expected, uintptr_t replacement) {
  auto b = [](uintptr_t v, int n) { return static_cast<BYTE>(v>>(8*n)); };
  return hook::Patch(address,{b(expected,0),b(expected,1),b(expected,2),b(expected,3)},
    {b(replacement,0),b(replacement,1),b(replacement,2),b(replacement,3)});
}

void BeginFrame() {
  devices::TakeAimMouse(mouseX,mouseY);
  mouseTaken = false;
  mouseCharacter = 0;
}

struct AimInput {
  int input=0;
  float x=0,y=0;
  bool injected=false;
};
AimInput BeginAim(int character,float* velocity,float speed) {
  int player = playercharacter::HumanSlot(character);
  if (player < 0) return {};
  int input = At<int>(character,1992);
  float savedX = At<float>(input,52), savedY = At<float>(input,56);
  int device = reinterpret_cast<DeviceFn>(0x533380)(player,1);
  uintptr_t manager = At<uintptr_t>(0x13640E4);
  bool mouse = device >= 10 && device <= 13 && manager && At<int>(manager,26264) == player;
  bool injected = false;
  if (device >= 0 && device < 10) {
    At<float>(input,52) *= percentages[2]/100.0f;
    At<float>(input,56) *= percentages[2]/100.0f;
  } else if (mouse) {
    mouseCharacter = character;
    float discardedX, discardedY;
    devices::TakeMouse(discardedX,discardedY);
    float dt = static_cast<float>(reinterpret_cast<double(__cdecl*)()>(0x8B4120)());
    if (!mouseTaken && dt > 0.0f && (mouseX != 0.0f || mouseY != 0.0f)) {
      mouseTaken = injected = true;
      float gain = percentages[0]/(100.0f*128.0f*dt);
      At<float>(input,52) = mouseX*gain;
      At<float>(input,56) = -mouseY*gain;
      if (At<float>(character,1188)<0.25f) speed *= At<float>(character,1188)*4.0f;
      float width = 1.0f;
      int viewport = At<int>(character,5984);
      if (viewport && !At<uint8_t>(viewport,436) && !At<uint8_t>(0x1398F73))
        width = At<float>(viewport,108);
      // Match A0C0D0's target velocity so its stick smoothing does not add mouse lag.
      velocity[0] = At<float>(input,52)*speed/width;
      velocity[1] = At<float>(input,56)*speed;
    }
  }
  return {input,savedX,savedY,injected};
}
void EndAim(const AimInput& saved,float* velocity) {
  if (!saved.input) return;
  At<float>(saved.input,52)=saved.x;At<float>(saved.input,56)=saved.y;
  if (saved.injected) velocity[0]=velocity[1]=0.0f;
}
void __cdecl OnCursor(int character,float* position,float* velocity,float targetTime) {
  auto saved=BeginAim(character,velocity,At<float>(0x11A1730));
  reinterpret_cast<CursorFn>(0xA0C0D0)(character,position,velocity,targetTime);
  EndAim(saved,velocity);
}

AimInput batarangInput;
void __cdecl BeginBatarang(int character) {
  // B15E00 integrates its own cursor at character+2020, with native speed 1.25.
  int aim=At<int>(character,2020);
  batarangInput=BeginAim(character,reinterpret_cast<float*>(aim+584),1.25f);
}
void __declspec(naked) BatarangInputBridge() {
  __asm {
    pushfd
    pushad
    push esi
    call BeginBatarang
    add esp,4
    popad
    popfd
    mov eax,[esi+1992]
    ret
  }
}
int __cdecl OnBatarangBounds(float* position,float* velocity,int character) {
  EndAim(batarangInput,velocity);
  batarangInput={};
  using BoundsFn=int(__cdecl*)(float*,float*,int);
  return reinterpret_cast<BoundsFn>(0x9084E0)(position,velocity,character);
}

} // namespace

bool MouseActive(int character) { return character && mouseCharacter == character; }

void Install(const std::string& settingsPath) {
  settings = settingsPath;
  for (int i=0;i<kOptions;++i) {
    int fallback = i==1 ? chasecam::MouseSensitivity() : kDefaults[i];
    int value = static_cast<int>(GetPrivateProfileIntA(sections[i],keys[i],fallback,settings.c_str()));
    percentages[i] = (i<5) ? std::clamp(value,10,500) : value!=0;
  }
  chasecam::SetMouseSensitivity(percentages[1]);
  chasecam::SetGamepadSensitivity(percentages[3]);
  struct Site { uintptr_t at, target; void* replacement; };
  const Site sites[] = {
    {0xB16768,0x9084E0,reinterpret_cast<void*>(OnBatarangBounds)},
    {0x50ED5F,0x45C0B0,reinterpret_cast<void*>(OnFlyingInput<464>)},
    {0x50EAD1,0x45C0B0,reinterpret_cast<void*>(OnFlyingInput<224>)},
    {0x45E920,0x45C0B0,reinterpret_cast<void*>(OnFlyingInput<224>)},
    {0x45F8A3,0x45C300,reinterpret_cast<void*>(OnFlyingCursor)},
    {0x45E98D,0x45C300,reinterpret_cast<void*>(OnFlyingCursor)},
    {0x4FC215,0xA0C0D0,reinterpret_cast<void*>(OnCursor)},
    {0x8359F7,0xA0C0D0,reinterpret_cast<void*>(OnCursor)},
    {0xAD525E,0xA0C0D0,reinterpret_cast<void*>(OnCursor)},
    {0xADA0A8,0xA0C0D0,reinterpret_cast<void*>(OnCursor)},
    {0xAE2A6F,0xA0C0D0,reinterpret_cast<void*>(OnCursor)},
    {0xAF0986,0xA0C0D0,reinterpret_cast<void*>(OnCursor)},
    {0xAF4458,0xA0C0D0,reinterpret_cast<void*>(OnCursor)},
    {0x91767E,0x71ECA0,reinterpret_cast<void*>(OnBackRow)}
  };
  if (At<uintptr_t>(0x11A8588)!=0x917040 || At<uintptr_t>(0x11A858C)!=0x966D50) {
    glideanim::Note("reticle: pause callback mismatch; disabled");return;
  }
  for (auto site:sites)
    if (At<uint8_t>(site.at)!=0xE8 || site.at+5+At<int32_t>(site.at+1)!=site.target) {
      glideanim::Note("reticle: hook mismatch; disabled");return;
    }
  for (auto pointer:drawPointers)
    if (At<uintptr_t>(pointer)!=0x93CF50) {
      glideanim::Note("reticle: draw callback mismatch; disabled");return;
    }
  if (At<uint16_t>(0xB1659E)!=0x868B || At<uint32_t>(0xB165A0)!=1992) {
    glideanim::Note("reticle: batarang input mismatch; disabled");return;
  }
  uintptr_t relative=reinterpret_cast<uintptr_t>(&BatarangInputBridge)-(0xB1659E+5);
  hook::Patch(0xB1659E,{0x8B,0x86,0xC8,0x07,0,0},
    {0xE8,static_cast<BYTE>(relative),static_cast<BYTE>(relative>>8),static_cast<BYTE>(relative>>16),static_cast<BYTE>(relative>>24),0x90});
  for (auto pointer:drawPointers)
    PatchPointer(pointer,0x93CF50,reinterpret_cast<uintptr_t>(OnReticleDraw));
  for (auto site:sites) hook::Call(site.at,reinterpret_cast<void*>(site.target),site.replacement);
  PatchPointer(0x11A8588,0x917040,reinterpret_cast<uintptr_t>(OnPauseDraw));
  PatchPointer(0x11A858C,0x966D50,reinterpret_cast<uintptr_t>(OnPauseUpdate));
  // 716BE0 copies game menu 4 into registered slot 9 after the five built-in menus.
  constexpr uintptr_t registered = 0x1190B88 + 9*28;
  if (At<int>(registered)==4 && At<uintptr_t>(registered,8)==0x917040 && At<uintptr_t>(registered,12)==0x966D50) {
    PatchPointer(registered+8,0x917040,reinterpret_cast<uintptr_t>(OnPauseDraw));
    PatchPointer(registered+12,0x966D50,reinterpret_cast<uintptr_t>(OnPauseUpdate));
  }
  inputframe::OnFrame(BeginFrame);
}
} // namespace reticle
