#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

// Waits for ready() like cv.wait(lock, ready). On the 3DS a wake-up can go missing (libctru's
// condition variable; seen in the renderer's handoff after hours of play), and a thread waiting for
// a single reply would then sleep for good, so there it checks again every 50 ms.
template <typename Ready>
void port_waitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, Ready ready) {
#ifdef __3DS__
    while (!ready()) {
        cv.wait_for(lock, std::chrono::milliseconds(50));
    }
#else
    cv.wait(lock, ready);
#endif
}
