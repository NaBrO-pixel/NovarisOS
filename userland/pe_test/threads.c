/* threads.c - exercises Milestone 17's real Win32 threads.
 *
 * Built with real i686-w64-mingw32-gcc against the real kernel32 import
 * library. Nothing here is written to accommodate Novaris: it is the
 * ordinary way a C program uses CreateThread, WaitForSingleObject,
 * InterlockedIncrement and a CRITICAL_SECTION, and it produces the same
 * output on Windows.
 *
 * Three things are being checked, and each fails differently:
 *
 *   1. The threads really run. Each prints its own GetCurrentThreadId,
 *      which must differ between them and from the main thread's.
 *   2. InterlockedIncrement is atomic across threads. WORKERS * PER_THREAD
 *      increments must all land.
 *   3. A CRITICAL_SECTION actually serialises. The guarded counter is
 *      incremented with a deliberately non-atomic read-modify-write, with
 *      a gap in the middle to widen the race. If EnterCriticalSection were
 *      still the no-op it was through Milestone 16, this count would come
 *      out low - which is precisely the bug a stubbed lock hides.
 *
 * WaitForSingleObject on each thread handle is what makes the totals
 * meaningful: without a real join, main would print them before the
 * workers had finished.
 */

#include <windows.h>
#include <stdio.h>

#define WORKERS    3
#define PER_THREAD 20

static volatile LONG atomic_counter = 0;
static volatile long guarded_counter = 0;
static CRITICAL_SECTION guard;

static DWORD WINAPI worker(LPVOID param) {
    int id = (int)(INT_PTR)param;

    printf("  worker %d starting, GetCurrentThreadId() = %lu\n",
           id, (unsigned long)GetCurrentThreadId());

    for (int i = 0; i < PER_THREAD; i++) {
        /* Unguarded ring-3 work, so the timer has somewhere to preempt
         * this thread that is not inside a syscall. Without it a worker
         * spends nearly all its time in int 0x81 with interrupts off and
         * can run to completion inside a single time slice - which would
         * make the whole test pass without the threads ever having been
         * concurrent. */
        for (volatile int spin = 0; spin < 30000; spin++) { }

        /* (2) atomic across threads, no lock needed */
        InterlockedIncrement(&atomic_counter);

        /* (3) deliberately NOT atomic, so the lock has to do the work.
         * The local temporary and the spin between read and write widen
         * the window a preemption can land in - and that spin runs in
         * ring 3 while the lock is held, which is what forces the other
         * workers down EnterCriticalSection's contended path. */
        EnterCriticalSection(&guard);
        long seen = guarded_counter;
        for (volatile int spin = 0; spin < 300000; spin++) { }
        guarded_counter = seen + 1;
        LeaveCriticalSection(&guard);
    }

    printf("  worker %d done\n", id);
    return (DWORD)(id * 100);
}

int main(void) {
    printf("Win32 threads test - real CreateThread on Novaris\n\n");
    printf("main thread GetCurrentThreadId() = %lu\n\n",
           (unsigned long)GetCurrentThreadId());

    InitializeCriticalSection(&guard);

    HANDLE handles[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        DWORD tid = 0;
        handles[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)(i + 1),
                                  0, &tid);
        if (handles[i] == NULL) {
            printf("CreateThread %d FAILED\n", i + 1);
            return 1;
        }
        printf("created worker %d, thread id %lu\n", i + 1,
               (unsigned long)tid);
    }
    printf("\n");

    /* The join. Every worker must have finished before the totals below
     * mean anything. */
    for (int i = 0; i < WORKERS; i++) {
        WaitForSingleObject(handles[i], INFINITE);
    }

    printf("\nall %d workers joined\n", WORKERS);

    for (int i = 0; i < WORKERS; i++) {
        DWORD code = 0;
        GetExitCodeThread(handles[i], &code);
        printf("  worker %d exit code %lu\n", i + 1, (unsigned long)code);
    }

    long expected = (long)WORKERS * PER_THREAD;
    printf("\ninterlocked counter: %ld, expected %ld  -> %s\n",
           (long)atomic_counter, expected,
           atomic_counter == expected ? "ok" : "WRONG");
    printf("guarded counter:     %ld, expected %ld  -> %s\n",
           guarded_counter, expected,
           guarded_counter == expected ? "ok" : "WRONG (the lock did nothing)");

    DeleteCriticalSection(&guard);

    printf("\nthreads test done.\n");
    return 0;
}
