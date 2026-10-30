#pragma once

#include <string>

// The settings menu on the 3DS's bottom screen. A [ Menu ] button on the status screen opens it;
// the game keeps running while it's up.

// Applies the saved settings the menu covers and draws its button; once the status screen is up
void BottomMenu_Start();

// Handles taps on the bottom screen; on the main thread, after the buttons are scanned
void BottomMenu_Update();

// Scenes with a 3D focus of their own, tuned to look best there. The menu adjusts the scene's own
// while it's up, and the player's change is kept for that scene.
enum BottomMenuScene { BOTTOM_MENU_SCENE_NONE = -1, BOTTOM_MENU_SCENE_FILE_SELECT, BOTTOM_MENU_SCENE_COUNT };

// The scene now loading; from the game thread
void BottomMenu_SetScene(int scene);

// Shows text on the status screen for a few seconds, such as what a randomizer check gave; from any
// thread
void BottomMenu_Notify(const std::string& text);
