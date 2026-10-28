#ifdef __3DS__
#include "BottomMenu.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fast/backends/gfx_3ds.h>
#include <libultraship/libultraship.h>

#include "port/Localization/Language.h"
#include "port/ShipInit.hpp"
#include "port/UI/cvar_prefixes.h"

namespace {

#define CVAR_CONVERGENCE CVAR_SETTING("3DS.Convergence")
#define CVAR_HUD_DEPTH CVAR_SETTING("3DS.HudDepth")

constexpr float kDefaultConvergence = 384.0f;
constexpr float kDefaultHudDepth = -0.5f;

struct Scene {
    const char* cvar;
    float tuned;
};
const Scene kScenes[BOTTOM_MENU_SCENE_COUNT] = {
    // The file select looks best with the scene nearer the screen's depth
    { CVAR_SETTING("3DS.SceneConvergence.FileSelect"), 200.0f },
};

// Set on the game thread when a map loads, read by the menu on the main thread
std::atomic<int> sScene{ BOTTOM_MENU_SCENE_NONE };
int sShownScene = BOTTOM_MENU_SCENE_NONE;

// The 3D focus the menu shows and changes: the scene's own, if it has one
const char* ConvergenceCvar(int scene) {
    return scene >= 0 ? kScenes[scene].cvar : CVAR_CONVERGENCE;
}

float Convergence(int scene) {
    return CVarGetFloat(ConvergenceCvar(scene), scene >= 0 ? kScenes[scene].tuned : kDefaultConvergence);
}

// The 40x30 screen's layout. Buttons are three rows tall, to be easy to hit.
constexpr const char* kButton = "\x1b[46;30m";
constexpr const char* kDim = "\x1b[2m";
constexpr int kMenuColumn = 31; // the status screen's [ Menu ], beside the quit hint
constexpr int kMenuRow = 26;
constexpr int kBackColumn = 30; // the menu's [ Back ], top right
constexpr int kBackRow = 0;
constexpr int kButtonWidth = 8;
// The menu's Reset 3D: one small row at the bottom, away from the rest, to be hard to hit by mistake
constexpr const char* kResetLabel = " Reset 3D ";
constexpr int kResetColumn = 1;
constexpr int kResetRow = 28;
constexpr int kFirstItemRow = 5; // each item's middle row, 4 apart
constexpr int kItemSpacing = 4;
constexpr int kMinusColumn = 11; // the item's [-] and [+], 5 wide
constexpr int kValueColumn = 17; // the value between them, 16 wide
constexpr int kValueWidth = 16;
constexpr int kPlusColumn = 34;

enum Item { ITEM_LANGUAGE, ITEM_CONVERGENCE, ITEM_HUD_DEPTH, ITEM_MASTER, ITEM_MUSIC, ITEM_SOUNDS, ITEM_COUNT };

constexpr const char* kLabels[ITEM_COUNT] = { "Language", "3D focus", "3D HUD", "Volume", "Music", "Sounds" };

struct Volume {
    const char* cvar;
    int fallback;
};
const Volume kVolumes[3] = {
    { CVAR_SETTING("Volume.Master"), 40 },
    { CVAR_SETTING("Volume.Music"), 100 },
    { CVAR_SETTING("Volume.SFX"), 100 },
};

bool sOpen = false;
bool sChanged = false;

// A button: label centred on the middle of three rows
void DrawButton(int column, int row, int width, const char* label, bool enabled = true) {
    char blank[48];
    snprintf(blank, sizeof(blank), "%s%*s", enabled ? kButton : "", width, "");
    char middle[64];
    const int length = (int)strlen(label);
    const int left = (width - length) / 2;
    snprintf(middle, sizeof(middle), "%s%*s%s%*s", enabled ? kButton : kDim, left, "", label, width - left - length,
             "");
    Fast::Draw3dsBottomText(column, row, blank);
    Fast::Draw3dsBottomText(column, row + 1, middle);
    Fast::Draw3dsBottomText(column, row + 2, blank);
}

bool InButton(int column, int row, int buttonColumn, int buttonRow, int width) {
    return column >= buttonColumn - 1 && column <= buttonColumn + width && row >= buttonRow - 1 &&
           row <= buttonRow + 3;
}

void DrawMenuButton() {
    DrawButton(kMenuColumn, kMenuRow, kButtonWidth, "Menu");
}

std::vector<std::string> Languages() {
    return Lighthouse::GetAvailableLanguageNames();
}

int CurrentLanguage(const std::vector<std::string>& names) {
    const int32_t key = CVarGetInteger(CVAR_SETTING("DialogLanguage"), 0);
    for (size_t i = 0; i < names.size(); i++) {
        if (Lighthouse::LanguageKey(names[i]) == key) {
            return (int)i;
        }
    }
    return 0;
}

void ApplyStereo() {
    Fast::Set3dsStereoDepth(Convergence(BOTTOM_MENU_SCENE_NONE), CVarGetFloat(CVAR_HUD_DEPTH, kDefaultHudDepth));
    const int scene = sScene.load();
    Fast::Set3dsSceneConvergence(scene >= 0 ? Convergence(scene) : 0.0f);
}

void DrawItem(int item) {
    const int row = kFirstItemRow + item * kItemSpacing;
    bool adjustable = true;
    char value[160];
    switch (item) {
        case ITEM_LANGUAGE: {
            const auto names = Languages();
            adjustable = names.size() > 1;
            snprintf(value, sizeof(value), "%s%-*.*s", adjustable ? "" : kDim, kValueWidth, kValueWidth,
                     names.empty() ? "" : names[CurrentLanguage(names)].c_str());
            break;
        }
        case ITEM_CONVERGENCE:
            sShownScene = sScene.load();
            snprintf(value, sizeof(value), "%-*.0f", kValueWidth, Convergence(sShownScene));
            break;
        case ITEM_HUD_DEPTH:
            snprintf(value, sizeof(value), "%-+*.2f", kValueWidth, CVarGetFloat(CVAR_HUD_DEPTH, kDefaultHudDepth));
            break;
        default: {
            // A bar of ten cells, then the percentage
            const Volume& volume = kVolumes[item - ITEM_MASTER];
            const int percent = CVarGetInteger(volume.cvar, volume.fallback);
            const int cells = std::clamp((percent + 5) / 10, 0, 10);
            char* out = value;
            out += sprintf(out, "\x1b[47m%*s\x1b[44m%*s\x1b[0m", cells, "", 10 - cells, "");
            sprintf(out, " %4d%%", percent);
            break;
        }
    }
    const bool isLanguage = item == ITEM_LANGUAGE;
    Fast::Draw3dsBottomText(1, row, kLabels[item]);
    Fast::Draw3dsBottomText(kValueColumn, row, value);
    if (adjustable) {
        DrawButton(kMinusColumn, row - 1, 5, isLanguage ? "<" : "-");
        DrawButton(kPlusColumn, row - 1, 5, isLanguage ? ">" : "+");
    }
}

void DrawMenu() {
    Fast::Draw3dsBottomText(1, 1, "\x1b[33;1mSettings");
    char reset[32];
    snprintf(reset, sizeof(reset), "%s%s", kButton, kResetLabel);
    Fast::Draw3dsBottomText(kResetColumn, kResetRow, reset);
    DrawButton(kBackColumn, kBackRow, kButtonWidth, "Back");
    for (int item = 0; item < ITEM_COUNT; item++) {
        DrawItem(item);
    }
}

// direction: -1 for [-] (or <), 1 for [+] (or >)
void Adjust(int item, int direction) {
    switch (item) {
        case ITEM_LANGUAGE: {
            const auto names = Languages();
            if (names.size() < 2) {
                return;
            }
            const int count = (int)names.size();
            const int next = (CurrentLanguage(names) + direction + count) % count;
            // Sets the setting too; the next line of dialog is in the new language
            Lighthouse::SetActiveLanguage(names[next]);
            break;
        }
        case ITEM_CONVERGENCE: {
            // In steps of a quarter, nearer or farther
            const int scene = sScene.load();
            const float now = Convergence(scene);
            CVarSetFloat(ConvergenceCvar(scene),
                         std::clamp(std::round(now * (direction > 0 ? 1.25f : 0.8f)), 50.0f, 5000.0f));
            ApplyStereo();
            break;
        }
        case ITEM_HUD_DEPTH: {
            const float now = CVarGetFloat(CVAR_HUD_DEPTH, kDefaultHudDepth);
            CVarSetFloat(CVAR_HUD_DEPTH, std::clamp(now + direction * 0.25f, -2.0f, 2.0f));
            ApplyStereo();
            break;
        }
        default: {
            const Volume& volume = kVolumes[item - ITEM_MASTER];
            const int now = CVarGetInteger(volume.cvar, volume.fallback);
            // Back onto the steps of ten, from a value set on a PC
            const int next = direction > 0 ? (now / 10 + 1) * 10 : ((now + 9) / 10 - 1) * 10;
            CVarSetInteger(volume.cvar, std::clamp(next, 0, 100));
            // The music and sound volumes are read from the setting here
            ShipInit::Init(volume.cvar);
            break;
        }
    }
    sChanged = true;
    DrawItem(item);
}

// Back to the tuned 3D: the player's settings go, so the defaults and each scene's tuned focus
// apply again
void Reset3D() {
    CVarClear(CVAR_CONVERGENCE);
    CVarClear(CVAR_HUD_DEPTH);
    for (const Scene& scene : kScenes) {
        CVarClear(scene.cvar);
    }
    ApplyStereo();
    sChanged = true;
    DrawItem(ITEM_CONVERGENCE);
    DrawItem(ITEM_HUD_DEPTH);
}

void Open() {
    sOpen = true;
    sChanged = false;
    Fast::Set3dsBottomPage(true);
    DrawMenu();
}

void Close() {
    sOpen = false;
    // Saved once on the way out rather than on every tap: writing the settings file takes a moment
    if (sChanged) {
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    Fast::Set3dsBottomPage(false);
    DrawMenuButton();
}

} // namespace

void BottomMenu_Start() {
    ApplyStereo();
    DrawMenuButton();
}

void BottomMenu_SetScene(int scene) {
    sScene.store(scene);
    ApplyStereo();
}

void BottomMenu_Update() {
    // A scene with its own 3D focus came or went while the menu was up
    if (sOpen && sScene.load() != sShownScene) {
        DrawItem(ITEM_CONVERGENCE);
    }
    int column;
    int row;
    if (!Fast::Take3dsTap(&column, &row)) {
        return;
    }
    if (!sOpen) {
        if (InButton(column, row, kMenuColumn, kMenuRow, kButtonWidth)) {
            Open();
        }
        return;
    }
    if (InButton(column, row, kBackColumn, kBackRow, kButtonWidth)) {
        Close();
        return;
    }
    if (row == kResetRow && column >= kResetColumn && column < kResetColumn + (int)strlen(kResetLabel)) {
        Reset3D();
        return;
    }
    // An item's rows: its buttons' three, and the gap under them
    const int item = (row - (kFirstItemRow - 1)) / kItemSpacing;
    if (row < kFirstItemRow - 1 || item >= ITEM_COUNT) {
        return;
    }
    if (column >= kMinusColumn - 1 && column <= kMinusColumn + 5) {
        Adjust(item, -1);
    } else if (column >= kPlusColumn - 1) {
        Adjust(item, 1);
    }
}
#endif
