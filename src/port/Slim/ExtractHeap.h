#pragma once

// While bk.o2r is made, small C++ allocations on the calling thread come from a slab allocator
// instead of newlib's malloc, which takes a lock and looks up the thread's state on every call.
// Torch makes about 12 million allocations, almost all of 256 bytes or less. No-ops off the 3DS.
void ExtractHeap_Begin();
void ExtractHeap_End();
