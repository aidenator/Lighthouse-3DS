#ifdef LH_ROM_EXTRACT

#include "RomExtract.h"

#include <algorithm>
#include <atomic>
#include <malloc.h>
#include <new>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include "Companion.h"
#include "lib/binarytools/BinaryWriter.h"
#include "port/build.h"

#ifdef __3DS__
#include <fast/backends/gfx_3ds.h>
#endif

namespace fs = std::filesystem;

// Torch's inputs, from the release: config.yml (the supported ROMs) and assets/yaml
static constexpr const char* kExtractFolder = "extract";

static std::string LowerExtension(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}

// Every Banjo-Kazooie ROM this app supports is 16 MB, which rules out most other games cheaply
static constexpr uintmax_t kRomSize = 16 * 1024 * 1024;

// ROM files in each folder in turn: a file named baserom first, then the rest in directory order
static std::vector<fs::path> FindRoms(const std::vector<fs::path>& folders) {
    std::vector<fs::path> roms;
    for (const auto& folder : folders) {
        std::vector<fs::path> here;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(folder, ec)) {
            const std::string ext = LowerExtension(entry.path());
            if (entry.is_regular_file(ec) && (ext == ".z64" || ext == ".v64" || ext == ".n64") &&
                std::find(roms.begin(), roms.end(), entry.path()) == roms.end()) {
                here.push_back(entry.path());
            }
        }
        std::stable_partition(here.begin(), here.end(),
                              [](const fs::path& rom) { return rom.stem().string() == "baserom"; });
        roms.insert(roms.end(), here.begin(), here.end());
    }
    return roms;
}

// One read into a buffer of the right size: much faster on the 3DS than reading through a stream
static std::vector<uint8_t> ReadFile(const fs::path& path) {
    std::vector<uint8_t> data;
    if (FILE* file = fopen(path.string().c_str(), "rb")) {
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        if (size > 0) {
            data.resize(size);
            if (fread(data.data(), 1, data.size(), file) != data.size()) {
                data.clear();
            }
        }
        fclose(file);
    }
    return data;
}

// Torch reads ROMs in the N64's own byte order (.z64). A .v64 has each pair of bytes swapped, a
// .n64 each group of four reversed; the first byte of a ROM's header tells which.
static void ToBigEndian(std::vector<uint8_t>& rom) {
    if (rom.empty() || rom[0] == 0x80) {
        return;
    }
    if (rom[0] == 0x37) {
        for (size_t i = 0; i + 1 < rom.size(); i += 2) {
            std::swap(rom[i], rom[i + 1]);
        }
    } else if (rom[0] == 0x40) {
        for (size_t i = 0; i + 3 < rom.size(); i += 4) {
            std::swap(rom[i], rom[i + 3]);
            std::swap(rom[i + 1], rom[i + 2]);
        }
    }
}

// Tells the game which version made the archive (see Slim_CheckRomArchive)
static std::vector<char> PortVersion() {
    auto writer = LUS::BinaryWriter();
    writer.SetEndianness(Torch::Endianness::Big);
    writer.Write((uint16_t)gBuildVersionMajor);
    writer.Write((uint16_t)gBuildVersionMinor);
    writer.Write((uint16_t)gBuildVersionPatch);
    writer.Close();
    return writer.ToVector();
}

static bool sReplacing = false;

static void ShowProgress(float fraction) {
#ifdef __3DS__
    char step[48];
    snprintf(step, sizeof(step), sReplacing ? "Updating bk.o2r for this version, %d%%"
                                            : "First start: making bk.o2r, %d%%",
             (int)(fraction * 100.0f + 0.5f));
    Fast::Set3dsLoadProgress(fraction, step);
#endif
}

#ifdef __3DS__
extern "C" u32 __ctru_heap_size;

// Heap still free: the app's whole heap is set aside at start, so it's that less what is in use
static size_t FreeHeap() {
    const size_t used = mallinfo().uordblks;
    return __ctru_heap_size > used ? __ctru_heap_size - used : 0;
}
#endif

// Extraction peaks at about 48 MB (the 16 MB ROM, and Torch's work on one asset at a time), measured
// on a New 3DS; the rest is a margin for a heap that isn't one free block
static constexpr size_t kHeapNeeded = 56 * 1024 * 1024;

static RomExtractResult ExtractRomArchive(const std::string& folder, const std::string& archiveName,
                                          const std::vector<std::string>& romFolders, bool replacing) {
    RomExtractResult result;
    sReplacing = replacing;
    std::vector<fs::path> folders;
    for (const auto& romFolder : romFolders) {
        folders.emplace_back(romFolder);
    }
    const std::vector<fs::path> roms = FindRoms(folders);
    if (roms.empty()) {
        return result;
    }
    result.foundRom = true;
    auto fail = [&](const std::string& title, const std::string& message) {
        result.errorTitle = title;
        result.errorMessage = message;
        return result;
    };

    const fs::path extractDir = fs::path(folder) / kExtractFolder;
    std::error_code ec;
    if (!fs::exists(extractDir / "config.yml", ec)) {
        return fail("The extract folder is missing",
                    "Making " + archiveName + " from " + roms.front().filename().string() + " needs the "
                    "extract folder from this release's download. Copy it to\n" + folder + "/");
    }
    YAML::Node config;
    try {
        config = YAML::LoadFile((extractDir / "config.yml").string());
    } catch (const std::exception& e) {
        return fail("config.yml can't be read", std::string(e.what()) + "\nCopy the extract folder from this "
                    "release's download again.");
    }

#ifdef __3DS__
    // An Old 3DS has about half the memory: say so, rather than run out partway
    const size_t freeHeap = FreeHeap();
    SPDLOG_INFO("{} KB of heap free for making {}", freeHeap / 1024, archiveName);
    if (freeHeap < kHeapNeeded) {
        return fail("Not enough memory to make " + archiveName,
                    "This console has " + std::to_string(freeHeap / (1024 * 1024)) + " MB free and needs about " +
                        std::to_string(kHeapNeeded / (1024 * 1024)) + " MB, which an Old 3DS doesn't have. Make " +
                        archiveName + " on a New 3DS, or with a PC build of Lighthouse 3DS, and copy it to\n" +
                        folder + "/");
    }
#endif

    // The first ROM that is one of the supported dumps; the folders may hold other games too
    ShowProgress(0.0f);
    std::vector<uint8_t> rom;
    fs::path romPath;
    for (const auto& candidate : roms) {
        if (fs::file_size(candidate, ec) != kRomSize) {
            SPDLOG_INFO("Not using {}: not the size of a Banjo-Kazooie ROM", candidate.string());
            continue;
        }
        rom = ReadFile(candidate);
        ToBigEndian(rom);
        if (!rom.empty() && config[Companion::CalculateHash(rom)]) {
            romPath = candidate;
            break;
        }
        SPDLOG_INFO("Not using {}: not a supported ROM", candidate.string());
        // Free it, not just empty it: Torch needs the memory
        std::vector<uint8_t>().swap(rom);
    }
    if (romPath.empty()) {
        const std::string first = roms.front().filename().string();
        return fail(roms.size() == 1 ? first + " isn't a supported ROM" : "No supported ROM found",
                    "This app works with the US 1.0, US 1.1, PAL and Japanese Banjo-Kazooie ROMs. Other "
                    "dumps and romhacks aren't supported yet. Put one of those four in\n" + folder + "/");
    }
    const std::string romName = romPath.filename().string();
    SPDLOG_INFO("Making {} from {}", archiveName, romPath.string());

    // Torch skips asset lists that its hash file says are already done, and a leftover archive
    // would only be a partial one
    const fs::path made = extractDir / "bk.o2r";
    fs::remove(extractDir / "torch.hash.yml", ec);
    fs::remove(made, ec);

    // Torch rebuilds the log pattern for every asset when info messages are on, which is slow here
    const auto logLevel = spdlog::get_level();
    spdlog::set_level(spdlog::level::warn);

    const auto start = std::chrono::steady_clock::now();
    std::atomic<size_t> assetCount{ 0 };
    int shownPercent = -1;
    bool ok = true;
    std::string error;
    // Torch makes paths canonical, which std::filesystem can't do with "sdmc:/" paths, so it runs
    // from inside the folder with relative ones
    const fs::path previousDir = fs::current_path(ec);
    fs::current_path(extractDir, ec);
    const std::string here = ".";
    try {
        Companion::Instance = new Companion(std::move(rom), ArchiveType::O2R, false, here, here);
        // Twice the size, but compressing would add almost a minute on the 3DS
        Companion::Instance->GetConfig().storeUncompressed = true;
        Companion::Instance->RegisterCompanionFile("portVersion", PortVersion());
#ifdef LH_ARCHIVE_STAMP
        // What made this archive (see CheckRomArchive in SlimStubs.cpp)
        const std::string stamp = LH_ARCHIVE_STAMP;
        Companion::Instance->RegisterCompanionFile("extractStamp", std::vector<char>(stamp.begin(), stamp.end()));
#endif
        Companion::Instance->SetProgressCallback([&shownPercent](size_t done, size_t total) {
            const int percent = total > 0 ? (int)(done * 100 / total) : 0;
            if (percent != shownPercent) {
                shownPercent = percent;
                ShowProgress(percent / 100.0f);
            }
        });
        Companion::Instance->Init(ExportType::Binary, assetCount, true);
        // Torch skips an asset it can't parse and carries on, which would leave a gap in the archive
        if (const size_t failures = Companion::Instance->GetFailures(); failures > 0) {
            ok = false;
            error = std::to_string(failures) + (failures == 1 ? " asset" : " assets") + " could not be made.";
        }
    } catch (const std::exception& e) {
        ok = false;
        error = e.what();
    }
    delete Companion::Instance;
    Companion::Instance = nullptr;
    spdlog::set_level(logLevel);
    fs::current_path(previousDir, ec);

    if (!ok || !fs::exists(made, ec)) {
        fs::remove(made, ec);
        return fail("Making " + archiveName + " failed",
                    (error.empty() ? std::string("Torch made no archive.") : error) +
                        "\nThe log in logs/Lighthouse.log has the details.");
    }
    // The 3DS won't rename over an existing file, so an out-of-date archive goes only now
    const fs::path target = fs::path(folder) / archiveName;
    fs::remove(target, ec);
    fs::rename(made, target, ec);
    if (ec) {
        return fail("Making " + archiveName + " failed", "It was made but could not be moved to\n" + folder +
                                                             "/\n(" + ec.message() + ")");
    }
    SPDLOG_INFO("Made {} in {:.1f} s", archiveName,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    ShowProgress(1.0f);
    return result;
}

RomExtractResult Slim_ExtractRomArchive(const std::string& folder, const std::string& archiveName,
                                        const std::vector<std::string>& romFolders, bool replacing) {
    // Out of memory anywhere (reading the ROM, say) must end in an error screen, not a crash
    try {
        return ExtractRomArchive(folder, archiveName, romFolders, replacing);
    } catch (const std::bad_alloc&) {
        // Torch's own run has its own handler, so this is from before it starts or after it ends
        RomExtractResult result;
        result.foundRom = true;
        result.errorTitle = "Not enough memory to make " + archiveName;
        result.errorMessage = "Make " + archiveName + " on a New 3DS, or with a PC build of Lighthouse 3DS, and "
                              "copy it to\n" + folder + "/";
        return result;
    }
}

#endif
