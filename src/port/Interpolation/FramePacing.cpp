#include "port/OS/Wait.h"
#include "port/Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <libultraship/libultraship.h>
#include <fast/Fast3dWindow.h>
#include <fast/interpreter.h>

#ifdef __3DS__
#include <fast/backends/gfx_3ds.h>
#endif

#include "FrameInterpolation.h"
#include "port/Enhancements/Events/Hooks/Events.h"
#include "port/Nametag/Nametag.h"
#include "port/OS/OS.h"
#include "port/Patches/Patches.h"
#include "port/UI/cvar_prefixes.h"

#define gVIsPerFrame 2 // 30 Hz

extern "C" bool prevAltAssets;
extern "C" void port_releaseRcpTask(void);

namespace {
long long sLastSubFrameNs = 0;
long long sFrameLatchNs = 0;
unsigned sFrameViSerial = 0;
bool sFrameTimed = false;

using Clock = std::chrono::steady_clock;
inline long long NsSince(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
}

void SyncAltAssets() {
    bool curAltAssets = CVarGetInteger(CVAR_SETTING("Mods.AlternateAssets"), 1);
    if (prevAltAssets != curAltAssets) {
        prevAltAssets = curAltAssets;
        Ship::Context::GetRawInstance()->GetResourceManager()->SetAltAssetsEnabled(curAltAssets);
        gfx_texture_cache_clear();
    }
}

// Draws one sub-frame without presenting it.
void DrawSubframe(Fast::Interpreter* interpreter, const std::shared_ptr<Ship::Window>& wndBase, Gfx* commands,
                  const std::unordered_map<Mtx*, MtxF>& replacements) {
    auto runT0 = Clock::now();
    auto gui = wndBase->GetGui();
    wndBase->GetMouseStateManager()->StartFrame();
    gui->StartDraw();
    interpreter->StartFrame();
    interpreter->Run(commands, replacements);
    if (OS_ViBlackActive()) {
        interpreter->mGfxFrameBuffer = 0;
        auto rapi = interpreter->GetCurrentRenderingAPI();
        rapi->StartDrawToFramebuffer(0, 1.0f);
        rapi->ClearFramebuffer(true, false);
    }
    gui->EndDraw();
    sLastSubFrameNs = NsSince(runT0);
}

void PresentSubframe(Fast::Interpreter* interpreter) {
    interpreter->EndFrame();
    CALL_EVENT(FrameDrawEnd);
    interpreter->mInterpolationIndex++;
}
} // namespace

void GameEngine::RunCommands(Gfx* Commands) {
    static const std::unordered_map<Mtx*, MtxF> kNoReplacements;
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());
    if (wnd == nullptr) {
        return;
    }
    auto interpreter = wnd->GetInterpreterWeak().lock().get();
    wnd->HandleEvents();
    interpreter->mInterpolationIndex = 0;
    auto wndBase = Ship::Context::GetRawInstance()->GetWindow();
    Nametag::SetSubframeBlend(1.0f);
    if (wndBase->IsFrameReady()) {
        DrawSubframe(interpreter, wndBase, Commands, kNoReplacements);
        PresentSubframe(interpreter);
    }
    SyncAltAssets();
}

int GameEngine::CurrentViPerTick() {
    int viPerTick = port_getDemoViCount();
    if (viPerTick <= 0) {
        viPerTick = gVIsPerFrame + port_getCutsceneExtraVis();
    }
    if (viPerTick < gVIsPerFrame) {
        viPerTick = gVIsPerFrame;
    }
    // Clamp to 15 for demo playbacks.
    if (viPerTick > 15) {
        viPerTick = 15;
    }
    return viPerTick;
}

namespace {
int EffectiveLogicFps() {
    int fps = 60 / GameEngine::CurrentViPerTick();
    return (fps < 1) ? 1 : fps;
}
} // namespace

bool GameEngine::IsInterpolationEnabled() {
    return (int)GetInterpolationFPS() > EffectiveLogicFps();
}

// Decided once per list at submit, so the pickup in ServiceRcp and the pass that draws it agree.
bool GameEngine::WantsTimedPass(bool recorded, int viPerTick) {
    return recorded && !GfxDebuggerIsDebugging() && (int)GetInterpolationFPS() > 60 / viPerTick;
}

void GameEngine::SetFrameTiming(long long latchNs, unsigned viSerial, bool timed) {
    sFrameViSerial = viSerial;
    sFrameLatchNs = latchNs;
    sFrameTimed = timed;
}

namespace {
// Room left before the next swap has to latch, for the last draw and the game's swap behind it.
constexpr long long kReleaseMarginNs = 4000000;
constexpr int kMaxTimedSubframes = 32;
// How far a prefetched blend may be from the sub-frame's own before it is redone, as a share of the tick.
constexpr float kPrefetchSlack = 0.03f;

void BuildReplacements(float t, std::unordered_map<Mtx*, MtxF>& replacements) {
    if (t < 1.0f) {
        FrameInterpolation_Interpolate(t, replacements);
    } else {
        replacements.clear();
    }
}

class Prefetcher {
public:
    void Start(float t, std::unordered_map<Mtx*, MtxF>* out) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mStarted) {
            mStarted = true;
            StartThread();
        }
        mT = t;
        mOut = out;
        mBusy = true;
        mCv.notify_all();
    }

    void Wait() {
        std::unique_lock<std::mutex> lock(mMutex);
        port_waitUntil(mCv, lock, [this] { return !mBusy; });
    }

private:
    void StartThread() {
#ifdef __3DS__
        // Off the renderer's core: core 3 when the system grants it, else core 1
        auto run = [](void* self) { ((Prefetcher*)self)->Run(); };
        for (int core : { 3, 1 }) {
            if (Fast::Start3dsThreadOnCore(run, this, core) != nullptr) {
                printf("Interpolation helper on core %d\n", core);
                return;
            }
        }
#endif
        mThread = std::thread([this] { Run(); });
    }

    void Run() {
        std::unique_lock<std::mutex> lock(mMutex);
        for (;;) {
            port_waitUntil(mCv, lock, [this] { return mOut != nullptr; });
            auto* out = mOut;
            const float t = mT;
            mOut = nullptr;
            lock.unlock();
            BuildReplacements(t, *out);
            lock.lock();
            mBusy = false;
            mCv.notify_all();
        }
    }

    std::thread mThread;
    bool mStarted = false;
    std::mutex mMutex;
    std::condition_variable mCv;
    std::unordered_map<Mtx*, MtxF>* mOut = nullptr;
    float mT = 0.0f;
    bool mBusy = false;
};

Prefetcher& GetPrefetcher() {
    static Prefetcher* sPrefetcher = new Prefetcher();
    return *sPrefetcher;
}

#ifdef __3DS__
// Where the renderer thread's time between draws goes, printed every few seconds
struct PassTimes {
    long long viWaitNs = 0;      // waiting for the next tick's VI count at a pass's start
    long long prefetchWaitNs = 0; // waiting for the helper's blend
    long long blendNs = 0;       // blending here, when no prefetch fit
    long long betweenPassesNs = 0; // from one pass's end to the next one's start
    long long lastPassEndNs = 0;
    long long windowStartNs = 0;
    int subframes = 0;
    int passes = 0;
    int blends = 0;

    void Report(long long now) {
        if (windowStartNs == 0) {
            windowStartNs = now;
            return;
        }
        if (now - windowStartNs < 3000000000LL || subframes == 0) {
            return;
        }
        const double perFrame = 1e-6 / subframes;
        printf("pacer: %d sub-frames in %d passes; per sub-frame: VI wait %.1f ms, prefetch wait %.1f, "
               "blending %.1f (%d), between passes %.1f\n",
               subframes, passes, viWaitNs * perFrame, prefetchWaitNs * perFrame, blendNs * perFrame, blends,
               betweenPassesNs * perFrame);
        *this = PassTimes{};
        windowStartNs = now;
        lastPassEndNs = now;
    }
};
PassTimes sPassTimes;
#endif

// Draws the frame at the window's rate until its successor is due to latch, blending each sub-frame to
// where its present lands between the previous frame and this one.
void RunTimedPass(Gfx* commands, int fps) {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());
    if (wnd == nullptr) {
        port_releaseRcpTask();
        return;
    }
    auto interpreter = wnd->GetInterpreterWeak().lock().get();
    auto wndBase = Ship::Context::GetRawInstance()->GetWindow();
    wnd->HandleEvents();
    interpreter->mInterpolationIndex = 0;
    static std::unordered_map<Mtx*, MtxF> maps[2];
    Prefetcher& prefetcher = GetPrefetcher();
    bool prefetching = false;
    float prefetchT = -1.0f;
    // The tick after this list's sets its VI count when it polls input, right after the submit, and that
    // count decides when this frame latches.
    const long long waitStartNs = OS_SteadyNs();
#ifdef __3DS__
    if (sPassTimes.lastPassEndNs != 0) {
        sPassTimes.betweenPassesNs += waitStartNs - sPassTimes.lastPassEndNs;
    }
    sPassTimes.passes++;
#endif
    const long long waitEndNs = waitStartNs + 3000000;
    while (port_getDemoViSerial() == sFrameViSerial && OS_SteadyNs() < waitEndNs) {
        port_serviceRenderRequests();
        port_waitDemoViSerial(sFrameViSerial, 250);
    }
#ifdef __3DS__
    sPassTimes.viWaitNs += OS_SteadyNs() - waitStartNs;
#endif
    const int vis = std::max(port_getDemoViCount(), 2);
    const long long tickNs = 1000000000LL * vis / 60;
    const long long presentNs = 1000000000LL / fps;
    const long long passStartNs = OS_SteadyNs();
    long long anchorNs = sFrameLatchNs;
    if (anchorNs == 0 || std::llabs(passStartNs - anchorNs) > tickNs) {
        const long long latchNs = OS_ViLastLatchNs();
        anchorNs = (latchNs > 0 && passStartNs - latchNs < tickNs) ? latchNs : passStartNs;
    }
    const long long releaseBy = anchorNs + tickNs - kReleaseMarginNs;
    long long prevRunNs = 0;
    long long runIntervalNs = presentNs;
    int count = 0;

    for (;;) {
        const long long now = OS_SteadyNs();
        if (prevRunNs != 0) {
            runIntervalNs = std::max(presentNs, now - prevRunNs);
        }
        prevRunNs = now;
        // Last when another draw after this one could not be finished by the release deadline.
        const bool last = count + 1 >= kMaxTimedSubframes || now + runIntervalNs + sLastSubFrameNs > releaseBy;
        float t = std::clamp((float)(now + presentNs - anchorNs) / (float)tickNs, 0.0f, 1.0f);
        auto& replacements = maps[count & 1];
        bool prefetched = false;
        if (prefetching) {
#ifdef __3DS__
            const long long prefetchWaitStart = OS_SteadyNs();
            prefetcher.Wait();
            sPassTimes.prefetchWaitNs += OS_SteadyNs() - prefetchWaitStart;
#else
            prefetcher.Wait();
#endif
            prefetching = false;
            if (std::fabs(prefetchT - t) <= kPrefetchSlack) {
                t = prefetchT;
                prefetched = true;
            }
        }
        if (!prefetched) {
#ifdef __3DS__
            const long long blendStart = OS_SteadyNs();
            BuildReplacements(t, replacements);
            sPassTimes.blendNs += OS_SteadyNs() - blendStart;
            sPassTimes.blends++;
#else
            BuildReplacements(t, replacements);
#endif
        }
        if (!last) {
            prefetchT = std::clamp((float)(now + runIntervalNs + presentNs - anchorNs) / (float)tickNs, 0.0f, 1.0f);
            prefetcher.Start(prefetchT, &maps[(count + 1) & 1]);
            prefetching = true;
        }
        FrameInterpolation_ApplyAnimVertices(t);
        Nametag::SetSubframeBlend(t);
        DrawSubframe(interpreter, wndBase, commands, replacements);
        if (last) {
            port_releaseRcpTask();
        }
        PresentSubframe(interpreter);
        count++;
#ifdef __3DS__
        sPassTimes.subframes++;
#endif
        if (last) {
            break;
        }
    }
    if (prefetching) {
        prefetcher.Wait();
    }
    SyncAltAssets();
#ifdef __3DS__
    sPassTimes.lastPassEndNs = OS_SteadyNs();
    sPassTimes.Report(sPassTimes.lastPassEndNs);
#endif
}
} // namespace

void GameEngine::ProcessGfxCommands(Gfx* commands) {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());

    if (wnd == nullptr) {
        return;
    }

    // if(gEnableGammaBoost) {
    //     wnd->EnableSRGBMode();
    // }
    wnd->SetRendererUCode(UcodeHandlers::ucode_f3dex);

    if (sFrameTimed) {
        const int fps = (int)GetInterpolationFPS();
        wnd->SetTargetFps(fps);
        wnd->SetMaximumFrameLatency(2);
        RunTimedPass(commands, fps);
        return;
    }

    wnd->SetTargetFps(EffectiveLogicFps());
    wnd->SetMaximumFrameLatency(2);
    RunCommands(commands);
}

uint32_t GameEngine::GetInterpolationFPS() {
#ifdef LH_SLIM
#ifdef __3DS__
    // A New 3DS draws in-between frames up to the screen's 60 Hz (the pass drops the ones that
    // don't fit); an Old 3DS can't afford them
    static const bool sIsNew3ds = Fast::Is3dsNew();
#ifdef LH_3DS_OLD_INTERPOLATION
    if (true) {
#else
    if (sIsNew3ds) {
#endif
        return 60;
    }
#endif
    // No frame interpolation on other constrained targets: render at the game's own rate, which
    // also skips recording interpolation data each tick.
    return EffectiveLogicFps();
#endif
    if (CVarGetInteger(CVAR_SETTING("MatchRefreshRate"), 0)) {
        return Ship::Context::GetRawInstance()->GetWindow()->GetCurrentRefreshRate();

    } else if (CVarGetInteger(CVAR_VSYNC_ENABLED, 1) ||
               !Ship::Context::GetRawInstance()->GetWindow()->CanDisableVerticalSync()) {
        return std::min<uint32_t>(Ship::Context::GetRawInstance()->GetWindow()->GetCurrentRefreshRate(),
                                  CVarGetInteger(CVAR_SETTING("InterpolationFPS"), 60));
    }

    return CVarGetInteger(CVAR_SETTING("InterpolationFPS"), 30);
}
