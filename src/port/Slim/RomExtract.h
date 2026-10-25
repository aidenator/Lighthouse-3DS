#pragma once

#include <string>
#include <vector>

// Makes the game's archive from a ROM on the console itself, for slim builds with LH_ROM_EXTRACT:
// the PC extractor's menus are left out, but Torch is linked in.
struct RomExtractResult {
    // False when none of the folders holds a ROM, so there was nothing to try
    bool foundRom = false;
    // When foundRom: empty if the archive was made, otherwise what went wrong and how to fix it
    std::string errorTitle;
    std::string errorMessage;
};

// Looks for a .z64, .v64 or .n64 ROM in romFolders, in order (baserom.* first in each), and makes
// folder/archiveName from the first supported one. Torch's inputs (config.yml and assets/) come
// from folder/extract.
RomExtractResult Slim_ExtractRomArchive(const std::string& folder, const std::string& archiveName,
                                        const std::vector<std::string>& romFolders);
