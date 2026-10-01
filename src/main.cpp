#include <windows.h>
#include <unknwn.h>

#include <cstring>
#include <string>

#include "borderless.h"
#include "cape_sound.h"
#include "chase_camera.h"
#include "climb.h"
#include "surface_run.h"
#include "water_run.h"
#include "cutscene.h"
#include "frame.h"
#include "glide.h"
#include "glide_anim.h"
#include "grapple.h"
#include "keybinds.h"
#include "ability_controls.h"
#include "devices.h"
#include "device_follow.h"
#include "quit.h"
#include "reticle.h"
#include "flash_jump.h"
#include "flash_roll.h"

namespace {

std::string iniPath;

int Setting(const char* section, const char* key, int fallback) {
  return GetPrivateProfileIntA(section, key, fallback, iniPath.c_str());
}

std::string Text(const char* section, const char* key, const char* fallback) {
  char value[512];
  GetPrivateProfileStringA(section, key, fallback, value, sizeof(value), iniPath.c_str());
  return value;
}

// The Steam stub unpacks the exe's code and data after our DllMain has run, so everything that
// reads or patches the exe waits for the game's own DirectInput8Create call, which comes from
// inside that code.
void InstallCodeHooks() {
  static bool done;
  if (done) return;
  done = true;
  abilitycontrols::Install(iniPath.substr(0, iniPath.find_last_of('\\') + 1) + "LegoArkham2_controls.ini");
  if (Setting("Camera", "Chase", 1))
    chasecam::Install(true, Setting("Camera", "Distance", 80), Setting("Camera", "MouseLook", 1) != 0,
                      Setting("Camera", "MouseSensitivity", 100), Setting("Camera", "InvertMouse", 0) != 0,
                      Setting("Camera", "Collision", 1) != 0);
  if (Setting("Gameplay", "SkipCutscenes", 1)) cutscene::Install();
  if (Setting("Gameplay", "Glide", 1)) {
    char list[512];
    GetPrivateProfileStringA("Gameplay", "GlideCharacters", "Batman,Robin,Batgirl,BatgirlClassic", list, sizeof(list),
                             iniPath.c_str());
    glide::Install(list);
    glide::Speed(Setting("Gameplay", "GlideSpeed", 225));
    char clip[MAX_PATH];
    GetPrivateProfileStringA("Gameplay", "GlideClip", "LegoArkham2_glide.an4", clip, sizeof(clip), iniPath.c_str());
    std::string folder = iniPath.substr(0, iniPath.find_last_of('\\') + 1);
    glideanim::Install(folder + clip, Text("Gameplay", "GlideClipEntrySize", "13117"),
                       Text("Gameplay", "GlideClipSize", "19022"),
                       Setting("Gameplay", "GlideClipLog", 0) ? folder + "LegoArkham2_clips.log" : "");
    char fallClip[MAX_PATH];
    GetPrivateProfileStringA("Gameplay", "FallClip", "", fallClip, sizeof(fallClip), iniPath.c_str());
    glideanim::FallClip(folder + fallClip, Text("Gameplay", "FallClipEntrySize", "15906"),
                        Text("Gameplay", "FallClipSize", "21135"));
    glide::FallSpeed(Setting("Gameplay", "FallSpeed", 70));
    glide::Ease(Setting("Gameplay", "AnimEase", 250));
    glide::DiveSetup(Setting("Gameplay", "DiveGain", 60),
                     Setting("Gameplay", "DiveMax", 150), Setting("Gameplay", "DiveDecay", 40),
                     Setting("Gameplay", "DiveDescent", 80), Setting("Gameplay", "DiveLift", 25),
                     Setting("Gameplay", "DiveForward", 0), Setting("Gameplay", "DiveLiftTime", 15),
                     Setting("Gameplay", "DiveEase", 500), Setting("Gameplay", "DivePullOut", 8),
                     Setting("Gameplay", "DiveTuckTime", 4), Setting("Gameplay", "DiveCurve", 30),
                     Setting("Gameplay", "DiveAttack", 50));
    glide::DivePitch(Setting("Gameplay", "DivePitch", 60), Setting("Gameplay", "DiveClimbPitch", 12));
    if (Setting("Gameplay", "GlideUnlimited", 1)) glide::Unlimited();
    glide::RollGlide(Setting("Gameplay", "RollGlideHold", 10));
    glide::Landing(Setting("Gameplay", "DiveLanding", 1) != 0);
    char diveSound[MAX_PATH], openSound[MAX_PATH], loopSound[MAX_PATH];
    GetPrivateProfileStringA("Gameplay", "DiveSound", "", diveSound, sizeof(diveSound), iniPath.c_str());
    GetPrivateProfileStringA("Gameplay", "GlideOpenSound", "", openSound, sizeof(openSound), iniPath.c_str());
    GetPrivateProfileStringA("Gameplay", "GlideLoopSound", "", loopSound, sizeof(loopSound), iniPath.c_str());
    capesound::Install(diveSound[0] ? folder + diveSound : "", openSound[0] ? folder + openSound : "",
                       loopSound[0] ? folder + loopSound : "", Setting("Gameplay", "CapeVolume", 700));
    char diveClip[MAX_PATH], divers[256];
    GetPrivateProfileStringA("Gameplay", "DiveClip", "LegoArkham2_dive.an4", diveClip, sizeof(diveClip), iniPath.c_str());
    GetPrivateProfileStringA("Gameplay", "DiveClipCharacters", "Batman,Robin,Batgirl,BatgirlClassic", divers,
                             sizeof(divers), iniPath.c_str());
    if (diveClip[0]) {
      glideanim::DiveClip(folder + diveClip);
      glide::DiveClip(divers);
      char pullout[MAX_PATH], pulloutDonor[64], tuck[MAX_PATH], tuckDonor[64];
      GetPrivateProfileStringA("Gameplay", "PulloutClip", "LegoArkham2_pullout.an4", pullout, sizeof(pullout), iniPath.c_str());
      GetPrivateProfileStringA("Gameplay", "PulloutClipDonor", "WhereDidHeGo", pulloutDonor, sizeof(pulloutDonor), iniPath.c_str());
      GetPrivateProfileStringA("Gameplay", "TuckClip", "LegoArkham2_tuck.an4", tuck, sizeof(tuck), iniPath.c_str());
      GetPrivateProfileStringA("Gameplay", "TuckClipDonor", "Whip_Pull_Pull", tuckDonor, sizeof(tuckDonor), iniPath.c_str());
      if (pullout[0] && pulloutDonor[0])
        glideanim::PulloutClip(folder + pullout, Text("Gameplay", "PulloutClipEntrySize", "21826"),
                               Text("Gameplay", "PulloutClipSize", "30113"));
      if (tuck[0] && tuckDonor[0])
        glideanim::TuckClip(folder + tuck, Text("Gameplay", "TuckClipEntrySize", "26510"),
                            Text("Gameplay", "TuckClipSize", "35645"));
      glide::TransitionClips(pullout[0] ? pulloutDonor : "", Setting("Gameplay", "PulloutFrames", 24),
                             tuck[0] ? tuckDonor : "", Setting("Gameplay", "TuckFrames", 12));
    }
    std::string overrideClip = Text("Gameplay", "GlideOverrideClip", "LegoArkham2_glide_robin.an4");
    if (!overrideClip.empty()) {
      glideanim::GlideOverrideClip(folder + overrideClip);
      glide::GlideOverride(Text("Gameplay", "GlideOverrideCharacters", "Robin"));
    }
    if (Setting("Gameplay", "GrappleAnywhere", 1))
      grapple::Install(Setting("Gameplay", "GrappleRange", 14), Setting("Gameplay", "GrappleHeight", 30));
    if (Setting("Gameplay", "ClimbAnywhere", 1))
      climb::Install(Setting("Gameplay", "ClimbMode", 2), Setting("Gameplay", "ClimbRange", 12),
                     Setting("Gameplay", "ClimbKey", 0));
    climb::Speed(Setting("Gameplay", "ClimbSpeed", 200));
    if (Setting("Gameplay", "FlashSurfaceRun", 1))
      surfacerun::Install(Setting("Gameplay", "FlashSurfaceSpeed", 75));
    if (Setting("Gameplay", "FlashWaterRun", 1))
      waterrun::Install(Setting("Gameplay", "FlashWaterSplash", 1) != 0);
    flashjump::Install(Setting("Gameplay", "FlashSprintJump", 300));
    if (Setting("Gameplay", "FlashRoll", 1))
      flashroll::Install(folder + "LegoArkham2_flash_roll.an4", Setting("Gameplay", "FlashRollOffLedges", 1) != 0);
  }
  reticle::Install(iniPath.substr(0, iniPath.find_last_of('\\') + 1) + "LegoArkham2_controls.ini");
  keybinds::Install(Setting("Controls", "ArkhamKeys", 1) != 0);
  keybinds::Attach();
  if (Setting("Controls", "WasdMenus", 1)) keybinds::WasdMenus();
  if (Setting("Controls", "DeviceSwitch", 1)) devicefollow::Install();
}

}  // namespace

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out,
                                             LPUNKNOWN outer) {
  static const auto real = [] {
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat_s(path, "\\dinput8.dll");
    return reinterpret_cast<decltype(&DirectInput8Create)>(GetProcAddress(LoadLibraryA(path), "DirectInput8Create"));
  }();
  InstallCodeHooks();
  HRESULT result = real ? real(instance, version, riid, out, outer) : E_FAIL;
  // IID_IDirectInput8A and IID_IDirectInput8W share a vtable layout, so either can be watched.
  if (SUCCEEDED(result) && out && *out && (riid.Data1 == 0xBF798030 || riid.Data1 == 0xBF798031)) devices::Attach(*out);
  return result;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  DisableThreadLibraryCalls(self);

  iniPath.assign(MAX_PATH, '\0');
  iniPath.resize(GetModuleFileNameA(self, iniPath.data(), MAX_PATH));
  iniPath = iniPath.substr(0, iniPath.find_last_of('\\') + 1) + "LegoArkham2.ini";

  HMODULE game = GetModuleHandleA(nullptr);
  if (Setting("Display", "Borderless", 1)) borderless::Install(game);
  quit::Install(game);
  frame::OnTick(devices::Tick);
  return TRUE;
}
