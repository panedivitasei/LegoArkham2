#pragma once
#include <cstdint>

namespace playercharacter {
template<class T> inline T Read(int character,int offset) {
  return *reinterpret_cast<const T*>(character+offset);
}
inline const char* ResourceName(int character) {
  int instance=Read<int>(character,4104);
  return instance?Read<const char*>(instance,28):nullptr;
}
// Drop-in/out sets +5994; switching transfers input and party membership independently.
inline int HumanSlot(int character,const int* party=reinterpret_cast<const int*>(0x138F4E0)) {
  if (!character || !Read<uint8_t>(character,5994)) return -1;
  int slot=Read<int8_t>(character,5993);
  if (slot<0 || slot>=2 || party[slot]!=character) return -1;
  int input=Read<int>(character,1992);
  return input && Read<int>(input,0)?slot:-1;
}
}
