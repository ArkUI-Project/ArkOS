/* Buildable native multi-core example. Register in the catalogue to execute. */
#include "thread.h"
static uint64_t counts[4];
static void worker(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    for (unsigned i = 0; i < 1000000; i++)
        __atomic_fetch_add(&counts[id], 1, __ATOMIC_RELAXED);
}
int main(void) {
    uint32_t tid[4];
    for (unsigned i = 0; i < 4; i++) {
        int64_t t = ark_thread_create(worker, (void *)(uintptr_t)i);
        if (t < 0)
            return 1;
        tid[i] = (uint32_t)t;
    }
    for (unsigned i = 0; i < 4; i++) {
        int status = -1;
        if (ark_thread_join(tid[i], &status) != 0 || status)
            return 2;
    }
    for (unsigned i = 0; i < 4; i++)
        if (counts[i] != 1000000)
            return 3;
    return 0;
}
