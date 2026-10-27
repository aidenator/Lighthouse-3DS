#include "port/OS/Wait.h"
#include "Engine.h"
#include <atomic>
#include <clocale>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fast/interpreter.h>
#include <libultraship.h>
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif
#include <SDL2/SDL.h>

#include "Controller/ControlSchemes.h"
#ifdef __3DS__
#include <fast/backends/gfx_3ds.h>
#endif
#include "DevTools/ThreadWatchdog.h"
#include "FrameHost.h"
#include "GameStatus.h"
#include "Interpolation/FrameInterpolation.h"
#include "Nametag/Nametag.h"
#include "Network/Anchor/Anchor.h"
#include "OS/OS.h"
#include "Patches/Patches.h"
#include "ShipUtils.h"
#include "ShipInit.hpp"
#include "src/port/Enhancements/Events/Hooks/Events.h"
#include "UI/LighthouseModMenuWindow.h"

extern "C" {
#include <libultra/rdp.h>
#include "enums.h"
#include "core1/core1.h"
#include "core1/main.h"
#include "core1/thread5.h"
void viMgr_entry(void* arg);
void thread5_entry(void* arg);
void audioManagerThread_entry(void* arg);
void core1_15B30_sendMesg3ToRenderThread(void);
OSMesgQueue* thread5_getTaskQueue(void);
OSMesgQueue* thread5_getSyncQueue(void);
u32 osDpGetStatus(void);
}

// The game tick runs on its own thread and submits display lists through the
// decomp's thread5 queue; this thread stays behind as the RCP and event pump.
namespace {
std::atomic<bool> sGameThreadDone{ false };
std::thread sGameThread;
#ifdef __3DS__
void* sGameThread3ds = nullptr;
#endif
thread_local bool tIsGameThread = false;
extern "C" double OS_ThreadBlockedMs(void); // OS/OS_Mesg.cpp

// The interpolation pair a submitted list was built from, carried to whoever
// renders it. At most a couple are live at once.
struct InterpPair {
    int prev = -1;
    int curr = -1;
    bool should = false;
    uint64_t serial = 0;
    long long swapNs = 0;
    unsigned viSerial = 0;
    bool timed = false;
};
std::mutex sInterpMutex;
std::map<void*, InterpPair> sTaskInterp;
uint64_t sInterpSerial = 0;
// Submissions of slack before an unrendered pair is assumed dropped. The ring is
// 4 slots, so by then its trees have been recycled regardless.
constexpr uint64_t kInterpStaleAfter = 4;

// Renderer calls made from tick code, run by the main loop between services.
std::mutex sSvcMutex;
std::condition_variable sSvcCv;
void (*sSvcFn)(void*) = nullptr;
void* sSvcArg = nullptr;
std::atomic<bool> sShutdownRequested{ false };

int sTitleMap = 0;

std::unique_ptr<Lighthouse::FrameHost> sHost;
std::mutex sParkMutex;
std::condition_variable sParkCv;
std::atomic<bool> sParked{ false };

std::vector<std::pair<std::string, std::unique_ptr<Lighthouse::FrameHost> (*)()>>& FrameHosts() {
    static std::vector<std::pair<std::string, std::unique_ptr<Lighthouse::FrameHost> (*)()>> hosts;
    return hosts;
}

void DrainRenderService() {
    std::unique_lock<std::mutex> lock(sSvcMutex);
    if (sSvcFn != nullptr) {
        auto* fn = sSvcFn;
        void* arg = sSvcArg;
        lock.unlock();
        fn(arg);
        lock.lock();
        sSvcFn = nullptr;
        sSvcCv.notify_all();
    }
}

// False on the window thread, including everything that runs during init
// before the tick thread exists.
bool OnGameThread() {
    return tIsGameThread;
}
} // namespace

Lighthouse::RegisterFrameHost::RegisterFrameHost(const char* flag, std::unique_ptr<FrameHost> (*make)()) {
    FrameHosts().emplace_back(flag, make);
}

bool Lighthouse::IsFrameHostFlag(const std::string& arg) {
    for (const auto& [flag, make] : FrameHosts()) {
        if (arg == flag) {
            return true;
        }
    }
    return false;
}

Lighthouse::FrameHost* Lighthouse::GetFrameHost() {
    return sHost.get();
}

void Lighthouse::WakeGame() {
    std::lock_guard<std::mutex> lock(sParkMutex);
    sParkCv.notify_all();
}

// A list is submitted while its tick is still recording, so the pair is
// captured here and travels with the task.
extern "C" void port_thread5_onSubmit(void* taskData) {
    if (!OnGameThread() || (uintptr_t)taskData < 100) {
        return;
    }
    struct ucode_task_data_s* task = (struct ucode_task_data_s*)taskData;
    if (task->task_type != UCODE_TASK_TYPE_F3DEX && task->task_type != UCODE_TASK_TYPE_L3DEX) {
        return;
    }
    InterpPair pair;
    FrameInterpolation_GetRecordingPair(&pair.prev, &pair.curr, &pair.should);
    FrameInterpolation_ClaimPair(pair.prev, pair.curr);
    pair.swapNs = OS_ViLastSwapNs();
    pair.viSerial = port_getDemoViSerial();
    pair.timed = GameEngine::WantsTimedPass(pair.curr >= 0, GameEngine::CurrentViPerTick());
    FrameInterpolation_StopRecord();
    Nametag::SubmitFrame(task->data_ptr);
    std::lock_guard<std::mutex> lock(sInterpMutex);
    pair.serial = ++sInterpSerial;
    auto [it, inserted] = sTaskInterp.emplace(task->data_ptr, pair);
    if (!inserted) {
        FrameInterpolation_ReleasePair(it->second.prev, it->second.curr);
        it->second = pair;
    }

    // Anything still here after a full trip round the ring can no longer be
    // blended against a live tree, so its claim is only holding a slot hostage.
    // Dropping the entry leaves RenderTask with a -1 pair, which renders
    // uninterpolated rather than against a recycled tree.
    for (auto stale = sTaskInterp.begin(); stale != sTaskInterp.end();) {
        if (stale->second.serial + kInterpStaleAfter < pair.serial) {
            FrameInterpolation_ReleasePair(stale->second.prev, stale->second.curr);
            stale = sTaskInterp.erase(stale);
        } else {
            ++stale;
        }
    }
}

namespace {
void RenderTask(void* dlStart) {
    InterpPair pair;
    {
        std::lock_guard<std::mutex> lock(sInterpMutex);
        auto it = sTaskInterp.find(dlStart);
        if (it != sTaskInterp.end()) {
            pair = it->second;
            sTaskInterp.erase(it);
        }
    }
    FrameInterpolation_BeginRenderPass(pair.prev, pair.curr, pair.should);
    Nametag::BeginRenderPass(dlStart, pair.should);
    GameEngine::SetFrameTiming(OS_ViNextRetraceAfterNs(pair.swapNs), pair.viSerial, pair.timed);
    GameEngine::ProcessGfxCommands((Gfx*)dlStart);
    FrameInterpolation_ReleasePair(pair.prev, pair.curr);
}

// A timed pass may start while the previous swap is still waiting to latch.
bool IsTimedTask(void* dlStart) {
    std::lock_guard<std::mutex> lock(sInterpMutex);
    auto it = sTaskInterp.find(dlStart);
    return it != sTaskInterp.end() && it->second.timed;
}

bool sTaskReleased = true;
bool sReleaseDeferred = false;

// DP and SP events thread5's queue had no room for. A lost DP means the frame's swap token never
// comes and the game waits for it forever, so they're kept and delivered in order on the next
// passes instead. The queue fills when thread5 gets no CPU for a while: it holds 20 messages, and
// VI retraces alone add 60 a second.
int sUndeliveredDp = 0;
int sUndeliveredSp = 0;

bool DeliverTaskDone() {
    while (sUndeliveredDp > 0) {
        if (OS_SendEventMesg(OS_EVENT_DP) != 0) {
            return false;
        }
        sUndeliveredDp--;
    }
    while (sUndeliveredSp > 0) {
        if (OS_SendEventMesg(OS_EVENT_SP) != 0) {
            return false;
        }
        sUndeliveredSp--;
    }
    return true;
}

}

extern "C" s32 thread5_isGfxIdle(void);
extern "C" void thread5_logTrace(void);

extern "C" void port_logWarn(const char* text) {
    SPDLOG_WARN("{}", text);
}

// The game waits here, in vimgr.c, for the token the frame's last graphics task sends when it's
// drawn. Rarely the token never comes, though everything handed over was drawn (seen on the 3DS,
// cause not yet known), and the game would wait for good. So wait in slices, and once nothing in
// the pipeline could still send it, carry on without it.
extern "C" void port_waitFrameToken(OSMesgQueue* mq) {
    thread5_trace('G', 0);
    while (OS_RecvMesgTimeout(mq, nullptr, 250) != 0) {
        if (!WindowIsRunning()) {
            return;
        }
        const bool rcpIdle = OS_SpPeekPendingTask() == nullptr && !sReleaseDeferred && sUndeliveredDp == 0 &&
                             sUndeliveredSp == 0;
        if (rcpIdle && thread5_isGfxIdle()) {
            static int sRecoveries = 0;
            SPDLOG_WARN("The frame's swap token never came; going on without it ({} so far)", ++sRecoveries);
            // What thread5 did leading up to it, the first few times
            if (sRecoveries <= 3) {
                thread5_logTrace();
            }
#if defined(__3DS__) && defined(LH_HALT_ON_LOST_TOKEN)
            // Testing aid: stop here so the state can be read with a debugger
            Fast::Show3dsFatalError("Lost frame token", "Logged to logs/Lighthouse.log. Attach the debugger now.");
#endif
            return;
        }
    }
    thread5_trace('g', 0);
}

extern "C" void port_noteDroppedFrameToken(void) {
    static int sDropped = 0;
    if (++sDropped <= 20) {
        SPDLOG_WARN("A frame's swap token was dropped: the game hadn't taken the last one ({} so far)", sDropped);
    }
}

namespace {
void SendTaskDone() {
    thread5_trace('X', 0);
    sUndeliveredDp++;
    sUndeliveredSp++;
    if (!DeliverTaskDone()) {
        static int sRefusals = 0;
        if (++sRefusals <= 20) {
            SPDLOG_WARN("Thread5's queue is full; delivering the task's DP/SP events later ({} so far)", sRefusals);
        }
    }
}
} // namespace

// Hands the task back once the list is no longer read. A timed pass calls this after its
// last draw; otherwise ServiceRcp does once the pass returns.
extern "C" void port_releaseRcpTask(void) {
    if (sTaskReleased) {
        return;
    }
    sTaskReleased = true;
    if (osDpGetStatus() & DPC_STATUS_FREEZE) {
        thread5_trace('X', 1);
        sReleaseDeferred = true;
        return;
    }
    SendTaskDone();
}

namespace {

// This thread plays the RCP: thread5 hands over a task, it runs and raises DP
// then SP. Hardware raises SP first, but the list is fully drawn before either
// goes out. DP has to lead: SP frees thread5 to start the next task, and starting
// one overwrites the flags the frame's swap token gates on.
int ServiceRcp() {
    // A finished task's events go out before the next task starts
    if (!DeliverTaskDone()) {
        return 0;
    }
    if (sReleaseDeferred) {
        if (osDpGetStatus() & DPC_STATUS_FREEZE) {
            return 0;
        }
        sReleaseDeferred = false;
        SendTaskDone();
    }
    OSTask* pending = OS_SpPeekPendingTask();
    if (pending == nullptr) {
        return 0;
    }
    const bool frozen = (osDpGetStatus() & DPC_STATUS_FREEZE) != 0;
    if (frozen && !IsTimedTask(pending->t.data_ptr)) {
        return 0;
    }
    OSTask* task = OS_SpTakePendingTask();
    if (task == nullptr) {
        return 0;
    }
    sTaskReleased = false;
    RenderTask(task->t.data_ptr);
    port_releaseRcpTask();
    return 1;
}

void ParkGame() {
    while ((OS_SpPeekPendingTask() != nullptr || sReleaseDeferred || sUndeliveredDp > 0 || sUndeliveredSp > 0) &&
           WindowIsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ThreadWatchdog_Beat(WATCHDOG_GAME_TICK);
    }
    std::unique_lock<std::mutex> lock(sParkMutex);
    sParked.store(true);
    while (sHost->WantsFrame() && WindowIsRunning()) {
        sParkCv.wait_for(lock, std::chrono::milliseconds(250));
        ThreadWatchdog_Beat(WATCHDOG_GAME_TICK);
    }
    sParked.store(false);
}

// Called before core1_init, which is where these threads are created.
void EnableThread5() {
    OS_EnableThreadEntry((void*)thread5_entry);
    OS_SetQueueBlocking(thread5_getTaskQueue(), 1);
    OS_SetQueueBlocking(thread5_getSyncQueue(), 1);
    // The controller manager parks on its polling queue waiting for OS_EVENT_SI.
    OS_EnableThreadEntry((void*)joy_main);
    OS_SetQueueBlocking(si_getEventQueue(), 1);
    OS_EnableThreadEntry((void*)audioManagerThread_entry);
#ifdef __3DS__
    // Audio mixing on core 1, beside the game (core 2) and the renderer (core 0)
    OS_SetThreadEntryCore((void*)audioManagerThread_entry, 1);
#endif
    OS_SetQueueBlocking(audioManager_getFrameMesgQueue(), 1);
    OS_SetQueueBlocking(audioManager_getReplyMesgQueue(), 1);
}
} // namespace

// Drain submitted lists for safety.
static void RegisterThread5MapSync_Init() {
    COND_HOOK(OnMapLoad, EVENT_PRIORITY_HIGH, true, [](IEvent* event) {
        (void)event;
        port_pipelineSyncPoint();
    });
}

static RegisterShipInitFunc sThread5MapSyncInit(RegisterThread5MapSync_Init);

// Whether a tick-side renderer call is waiting on the window thread. The
// handshake below is a condvar rather than a message queue, so it is the one
// park the watchdog's blocked-wait registry cannot see.
extern "C" int port_renderServicePending(void) {
    return sSvcFn != nullptr;
}

// Renderer calls from tick code come through here; D3D11 hangs if they run
// off the window thread. Inline when there is no separate tick thread.
extern "C" void port_runOnRenderThread(void (*fn)(void*), void* arg) {
    if (!OnGameThread()) {
        fn(arg);
        return;
    }
    std::unique_lock<std::mutex> lock(sSvcMutex);
    auto done = [] { return sSvcFn == nullptr || sShutdownRequested.load(std::memory_order_acquire); };
    if (sShutdownRequested.load(std::memory_order_acquire)) {
        return;
    }
    port_waitUntil(sSvcCv, lock, done);
    if (sShutdownRequested.load(std::memory_order_acquire)) {
        return;
    }
    sSvcFn = fn;
    sSvcArg = arg;
    port_waitUntil(sSvcCv, lock, done);
}

extern "C" void port_serviceRenderRequests(void) {
    DrainRenderService();
}

// Barrier before the tick frees or reads memory an in-flight list references.
// The game's own EVENT_SYNC handshake is the RDP-done wait.
extern "C" void port_pipelineSyncPoint(void) {
    if (OnGameThread()) {
        core1_15B30_sendMesg3ToRenderThread();
    }
}

// Tracks whether mainLoop actually fed the renderer this iteration.
// BK's gameloop conditionally skips game_draw during scene transitions.
static bool sFrameRendered = false;

// The list itself reaches the renderer through thread5's task queue, submitted
// by core1_15B30_addF3DEXTaskData right after this call; all that is left here
// is noting that the tick drew.
extern "C" void Graphics_PushFrame(Gfx* data) {
    (void)data;
    sFrameRendered = true;
}

void push_frame() {
    static int sTitleCounter = 0;
    sFrameRendered = false;

    // The window thread keeps the progress modal alive while an inline mod
    // extraction runs; the tick just idles so the extractor gets the machine.
    if (IsInlineModExtractionBusy()) {
        SDL_Delay(16);
        return;
    }

    GameEngine::Instance->StartFrame();
    port_animVtx_beginTick();
    const bool recordInterpolation = GameEngine::IsInterpolationEnabled();
    if (recordInterpolation) {
        FrameInterpolation_StartRecord();
    } else {
        FrameInterpolation_DontInterpolateCamera();
    }
    mainLoop();
    if (recordInterpolation) {
        FrameInterpolation_StopRecord();
    }
    if (sFrameRendered) {
        port_tickDemoAudioHold();
    }

    // Refresh window title stats once per second (every 30 game ticks). The
    // window belongs to the other thread, so hand the call over.
    if (++sTitleCounter >= 30) {
        sTitleCounter = 0;
        sTitleMap = gsworld_getMap();
        port_runOnRenderThread([](void*) { port_setWindowTitle(sTitleMap); }, nullptr);
    }

    if (!sFrameRendered) {
        SDL_Delay(33);
    }
}

#ifdef __linux__
static void SetAppImageHome() {
    const char* appImage = std::getenv("APPIMAGE");
    if (appImage == nullptr || appImage[0] == '\0' || std::getenv("SHIP_HOME") != nullptr) {
        return;
    }

    std::error_code ec;
    const std::filesystem::path appImageDir = std::filesystem::path(appImage).parent_path();
    for (const char* file : { "lighthouse.cfg.json", "bk.o2r" }) {
        if (std::filesystem::exists(appImageDir / file, ec)) {
            return;
        }
    }

    std::filesystem::path home;
    const char* dataHome = std::getenv("XDG_DATA_HOME");
    const char* userHome = std::getenv("HOME");
    if (dataHome != nullptr && std::filesystem::path(dataHome).is_absolute()) {
        home = dataHome;
    } else if (userHome != nullptr && userHome[0] != '\0') {
        home = std::filesystem::path(userHome) / ".local" / "share";
    } else {
        return;
    }
    home /= "lighthouse";

    std::filesystem::create_directories(home, ec);
    if (!ec) {
        setenv("SHIP_HOME", home.c_str(), 0);
    }
}
#endif

#ifdef __3DS__
// For the loading bar from C code
extern "C" void port_3dsLoadProgress(float fraction, const char* step) {
    Fast::Set3dsLoadProgress(fraction, step);
}
#endif

/* Rename SDL_main to main for SDL compatibility */
#ifdef __GNUC__
#define SDL_main main
#endif

int SDL_main(int argc, char* argv[]) {
#ifdef _WIN32
    setlocale(LC_ALL, ".UTF8");
    timeBeginPeriod(1);
#endif
#ifdef __linux__
    SetAppImageHome();
#endif

    // Anchor relative paths to the executable instead of cwd
    // when SHIP_HOME is not in use
    std::error_code ec;
    const char* shipHome = std::getenv("SHIP_HOME");
    const char* appImage = std::getenv("APPIMAGE");
    if (shipHome != nullptr && shipHome[0] != '\0') {
        std::filesystem::current_path(shipHome, ec);
    } else if (appImage != nullptr && appImage[0] != '\0') {
        // Running from an AppImage: the executable lives in a read-only squashfs
        // mount under /tmp, so anchor to the .AppImage file's directory instead.
        std::filesystem::current_path(std::filesystem::path(appImage).parent_path(), ec);
    } else {
        std::string base = Ship::Context::GetAppBundlePath();
        if (!base.empty() && base != ".") {
            std::filesystem::current_path(base, ec);
        }
    }

#ifdef __3DS__
    Fast::Show3dsLoadingScreen("Banjo-Kazooie", "v" LH_3DS_VERSION);
#endif
    for (int i = 1; i < argc; i++) {
        for (const auto& [flag, make] : FrameHosts()) {
            if (sHost == nullptr && flag == argv[i]) {
                sHost = make();
            }
        }
    }

    GameEngine::Create(argc, argv);
#ifdef __3DS__
    ControlSchemes_Apply3dsLayoutOnce();
#endif
    // Both threads are created during core1_init, so allowlist them first.
    OS_EnableThreadEntry((void*)viMgr_entry);
    EnableThread5();
    core1_init();
    if (sHost != nullptr) {
        sHost->Ready();
    }
    ThreadWatchdog_Start();

    static const auto gameLoop = [] {
        tIsGameThread = true;
#ifdef __3DS__
        // How long each game frame (logic and building the display list) takes, for the log
        using Clock = std::chrono::steady_clock;
        auto summaryStart = Clock::now();
        double totalMs = 0;
        double worstMs = 0;
        double waitedMs = 0;
        int frames = 0;
#endif
        while (WindowIsRunning()) {
            ThreadWatchdog_Beat(WATCHDOG_GAME_TICK);
            if (sHost != nullptr && sHost->WantsFrame() && func_8023E000() == 0) {
                ParkGame();
                continue;
            }
#ifdef __3DS__
            const auto frameStart = Clock::now();
            const double blockedBefore = OS_ThreadBlockedMs();
#endif
            push_frame();
#ifdef __3DS__
            const double blocked = OS_ThreadBlockedMs() - blockedBefore;
            const double ms =
                std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count() - blocked;
            waitedMs += blocked;
            totalMs += ms;
            worstMs = std::max(worstMs, ms);
            frames++;
            if (Clock::now() - summaryStart >= std::chrono::seconds(3)) {
                // The game's own rate (normally 30 or 20 a second): lower means the game itself slows down
                const double seconds = std::chrono::duration<double>(Clock::now() - summaryStart).count();
                printf("game: %.1f frames/s, busy %.1f ms per frame (worst %.0f), waiting %.1f\n",
                       frames / seconds, totalMs / frames, worstMs, waitedMs / frames);
                summaryStart = Clock::now();
                waitedMs = 0;
                totalMs = 0;
                worstMs = 0;
                frames = 0;
            }
#endif
        }
        sGameThreadDone.store(true);
    };
#ifdef __3DS__
    // On a New 3DS the game logic gets core 2 to itself, alongside the renderer on core 0
    sGameThread3ds = Fast::Start3dsThreadOnCore([](void*) { gameLoop(); }, nullptr, 2);
    printf("Game thread on core %d\n", sGameThread3ds != nullptr ? 2 : 0);
    // Startup is done: the bottom screen now shows the game's stats and frame rate instead
    Fast::Set3dsLoadProgress(1.0f, "Done");
    Fast::Show3dsStatusScreen("Banjo-Kazooie", "v" LH_3DS_VERSION);
    if (sGameThread3ds == nullptr) {
        sGameThread = std::thread(gameLoop);
    }
#else
    sGameThread = std::thread(gameLoop);
#endif
    while (WindowIsRunning() || !sGameThreadDone.load()) {
        ThreadWatchdog_Beat(WATCHDOG_MAIN_LOOP);
        port_noteMainLoopAlive();
        // Pump events every iteration: a task-starved pass must not starve
        // input and window messages.
        Ship::Context::GetRawInstance()->GetWindow()->HandleEvents();
        OS_SiService();
        if (IsInlineModExtractionBusy()) {
            GameEngine::Instance->RenderGuiFrame();
            SDL_Delay(16);
            continue;
        }
        DrainRenderService();
        if (sHost != nullptr) {
            sHost->Poll();
            if (sParked.load() && sHost->WantsFrame() && OS_SpPeekPendingTask() == nullptr && !sReleaseDeferred) {
                sHost->Frame();
                continue;
            }
        }
        if (!ServiceRcp()) {
            // The gui only draws inside serviced frames, so a stalled game
            // thread would freeze ImGui with it. Render gui-only frames during
            // a stall so the menu (and the watchdog dump) stays reachable.
            if (ThreadWatchdog_IsStalled(WATCHDOG_GAME_TICK)) {
                GameEngine::Instance->RenderGuiFrame();
                SDL_Delay(16);
                continue;
            }
            SDL_Delay(1);
        }
    }
    // Ask first, then release: a thread woken before the request is set would just
    // park again.
    OS_RequestThreadExit();
    {
        std::lock_guard<std::mutex> lock(sSvcMutex);
        sShutdownRequested.store(true, std::memory_order_release);
        sSvcFn = nullptr;
    }
    sSvcCv.notify_all();
    OS_BeginShutdown();

#ifdef __3DS__
    if (sGameThread3ds != nullptr) {
        Fast::Join3dsThread(sGameThread3ds);
    }
#endif
    if (sGameThread.joinable()) {
        sGameThread.join();
    }
    // Before Destroy: these threads draw and play audio through the engine.
    OS_JoinDecompThreads();
    ThreadWatchdog_Stop();
    OS_StopViTicker();
    OS_StopTimerWorker();
#ifdef USE_NETWORKING
    Anchor::GetInstance()->Disable();
    SDLNet_Quit();
#endif
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    GameEngine::Instance->Destroy();
    sHost.reset();
#ifdef __3DS__
    // Tear the context down here rather than in static destructors after main returns, so the
    // window's Destroy (and with it SDL_Quit, which stops the sound threads) runs at a known
    // point, before the audio player and everything else that uses SDL
    Ship::Context::DestroyInstance();
#endif
    GameEngine::RelaunchIfRequested(argc, argv);
    return 0;
}
