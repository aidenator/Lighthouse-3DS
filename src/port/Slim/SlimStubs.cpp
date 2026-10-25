// Stand-ins for the menu, mod manager and extractor entry points that LH_SLIM builds leave out.
// Popups and notifications go to the log; mods and romhacks are never loaded.

#include <filesystem>
#include <memory>
#include <string>

#include <libultraship/libultraship.h>
#include <spdlog/spdlog.h>

#include "port/build.h"
#include "port/Engine.h"
#include "port/Enhancements/Events/Hooks/Events.h"
#include "port/Extractor/ExtractFlow.h"
#include "port/FilePicker.h"
#include "port/Nametag/Nametag.h"
#include "port/Network/Anchor/FightSync.h"
#include "port/Rando/Rando.h"
#include "port/UI/LighthouseGui.hpp"
#include "port/UI/LighthouseModMenuWindow.h"
#include "port/UI/Notification.h"
#include "port/Slim/RomExtract.h"

#ifdef __3DS__
#include <fast/backends/gfx_3ds.h>
#endif

extern "C" {
#include "variables.h"
}

// Menus

namespace LighthouseGui {
void SetupMenu() {
}

void SetupGuiElements() {
}

void Draw() {
}

void Destroy() {
}

void RegisterPopup(std::string title, std::string message, std::string button1, std::string button2,
                   std::function<void()> button1callback, std::function<void()> button2callback) {
    SPDLOG_WARN("[Popup] {}: {}", title, message);
}

size_t PopupsQueued() {
    return 0;
}
} // namespace LighthouseGui

namespace Notification {
void Emit(Options notification) {
    SPDLOG_INFO("[Notification] {}{}{}", notification.prefix, notification.message, notification.suffix);
}
} // namespace Notification

// Mods and romhacks

void UpdateModFiles(bool init, bool reset) {
}

bool IsScopedModFolderName(const std::string& topLevelName) {
    return false;
}

std::string GetActiveRomhackBasename() {
    return "";
}

void EnableMod(std::string file) {
}

void DisableMod(std::string file) {
}

void MaybeShowModConflictPopup() {
}

void MaybeShowRomhackBaseMismatchPopup() {
}

void SetSoleEnabledRomhack(const std::string& keepBasename) {
}

bool IsInlineModExtractionBusy() {
    return false;
}

void Lighthouse::PickFile(Ship::FileBrowserRequest request,
                          std::function<void(std::optional<std::filesystem::path>)> onResult) {
    onResult(std::nullopt); // no file browser: report a cancelled pick
}

// Randomizer: only the active-file tracking survives, since retention enhancements read it

int16_t selectedFileNum = DEFAULT_FILE_NUM;

void Rando::Init() {
    REGISTER_LISTENER(OnGameLoad, EVENT_PRIORITY_NORMAL,
                      [](IEvent* event) { selectedFileNum = ((OnGameLoad*)event)->fileNum; });
    REGISTER_LISTENER(OnLoadFileSelect, EVENT_PRIORITY_NORMAL, [](IEvent* event) { selectedFileNum = DEFAULT_FILE_NUM; });
}

// Nametags only label remote players

void Nametag::SetSubframeBlend(float blend) {
}

void Nametag::SubmitFrame(const void* key) {
}

void Nametag::BeginRenderPass(const void* key, bool interpolate) {
}

// Multiplayer sync hooks, matching the real ones' offline behaviour (never connected, world sync inactive)

extern "C" {
s32 port_anchor_isConnected(void) {
    return 0;
}

s32 port_anchor_isWorldSyncActive(void) {
    return 0;
}

s32 port_anchor_getDummyTransform(u32 clientId, f32 pos[3], f32* yaw) {
    return 0;
}

void port_anchor_onCarryThrow(s32 markerId, f32 start[3], f32 target[3]) {
    CALL_EVENT(OnCarryThrow, markerId, start, target); // fires before the connection check
}

void port_anchorDummies_onActorsFreed(void) {
}

int32_t port_mapFlag_wasSetRemotely(int32_t index) {
    return 0;
}

int32_t port_breakable_isBroken(int32_t map, int32_t markerId, int32_t x, int32_t y, int32_t z) {
    return 0;
}

void port_breakable_recordBreak(int32_t markerId, int32_t x, int32_t y, int32_t z) {
}

void port_breakable_broadcastBreak(int32_t markerId, int32_t x, int32_t y, int32_t z) {
}

int32_t port_hutSmash_get(int32_t x, int32_t y, int32_t z) {
    return -1;
}

void port_hutSmash_record(int32_t x, int32_t y, int32_t z, int32_t loot) {
}

int32_t port_hutSmash_countForCurrentLevel(int32_t localCount) {
    return localCount;
}

int32_t port_eggToll_getStage(int32_t map, int32_t secondaryId) {
    return 0;
}

void port_eggToll_onAdvance(int32_t map, int32_t secondaryId, int32_t stage) {
}

int32_t port_puzzleStep_get(int32_t puzzleId) {
    return 0;
}

int32_t port_puzzleStep_getForMap(int32_t map, int32_t puzzleId) {
    return 0;
}

void port_puzzleStep_orBits(int32_t puzzleId, int32_t bits) {
}

int32_t port_puzzleCount_get(int32_t counterId) {
    return 0;
}

int32_t port_puzzleCount_add(int32_t counterId, int32_t localCount, int32_t delta) {
    return localCount + delta;
}

int32_t port_puzzlePos_isMarked(int32_t puzzleId, int32_t x, int32_t y, int32_t z) {
    return 0;
}

void port_puzzlePos_mark(int32_t puzzleId, int32_t x, int32_t y, int32_t z) {
}

int32_t port_lairWater_targetLevel(int32_t map, int32_t flagLevel) {
    return flagLevel;
}

void port_lairWater_onRiseTrigger(int32_t waterMap, int32_t levelFlag) {
}

void port_ccWater_broadcastRise(int32_t map, int32_t waterId, int32_t targetDy, f32 duration) {
}

void port_jiggyCrane_broadcast(int32_t stage) {
}

void port_fpTwinkly_release(void) {
}

int32_t port_jigsawPedestal_tryClaim(int32_t id) {
    return 1;
}

int32_t port_jigsawPedestal_isSelf(int32_t id) {
    return 1;
}

void port_jigsawPedestal_release(int32_t id) {
}

bool FightSync_BossFollowerTick(void* boss) {
    return false;
}

bool FightSync_ForwardBossHit(int32_t phase) {
    return false;
}

bool FightSync_ForwardEgg(int32_t statue_id, int32_t pad_index) {
    return false;
}

void FightSync_ReplicateEgg(int32_t statue_id, int32_t pad_index) {
}

void FightSync_OnBossSpawned(void) {
}

void FightSync_OnBossDefeated(void) {
}

void FightSync_OnSpellSpawned(s32 kind) {
}

void FightSync_OnStatueSpawned(s32 statue_id) {
}

void FightSync_OnJinjoSlam(s32 statue_id) {
}

void FightSync_OnBarrierSpawned(void) {
}

void FightSync_OnFlightPadSpawned(void) {
}

void FightSync_OnFlightPadDespawned(void) {
}
} // extern "C"

// Called from C++ with C++ linkage (TransientLevelState)
void port_breakable_clearForLevel(int32_t levelId) {
}

void port_hutSmash_clearForLevel(int32_t levelId) {
}

void port_eggToll_clearForLevel(int32_t levelId) {
}

void port_puzzleStep_clearForLevel(int32_t levelId) {
}

// Extraction: bk.o2r is made on a PC, or (LH_ROM_EXTRACT) here from a ROM on first start

// Checked on the archive FinishInit opens: opening bk.o2r costs seconds on the 3DS, mostly
// indexing its file list, so it must not be opened just for this
// A problem the player has to fix before the game can start: say what it is and how, then exit
[[noreturn]] static void FatalSetupError(const std::string& title, const std::string& message) {
    SPDLOG_CRITICAL("{}: {}", title, message);
#ifdef __3DS__
    Fast::Show3dsFatalError(title.c_str(), message.c_str());
#else
    exit(1);
#endif
}

// How to get a new archive: the app makes one from a ROM, or (without LH_ROM_EXTRACT) a PC build does
static std::string RemakeArchiveHint(const std::string& name) {
#ifdef LH_ROM_EXTRACT
    return "Delete " + name + " and put your Banjo-Kazooie ROM in\n" + Ship::Context::GetAppBundlePath() +
           "/\nThe next start makes a new one from it.";
#else
    return "Make it again from your ROM with the PC build of Lighthouse " + std::to_string(gBuildVersionMajor) +
           "." + std::to_string(gBuildVersionMinor) + ", from the same release as this app.";
#endif
}

void Slim_CheckRomArchive(const std::shared_ptr<Ship::Archive>& archive, const std::string& name) {
    if (archive == nullptr) {
        FatalSetupError(name + " can't be read",
                        "The file may be damaged or only partly copied. " + RemakeArchiveHint(name));
    }
    auto file = archive->LoadFile("portVersion");
    const std::string wanted = std::to_string(gBuildVersionMajor) + "." + std::to_string(gBuildVersionMinor);
    if (file == nullptr || !file->IsLoaded || file->Buffer->size() < 4) {
        FatalSetupError(name + " is from another version",
                        "It was made by an older Lighthouse. " + RemakeArchiveHint(name));
    }
    auto stream = std::make_shared<Ship::MemoryStream>(file->Buffer->data(), file->Buffer->size());
    auto reader = std::make_shared<Ship::BinaryReader>(stream);
    reader->SetEndianness(Ship::Endianness::Big);
    const uint16_t major = reader->ReadUInt16();
    const uint16_t minor = reader->ReadUInt16();
    if (major != gBuildVersionMajor || minor != gBuildVersionMinor) {
        FatalSetupError(name + " is from another version",
                        "It was made by Lighthouse " + std::to_string(major) + "." + std::to_string(minor) +
                            ", but this app needs " + wanted + ". " + RemakeArchiveHint(name));
    }
}

void GameEngine::RunExtract(int argc, char* argv[]) {
    const std::string folder = Ship::Context::GetAppBundlePath();
    if (!portArchiveVersionMatch) {
        FatalSetupError("lighthouse.o2r is missing", "Copy lighthouse.o2r from this release's download to\n" +
                                                         folder + "/");
    }
    for (const auto& archive : kRomArchives) {
        const std::string path = Ship::Context::LocateFileAcrossAppDirs(archive, "bk");
        if (std::filesystem::exists(path)) {
            continue;
        }
#ifdef LH_ROM_EXTRACT
        // First start with a ROM instead: make the archive here. The ROM can be in the app's folder,
        // beside the .3dsx (the Homebrew Launcher passes its path), or at the SD card's root.
        std::vector<std::string> romFolders = { folder };
        const std::string appPath = argc > 0 && argv[0] != nullptr ? argv[0] : "";
        if (appPath.rfind("sdmc:/", 0) == 0) {
            romFolders.push_back(std::filesystem::path(appPath).parent_path().string());
        }
        romFolders.push_back("sdmc:/");
        const RomExtractResult extract = Slim_ExtractRomArchive(folder, archive, romFolders);
        if (extract.foundRom && !extract.errorTitle.empty()) {
            FatalSetupError(extract.errorTitle, extract.errorMessage);
        }
        if (extract.foundRom) {
            continue;
        }
        FatalSetupError(archive + " is missing",
                        "Put your Banjo-Kazooie ROM (.z64, .v64 or .n64) in\n" + folder + "/\nand start again: "
                        "the app makes " + archive + " from it, in about a minute and a half. The ROM can also "
                        "be next to the .3dsx or at the root of the SD card.");
#else
        FatalSetupError(archive + " is missing",
                        "Make it from your Banjo-Kazooie ROM with the PC build of Lighthouse from this "
                        "release, then copy it to\n" + folder + "/");
#endif
    }
}
