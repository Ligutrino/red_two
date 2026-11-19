// Gyro aiming (gyro.cpp): the controller's gyro - the Steam Deck's own - steers the craft in
// flight, on top of the main stick. Settings: gyroMode / gyroButton / gyroSpeedX / gyroSpeedY /
// gyroInvertY (Controls page).
#pragma once
#include <string>

#include "runtime.h"

struct PADStatus;

namespace rt::gyro {

void init();  // with Aurora's pads: registers the input layer that collects sensor samples
// once per retrace from input::poll, after PADRead: adds the gyro to port 1's main stick.
// joy: the input thread's controller lock is held (SDL's controller API may be called)
void apply(PADStatus& port1, bool joy);
void setFlight(bool flying);  // each game frame (hle_game.cpp): the player's craft flies
// the controller whose gyro is read (empty: none found) - for the menu
std::string controllerName();

// the activation buttons offered in the menu (SDL gamepad button names; misc3 / misc4 are the
// Steam Deck's stick touches, paddle1-4 its back buttons)
struct Button {
    const char* sdlName;
    const char* label;
};
inline constexpr Button kButtons[] = {
    {"misc3", "Left stick touch"},  {"misc4", "Right stick touch"},     {"paddle1", "R4"},
    {"paddle2", "L4"},              {"paddle3", "R5"},                  {"paddle4", "L5"},
    {"rightshoulder", "RB / R1"},   {"leftshoulder", "LB / L1"},        {"rightstick", "Right stick click"},
    {"leftstick", "Left stick click"},
};

}  // namespace rt::gyro
