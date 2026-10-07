#pragma once
#include <cstdint>
#include <cstring>
#include "hook.h"
#include "player_character.h"

namespace lanterngrapple {
namespace {
bool (*eligible)(int);
struct Character {
  int character, resource, item;
  bool active, owned;
};
Character characters[2]{};
thread_local int drawingCharacter;
thread_local int tintRope;
int ropeOwners[16]{};
int constructResource[4]{};
bool constructRequested;
int constructMaterial;
int materialMap[256]{};
thread_local unsigned char ropeType[96];
int Read(int address) { return *reinterpret_cast<int*>(address); }
void LoadMaterial() {
  if (constructMaterial) return;
  if (!constructRequested) {
    constructRequested = true;
    reinterpret_cast<void(__thiscall*)(int*,int,const char*)>(0x974580)
        (constructResource,0,"CUT_GIANTFIST");
  }
  int resource = reinterpret_cast<int(__thiscall*)(int*)>(0x6EAD80)(constructResource+3);
  int scene = resource ? reinterpret_cast<int(__thiscall*)(int)>(0x7EDB60)(resource+120) : 0;
  int data = scene ? Read(scene+564) : 0;
  if (!data) return;
  int count = Read(data+76), specials = Read(data+80), materials = Read(data+72);
  for (int i=0; i<count; ++i) {
    int special = specials+i*160;
    const char* name = reinterpret_cast<const char*>(Read(special+116));
    if (!name || strcmp(name,"CUT_GiantFisthh")) continue;
    int groups = Read(special+112);
    for (int g=0; g<Read(special+132); ++g) {
      int group = groups+g*12;
      int n = *reinterpret_cast<unsigned short*>(group);
      for (int j=0; j<n; ++j) {
        int material = Read(materials+4*Read(Read(group+4)+4*j));
        // The fist's untextured construct pass, verified in the native scene.
        if (!material || Read(material+2996)!=static_cast<int>(0x8C337C00u) ||
            Read(material+2676) || Read(material+4064) || Read(material+4100)!=3 ||
            (Read(material+2620)&6)) continue;
        constructMaterial = material;
        for (int& entry : materialMap) entry = material;
        return;
      }
    }
  }
}
bool Current(const Character& entry) {
  auto party = reinterpret_cast<const int*>(0x138F4E0);
  return entry.character && (party[0] == entry.character || party[1] == entry.character) &&
      Read(entry.character + 4104) == entry.resource;
}
void Destroy(int item) {
  int manager = Read(item + 32);
  if (manager) reinterpret_cast<void(__thiscall*)(int,int,int)>(0x97CE80)(manager,item,0);
}
void RemoveItem(Character& entry) {
  if (entry.item && entry.owned && Current(entry)) {
    int inventory = Read(entry.character + 5812);
    int node = inventory ? reinterpret_cast<int(__thiscall*)(int,int)>(0x8D5C20)(inventory,entry.item) : 0;
    if (node) {
      int item = reinterpret_cast<int(__thiscall*)(int,int,int,int)>(0x9858E0)(inventory,node,0,0);
      if (item) Destroy(item);
    }
  }
  entry.item = 0; entry.owned = false;
}
Character* Record(int character) {
  int slot = playercharacter::HumanSlot(character);
  if (slot < 0 || slot > 1) return nullptr;
  auto& entry = characters[slot];
  int resource = Read(character + 4104);
  if (entry.character != character || entry.resource != resource) entry = {character, resource, 0, false};
  return &entry;
}

bool OwnsRenderObject(int instance) {
  for (auto& entry : characters) {
    if (!Current(entry) || !entry.active || !entry.item || !eligible || !eligible(entry.character)) continue;
    int inventory = Read(entry.character + 5812);
    if (!inventory || !reinterpret_cast<int(__thiscall*)(int,int)>(0x8D5C20)(inventory,entry.item)) continue;
    auto entityOf = reinterpret_cast<int(__thiscall*)(int)>(Read(Read(entry.item) + 420));
    int entity = entityOf(entry.item);
    int adapter = entity ? Read(entity + 36) : 0;
    if (adapter && Read(adapter + 64) == instance) return true;
  }
  return false;
}

char __fastcall DrawItem(int instance, void*, int pass, int flags) {
  using Draw = char(__thiscall*)(int,int,int);
  if (!OwnsRenderObject(instance)) return reinterpret_cast<Draw>(0x577BC0)(instance,pass,flags);
  if (!constructMaterial) return reinterpret_cast<Draw>(0x577BC0)(instance,pass,flags);
  // The native submission resolves this table before queuing material pointers.
  alignas(16) int block[27]{};
  int saved = Read(instance+216);
  if (saved) memcpy(block,reinterpret_cast<const void*>(saved),sizeof block);
  block[23] = reinterpret_cast<int>(materialMap);
  block[24] = 256;
  *reinterpret_cast<int*>(instance+216) = reinterpret_cast<int>(block);
  char result = reinterpret_cast<Draw>(0x577BC0)(instance,pass,flags);
  *reinterpret_cast<int*>(instance+216) = saved;
  return result;
}

int __fastcall QueueRope(int manager, void*, const float* target, const void* points,
                        int count, int type, float sag, float length) {
  int index = reinterpret_cast<int(__thiscall*)(int,const float*,const void*,int,int,float,float)>(0x8A7130)
      (manager,target,points,count,type,sag,length);
  if (index >= 0 && index < 16) ropeOwners[index] = eligible && eligible(drawingCharacter) ? drawingCharacter : 0;
  return index;
}

void __cdecl DrawGrapple(int character, int partner) {
  int previous = drawingCharacter;
  drawingCharacter = character;
  reinterpret_cast<void(__cdecl*)(int,int)>(0x9F8370)(character,partner);
  drawingCharacter = previous;
}

int __fastcall ClearRope(int rope, void*) {
  int manager = Read(0x11BDFD8);
  int offset = rope - manager - 112;
  if (offset >= 0 && offset < 16*320 && offset%320 == 0) ropeOwners[offset/320] = 0;
  return reinterpret_cast<int(__thiscall*)(int)>(0x8A7030)(rope);
}

void __stdcall DrawQueuedRope(int rope, char pass) {
  int manager = Read(0x11BDFD8);
  int offset = rope - manager - 112;
  int owner = offset >= 0 && offset < 16*320 && offset%320 == 0 ? ropeOwners[offset/320] : 0;
  int previous = tintRope;
  tintRope = owner && eligible && eligible(owner);
  reinterpret_cast<void(__stdcall*)(int,char)>(0x953D30)(rope,pass);
  tintRope = previous;
}

int __fastcall RopeType(int manager, void*, short type) {
  int original = reinterpret_cast<int(__thiscall*)(int,short)>(0x69F2E0)(manager,type);
  if (!original || !tintRope || !constructMaterial) return original;
  // 953D30 uses +60 for its ribbon and +84 for its second material pass.
  memcpy(ropeType,reinterpret_cast<const void*>(original),sizeof ropeType);
  *reinterpret_cast<int*>(ropeType+60) = constructMaterial;
  if (Read(original+84)) *reinterpret_cast<int*>(ropeType+84) = constructMaterial;
  return reinterpret_cast<int>(ropeType);
}

} // namespace

inline bool AnimSet(int set) {
  for (auto& entry : characters) {
    if (!Current(entry) || !entry.active || !eligible || !eligible(entry.character)) continue;
    int animation = Read(entry.character+4100);
    if (animation && Read(animation+4)==set) return true;
  }
  return false;
}

inline void Prepare(int character) {
  if (!eligible || !eligible(character)) return;
  auto entry = Record(character);
  if (!entry) return;
  entry->active = true;
  LoadMaterial();
  int inventory = Read(character+5812);
  if (!inventory || entry->item) return;
  int existing = reinterpret_cast<int(__stdcall*)(int,int)>(0x92D410)(character,0);
  if (existing) { entry->item = Read(existing+8); return; }
  int root = reinterpret_cast<int(__cdecl*)(int)>(0x930E40)(character);
  if (!root) return;
  int definition = reinterpret_cast<int(__cdecl*)(int,const char*)>(0x4034C0)(root,"batGrapple");
  if (!definition) return;
  int item = reinterpret_cast<int(__cdecl*)(int,int,int,int,void*)>(0x9FACF0)(root,definition,0,0,nullptr);
  if (!item) return;
  // Match native inventory insertion; the grapple start routine takes the gun out.
  int node = reinterpret_cast<int(__thiscall*)(int,int,int,int,int,float,int,int)>(0x985F50)
      (inventory,item,0,0,1,0.0f,1,-1);
  if (node) { entry->item = item; entry->owned = true; }
  else Destroy(item);
}

inline void Tick(int character, bool active) {
  auto entry = Record(character);
  if (!entry) return;
  bool listed = eligible && eligible(character);
  if (listed) LoadMaterial();
  entry->active = active && listed;
  if (!entry->active) RemoveItem(*entry);
}

inline void FinishStart(int character, int gizmo) {
  if (!eligible || !eligible(character) || Read(character+1228)!=gizmo ||
      *reinterpret_cast<uint16_t*>(character+1270)!=*reinterpret_cast<uint16_t*>(0x11A2E7C) ||
      *reinterpret_cast<int16_t*>(character+1258)!=*reinterpret_cast<int16_t*>(0x11A1C30)) return;
  auto entry = Record(character);
  if (!entry || !entry->active || !entry->item) return;
  int inventory = Read(character+5812);
  int node = inventory ? reinterpret_cast<int(__thiscall*)(int,int)>(0x8D5C20)(inventory,entry->item) : 0;
  if (!node || *reinterpret_cast<uint8_t*>(node+60)!=3 || Read(inventory+36)==node) return;
  // AEF2B0 draws the gun without selecting it; 8D2680 otherwise starts the rope at the hands.
  reinterpret_cast<int(__thiscall*)(int,int,int,int,int)>(0xADF610)(inventory,node,0,1,1);
}

inline void Install(bool (*predicate)(int)) {
  eligible = predicate;
  uintptr_t draw = reinterpret_cast<uintptr_t>(DrawItem);
  hook::Patch(0xD1EBF8,{0xC0,0x7B,0x57,0x00},
      {static_cast<BYTE>(draw),static_cast<BYTE>(draw>>8),static_cast<BYTE>(draw>>16),static_cast<BYTE>(draw>>24)});
  hook::Call(0x953F6A,reinterpret_cast<void*>(0x69F2E0),reinterpret_cast<void*>(RopeType));
  hook::Call(0x9BE3CC,reinterpret_cast<void*>(0x8A7030),reinterpret_cast<void*>(ClearRope));
  for (uintptr_t site : {0x9FA1F1,0xA539FE,0xA6BD8D})
    hook::Call(site,reinterpret_cast<void*>(0x9F8370),reinterpret_cast<void*>(DrawGrapple));
  hook::Call(0x9F8D61,reinterpret_cast<void*>(0x8A7130),reinterpret_cast<void*>(QueueRope));
  for (uintptr_t site : {0x9BE478,0x9BE481})
    hook::Call(site,reinterpret_cast<void*>(0x953D30),reinterpret_cast<void*>(DrawQueuedRope));
}
} // namespace lanterngrapple
