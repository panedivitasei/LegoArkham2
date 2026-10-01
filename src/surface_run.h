#pragma once

namespace surfacerun {
void Install(int speedTenths);
bool Active(int character);
bool Frame(int character, float* normal, float* forward);
void BeforeSync(int character);
void AfterSync(int character);
}
