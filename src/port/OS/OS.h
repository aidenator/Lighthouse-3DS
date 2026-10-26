#ifndef PORT_OS_H
#define PORT_OS_H

// N64 OS SDK
//
// libultraship already implements most of the libultra surface, and where its
// version is correct for a PC port we use it as-is. This layer exists only for
// the calls libultraship cannot serve, so that the decomp's own threading code
// can run instead of being replaced by port stand-ins.

#ifdef __cplusplus
extern "C" {
#endif

#include "libultraship/libultra/message.h"
#include "libultraship/libultra/thread.h"
#include "libultraship/libultra/pfs.h"
#include "libultraship/libultra/sptask.h"
#include "libultraship/libultra/time.h"

void OS_CreateThread(OSThread* thread, OSId id, void* entry, void* arg, void* sp, OSPri p);
void OS_StartThread(OSThread* thread);
void OS_StopThread(OSThread* thread);
void OS_DestroyThread(OSThread* thread);
void OS_SetThreadPri(OSThread* thread, OSPri p);

// Allowlist one decomp thread entry point. Until its entry is passed here, an
// osCreateThread/osStartThread pair is recorded but never launched, so threads
// are revived deliberately, one consumer at a time.
void OS_EnableThreadEntry(void* entry);
// The CPU core a thread started from entry runs on, where the platform can choose (the 3DS)
void OS_SetThreadEntryCore(void* entry, int core);

// OS_MESG_BLOCK only blocks on queues opted in here.
void OS_SetQueueBlocking(OSMesgQueue* mq, int enabled);

// Cooperative exit for threads.
void OS_RequestThreadExit(void);
int OS_ThreadShouldExit(void);
void OS_JoinDecompThreads(void);

// Stop every queue blocking and wake all waiters, so the tick can unwind and be
// joined. Call once the window has closed and before joining it.
void OS_BeginShutdown(void);

// Watchdog diagnostics: threads currently parked inside a blocking
// osSendMesg/osRecvMesg, so a stall dump can say where each one waits.
typedef struct OS_BlockedWait {
    unsigned long tid; // SDL thread id
    OSMesgQueue* mq;
    int isSend; // 0 = recv, 1 = send/jam
} OS_BlockedWait;
int OS_MesgSnapshotBlockedWaits(OS_BlockedWait* out, int max);

// Raise a registered hardware event (VI retrace, SI done, RDP done).
// 0 when sent, -1 when the event isn't set up or its queue is full (the message is dropped)
int OS_SendEventMesg(OSEvent event);

// Raise an event ahead of whatever the recipient already has queued, the way
// an interrupt preempts. Used for completions that must not be reordered
// behind pending messages.
void OS_JamEventMesg(OSEvent event);

// The VI ticker starts with osCreateViManager; this stops it at shutdown.
void OS_StopViTicker(void);

// Complete one pending controller read: poll the pads and raise OS_EVENT_SI.
// Called by the thread that owns SDL input.
int OS_SiService(void);

// Whether that thread has begun servicing SI at all.
int OS_SiPumpLive(void);

// Cancel a pending timer. Both halves of the pair are ours (OS_Timer.cpp), but
// only osSetTimer has a prototype in LUS to be found through, so a decomp caller
// needs this one declared here.
int osStopTimer(OSTimer* t);

// The timer worker starts with the first osSetTimer; this stops it at shutdown.
void OS_StopTimerWorker(void);

s32 osPfsInit(OSMesgQueue* mq, OSPfs* pfs, s32 channel);

// Whether osViBlack has the display blanked. The renderer blacks the presented
// frame while the world keeps rendering underneath.
int OS_ViBlackActive(void);

// Take the task osSpTaskStartGo handed over, or NULL if none is pending.
OSTask* OS_SpTakePendingTask(void);

// Same, but without consuming it; the watchdog reports what is in flight.
OSTask* OS_SpPeekPendingTask(void);

// osRecvMesg on a blocking queue, but giving up after timeoutMs: 0 when a message came, -1 when
// it timed out (or the queue isn't blocking and is empty)
int32_t OS_RecvMesgTimeout(OSMesgQueue* mq, OSMesg* msg, int32_t timeoutMs);

// Waits for the frame's swap token on mq (vimgr.c), recovering if it was lost: see Game.cpp
void port_waitFrameToken(OSMesgQueue* mq);

// The swap token couldn't be sent because the game hadn't taken the last one
void port_noteDroppedFrameToken(void);

// VI swap timing, for starting a frame's render on the retrace its swap latches on.
long long OS_SteadyNs(void);
long long OS_ViLastSwapNs(void);
long long OS_ViLastLatchNs(void);
long long OS_ViNextRetraceAfterNs(long long t);

#ifdef __cplusplus
}
#endif

#endif // PORT_OS_H
