// XAudio2 and X3DAudio test: engine, callbacks, buffers, loops, frequency ratio, submix routing,
// 2.7 engine through CoCreateInstance. Pass "8" to use xaudio2_8.dll instead of xaudio2_9.dll.
#define INITGUID
#include <windows.h>
#include <xaudio2.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static const double PI = 3.14159265358979323846;

// ---- X3DAudio structures (x3daudio.h is packed to one byte) ----
#pragma pack(push, 1)
struct V3 { float x, y, z; };
struct XCurvePt { float d, v; };
struct XCurve { XCurvePt *pts; UINT32 n; };
struct XListener { V3 front, top, pos, vel; void *cone; };
struct XEmitter {
    void *cone; V3 front, top, pos, vel; float innerR, innerRA; UINT32 ch; float chR; float *az;
    XCurve *vol, *lfe, *lpfd, *lpfr, *rev; float curveScaler, dopScaler;
};
struct XDsp { float *m; float *delay; UINT32 src, dst; float lpfd, lpfr, rev, dop, angle, dist, evel, lvel; };
#pragma pack(pop)
typedef void (__cdecl *PFN_X3DInit)(UINT32, float, BYTE *);
typedef void (__cdecl *PFN_X3DCalc)(const BYTE *, const XListener *, const XEmitter *, UINT32, XDsp *);

// ---- voice callback recording an ordered event log ----
struct CB : IXAudio2VoiceCallback {
    HANDLE ev;
    CRITICAL_SECTION cs;
    char log[256];
    LONG passes, starts, ends, loops;
    CB() { ev = CreateEventA(NULL, FALSE, FALSE, NULL); InitializeCriticalSection(&cs); log[0] = 0; passes = starts = ends = loops = 0; }
    void add(const char *fmt, int v) {
        EnterCriticalSection(&cs);
        size_t n = strlen(log);
        snprintf(log + n, sizeof log - n, fmt, v);
        LeaveCriticalSection(&cs);
    }
    void reset() { EnterCriticalSection(&cs); log[0] = 0; starts = ends = loops = 0; LeaveCriticalSection(&cs); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) { InterlockedIncrement(&passes); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() {}
    void STDMETHODCALLTYPE OnStreamEnd() { add("SE ", 0); SetEvent(ev); }
    void STDMETHODCALLTYPE OnBufferStart(void *c) { InterlockedIncrement(&starts); add("S%d ", (int)(intptr_t)c); }
    void STDMETHODCALLTYPE OnBufferEnd(void *c) { InterlockedIncrement(&ends); add("E%d ", (int)(intptr_t)c); }
    void STDMETHODCALLTYPE OnLoopEnd(void *c) { InterlockedIncrement(&loops); add("L%d ", (int)(intptr_t)c); }
    void STDMETHODCALLTYPE OnVoiceError(void *, HRESULT) {}
};
struct ECB : IXAudio2EngineCallback {
    volatile LONG passes;
    ECB() : passes(0) {}
    void STDMETHODCALLTYPE OnProcessingPassStart() { InterlockedIncrement(&passes); }
    void STDMETHODCALLTYPE OnProcessingPassEnd() {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT) {}
};

static int16_t *sine16(int frames, int rate, double freq, double amp) {
    int16_t *p = (int16_t *)malloc(sizeof(int16_t) * (size_t)frames);
    for (int i = 0; i < frames; i++) p[i] = (int16_t)(sin(2 * PI * freq * i / rate) * amp * 32767);
    return p;
}
static WAVEFORMATEX fmt(int tag, int ch, int rate, int bits) {
    WAVEFORMATEX w;
    memset(&w, 0, sizeof w);
    w.wFormatTag = (WORD)tag; w.nChannels = (WORD)ch; w.nSamplesPerSec = (DWORD)rate; w.wBitsPerSample = (WORD)bits;
    w.nBlockAlign = (WORD)(ch * bits / 8); w.nAvgBytesPerSec = (DWORD)rate * w.nBlockAlign;
    return w;
}
static XAUDIO2_BUFFER buf(const void *data, int frames, int block, void *ctx, int flags) {
    XAUDIO2_BUFFER b;
    memset(&b, 0, sizeof b);
    b.Flags = (UINT32)flags; b.AudioBytes = (UINT32)(frames * block); b.pAudioData = (const BYTE *)data; b.pContext = ctx;
    return b;
}
static bool wait_end(CB &cb, DWORD ms = 8000) { return WaitForSingleObject(cb.ev, ms) == WAIT_OBJECT_0; }
static void settle() { Sleep(100); } // callbacks for the last events run on the audio thread

typedef HRESULT (WINAPI *PFN_Create)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateMaster28)(IXAudio2 *, IXAudio2MasteringVoice **, UINT32, UINT32, UINT32, LPCWSTR, const XAUDIO2_EFFECT_CHAIN *);

int main(int argc, char **argv) {
    bool v28 = argc > 1 && argv[1][0] == '8';
    HMODULE xa = LoadLibraryA(v28 ? "xaudio2_8.dll" : "xaudio2_9.dll");
    PFN_Create create = xa ? (PFN_Create)GetProcAddress(xa, "XAudio2Create") : NULL;
    printf("library: %d\n", create != NULL);
    if (!create) return 1;

    IXAudio2 *xa2 = NULL;
    HRESULT hr = create(&xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    printf("XAudio2Create: %lx\n", (unsigned long)hr);
    ECB ecb;
    hr = xa2->RegisterForCallbacks(&ecb);
    printf("RegisterForCallbacks: %lx\n", (unsigned long)hr);
    IXAudio2MasteringVoice *master = NULL;
    if (v28) hr = ((PFN_CreateMaster28)((void **)*(void ***)xa2)[7])(xa2, &master, 2, 44100, 0, NULL, NULL);
    else hr = xa2->CreateMasteringVoice(&master, 2, 44100, 0, NULL, NULL, AudioCategory_GameEffects);
    printf("CreateMasteringVoice: %lx\n", (unsigned long)hr);
    XAUDIO2_VOICE_DETAILS vd;
    master->GetVoiceDetails(&vd);
    printf("master channels=%u rate=%u\n", vd.InputChannels, vd.InputSampleRate);
    IXAudio2MasteringVoice *master2 = NULL;
    if (!v28) printf("second mastering voice: %lx\n", (unsigned long)xa2->CreateMasteringVoice(&master2, 2, 44100, 0, NULL, NULL, AudioCategory_GameEffects));

    // ---- two buffers with contexts, end of stream ----
    CB cb;
    WAVEFORMATEX m22 = fmt(WAVE_FORMAT_PCM, 1, 22050, 16);
    IXAudio2SourceVoice *sv = NULL;
    hr = xa2->CreateSourceVoice(&sv, &m22, 0, 2.0f, &cb, NULL, NULL);
    printf("CreateSourceVoice: %lx\n", (unsigned long)hr);
    const int N = 5500;
    int16_t *a = sine16(N, 22050, 440, 0.5), *b = sine16(N, 22050, 660, 0.5);
    XAUDIO2_BUFFER b1 = buf(a, N, 2, (void *)1, 0), b2 = buf(b, N, 2, (void *)2, XAUDIO2_END_OF_STREAM);
    HRESULT h1 = sv->SubmitSourceBuffer(&b1);
    HRESULT h2 = sv->SubmitSourceBuffer(&b2);
    printf("submit: %lx %lx\n", (unsigned long)h1, (unsigned long)h2);
    XAUDIO2_VOICE_STATE vs;
    sv->GetState(&vs);
    printf("queued before start=%u context=%d samples=%llu\n", vs.BuffersQueued, (int)(intptr_t)vs.pCurrentBufferContext, (unsigned long long)vs.SamplesPlayed);
    printf("start: %lx\n", (unsigned long)sv->Start(0));
    printf("stream end seen: %d\n", wait_end(cb));
    settle();
    sv->GetState(&vs);
    printf("events: %s\n", cb.log);
    printf("queued=%u samples=%llu\n", vs.BuffersQueued, (unsigned long long)vs.SamplesPlayed);
    printf("voice pass callbacks: %d engine pass callbacks: %d\n", cb.passes > 0, ecb.passes > 0);
    sv->GetState(&vs, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    printf("nosamplesplayed: queued=%u\n", vs.BuffersQueued);
    sv->Stop(0);
    sv->DestroyVoice();

    // ---- loops ----
    cb.reset();
    hr = xa2->CreateSourceVoice(&sv, &m22, 0, 2.0f, &cb, NULL, NULL);
    XAUDIO2_BUFFER lb = buf(a, 2000, 2, (void *)7, XAUDIO2_END_OF_STREAM);
    lb.LoopCount = 2;
    printf("loop submit: %lx\n", (unsigned long)sv->SubmitSourceBuffer(&lb));
    sv->Start(0);
    printf("loop end seen: %d\n", wait_end(cb));
    settle();
    sv->GetState(&vs);
    printf("events: %s\n", cb.log);
    printf("loop ends=%ld samples=%llu\n", (long)cb.loops, (unsigned long long)vs.SamplesPlayed);
    sv->DestroyVoice();

    // ---- frequency ratio ----
    DWORD t[2];
    for (int k = 0; k < 2; k++) {
        cb.reset();
        xa2->CreateSourceVoice(&sv, &m22, 0, 2.0f, &cb, NULL, NULL);
        float r = k ? 2.0f : 1.0f, rr = 0;
        sv->SetFrequencyRatio(r);
        sv->GetFrequencyRatio(&rr);
        printf("ratio %.1f -> %.1f\n", r, rr);
        XAUDIO2_BUFFER fb = buf(a, 4410, 2, (void *)3, XAUDIO2_END_OF_STREAM);
        sv->SubmitSourceBuffer(&fb);
        DWORD t0 = GetTickCount();
        sv->Start(0);
        wait_end(cb);
        t[k] = GetTickCount() - t0;
        settle();
        sv->GetState(&vs);
        printf("samples=%llu\n", (unsigned long long)vs.SamplesPlayed);
        sv->DestroyVoice();
    }
    printf("double speed plays in about half the time: %s\n", (t[1] < t[0] * 3 / 4 && t[0] > 100) ? "yes" : "no");

    // ---- float32 stereo through a submix voice ----
    IXAudio2SubmixVoice *sub = NULL;
    hr = xa2->CreateSubmixVoice(&sub, 2, 44100, 0, 1, NULL, NULL);
    printf("CreateSubmixVoice: %lx\n", (unsigned long)hr);
    WAVEFORMATEX f32 = fmt(WAVE_FORMAT_IEEE_FLOAT, 2, 44100, 32);
    XAUDIO2_SEND_DESCRIPTOR sd = { 0, sub };
    XAUDIO2_VOICE_SENDS sends = { 1, &sd };
    cb.reset();
    hr = xa2->CreateSourceVoice(&sv, &f32, 0, 2.0f, &cb, &sends, NULL);
    printf("CreateSourceVoice float via submix: %lx\n", (unsigned long)hr);
    const int FN = 11025;
    float *fl = (float *)malloc(sizeof(float) * 2 * FN);
    for (int i = 0; i < FN; i++) fl[2 * i] = fl[2 * i + 1] = (float)(sin(2 * PI * 660 * i / 44100.0) * 0.5);
    XAUDIO2_BUFFER fb = buf(fl, FN, 8, (void *)5, XAUDIO2_END_OF_STREAM);
    sv->SubmitSourceBuffer(&fb);
    float v = 0;
    printf("set submix volume: %lx\n", (unsigned long)sub->SetVolume(0.5f));
    sub->GetVolume(&v);
    printf("submix volume %.2f\n", v);
    float mat[4] = { 1, 0, 0, 1 }, got[4] = { 9, 9, 9, 9 };
    printf("matrix: %lx ", (unsigned long)sv->SetOutputMatrix(sub, 2, 2, mat));
    sv->GetOutputMatrix(sub, 2, 2, got);
    printf("%.0f %.0f %.0f %.0f\n", got[0], got[1], got[2], got[3]);
    float cv[2] = { 0.25f, 0.75f }, cg[2] = { 0, 0 };
    sv->SetChannelVolumes(2, cv);
    sv->GetChannelVolumes(2, cg);
    printf("channel volumes %.2f %.2f\n", cg[0], cg[1]);
    sv->SetChannelVolumes(2, (const float[]){ 1, 1 });
    sv->Start(0);
    printf("submix stream end seen: %d\n", wait_end(cb));
    settle();
    sv->GetState(&vs);
    printf("events: %s samples=%llu\n", cb.log, (unsigned long long)vs.SamplesPlayed);
    sv->DestroyVoice();
    sub->DestroyVoice();

    // ---- volume, flush, invalid calls ----
    cb.reset();
    xa2->CreateSourceVoice(&sv, &m22, 0, 2.0f, &cb, NULL, NULL);
    sv->SetVolume(0.5f);
    sv->GetVolume(&v);
    printf("volume %.2f\n", v);
    XAUDIO2_BUFFER q1 = buf(a, 1000, 2, (void *)11, 0), q2 = buf(a, 1000, 2, (void *)12, 0), q3 = buf(a, 1000, 2, (void *)13, 0);
    sv->SubmitSourceBuffer(&q1); sv->SubmitSourceBuffer(&q2); sv->SubmitSourceBuffer(&q3);
    sv->GetState(&vs);
    printf("queued=%u\n", vs.BuffersQueued);
    printf("flush: %lx\n", (unsigned long)sv->FlushSourceBuffers());
    settle();
    sv->GetState(&vs);
    printf("queued after flush=%u events: %s\n", vs.BuffersQueued, cb.log);
    XAUDIO2_BUFFER bad = buf(a, 100, 2, NULL, 0);
    bad.PlayBegin = 5000;
    printf("bad play region: %lx\n", (unsigned long)sv->SubmitSourceBuffer(&bad));
    XAUDIO2_BUFFER badloop = buf(a, 100, 2, NULL, 0);
    badloop.LoopCount = 1; badloop.LoopBegin = 500;
    printf("bad loop region: %lx\n", (unsigned long)sv->SubmitSourceBuffer(&badloop));
    int n = 0;
    XAUDIO2_BUFFER many = buf(a, 100, 2, NULL, 0);
    while (n < 100 && sv->SubmitSourceBuffer(&many) == S_OK) n++;
    printf("queue limit: %d\n", n);
    WAVEFORMATEX wma = fmt(0x161, 2, 44100, 16);
    IXAudio2SourceVoice *bad_sv = NULL;
    printf("xWMA source voice: %lx\n", (unsigned long)xa2->CreateSourceVoice(&bad_sv, &wma, 0, 2.0f, NULL, NULL, NULL));
    sv->DestroyVoice();

    // ---- no callback after DestroyVoice, even with the callback object freed (its vtable zeroed here) ----
    {
        CB *dead = new CB;
        IXAudio2SourceVoice *dv = NULL;
        xa2->CreateSourceVoice(&dv, &m22, 0, 2.0f, dead, NULL, NULL);
        XAUDIO2_BUFFER d1 = buf(a, 5500, 2, (void *)21, 0), d2 = buf(a, 5500, 2, (void *)22, 0);
        dv->SubmitSourceBuffer(&d1); dv->SubmitSourceBuffer(&d2);
        dv->Start(0);
        for (int i = 0; i < 100 && dead->starts == 0; i++) Sleep(10);
        printf("playing before destroy: %d\n", dead->starts > 0);
        dv->DestroyVoice();
        LONG c0 = dead->passes + dead->starts + dead->ends + dead->loops;
        memset((void *)dead, 0, sizeof(void *));
        Sleep(300);
        LONG c1 = dead->passes + dead->starts + dead->ends + dead->loops;
        printf("callbacks after destroy: %ld\n", (long)(c1 - c0));
    }

    // ---- effects are accepted and ignored ----
    IUnknown *rev = NULL;
    typedef HRESULT (WINAPI *PFN_Rev)(IUnknown **);
    HMODULE fxm = v28 ? xa : xa;
    PFN_Rev crev = (PFN_Rev)GetProcAddress(fxm, "CreateAudioReverb");
    printf("CreateAudioReverb: %lx\n", crev ? (unsigned long)crev(&rev) : 0xdeadu);
    if (rev) {
        XAUDIO2_EFFECT_DESCRIPTOR ed = { rev, TRUE, 2 };
        XAUDIO2_EFFECT_CHAIN ec = { 1, &ed };
        xa2->CreateSourceVoice(&sv, &m22, 0, 2.0f, NULL, NULL, &ec);
        printf("SetEffectChain: %lx\n", (unsigned long)sv->SetEffectChain(&ec));
        sv->DestroyVoice();
        printf("release: %lu\n", (unsigned long)rev->Release());
    }

    XAUDIO2_PERFORMANCE_DATA pd;
    xa2->GetPerformanceData(&pd);
    printf("performance: latency>0=%d\n", pd.CurrentLatencyInSamples > 0);
    xa2->UnregisterForCallbacks(&ecb);

    // ---- X3DAudio ----
    HMODULE x3 = LoadLibraryA("x3daudio1_7.dll");
    PFN_X3DInit xi = x3 ? (PFN_X3DInit)GetProcAddress(x3, "X3DAudioInitialize") : NULL;
    PFN_X3DCalc xc = x3 ? (PFN_X3DCalc)GetProcAddress(x3, "X3DAudioCalculate") : NULL;
    printf("x3daudio: %d\n", xi && xc);
    if (xi && xc) {
        BYTE h[20];
        xi(3, 343.5f, h);
        XListener l;
        memset(&l, 0, sizeof l);
        l.front = { 0, 0, 1 }; l.top = { 0, 1, 0 };
        XEmitter e;
        memset(&e, 0, sizeof e);
        e.front = { 0, 0, 1 }; e.top = { 0, 1, 0 }; e.ch = 1; e.curveScaler = 1; e.dopScaler = 1;
        float mm[2] = { 0, 0 };
        XDsp d;
        float res[3][2];
        V3 pos[3] = { { 10, 0, 0 }, { -10, 0, 0 }, { 0, 0, 1 } };
        for (int i = 0; i < 3; i++) {
            memset(&d, 0, sizeof d);
            d.m = mm; d.src = 1; d.dst = 2;
            e.pos = pos[i];
            xc(h, &l, &e, 1 | 0x20 | 0x40, &d);
            res[i][0] = mm[0]; res[i][1] = mm[1];
            printf("emitter %d: dist=%.1f right>left=%d left>right=%d doppler=%.2f\n", i, d.dist, mm[1] > mm[0], mm[0] > mm[1], d.dop);
        }
        printf("farther is quieter: %d\n", res[0][1] < res[2][0] + res[2][1]);
        // moving emitter: approaching makes the pitch higher
        e.pos = { 0, 0, 100 }; e.vel = { 0, 0, -50 };
        memset(&d, 0, sizeof d);
        d.m = mm; d.src = 1; d.dst = 2;
        xc(h, &l, &e, 1 | 0x20, &d);
        printf("approaching doppler>1: %d\n", d.dop > 1.0f);
        // custom curve: silent beyond the distance scaler
        XCurvePt pts[2] = { { 0, 1 }, { 1, 0 } };
        XCurve c = { pts, 2 };
        e.vol = &c; e.pos = { 5, 0, 0 }; e.vel = { 0, 0, 0 }; e.curveScaler = 4;
        memset(&d, 0, sizeof d);
        d.m = mm; d.src = 1; d.dst = 2;
        xc(h, &l, &e, 1, &d);
        printf("curve: silent=%d\n", mm[0] == 0 && mm[1] == 0);
    }

    // ---- XAudio2 2.7 through CoCreateInstance ----
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IXAudio27 *x7 = NULL;
    hr = CoCreateInstance(CLSID_XAudio27, NULL, CLSCTX_INPROC_SERVER, IID_IXAudio27, (void **)&x7);
    printf("CoCreateInstance XAudio27: %lx\n", (unsigned long)hr);
    if (x7) {
        UINT32 dc = 0;
        x7->GetDeviceCount(&dc);
        printf("devices=%u init=%lx\n", dc, (unsigned long)x7->Initialize(0, XAUDIO2_DEFAULT_PROCESSOR));
        IXAudio2MasteringVoice *mv7 = NULL;
        hr = x7->CreateMasteringVoice(&mv7, 2, 44100, 0, 0, NULL);
        printf("27 CreateMasteringVoice: %lx\n", (unsigned long)hr);
        CB cb7;
        IXAudio2SourceVoice *tmp = NULL;
        hr = x7->CreateSourceVoice(&tmp, &m22, 0, 2.0f, &cb7, NULL, NULL);
        printf("27 CreateSourceVoice: %lx\n", (unsigned long)hr);
        IXAudio27SourceVoice *s7 = (IXAudio27SourceVoice *)tmp;
        XAUDIO2_BUFFER xb = buf(a, 2200, 2, (void *)9, XAUDIO2_END_OF_STREAM);
        s7->SubmitSourceBuffer(&xb);
        s7->Start(0);
        printf("27 stream end seen: %d\n", wait_end(cb7));
        settle();
        XAUDIO2_VOICE_STATE s7st;
        s7->GetState(&s7st);
        XAUDIO27_VOICE_DETAILS d7;
        s7->GetVoiceDetails(&d7);
        printf("27 events: %s queued=%u samples=%llu channels=%u rate=%u\n", cb7.log, s7st.BuffersQueued, (unsigned long long)s7st.SamplesPlayed, d7.InputChannels, d7.InputSampleRate);
        s7->DestroyVoice();
        ((IXAudio27Voice *)mv7)->DestroyVoice();
        x7->StopEngine();
        printf("27 release: %lu\n", (unsigned long)x7->Release());
    }

    master->DestroyVoice();
    printf("release: %lu\n", (unsigned long)xa2->Release());
    printf("done\n");
    fflush(stdout);
    return 0;
}
