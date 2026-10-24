// First start of a release build: game files and game-code module (installer.cpp,
// docs/28-easy-install.md). Module builds only.
#pragma once
#include <string>

namespace installer {

// Makes sure the game files and the game-code module exist (unpacking the GOG installer and building
// the module with the kit next to the program, behind a progress screen) and returns the game
// folder; "" for developer builds (no kit, or a module next to the program), which find their game
// folder as before. Exits if the player quits.
std::string prepare(const std::string &explicit_game_dir);

}  // namespace installer
