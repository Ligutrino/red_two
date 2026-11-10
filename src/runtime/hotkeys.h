// Hotkeys: actions on a combination of controller buttons or keys held together (Back + RB
// saves a state, Back + LB loads it, ...). Bindings live in settings (hotkeyPad / hotkeyKeys).
#pragma once
#include <string>

namespace rt::hotkeys {

void tick();  // once per presented frame (guest thread): fire the actions whose buttons went down
// The game sees no buttons or triggers now: a hotkey's modifier (a button in a combination that
// plays no GameCube control, e.g. Back) is held, or a combination just fired and is still down.
bool muteGame();

std::string label(int action, bool pad);  // "BACK + RB", "F5", "-" for none

// the settings menu's capture: hold the buttons (or keys), then let go
void captureStart(int action, bool pad);
void captureTick();     // while capturing (menu draw)
bool capturing();
int captureAction();    // the action being captured, -1 none
void captureCancel();
void clear(int action, bool pad);
void resetDefaults(bool pad);

}  // namespace rt::hotkeys
