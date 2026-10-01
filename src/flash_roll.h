#pragma once
#include <string>

namespace flashroll {
void Install(const std::string& clipPath,bool allowLedges);
bool Active(int character);
float Speed(int character,float fallback);
int WallAnim(int character,int runAnim);
void BeforeSync(int character);
void AfterSync(int character);
}
