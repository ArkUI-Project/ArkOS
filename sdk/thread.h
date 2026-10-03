#ifndef ARK_SDK_THREAD_H
#define ARK_SDK_THREAD_H
#include "ark_api.h"
/* Native shared-address threads. Return from entry exits only that thread.
 * No pthread/Linux ABI is claimed. Synchronize shared data with C11 atomics. */
static void ark_thread_return(void) {
    ArkThreadRequest r = {0};
    r.op = ARK_THREAD_EXIT;
    (void)ark_thread(&r);
    for (;;)
        ark_yield();
}
static inline int64_t ark_thread_create(void (*entry)(void *), void *arg) {
    ArkThreadRequest r = {0};
    r.op = ARK_THREAD_CREATE;
    r.entry = (uintptr_t)entry;
    r.argument = (uintptr_t)arg;
    r.trampoline = (uintptr_t)ark_thread_return;
    int64_t result = ark_thread(&r);
    return result < 0 ? result : (int64_t)r.tid;
}
static inline int64_t ark_thread_join(uint32_t tid, int *status) {
    ArkThreadRequest r = {0};
    r.op = ARK_THREAD_JOIN;
    r.tid = tid;
    int64_t result;
    while ((result = ark_thread(&r)) == -11)
        ark_yield();
    if (!result && status)
        *status = r.status;
    return result;
}
static inline int64_t ark_sleep(uint64_t ticks) {
    ArkThreadRequest r = {0};
    r.op = ARK_THREAD_SLEEP;
    r.ticks = ticks;
    return ark_thread(&r);
}
/* Event-aware wait; returns early for queued input/messages. Maximum 24 hours. */
static inline int64_t ark_wait_milliseconds(uint64_t ms) {
    return ark_wait_ms(ms);
}
#endif
