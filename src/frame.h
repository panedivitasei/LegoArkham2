#pragma once

namespace frame {

// Registers `fn` to run once per presented frame, menus and cutscenes included.
void OnTick(void (*fn)());

// Called by the present hook.
void Tick();

}  // namespace frame
