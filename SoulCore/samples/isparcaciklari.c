// Soul örnek programı: çoklu iş parçacığı (CreateThread, kritik bölge, olaylar)
#include <windows.h>
#include <stdio.h>
static CRITICAL_SECTION cs;
static long long total;
static HANDLE done_event;
static volatile LONG finished;
static DWORD WINAPI worker(LPVOID arg) {
    int id = (int)(INT_PTR)arg;
    long long local = 0;
    for (int i = 1; i <= 2000000; i++) local += i % (id + 3);
    EnterCriticalSection(&cs);
    total += local;
    printf("  İş parçacığı #%lu bitti (yerel toplam %lld)\n", GetCurrentThreadId(), local);
    LeaveCriticalSection(&cs);
    if (InterlockedIncrement(&finished) == 4) SetEvent(done_event);
    return 0;
}
int main(void) {
    InitializeCriticalSection(&cs);
    done_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    printf("4 iş parçacığı başlatılıyor (ana iş parçacığı #%lu)...\n", GetCurrentThreadId());
    HANDLE t[4];
    for (int i = 0; i < 4; i++) t[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)i, 0, NULL);
    WaitForSingleObject(done_event, INFINITE);
    WaitForMultipleObjects(4, t, TRUE, INFINITE);
    printf("Genel toplam: %lld\n", total);
    return 0;
}
