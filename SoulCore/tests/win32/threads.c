#include <windows.h>
#include <process.h>
#include <stdio.h>

static CRITICAL_SECTION cs;
static long counter;
static volatile LONG spin_flag;
static __thread int tls_value = 100;
static int tls_results[4];

static DWORD WINAPI worker(LPVOID p) {
    int id = (int)(INT_PTR)p;
    for (int i = 0; i < 20000; i++) { EnterCriticalSection(&cs); counter++; LeaveCriticalSection(&cs); }
    tls_value += id;               // per-thread copy
    tls_results[id] = tls_value;
    return (DWORD)(id * 10);
}

static DWORD WINAPI late_setter(LPVOID p) { (void)p; Sleep(30); InterlockedExchange(&spin_flag, 1); return 0; }

// producer / consumer with SRW lock + condition variable
static SRWLOCK lock = SRWLOCK_INIT;
static CONDITION_VARIABLE not_empty = CONDITION_VARIABLE_INIT, not_full = CONDITION_VARIABLE_INIT;
static int queue[8], qhead, qcount;
static long long consumed_sum;
static unsigned __stdcall producer(void *p) {
    (void)p;
    for (int i = 1; i <= 500; i++) {
        AcquireSRWLockExclusive(&lock);
        while (qcount == 8) SleepConditionVariableSRW(&not_full, &lock, INFINITE, 0);
        queue[(qhead + qcount) % 8] = i;
        qcount++;
        WakeConditionVariable(&not_empty);
        ReleaseSRWLockExclusive(&lock);
    }
    return 0;
}
static unsigned __stdcall consumer(void *p) {
    (void)p;
    for (int i = 0; i < 500; i++) {
        AcquireSRWLockExclusive(&lock);
        while (qcount == 0) SleepConditionVariableSRW(&not_empty, &lock, INFINITE, 0);
        consumed_sum += queue[qhead];
        qhead = (qhead + 1) % 8;
        qcount--;
        WakeConditionVariable(&not_full);
        ReleaseSRWLockExclusive(&lock);
    }
    return 0;
}

static HANDLE sem, go_event;
static volatile LONG passed;
static DWORD WINAPI sem_waiter(LPVOID p) {
    (void)p;
    WaitForSingleObject(go_event, INFINITE);
    WaitForSingleObject(sem, INFINITE);
    InterlockedIncrement(&passed);
    return 0;
}

int main(void) {
    InitializeCriticalSection(&cs);
    HANDLE th[4];
    DWORD ids[4];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)i, 0, &ids[i]);
    DWORD r = WaitForMultipleObjects(4, th, TRUE, INFINITE);
    DWORD codes = 0;
    for (int i = 0; i < 4; i++) { DWORD c; GetExitCodeThread(th[i], &c); codes += c; CloseHandle(th[i]); }
    printf("counter=%ld wait=%lu exitcodes=%lu distinct_ids=%d\n", counter, (unsigned long)r, (unsigned long)codes, ids[0] != ids[1] && ids[1] != ids[2]);
    printf("tls main=%d threads=%d,%d,%d,%d\n", tls_value, tls_results[0], tls_results[1], tls_results[2], tls_results[3]);

    HANDLE s = CreateThread(NULL, 0, late_setter, NULL, 0, NULL);
    long spins = 0;
    while (!spin_flag) spins++;          // needs time slicing to finish
    WaitForSingleObject(s, INFINITE);
    printf("spin finished=%d\n", spins > 0);

    HANDLE pc[2];
    pc[0] = (HANDLE)_beginthreadex(NULL, 0, producer, NULL, 0, NULL);
    pc[1] = (HANDLE)_beginthreadex(NULL, 0, consumer, NULL, 0, NULL);
    WaitForMultipleObjects(2, pc, TRUE, INFINITE);
    printf("producer/consumer sum=%lld\n", consumed_sum);

    sem = CreateSemaphoreA(NULL, 0, 10, NULL);
    go_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE w[3];
    for (int i = 0; i < 3; i++) w[i] = CreateThread(NULL, 0, sem_waiter, NULL, 0, NULL);
    Sleep(10);
    printf("before release passed=%ld\n", passed);
    SetEvent(go_event);
    ReleaseSemaphore(sem, 3, NULL);
    WaitForMultipleObjects(3, w, TRUE, INFINITE);
    printf("after release passed=%ld\n", passed);

    HANDLE sus = CreateThread(NULL, 0, late_setter, NULL, CREATE_SUSPENDED, NULL);
    printf("suspended wait=%lu\n", (unsigned long)WaitForSingleObject(sus, 50));
    ResumeThread(sus);
    printf("resumed wait=%lu\n", (unsigned long)WaitForSingleObject(sus, INFINITE));
    DeleteCriticalSection(&cs);
    return 0;
}
