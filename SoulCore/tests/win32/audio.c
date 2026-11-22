// Sound: waveOut with every callback kind, PlaySound (memory, file, system sounds), Beep and
// DirectSound (looping buffer with notify positions). The output is checked for the API's
// behaviour; the recorded mix (soulrun --audio) is checked by the test script.
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define RATE 22050

static void sine(short *s, int n, int freq, int rate, int channels) {
    for (int i = 0; i < n; i++)
        for (int c = 0; c < channels; c++) s[i * channels + c] = (short)(sin(2 * 3.14159265 * freq * i / rate) * 12000);
}
static volatile LONG g_open, g_done, g_close;
static void CALLBACK wave_proc(HWAVEOUT h, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2) {
    (void)h; (void)p2;
    if (inst != 0x1234) return;
    if (msg == WOM_OPEN) InterlockedIncrement(&g_open);
    if (msg == WOM_DONE && p1) InterlockedIncrement(&g_done);
    if (msg == WOM_CLOSE) InterlockedIncrement(&g_close);
}
static int g_win_done;
static LRESULT CALLBACK wndproc(HWND hw, UINT m, WPARAM w, LPARAM l) {
    if (m == MM_WOM_DONE) { g_win_done++; PostQuitMessage(0); return 0; }
    return DefWindowProcA(hw, m, w, l);
}
static BOOL CALLBACK enum_cb(LPGUID g, LPCSTR desc, LPCSTR mod, LPVOID ctx) {
    (void)mod;
    (*(int *)ctx)++;
    if (!g) printf("enum: primary device %s\n", desc && desc[0] ? "named" : "unnamed");
    return TRUE;
}

static void wave_tests(void) {
    printf("devices=%u\n", waveOutGetNumDevs());
    WAVEOUTCAPSA caps;
    MMRESULT r = waveOutGetDevCapsA(0, &caps, sizeof caps);
    printf("caps=%u channels=%u name=%d\n", r, caps.wChannels, caps.szPname[0] != 0);

    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, RATE, RATE * 2, 2, 16, 0 };
    printf("query ok=%u\n", waveOutOpen(NULL, WAVE_MAPPER, &wf, 0, 0, WAVE_FORMAT_QUERY));
    WAVEFORMATEX bad = wf;
    bad.wFormatTag = 0x55; // MP3: not a PCM format
    printf("query mp3=%u\n", waveOutOpen(NULL, WAVE_MAPPER, &bad, 0, 0, WAVE_FORMAT_QUERY));

    // CALLBACK_FUNCTION: three buffers of 440 Hz, 0.1 s each
    HWAVEOUT h;
    r = waveOutOpen(&h, WAVE_MAPPER, &wf, (DWORD_PTR)wave_proc, 0x1234, CALLBACK_FUNCTION);
    printf("open=%u\n", r);
    static short data[3][RATE / 10];
    WAVEHDR hdr[3];
    for (int k = 0; k < 3; k++) {
        sine(data[k], RATE / 10, 440, RATE, 1);
        memset(&hdr[k], 0, sizeof hdr[k]);
        hdr[k].lpData = (LPSTR)data[k];
        hdr[k].dwBufferLength = sizeof data[k];
        printf("prepare=%u ", waveOutPrepareHeader(h, &hdr[k], sizeof hdr[k]));
    }
    printf("\n");
    WAVEHDR raw = { 0 };
    raw.lpData = (LPSTR)data[0];
    raw.dwBufferLength = 4;
    printf("write unprepared=%u\n", waveOutWrite(h, &raw, sizeof raw));
    DWORD t0 = GetTickCount();
    for (int k = 0; k < 3; k++) waveOutWrite(h, &hdr[k], sizeof hdr[k]);
    printf("queued=%d\n", (hdr[0].dwFlags & WHDR_INQUEUE) != 0);
    printf("unprepare while playing=%u\n", waveOutUnprepareHeader(h, &hdr[2], sizeof hdr[2]));
    while (!(hdr[2].dwFlags & WHDR_DONE) && GetTickCount() - t0 < 5000) Sleep(10);
    DWORD took = GetTickCount() - t0;
    printf("all done=%d took>=250ms=%d\n", (hdr[0].dwFlags & hdr[1].dwFlags & hdr[2].dwFlags & WHDR_DONE) != 0, took >= 250);
    MMTIME mt = { TIME_BYTES };
    waveOutGetPosition(h, &mt, sizeof mt);
    printf("position bytes=%lu\n", (unsigned long)mt.u.cb);
    mt.wType = TIME_SAMPLES;
    waveOutGetPosition(h, &mt, sizeof mt);
    printf("position samples=%lu\n", (unsigned long)mt.u.sample);
    for (int k = 0; k < 3; k++) waveOutUnprepareHeader(h, &hdr[k], sizeof hdr[k]);
    waveOutSetVolume(h, 0x8000FFFF);
    DWORD vol = 0;
    waveOutGetVolume(h, &vol);
    printf("volume=%08lx\n", (unsigned long)vol);
    printf("close=%u\n", waveOutClose(h));
    for (int i = 0; i < 100 && !g_close; i++) Sleep(10);
    printf("callbacks open=%ld done=%ld close=%ld\n", g_open, g_done, g_close);

    // CALLBACK_EVENT
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    waveOutOpen(&h, 0, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
    WaitForSingleObject(ev, 1000); // WOM_OPEN
    waveOutPrepareHeader(h, &hdr[0], sizeof hdr[0]);
    waveOutWrite(h, &hdr[0], sizeof hdr[0]);
    DWORD w = WaitForSingleObject(ev, 3000);
    printf("event wait=%lu done=%d\n", (unsigned long)w, (hdr[0].dwFlags & WHDR_DONE) != 0);
    // waveOutReset hands queued buffers back at once
    waveOutPrepareHeader(h, &hdr[1], sizeof hdr[1]);
    waveOutWrite(h, &hdr[1], sizeof hdr[1]);
    waveOutPause(h);
    waveOutReset(h);
    printf("reset done=%d\n", (hdr[1].dwFlags & WHDR_DONE) != 0);
    waveOutUnprepareHeader(h, &hdr[0], sizeof hdr[0]);
    waveOutUnprepareHeader(h, &hdr[1], sizeof hdr[1]);
    waveOutClose(h);
    CloseHandle(ev);

    // CALLBACK_WINDOW, 8-bit stereo
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "AudioTest";
    RegisterClassA(&wc);
    HWND hw = CreateWindowA("AudioTest", "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    WAVEFORMATEX w8 = { WAVE_FORMAT_PCM, 2, 11025, 11025 * 2, 2, 8, 0 };
    r = waveOutOpen(&h, 0, &w8, (DWORD_PTR)hw, 0, CALLBACK_WINDOW);
    static unsigned char d8[11025 / 10 * 2];
    for (int i = 0; i < 11025 / 10; i++) d8[2 * i] = d8[2 * i + 1] = (unsigned char)(128 + 60 * sin(2 * 3.14159265 * 550 * i / 11025));
    WAVEHDR h8 = { 0 };
    h8.lpData = (LPSTR)d8;
    h8.dwBufferLength = sizeof d8;
    waveOutPrepareHeader(h, &h8, sizeof h8);
    waveOutWrite(h, &h8, sizeof h8);
    SetTimer(hw, 1, 3000, NULL);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { if (msg.message == WM_TIMER) break; DispatchMessageA(&msg); }
    printf("window open=%u done messages=%d\n", r, g_win_done);
    waveOutUnprepareHeader(h, &h8, sizeof h8);
    waveOutClose(h);
    DestroyWindow(hw);
}

static void playsound_tests(void) {
    // a WAV image in memory: 0.2 s of 660 Hz
    int n = RATE / 5;
    size_t size = 44 + (size_t)n * 2;
    unsigned char *wav = malloc(size);
    DWORD riff = (DWORD)size - 8, fmt = 16, rate = RATE, bps = RATE * 2, data = (DWORD)n * 2;
    WORD pcm = 1, ch = 1, align = 2, bits = 16;
    memcpy(wav, "RIFF", 4); memcpy(wav + 4, &riff, 4); memcpy(wav + 8, "WAVEfmt ", 8); memcpy(wav + 16, &fmt, 4);
    memcpy(wav + 20, &pcm, 2); memcpy(wav + 22, &ch, 2); memcpy(wav + 24, &rate, 4); memcpy(wav + 28, &bps, 4);
    memcpy(wav + 32, &align, 2); memcpy(wav + 34, &bits, 2); memcpy(wav + 36, "data", 4); memcpy(wav + 40, &data, 4);
    sine((short *)(wav + 44), n, 660, RATE, 1);
    DWORD t0 = GetTickCount();
    BOOL ok = PlaySoundA((LPCSTR)wav, NULL, SND_MEMORY | SND_SYNC);
    printf("PlaySound memory=%d waited=%d\n", ok, GetTickCount() - t0 >= 150);
    // the same sound as a file, asynchronously, then stopped
    HANDLE f = CreateFileA("C:\\users\\soul\\Temp\\ses.wav", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wr;
    WriteFile(f, wav, (DWORD)size, &wr, NULL);
    CloseHandle(f);
    t0 = GetTickCount();
    ok = PlaySoundW(L"C:\\users\\soul\\Temp\\ses.wav", NULL, SND_FILENAME | SND_ASYNC);
    printf("PlaySound file async=%d returned at once=%d\n", ok, GetTickCount() - t0 < 100);
    printf("nostop while playing=%d\n", PlaySoundA("C:\\users\\soul\\Temp\\ses.wav", NULL, SND_FILENAME | SND_ASYNC | SND_NOSTOP));
    printf("stop=%d\n", PlaySoundA(NULL, NULL, 0));
    printf("sndPlaySound=%d\n", sndPlaySoundA("C:\\users\\soul\\Temp\\ses.wav", SND_SYNC));
    printf("missing file nodefault=%d\n", PlaySoundA("C:\\yok\\yok.wav", NULL, SND_FILENAME | SND_NODEFAULT));
    printf("system sound=%d\n", PlaySoundA("SystemAsterisk", NULL, SND_ALIAS | SND_ASYNC));
    DeleteFileA("C:\\users\\soul\\Temp\\ses.wav");
    free(wav);
    t0 = GetTickCount();
    BOOL beep = Beep(880, 100);
    printf("Beep=%d waited=%d\n", beep, GetTickCount() - t0 >= 80);
    printf("MessageBeep=%d\n", MessageBeep(MB_ICONASTERISK));
}

static void dsound_tests(void) {
    int count = 0;
    DirectSoundEnumerateA(enum_cb, &count);
    printf("enum devices=%d\n", count);
    IDirectSound8 *ds = NULL;
    HRESULT hr = DirectSoundCreate8(NULL, &ds, NULL);
    printf("create=%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return;
    HWND desk = GetDesktopWindow();
    printf("cooperative=%08lx\n", (unsigned long)IDirectSound8_SetCooperativeLevel(ds, desk, DSSCL_PRIORITY));
    DSBUFFERDESC pd = { sizeof pd, DSBCAPS_PRIMARYBUFFER, 0, 0, NULL };
    IDirectSoundBuffer *primary = NULL;
    hr = IDirectSound8_CreateSoundBuffer(ds, &pd, &primary, NULL);
    WAVEFORMATEX pf = { WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0 };
    printf("primary=%08lx setformat=%08lx\n", (unsigned long)hr, (unsigned long)IDirectSoundBuffer_SetFormat(primary, &pf));

    // secondary buffer: 0.4 s, 16-bit mono, looping, notify at the middle and the start
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, RATE, RATE * 2, 2, 16, 0 };
    DWORD bytes = RATE * 2 * 4 / 10;
    DSBUFFERDESC bd = { sizeof bd, DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_GETCURRENTPOSITION2, bytes, 0, &wf };
    IDirectSoundBuffer *buf = NULL;
    hr = IDirectSound8_CreateSoundBuffer(ds, &bd, &buf, NULL);
    printf("secondary=%08lx\n", (unsigned long)hr);
    DSBCAPS bc = { sizeof bc };
    IDirectSoundBuffer_GetCaps(buf, &bc);
    printf("buffer bytes=%lu\n", (unsigned long)bc.dwBufferBytes);
    void *p1, *p2;
    DWORD n1, n2;
    hr = IDirectSoundBuffer_Lock(buf, 0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER);
    printf("lock=%08lx n1=%lu n2=%lu\n", (unsigned long)hr, (unsigned long)n1, (unsigned long)n2);
    sine(p1, (int)(n1 / 2), 330, RATE, 1);
    IDirectSoundBuffer_Unlock(buf, p1, n1, p2, n2);
    hr = IDirectSoundBuffer_Lock(buf, bytes - 100, 200, &p1, &n1, &p2, &n2, 0);
    printf("wrapped lock n1=%lu n2=%lu second=%d\n", (unsigned long)n1, (unsigned long)n2, p2 != NULL);
    IDirectSoundBuffer_Unlock(buf, p1, n1, p2, n2);

    IDirectSoundNotify *nt = NULL;
    hr = IDirectSoundBuffer_QueryInterface(buf, &IID_IDirectSoundNotify, (void **)&nt);
    printf("notify interface=%08lx\n", (unsigned long)hr);
    HANDLE evs[2] = { CreateEventA(NULL, FALSE, FALSE, NULL), CreateEventA(NULL, FALSE, FALSE, NULL) };
    DSBPOSITIONNOTIFY pn[2] = { { bytes / 2, evs[0] }, { 0, evs[1] } };
    printf("positions=%08lx\n", (unsigned long)IDirectSoundNotify_SetNotificationPositions(nt, 2, pn));
    IDirectSound3DListener *l3d = NULL;
    printf("3d listener=%s\n", IDirectSoundBuffer_QueryInterface(primary, &IID_IDirectSound3DListener, (void **)&l3d) == E_NOINTERFACE ? "no" : "yes");

    IDirectSoundBuffer_SetVolume(buf, -600);
    LONG vol = 0;
    IDirectSoundBuffer_GetVolume(buf, &vol);
    printf("volume=%ld\n", vol);
    DWORD t0 = GetTickCount();
    printf("play=%08lx\n", (unsigned long)IDirectSoundBuffer_Play(buf, 0, 0, DSBPLAY_LOOPING));
    DWORD status = 0;
    IDirectSoundBuffer_GetStatus(buf, &status);
    printf("status playing=%d looping=%d\n", (status & DSBSTATUS_PLAYING) != 0, (status & DSBSTATUS_LOOPING) != 0);
    int halves = 0, wraps = 0;
    while ((halves < 2 || wraps < 2) && GetTickCount() - t0 < 5000) {
        DWORD r = WaitForMultipleObjects(2, evs, FALSE, 1000);
        if (r == WAIT_OBJECT_0) halves++;
        else if (r == WAIT_OBJECT_0 + 1) wraps++;
    }
    DWORD took = GetTickCount() - t0;
    // offset 0 fires at once, the middle at 0.2 s, the start again at 0.4 s, the middle at 0.6 s
    printf("notifications middle=%d start=%d took>=500ms=%d\n", halves >= 2, wraps >= 2, took >= 500);
    DWORD play = 0, write = 0;
    IDirectSoundBuffer_GetCurrentPosition(buf, &play, &write);
    printf("cursors inside=%d\n", play < bytes && write < bytes);
    IDirectSoundBuffer_Stop(buf);
    IDirectSoundBuffer_GetStatus(buf, &status);
    printf("stopped=%d\n", status == 0);
    IDirectSoundBuffer *dup = NULL;
    printf("duplicate=%08lx\n", (unsigned long)IDirectSound8_DuplicateSoundBuffer(ds, buf, &dup));
    if (dup) IDirectSoundBuffer_Release(dup);
    IDirectSoundNotify_Release(nt);
    IDirectSoundBuffer_Release(buf);
    IDirectSoundBuffer_Release(primary);
    IDirectSound8_Release(ds);
    CloseHandle(evs[0]);
    CloseHandle(evs[1]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    wave_tests();
    playsound_tests();
    dsound_tests();
    printf("done\n");
    return 0;
}
