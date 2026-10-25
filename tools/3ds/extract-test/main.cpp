// Runs Torch's bk.o2r extraction on the 3DS and reports how long it took and the most memory it
// used. Everything is read from and written to sdmc:/3ds/lighthouse/extract-test/: rom.z64,
// config.yml, torch.hash.yml and assets/ go in; bk.o2r and result.txt come out.
#include <3ds.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <malloc.h>
#include <unistd.h>
#include <vector>

#include "Companion.h"

extern "C" u32 __ctru_heap_size;

// Every SD card call libctru makes, counted and timed, linked in with --wrap (see CMakeLists.txt)
struct FsStat {
    const char* name;
    u32 calls = 0;
    u64 bytes = 0;
    u64 ticks = 0;
};
static FsStat sFsWrite{ "Write" }, sFsRead{ "Read" }, sFsOpenFile{ "OpenFile" }, sFsOpenDir{ "OpenDir" },
    sFsCreateDir{ "CreateDir" }, sFsSetSize{ "SetSize" }, sFsGetSize{ "GetSize" }, sFsClose{ "Close" };
static FsStat* const kFsStats[] = { &sFsWrite,     &sFsRead,    &sFsOpenFile, &sFsOpenDir,
                                    &sFsCreateDir, &sFsSetSize, &sFsGetSize,  &sFsClose };

#define FS_TIMED(stat, nbytes, call)                  \
    do {                                              \
        const u64 t0 = svcGetSystemTick();            \
        const Result res = call;                      \
        stat.ticks += svcGetSystemTick() - t0;        \
        stat.calls++;                                 \
        stat.bytes += (nbytes);                       \
        return res;                                   \
    } while (0)

extern "C" {
Result __real_FSFILE_Write(Handle h, u32* written, u64 offset, const void* buffer, u32 size, u32 flags);
Result __real_FSFILE_Read(Handle h, u32* read, u64 offset, void* buffer, u32 size);
Result __real_FSUSER_OpenFile(Handle* out, FS_Archive archive, FS_Path path, u32 openFlags, u32 attributes);
Result __real_FSUSER_OpenDirectory(Handle* out, FS_Archive archive, FS_Path path);
Result __real_FSUSER_CreateDirectory(FS_Archive archive, FS_Path path, u32 attributes);
Result __real_FSFILE_SetSize(Handle h, u64 size);
Result __real_FSFILE_GetSize(Handle h, u64* size);
Result __real_FSFILE_Close(Handle h);

// X: the archive's writes are thrown away, to time the extraction without the SD card
static bool sDiscardWrites = false;

Result __wrap_FSFILE_Write(Handle h, u32* written, u64 offset, const void* buffer, u32 size, u32 flags) {
    if (sDiscardWrites) {
        sFsWrite.calls++;
        sFsWrite.bytes += size;
        if (written != nullptr) {
            *written = size;
        }
        return 0;
    }
    FS_TIMED(sFsWrite, size, __real_FSFILE_Write(h, written, offset, buffer, size, flags));
}
Result __wrap_FSFILE_Read(Handle h, u32* read, u64 offset, void* buffer, u32 size) {
    FS_TIMED(sFsRead, size, __real_FSFILE_Read(h, read, offset, buffer, size));
}
Result __wrap_FSUSER_OpenFile(Handle* out, FS_Archive archive, FS_Path path, u32 openFlags, u32 attributes) {
    FS_TIMED(sFsOpenFile, 0, __real_FSUSER_OpenFile(out, archive, path, openFlags, attributes));
}
Result __wrap_FSUSER_OpenDirectory(Handle* out, FS_Archive archive, FS_Path path) {
    FS_TIMED(sFsOpenDir, 0, __real_FSUSER_OpenDirectory(out, archive, path));
}
Result __wrap_FSUSER_CreateDirectory(FS_Archive archive, FS_Path path, u32 attributes) {
    FS_TIMED(sFsCreateDir, 0, __real_FSUSER_CreateDirectory(archive, path, attributes));
}
Result __wrap_FSFILE_SetSize(Handle h, u64 size) {
    FS_TIMED(sFsSetSize, 0, __real_FSFILE_SetSize(h, size));
}
Result __wrap_FSFILE_GetSize(Handle h, u64* size) {
    FS_TIMED(sFsGetSize, 0, __real_FSFILE_GetSize(h, size));
}
Result __wrap_FSFILE_Close(Handle h) {
    FS_TIMED(sFsClose, 0, __real_FSFILE_Close(h));
}
}

static constexpr const char* kDir = "sdmc:/3ds/lighthouse/extract-test";

static std::atomic<bool> sSampling{ true };
static std::atomic<size_t> sPeakHeap{ 0 };
static std::atomic<size_t> sAssetCount{ 0 };

static double Seconds(u64 ticks) {
    return (double)ticks / SYSCLOCK_ARM11;
}

// Watches heap use from another thread, so peaks inside Torch are caught too
static void SampleHeap(void*) {
    while (sSampling) {
        const size_t used = mallinfo().uordblks;
        if (used > sPeakHeap) {
            sPeakHeap = used;
        }
        svcSleepThread(10 * 1000 * 1000);
    }
}

// Writes 26 MB (about a compressed bk.o2r) three ways and reports the speed of each: 1 MiB blocks
// into a file that grows, the same into a file whose size is set first, and blocks the size the
// archive writer makes (not a multiple of the sector size), each after a seek, like the extraction
static void SdSpeedTest() {
    static constexpr size_t kBlock = 1024 * 1024;
    static constexpr size_t kOddBlock = 1032486;
    static constexpr size_t kTotal = 26 * kBlock;
    std::vector<char> block(kBlock, 0x5A);
    static const char* const kNames[] = { "Growing file", "Size set first", "Odd blocks, seeks" };
    for (int mode = 0; mode < 3; mode++) {
        FILE* f = fopen("speedtest.bin", "wb");
        if (f == nullptr) {
            printf("Could not create speedtest.bin\n");
            return;
        }
        setvbuf(f, nullptr, _IONBF, 0);
        const u64 t0 = svcGetSystemTick();
        if (mode == 1) {
            ftruncate(fileno(f), (off_t)kTotal);
        }
        const size_t step = mode == 2 ? kOddBlock : kBlock;
        for (size_t done = 0; done < kTotal; done += step) {
            const size_t n = std::min(step, kTotal - done);
            if (mode == 2) {
                fseeko(f, (off_t)done, SEEK_SET);
            }
            fwrite(block.data(), 1, n, f);
        }
        fclose(f);
        const double s = Seconds(svcGetSystemTick() - t0);
        printf("%s: 26 MB in %.1f s, %.2f MB/s\n", kNames[mode], s, 26 / s);
        gfxFlushBuffers();
        gfxSwapBuffers();
    }
    remove("speedtest.bin");
}

static void WaitForStart() {
    printf("\nPress START to quit.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gspWaitForVBlank();
    }
}

int main() {
    osSetSpeedupEnable(true);
    gfxInitDefault();
    consoleInit(GFX_TOP, nullptr);

    printf("bk.o2r extraction test\n\n");
    if (chdir(kDir) != 0) {
        printf("Missing %s\n", kDir);
        WaitForStart();
        gfxExit();
        return 0;
    }

    printf("A: compressed (like the PC)\nB: uncompressed\nX: compressed, archive not written\n"
           "R: uncompressed, archive not written\nY: SD card write speed only (about 45 s)\n");
    bool storeUncompressed = false;
    while (aptMainLoop()) {
        hidScanInput();
        const u32 keys = hidKeysDown();
        if (keys & KEY_Y) {
            printf("\nWriting...\n");
            gfxFlushBuffers();
            gfxSwapBuffers();
            SdSpeedTest();
            WaitForStart();
            gfxExit();
            return 0;
        }
        if (keys & (KEY_A | KEY_B | KEY_X | KEY_R)) {
            storeUncompressed = (keys & (KEY_B | KEY_R)) != 0;
            sDiscardWrites = (keys & (KEY_X | KEY_R)) != 0;
            break;
        }
        gspWaitForVBlank();
    }
    printf("\n%s%s. Working, the screen stays still until it is done...\n",
           storeUncompressed ? "Uncompressed" : "Compressed", sDiscardWrites ? ", not written" : "");
    gfxFlushBuffers();
    gfxSwapBuffers();

    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    Thread sampler = threadCreate(SampleHeap, nullptr, 16 * 1024, priority - 1, 0, false);

    const u64 start = svcGetSystemTick();
    // One read into a buffer of the right size: a stream iterator reads a byte at a time and
    // grows the buffer as it goes, which takes half a minute and twice the memory
    std::vector<uint8_t> rom;
    if (FILE* romFile = fopen("rom.z64", "rb")) {
        fseek(romFile, 0, SEEK_END);
        rom.resize(ftell(romFile));
        fseek(romFile, 0, SEEK_SET);
        if (fread(rom.data(), 1, rom.size(), romFile) != rom.size()) {
            rom.clear();
        }
        fclose(romFile);
    }
    const u64 romRead = svcGetSystemTick();

    bool ok = !rom.empty();
    std::string error = ok ? "" : "Could not read rom.z64";
    if (ok) {
        try {
            // Strings, not "." literals: a literal converts to bool first and picks the modding constructor
            const std::string here = ".";
            Companion::Instance = new Companion(std::move(rom), ArchiveType::O2R, false, here, here);
            Companion::Instance->GetConfig().storeUncompressed = storeUncompressed;
            Companion::Instance->Init(ExportType::Binary, sAssetCount, true);
        } catch (const std::exception& e) {
            ok = false;
            error = e.what();
        }
        delete Companion::Instance;
        Companion::Instance = nullptr;
    }
    const u64 end = svcGetSystemTick();
    const bool discarded = sDiscardWrites;
    sDiscardWrites = false;

    sSampling = false;
    threadJoin(sampler, U64_MAX);
    threadFree(sampler);

    char result[2048];
    int len = snprintf(result, sizeof(result),
             "%s\n%s\nROM read: %.1f s\nExtraction: %.1f s\nTotal: %.1f s\nPeak heap: %.1f MB\n"
             "Heap size: %.1f MB\n",
             ok ? "Done" : ("Failed: " + error).c_str(),
             discarded ? (storeUncompressed ? "Uncompressed, not written" : "Compressed, not written")
                       : (storeUncompressed ? "Uncompressed" : "Compressed"),
             Seconds(romRead - start), Seconds(end - romRead), Seconds(end - start), sPeakHeap / 1048576.0,
             __ctru_heap_size / 1048576.0);
    len += snprintf(result + len, sizeof(result) - len, "SD calls (count, MB, s):\n");
    for (const FsStat* stat : kFsStats) {
        if (stat->calls > 0) {
            len += snprintf(result + len, sizeof(result) - len, "  %-9s %6lu %7.1f %6.1f\n", stat->name,
                            (unsigned long)stat->calls, stat->bytes / 1048576.0, Seconds(stat->ticks));
        }
    }
    printf("\n%s", result);
    if (FILE* f = fopen("result.txt", "w")) {
        fputs(result, f);
        fclose(f);
    }

    WaitForStart();
    gfxExit();
    return 0;
}
