#ifdef LH_ROM_EXTRACT

#include "RomExtract.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
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

static fs::path FindRom(const fs::path& folder) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        const std::string ext = LowerExtension(entry.path());
        if (entry.is_regular_file(ec) && (ext == ".z64" || ext == ".v64" || ext == ".n64")) {
            return entry.path();
        }
    }
    return {};
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

static void ShowProgress(float fraction) {
#ifdef __3DS__
    char step[48];
    snprintf(step, sizeof(step), "First start: making bk.o2r, %d%%", (int)(fraction * 100.0f));
    Fast::Set3dsLoadProgress(fraction, step);
#endif
}

RomExtractResult Slim_ExtractRomArchive(const std::string& folder, const std::string& archiveName) {
    RomExtractResult result;
    const fs::path romPath = FindRom(folder);
    if (romPath.empty()) {
        return result;
    }
    result.foundRom = true;
    const std::string romName = romPath.filename().string();
    auto fail = [&](const std::string& title, const std::string& message) {
        result.errorTitle = title;
        result.errorMessage = message;
        return result;
    };

    const fs::path extractDir = fs::path(folder) / kExtractFolder;
    std::error_code ec;
    if (!fs::exists(extractDir / "config.yml", ec)) {
        return fail("The extract folder is missing",
                    "Making " + archiveName + " from " + romName + " needs the extract folder from this "
                    "release's download. Copy it to\n" + folder + "/");
    }

    SPDLOG_INFO("Making {} from {}", archiveName, romName);
    ShowProgress(0.0f);
    std::vector<uint8_t> rom = ReadFile(romPath);
    if (rom.empty()) {
        return fail(romName + " can't be read", "The file may be damaged. Copy it to the SD card again.");
    }
    ToBigEndian(rom);

    const std::string hash = Companion::CalculateHash(rom);
    try {
        YAML::Node config = YAML::LoadFile((extractDir / "config.yml").string());
        if (!config[hash]) {
            return fail(romName + " isn't a supported ROM",
                        "This app works with the US 1.0, US 1.1, PAL and Japanese Banjo-Kazooie ROMs, not "
                        "romhacks or other dumps. Remove " + romName + " and use one of those, or make " +
                        archiveName + " on a PC.");
        }
    } catch (const std::exception& e) {
        return fail("config.yml can't be read", std::string(e.what()) + "\nCopy the extract folder from this "
                    "release's download again.");
    }

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
        Companion::Instance->SetProgressCallback([&shownPercent](size_t done, size_t total) {
            const int percent = total > 0 ? (int)(done * 100 / total) : 0;
            if (percent != shownPercent) {
                shownPercent = percent;
                ShowProgress(percent / 100.0f);
            }
        });
        Companion::Instance->Init(ExportType::Binary, assetCount, true);
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
    fs::rename(made, fs::path(folder) / archiveName, ec);
    if (ec) {
        return fail("Making " + archiveName + " failed", "It was made but could not be moved to\n" + folder +
                                                             "/\n(" + ec.message() + ")");
    }
    SPDLOG_INFO("Made {} in {:.1f} s", archiveName,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    ShowProgress(1.0f);
    return result;
}

#endif
