// SoulCore - XAudio2 (2.7, 2.8, 2.9) and X3DAudio on top of the stream voices of the mixer (audio.c).
//
// COM objects handed to the program are blocks of guest memory whose first pointer-sized field is a
// vtable of thunks. The engine (IXAudio2) is COM; voices are not (no IUnknown methods: their vtables
// start with GetVoiceDetails). Callback objects of the program (IXAudio2VoiceCallback,
// IXAudio2EngineCallback) are called by audio.c on the process's audio callback thread.
//
// Versions: 2.7 (CoCreateInstance, CLSID_XAudio2) has the extra IXAudio2 methods GetDeviceCount /
// GetDeviceDetails / Initialize, CreateMasteringVoice takes a device index instead of a device id,
// UnregisterForCallbacks / StopEngine / GetPerformanceData return nothing, GetState has no flags and
// XAUDIO2_VOICE_DETAILS has no ActiveFlags. 2.8 and 2.9 share the voice vtables; the 2.9 engine's
// CreateMasteringVoice has one more argument (AUDIO_STREAM_CATEGORY, 32-bit stack sizes differ). The
// version is taken from the DLL the program imported XAudio2Create from, or is 2.7 for CoCreateInstance.
// 2.7 voices use the same layout as 2.8 (SetOutputFilterParameters / GetOutputFilterParameters are in
// both; GetChannelMask exists only on 2.8+ mastering voices, harmlessly also provided for 2.7).
//
// All the structures of xaudio2.h and x3daudio.h are packed to one byte: offsets are computed here.
#include "hle.h"
#include "audio.h"
#include <math.h>

#define S_OK_ 0u
#define E_NOINTERFACE_ 0x80004002u
#define E_INVALIDARG_ 0x80070057u
#define E_OUTOFMEMORY_ 0x8007000Eu
#define XA_INVALID_CALL 0x88960001u
#define XA_VOICE_NOPITCH 0x2
#define XA_END_OF_STREAM 0x40
#define XA_NOSAMPLESPLAYED 0x100

typedef struct XM { const char *name; HleFn fn; int nargs; } XM;
static uint64_t g_xvt[16]; // vtable cache per process: engines 0..2 (2.7, 2.8, 2.9), voices 3 + ver8 * 3 + kind, effects 9
SOUL_PROCESS_VAR(g_xvt)

// A float argument: Win64 passes the first four in XMM registers, everything else is a stack slot.
static float farg(X86State *st, int n) {
    if (!g_mode32 && n < 4) return st->xmm[n].f[0];
    uint32_t u = (uint32_t)hle_arg(st, n);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static float rdf(uint64_t a) { uint32_t u = (uint32_t)mem_rd(a, 4); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint64_t a, float f) { uint32_t u; memcpy(&u, &f, 4); mem_wr(a, 4, u); }

static uint64_t make_vt(int slot, const char *iface, const XM *m, int n) {
    if (g_xvt[slot]) return g_xvt[slot];
    uint8_t *t = g_calloc((size_t)n, (size_t)PSZ);
    for (int i = 0; i < n; i++) {
        char name[96];
        snprintf(name, sizeof name, "%s::%s", iface, m[i].name);
        uint32_t id = hle_thunk_for(g_proc, m[i].fn, "xaudio2", name);
        g_proc->thunks[id].argbytes = g_mode32 ? (uint16_t)(4 * m[i].nargs) : 0;
        wr_ptr(H2G(t) + (uint64_t)i * (uint64_t)PSZ, g_proc->thunk_base + (uint64_t)id * SOUL_THUNK_STRIDE);
    }
    g_xvt[slot] = H2G(t);
    return g_xvt[slot];
}

// ---- object layouts ------------------------------------------------------------------------------------------
// Engine: [vtable][refs:4][version:4][engine id:4][pad:4]
// Voice:  [vtable][type:4][voice id:4][engine id:4][creation flags:4][channels:4][rate:4][version:4]
#define E_REFS(o) ((o) + (uint64_t)PSZ)
#define E_VER(o) ((o) + (uint64_t)PSZ + 4)
#define E_ID(o) ((o) + (uint64_t)PSZ + 8)
#define V_F(o, i) ((o) + (uint64_t)PSZ + 4 * (uint64_t)(i))
#define V_TYPE 0
#define V_ID 1
#define V_ENG 2
#define V_FLAGS 3
#define V_CH 4
#define V_RATE 5
#define V_VER 6

static int eng_id(uint64_t o) { return (int)(int32_t)mem_rd(E_ID(o), 4); }
static int vid(uint64_t o) { return o ? (int)(int32_t)mem_rd(V_F(o, V_ID), 4) : -1; }

// ---- effects (accepted and ignored) -------------------------------------------------------------------------
static int is_xa_iid(uint64_t riid, int *which) { // 0 = IUnknown / unknown to us, 1 = IXAudio2 of some version
    if (!riid) return 0;
    uint32_t d1 = (uint32_t)mem_rd(riid, 4);
    if (d1 == 0x8bcf1f58u || d1 == 0x60d8dac8u || d1 == 0x2b02e3cfu) { *which = 1; return 1; }
    if (d1 == 0) { *which = 0; return 1; }
    return 0;
}
HLE(fx_QueryInterface) {
    int w;
    if (!is_xa_iid(ARG(1), &w) || w) { if (ARG(2)) wr_ptr(ARG(2), 0); RET(E_NOINTERFACE_); return; }
    mem_wr(E_REFS(ARG(0)), 4, mem_rd(E_REFS(ARG(0)), 4) + 1);
    wr_ptr(ARG(2), ARG(0));
    RET(S_OK_);
}
HLE(fx_AddRef) { uint32_t r = (uint32_t)mem_rd(E_REFS(ARG(0)), 4) + 1; mem_wr(E_REFS(ARG(0)), 4, r); RET(r); }
HLE(fx_Release) {
    uint32_t r = (uint32_t)mem_rd(E_REFS(ARG(0)), 4) - 1;
    mem_wr(E_REFS(ARG(0)), 4, r);
    if (r == 0) g_free(G2H(ARG(0)));
    RET(r);
}
static uint64_t effect_new(void) {
    static const XM m[] = { { "QueryInterface", fx_QueryInterface, 3 }, { "AddRef", fx_AddRef, 1 }, { "Release", fx_Release, 1 } };
    uint8_t *o = g_calloc(1, (size_t)PSZ + 16);
    wr_ptr(H2G(o), make_vt(9, "IXAPO", m, 3));
    mem_wr(E_REFS(H2G(o)), 4, 1);
    return H2G(o);
}
HLE(xa_CreateAudioEffect) { // CreateAudioReverb / CreateAudioVolumeMeter (IUnknown **)
    if (!ARG(0)) { RET(E_INVALIDARG_); return; }
    wr_ptr(ARG(0), effect_new());
    RET(S_OK_);
}
HLE(xa_CreateFX) { // (REFCLSID, IUnknown **, void *init, UINT32 size)
    if (!ARG(1)) { RET(E_INVALIDARG_); return; }
    wr_ptr(ARG(1), effect_new());
    RET(S_OK_);
}

// ---- voices -------------------------------------------------------------------------------------------------
static int cur_ver(uint64_t self) { return (int)mem_rd(V_F(self, V_VER), 4); }

HLE(xv_GetVoiceDetails) {
    uint64_t s = ARG(0), d = ARG(1);
    if (!d) return;
    mem_wr(d, 4, mem_rd(V_F(s, V_FLAGS), 4));
    if (cur_ver(s) >= 8) { mem_wr(d + 4, 4, 0); d += 4; } // ActiveFlags
    mem_wr(d + 4, 4, mem_rd(V_F(s, V_CH), 4));
    mem_wr(d + 8, 4, mem_rd(V_F(s, V_RATE), 4));
}
// XAUDIO2_VOICE_SENDS { UINT32 SendCount; XAUDIO2_SEND_DESCRIPTOR *pSends } and descriptors { UINT32 Flags; voice }, packed
static int parse_sends(uint64_t list, int *dst) { // -1 = default output, else the number of sends (-2 invalid)
    if (!list) return -1;
    uint32_t n = (uint32_t)mem_rd(list, 4);
    uint64_t p = rd_ptr(list + 4);
    if (n > 16 || (n && !p)) return -2;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t v = rd_ptr(p + (uint64_t)i * (4 + (uint64_t)PSZ) + 4);
        if (!v) return -2;
        dst[i] = vid(v);
    }
    return (int)n;
}
HLE(xv_SetOutputVoices) {
    int dst[16];
    int n = parse_sends(ARG(1), dst);
    RET(n == -2 || saudio_voice_sends(vid(ARG(0)), n, dst) != 0 ? XA_INVALID_CALL : S_OK_);
}
HLE(xv_Ok) { RET(S_OK_); }
HLE(xv_GetEffectState) { if (ARG(2)) mem_wr(ARG(2), 4, 1); }
HLE(xv_GetEffectParameters) { if (ARG(2) && ARG(3)) memset(G2H(ARG(2)), 0, (size_t)ARG(3) & 0xFFFFFF); RET(S_OK_); }
HLE(xv_GetFilterParameters) { // XAUDIO2_FILTER_PARAMETERS { Type, Frequency, OneOverQ }: low pass, no filtering
    uint64_t p = ARG(1);
    if (p) { mem_wr(p, 4, 0); wrf(p + 4, 1.0f); wrf(p + 8, 1.0f); }
}
HLE(xv_GetOutputFilterParameters) { uint64_t p = ARG(2); if (p) { mem_wr(p, 4, 0); wrf(p + 4, 1.0f); wrf(p + 8, 1.0f); } RET(S_OK_); }
HLE(xv_SetVolume) { saudio_voice_set_volume(vid(ARG(0)), farg(st, 1)); RET(S_OK_); }
HLE(xv_GetVolume) { if (ARG(1)) wrf(ARG(1), saudio_voice_get_volume(vid(ARG(0)))); }
HLE(xv_SetChannelVolumes) {
    uint32_t n = (uint32_t)ARG(1);
    float v[8];
    if (n < 1 || n > 8 || !ARG(2)) { RET(XA_INVALID_CALL); return; }
    for (uint32_t i = 0; i < n; i++) v[i] = rdf(ARG(2) + 4 * (uint64_t)i);
    RET(saudio_voice_set_chvol(vid(ARG(0)), (int)n, v) ? XA_INVALID_CALL : S_OK_);
}
HLE(xv_GetChannelVolumes) {
    uint32_t n = (uint32_t)ARG(1);
    float v[8] = { 0 };
    if (n < 1 || n > 8 || !ARG(2) || saudio_voice_get_chvol(vid(ARG(0)), (int)n, v)) return;
    for (uint32_t i = 0; i < n; i++) wrf(ARG(2) + 4 * (uint64_t)i, v[i]);
}
HLE(xv_SetOutputMatrix) { // (dst voice or NULL, src channels, dst channels, matrix, opset)
    uint32_t sc = (uint32_t)ARG(2), dc = (uint32_t)ARG(3);
    float m[64];
    if (sc < 1 || sc > 8 || dc < 1 || dc > 8 || !ARG(4)) { RET(XA_INVALID_CALL); return; }
    for (uint32_t i = 0; i < sc * dc; i++) m[i] = rdf(ARG(4) + 4 * (uint64_t)i);
    RET(saudio_voice_set_matrix(vid(ARG(0)), ARG(1) ? vid(ARG(1)) : -1, (int)sc, (int)dc, m) ? XA_INVALID_CALL : S_OK_);
}
HLE(xv_GetOutputMatrix) {
    uint32_t sc = (uint32_t)ARG(2), dc = (uint32_t)ARG(3);
    float m[64];
    if (sc < 1 || sc > 8 || dc < 1 || dc > 8 || !ARG(4)) return;
    if (saudio_voice_get_matrix(vid(ARG(0)), ARG(1) ? vid(ARG(1)) : -1, (int)sc, (int)dc, m)) return;
    for (uint32_t i = 0; i < sc * dc; i++) wrf(ARG(4) + 4 * (uint64_t)i, m[i]);
}
HLE(xv_DestroyVoice) {
    uint64_t s = ARG(0);
    saudio_voice_free(vid(s));
    mem_wr(V_F(s, V_ID), 4, 0xFFFFFFFFu);
}
HLE(xv_Start) { RET(saudio_voice_start(vid(ARG(0))) ? XA_INVALID_CALL : S_OK_); }
HLE(xv_Stop) { RET(saudio_voice_stop(vid(ARG(0))) ? XA_INVALID_CALL : S_OK_); } // no effects: PLAY_TAILS makes no difference
HLE(xv_SubmitSourceBuffer) { // XAUDIO2_BUFFER (packed): Flags, AudioBytes, pAudioData, PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount, pContext
    uint64_t b = ARG(1);
    if (!b || ARG(2)) { RET(XA_INVALID_CALL); return; } // no xWMA
    SAudioBuffer sb;
    uint64_t data = rd_ptr(b + 8), o = b + 8 + (uint64_t)PSZ;
    sb.bytes = (uint32_t)mem_rd(b + 4, 4);
    sb.data = data ? (const uint8_t *)G2H(data) : NULL;
    sb.play_begin = (uint32_t)mem_rd(o, 4); sb.play_length = (uint32_t)mem_rd(o + 4, 4);
    sb.loop_begin = (uint32_t)mem_rd(o + 8, 4); sb.loop_length = (uint32_t)mem_rd(o + 12, 4); sb.loop_count = (uint32_t)mem_rd(o + 16, 4);
    sb.context = rd_ptr(o + 20);
    sb.eos = (mem_rd(b, 4) & XA_END_OF_STREAM) != 0;
    int r = saudio_voice_submit(vid(ARG(0)), &sb);
    RET(r == 0 ? S_OK_ : r == -2 ? E_OUTOFMEMORY_ : XA_INVALID_CALL);
}
HLE(xv_FlushSourceBuffers) { saudio_voice_flush(vid(ARG(0))); RET(S_OK_); }
HLE(xv_ExitLoop) { saudio_voice_exit_loop(vid(ARG(0))); RET(S_OK_); }
HLE(xv_GetState) { // (XAUDIO2_VOICE_STATE *, flags); the state is { pCurrentBufferContext, BuffersQueued, SamplesPlayed }, packed
    uint64_t p = ARG(1), self = ARG(0);
    uint64_t ctx, samples;
    uint32_t queued;
    if (!p) return;
    saudio_voice_state(vid(self), &ctx, &queued, &samples);
    if (cur_ver(self) >= 8 && (ARG(2) & XA_NOSAMPLESPLAYED)) samples = 0;
    wr_ptr(p, ctx);
    mem_wr(p + (uint64_t)PSZ, 4, queued);
    mem_wr(p + (uint64_t)PSZ + 4, 8, samples);
}
HLE(xv_SetFrequencyRatio) { saudio_voice_set_ratio(vid(ARG(0)), farg(st, 1)); RET(S_OK_); }
HLE(xv_GetFrequencyRatio) { if (ARG(1)) wrf(ARG(1), saudio_voice_get_ratio(vid(ARG(0)))); }
HLE(xv_SetSourceSampleRate) {
    uint64_t ctx, s;
    uint32_t q;
    saudio_voice_state(vid(ARG(0)), &ctx, &q, &s);
    if (q || ARG(1) < 1000 || ARG(1) > 200000) { RET(XA_INVALID_CALL); return; }
    saudio_voice_set_source_rate(vid(ARG(0)), (int)ARG(1));
    mem_wr(V_F(ARG(0), V_RATE), 4, ARG(1));
    RET(S_OK_);
}
HLE(xv_GetChannelMask) {
    static const uint32_t mask[9] = { 0, 4, 3, 7, 0x33, 0x37, 0x3F, 0x13F, 0x63F };
    uint32_t c = (uint32_t)mem_rd(V_F(ARG(0), V_CH), 4);
    if (ARG(1)) mem_wr(ARG(1), 4, c <= 8 ? mask[c] : 0);
    RET(S_OK_);
}

#define VOICE_COMMON(v, SRC_GETSTATE_ARGS) \
    { "GetVoiceDetails", xv_GetVoiceDetails, 2 }, { "SetOutputVoices", xv_SetOutputVoices, 2 }, { "SetEffectChain", xv_Ok, 2 }, \
    { "EnableEffect", xv_Ok, 3 }, { "DisableEffect", xv_Ok, 3 }, { "GetEffectState", xv_GetEffectState, 3 }, \
    { "SetEffectParameters", xv_Ok, 5 }, { "GetEffectParameters", xv_GetEffectParameters, 4 }, { "SetFilterParameters", xv_Ok, 3 }, \
    { "GetFilterParameters", xv_GetFilterParameters, 2 }, { "SetOutputFilterParameters", xv_Ok, 4 }, \
    { "GetOutputFilterParameters", xv_GetOutputFilterParameters, 3 }, { "SetVolume", xv_SetVolume, 3 }, { "GetVolume", xv_GetVolume, 2 }, \
    { "SetChannelVolumes", xv_SetChannelVolumes, 4 }, { "GetChannelVolumes", xv_GetChannelVolumes, 3 }, \
    { "SetOutputMatrix", xv_SetOutputMatrix, 6 }, { "GetOutputMatrix", xv_GetOutputMatrix, 5 }, { "DestroyVoice", xv_DestroyVoice, 1 }

static uint64_t voice_vt(int ver, int kind) { // kind: SA_SOURCE / SA_SUBMIX / SA_MASTER
    int slot = 3 + (ver >= 8) * 3 + kind;
    if (g_xvt[slot]) return g_xvt[slot];
    static const char *names[2][3] = { { "IXAudio27SourceVoice", "IXAudio27SubmixVoice", "IXAudio27MasteringVoice" },
                                       { "IXAudio2SourceVoice", "IXAudio2SubmixVoice", "IXAudio2MasteringVoice" } };
    const char *nm = names[ver >= 8][kind];
    if (kind == SA_SOURCE) {
        const XM m[] = { VOICE_COMMON(v, 0),
            { "Start", xv_Start, 3 }, { "Stop", xv_Stop, 3 }, { "SubmitSourceBuffer", xv_SubmitSourceBuffer, 3 },
            { "FlushSourceBuffers", xv_FlushSourceBuffers, 1 }, { "Discontinuity", xv_Ok, 1 }, { "ExitLoop", xv_ExitLoop, 2 },
            { "GetState", xv_GetState, ver >= 8 ? 3 : 2 }, { "SetFrequencyRatio", xv_SetFrequencyRatio, 3 },
            { "GetFrequencyRatio", xv_GetFrequencyRatio, 2 }, { "SetSourceSampleRate", xv_SetSourceSampleRate, 2 } };
        return make_vt(slot, nm, m, (int)(sizeof m / sizeof m[0]));
    }
    if (kind == SA_SUBMIX) {
        const XM m[] = { VOICE_COMMON(v, 0) };
        return make_vt(slot, nm, m, (int)(sizeof m / sizeof m[0]));
    }
    const XM m[] = { VOICE_COMMON(v, 0), { "GetChannelMask", xv_GetChannelMask, 2 } };
    return make_vt(slot, nm, m, (int)(sizeof m / sizeof m[0]));
}
static uint64_t voice_obj(int ver, int kind, int id, int eng, uint32_t flags, uint32_t ch, uint32_t rate) {
    uint8_t *o = g_calloc(1, (size_t)PSZ + 32);
    uint64_t a = H2G(o);
    wr_ptr(a, voice_vt(ver, kind));
    mem_wr(V_F(a, V_TYPE), 4, (uint64_t)kind); mem_wr(V_F(a, V_ID), 4, (uint64_t)id); mem_wr(V_F(a, V_ENG), 4, (uint64_t)eng);
    mem_wr(V_F(a, V_FLAGS), 4, flags); mem_wr(V_F(a, V_CH), 4, ch); mem_wr(V_F(a, V_RATE), 4, rate); mem_wr(V_F(a, V_VER), 4, (uint64_t)ver);
    return a;
}

// ---- the engine -----------------------------------------------------------------------------------------------
static int eng_ver(uint64_t self) { return (int)mem_rd(E_VER(self), 4); }

HLE(xe_QueryInterface) {
    int w;
    if (!is_xa_iid(ARG(1), &w)) { if (ARG(2)) wr_ptr(ARG(2), 0); RET(E_NOINTERFACE_); return; }
    mem_wr(E_REFS(ARG(0)), 4, mem_rd(E_REFS(ARG(0)), 4) + 1);
    wr_ptr(ARG(2), ARG(0));
    RET(S_OK_);
}
HLE(xe_AddRef) { uint32_t r = (uint32_t)mem_rd(E_REFS(ARG(0)), 4) + 1; mem_wr(E_REFS(ARG(0)), 4, r); RET(r); }
HLE(xe_Release) {
    uint32_t r = (uint32_t)mem_rd(E_REFS(ARG(0)), 4) - 1;
    mem_wr(E_REFS(ARG(0)), 4, r);
    if (r == 0) { saudio_engine_free(eng_id(ARG(0))); mem_wr(E_ID(ARG(0)), 4, 0xFFFFFFFFu); }
    RET(r);
}
HLE(xe_GetDeviceCount) { if (ARG(1)) mem_wr(ARG(1), 4, 1); RET(S_OK_); }
HLE(xe_GetDeviceDetails) { // XAUDIO2_DEVICE_DETAILS: DeviceID[256], DisplayName[256], Role, WAVEFORMATEXTENSIBLE
    uint64_t d = ARG(2);
    if (ARG(1) != 0 || !d) { RET(XA_INVALID_CALL); return; }
    memset(G2H(d), 0, 1068);
    u2w("0", -1, (uint16_t *)G2H(d), 256);
    u2w("Soul Hoparlör", -1, (uint16_t *)G2H(d + 512), 256);
    mem_wr(d + 1024, 4, 0xF); // GlobalDefaultDevice
    uint64_t f = d + 1028;
    mem_wr(f, 2, 0xFFFE); mem_wr(f + 2, 2, 2); mem_wr(f + 4, 4, 44100); mem_wr(f + 8, 4, 44100 * 4); mem_wr(f + 12, 2, 4); mem_wr(f + 14, 2, 16);
    mem_wr(f + 16, 2, 22); mem_wr(f + 18, 2, 16); mem_wr(f + 20, 4, 3);
    static const uint8_t pcm[16] = { 1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
    memcpy(G2H(f + 24), pcm, 16);
    RET(S_OK_);
}
HLE(xe_Initialize) { RET(S_OK_); }
HLE(xe_RegisterForCallbacks) {
    if (!ARG(1)) { RET(E_INVALIDARG_); return; }
    saudio_ensure_callbacks(st);
    RET(saudio_engine_callback(eng_id(ARG(0)), ARG(1), 1) ? XA_INVALID_CALL : S_OK_);
}
HLE(xe_UnregisterForCallbacks) { saudio_engine_callback(eng_id(ARG(0)), ARG(1), 0); }

static uint32_t finish_voice(X86State *st, uint64_t self, int id, uint64_t pp, uint64_t sendlist, uint64_t obj) {
    int dst[16];
    int n = parse_sends(sendlist, dst);
    if (n == -2 || (n >= 0 && saudio_voice_sends(id, n, dst) != 0)) { saudio_voice_free(id); g_free(G2H(obj)); return XA_INVALID_CALL; }
    (void)self; (void)st;
    wr_ptr(pp, obj);
    return S_OK_;
}
HLE(xe_CreateSourceVoice) { // (this, ppVoice, WAVEFORMATEX, Flags, float MaxFrequencyRatio, callback, sends, effects)
    uint64_t self = ARG(0), pp = ARG(1), fmt = ARG(2);
    uint32_t flags = (uint32_t)ARG(3);
    float maxr = farg(st, 4);
    SAudioFormat f;
    if (!pp || !fmt) { RET(E_INVALIDARG_); return; }
    if (saudio_format_parse(G2H(fmt), &f) != 0 || !(maxr >= 1.0f / 1024 && maxr <= 1024.0f)) { RET(XA_INVALID_CALL); return; }
    if (flags & XA_VOICE_NOPITCH) maxr = 1.0f;
    int id = saudio_voice_new(eng_id(self), SA_SOURCE, &f, 0, 0, maxr, ARG(5), 0);
    if (id < 0) { RET(XA_INVALID_CALL); return; }
    if (ARG(5)) saudio_ensure_callbacks(st);
    uint64_t o = voice_obj(eng_ver(self), SA_SOURCE, id, eng_id(self), flags, (uint32_t)f.channels, (uint32_t)f.rate);
    RET(finish_voice(st, self, id, pp, ARG(6), o));
}
HLE(xe_CreateSubmixVoice) { // (this, ppVoice, InputChannels, InputSampleRate, Flags, ProcessingStage, sends, effects)
    uint64_t self = ARG(0), pp = ARG(1);
    uint32_t ch = (uint32_t)ARG(2), rate = (uint32_t)ARG(3);
    if (!pp) { RET(E_INVALIDARG_); return; }
    if (ch < 1 || ch > 8 || rate < 1000 || rate > 200000) { RET(XA_INVALID_CALL); return; }
    int id = saudio_voice_new(eng_id(self), SA_SUBMIX, NULL, (int)ch, (int)rate, 1, 0, (int)ARG(5));
    if (id < 0) { RET(XA_INVALID_CALL); return; }
    uint64_t o = voice_obj(eng_ver(self), SA_SUBMIX, id, eng_id(self), (uint32_t)ARG(4), ch, rate);
    RET(finish_voice(st, self, id, pp, ARG(6), o));
}
HLE(xe_CreateMasteringVoice) { // (this, ppVoice, InputChannels, InputSampleRate, Flags, device, effects[, category])
    uint64_t self = ARG(0), pp = ARG(1);
    uint32_t ch = (uint32_t)ARG(2), rate = (uint32_t)ARG(3);
    if (!pp) { RET(E_INVALIDARG_); return; }
    if (ch == 0) ch = 2;
    if (rate == 0) rate = 44100;
    if (ch > 8 || rate < 1000 || rate > 200000) { RET(XA_INVALID_CALL); return; }
    int id = saudio_voice_new(eng_id(self), SA_MASTER, NULL, (int)ch, (int)rate, 1, 0, 0);
    if (id < 0) { RET(XA_INVALID_CALL); return; }
    wr_ptr(pp, voice_obj(eng_ver(self), SA_MASTER, id, eng_id(self), (uint32_t)ARG(4), ch, rate));
    RET(S_OK_);
}
HLE(xe_StartEngine) { saudio_engine_run(eng_id(ARG(0)), 1); RET(S_OK_); }
HLE(xe_StopEngine) { saudio_engine_run(eng_id(ARG(0)), 0); }
HLE(xe_CommitChanges) { RET(S_OK_); }
HLE(xe_GetPerformanceData) { // XAUDIO2_PERFORMANCE_DATA: 2 x UINT64, then 12 x UINT32
    uint64_t p = ARG(1);
    if (!p) return;
    uint32_t act, tot, sub;
    saudio_engine_stats(eng_id(ARG(0)), &act, &tot, &sub);
    memset(G2H(p), 0, 64);
    mem_wr(p + 16, 4, 100);                   // MinimumCyclesPerQuantum
    mem_wr(p + 20, 4, 200);                   // MaximumCyclesPerQuantum
    mem_wr(p + 24, 4, 65536);                 // MemoryUsageInBytes
    mem_wr(p + 28, 4, 1024);                  // CurrentLatencyInSamples
    mem_wr(p + 36, 4, act);                   // ActiveSourceVoiceCount
    mem_wr(p + 40, 4, tot);                   // TotalSourceVoiceCount
    mem_wr(p + 44, 4, sub);                   // ActiveSubmixVoiceCount
    mem_wr(p + 48, 4, act);                   // ActiveResamplerCount
    mem_wr(p + 52, 4, act);                   // ActiveMatrixMixCount
}
HLE(xe_SetDebugConfiguration) { }

static uint64_t engine_obj(int ver) {
    static const XM m7[] = {
        { "QueryInterface", xe_QueryInterface, 3 }, { "AddRef", xe_AddRef, 1 }, { "Release", xe_Release, 1 },
        { "GetDeviceCount", xe_GetDeviceCount, 2 }, { "GetDeviceDetails", xe_GetDeviceDetails, 3 }, { "Initialize", xe_Initialize, 3 },
        { "RegisterForCallbacks", xe_RegisterForCallbacks, 2 }, { "UnregisterForCallbacks", xe_UnregisterForCallbacks, 2 },
        { "CreateSourceVoice", xe_CreateSourceVoice, 8 }, { "CreateSubmixVoice", xe_CreateSubmixVoice, 8 },
        { "CreateMasteringVoice", xe_CreateMasteringVoice, 7 }, { "StartEngine", xe_StartEngine, 1 }, { "StopEngine", xe_StopEngine, 1 },
        { "CommitChanges", xe_CommitChanges, 2 }, { "GetPerformanceData", xe_GetPerformanceData, 2 },
        { "SetDebugConfiguration", xe_SetDebugConfiguration, 3 } };
    const XM m8[] = {
        { "QueryInterface", xe_QueryInterface, 3 }, { "AddRef", xe_AddRef, 1 }, { "Release", xe_Release, 1 },
        { "RegisterForCallbacks", xe_RegisterForCallbacks, 2 }, { "UnregisterForCallbacks", xe_UnregisterForCallbacks, 2 },
        { "CreateSourceVoice", xe_CreateSourceVoice, 8 }, { "CreateSubmixVoice", xe_CreateSubmixVoice, 8 },
        { "CreateMasteringVoice", xe_CreateMasteringVoice, ver >= 9 ? 8 : 7 }, { "StartEngine", xe_StartEngine, 1 },
        { "StopEngine", xe_StopEngine, 1 }, { "CommitChanges", xe_CommitChanges, 2 }, { "GetPerformanceData", xe_GetPerformanceData, 2 },
        { "SetDebugConfiguration", xe_SetDebugConfiguration, 3 } };
    int e = saudio_engine_new();
    if (e < 0) return 0;
    uint64_t vt = ver == 7 ? make_vt(0, "IXAudio27", m7, 16) : make_vt(ver == 8 ? 1 : 2, ver == 8 ? "IXAudio28" : "IXAudio29", m8, 13);
    uint8_t *o = g_calloc(1, (size_t)PSZ + 16);
    uint64_t a = H2G(o);
    wr_ptr(a, vt);
    mem_wr(E_REFS(a), 4, 1); mem_wr(E_VER(a), 4, (uint64_t)ver); mem_wr(E_ID(a), 4, (uint64_t)e);
    return a;
}
// 8 for xaudio2_8, 9 for xaudio2_9 / xaudio2_9redist
static int dll_version(X86State *st) {
    const char *d = g_proc->thunks[st->thread->cur_thunk].dll;
    return d && strstr(d, "2_8") ? 8 : 9;
}
HLE(xa_XAudio2Create) { // (IXAudio2 **, Flags, Processor)
    if (!ARG(0)) { RET(E_INVALIDARG_); return; }
    uint64_t e = engine_obj(dll_version(st));
    if (!e) { RET(E_OUTOFMEMORY_); return; }
    wr_ptr(ARG(0), e);
    RET(S_OK_);
}
HLE(xa_XAudio2CreateWithVersionInfo) { // (IXAudio2 **, Flags, Processor, NTDDI_VERSION)
    if (!ARG(0)) { RET(E_INVALIDARG_); return; }
    uint64_t e = engine_obj(9);
    if (!e) { RET(E_OUTOFMEMORY_); return; }
    wr_ptr(ARG(0), e);
    RET(S_OK_);
}

// CLSIDs by first dword: XAudio 2.0 .. 2.7 and the debug engine, the volume meters and reverbs of 2.0 .. 2.7
uint32_t xaudio2_com_create(X86State *st, uint64_t clsid, uint64_t riid, uint64_t ppv) {
    (void)st;
    if (!clsid) return 0x80040154u;
    static const uint32_t engines[] = { 0xfac23f48u, 0xe21a7345u, 0xb802058au, 0x4c5e637au, 0x03219e78u, 0x4c9b6ddeu, 0x3eda9b49u, 0x5a508685u, 0xdb05ea35u };
    static const uint32_t fx[] = { 0xc0c56f46u, 0xc1e3f122u, 0xf5ca7b34u, 0xe180344bu, 0xc7338b95u, 0x2139e6dau, 0xe48c5a3fu, 0xcac1105fu,
                                   0x6f6ea3a9u, 0xf4769300u, 0x629cf0deu, 0x9cab402cu, 0x8bb7778bu, 0xd06df0d0u, 0xcecec95au, 0x6a93130eu };
    uint32_t d1 = (uint32_t)mem_rd(clsid, 4);
    int is_engine = 0, is_fx = 0;
    for (size_t i = 0; i < sizeof engines / sizeof *engines; i++) if (engines[i] == d1) is_engine = 1;
    for (size_t i = 0; i < sizeof fx / sizeof *fx; i++) if (fx[i] == d1) is_fx = 1;
    if (!is_engine && !is_fx) return 0x80040154u;
    if (!ppv) return E_INVALIDARG_;
    int w;
    if (!is_xa_iid(riid, &w) || (is_fx && w)) { wr_ptr(ppv, 0); return E_NOINTERFACE_; }
    uint64_t o = is_fx ? effect_new() : engine_obj(7);
    if (!o) return E_OUTOFMEMORY_;
    wr_ptr(ppv, o);
    return S_OK_;
}

// ---- X3DAudio ----------------------------------------------------------------------------------------------
// All structures are packed to one byte. Layouts (P = pointer size):
//   LISTENER: OrientFront(12) OrientTop(12) Position(12) Velocity(12) pCone(P)
//   EMITTER:  pCone(P) OrientFront OrientTop Position Velocity (4x12), InnerRadius, InnerRadiusAngle, ChannelCount, ChannelRadius,
//             pChannelAzimuths(P), pVolumeCurve, pLFECurve, pLPFDirectCurve, pLPFReverbCurve, pReverbCurve (P each),
//             CurveDistanceScaler, DopplerScaler
//   DSP:      pMatrixCoefficients(P) pDelayTimes(P) SrcChannelCount(4) DstChannelCount(4), LPFDirect, LPFReverb, ReverbLevel,
//             DopplerFactor, EmitterToListenerAngle, EmitterToListenerDistance, EmitterVelocityComponent, ListenerVelocityComponent
//   CURVE:    pPoints(P) PointCount(4); POINT: Distance(4) DSPSetting(4)
#define X3D_MAGIC 0x44335853u
HLE(x3d_Initialize) { // (SpeakerChannelMask, float SpeedOfSound, X3DAUDIO_HANDLE) - the 20-byte handle holds our settings
    uint64_t h = ARG(2);
    if (!h) { RET(E_INVALIDARG_); return; }
    float c = farg(st, 1);
    if (!(c > 0)) { RET(E_INVALIDARG_); return; }
    memset(G2H(h), 0, 20);
    mem_wr(h, 4, ARG(0));
    wrf(h + 4, c);
    mem_wr(h + 8, 4, X3D_MAGIC);
    RET(S_OK_);
}
static void rdv(uint64_t a, float v[3]) { for (int i = 0; i < 3; i++) v[i] = rdf(a + 4 * (uint64_t)i); }
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float *a, const float *b, float *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static void norm3(float *v) { float l = sqrtf(dot3(v, v)); if (l > 1e-9f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static float curve_at(uint64_t curve, float x, float dflt) {
    if (!curve) return dflt;
    uint64_t pts = rd_ptr(curve);
    uint32_t n = (uint32_t)mem_rd(curve + (uint64_t)PSZ, 4);
    if (!pts || n == 0) return dflt;
    float x0 = rdf(pts), y0 = rdf(pts + 4);
    if (x <= x0) return y0;
    for (uint32_t i = 1; i < n; i++) {
        float x1 = rdf(pts + 8 * (uint64_t)i), y1 = rdf(pts + 8 * (uint64_t)i + 4);
        if (x <= x1) return x1 > x0 ? y0 + (y1 - y0) * (x - x0) / (x1 - x0) : y1;
        x0 = x1; y0 = y1;
    }
    return y0;
}
HLE(x3d_Calculate) { // (handle, listener, emitter, flags, dsp)
    uint64_t h = ARG(0), L = ARG(1), E = ARG(2), D = ARG(4);
    uint32_t flags = (uint32_t)ARG(3);
    if (!h || !L || !E || !D || mem_rd(h + 8, 4) != X3D_MAGIC) { RET(E_INVALIDARG_); return; }
    uint64_t P = (uint64_t)PSZ;
    float lf[3], lt[3], lp[3], lv[3], ef[3], ep[3], ev[3];
    rdv(L, lf); rdv(L + 12, lt); rdv(L + 24, lp); rdv(L + 36, lv);
    rdv(E + P, ef); rdv(E + P + 24, ep); rdv(E + P + 36, ev);
    uint64_t eb = E + P + 48;
    uint32_t srcn = (uint32_t)mem_rd(D + 2 * P, 4), dstn = (uint32_t)mem_rd(D + 2 * P + 4, 4);
    if (srcn < 1 || dstn < 1 || srcn > 64 || dstn > 64) { RET(E_INVALIDARG_); return; }
    float scaler = rdf(eb + 16 + 6 * P), doppler_scaler = rdf(eb + 20 + 6 * P);
    if (!(scaler > 0)) scaler = 1;
    norm3(lf);
    float right[3], d[3];
    cross3(lt, lf, right); // left-handed coordinates: right = up x front
    norm3(right);
    for (int i = 0; i < 3; i++) d[i] = ep[i] - lp[i];
    float dist = sqrtf(dot3(d, d));
    float x = dot3(d, right), z = dot3(d, lf);
    float az = (fabsf(x) < 1e-9f && fabsf(z) < 1e-9f) ? 0 : atan2f(x, z);
    uint64_t df = D + 2 * P + 8;
    wrf(df + 20, dist);
    if (flags & 1) { // matrix: distance attenuation and panning (matrix index = destination * source channels + source)
        float dn = dist / scaler;
        float gain = curve_at(rd_ptr(eb + 16 + P), dn, dn > 1 ? 1.0f / dn : 1.0f);
        uint64_t m = rd_ptr(D);
        uint64_t az_p = rd_ptr(eb + 16);
        uint32_t chn = (uint32_t)mem_rd(eb + 8, 4);
        if (m) for (uint32_t s = 0; s < srcn; s++) {
            float a = az;
            if (az_p && chn > 1 && s < chn) a += rdf(az_p + 4 * (uint64_t)s);
            float pan = sinf(a);
            for (uint32_t dd = 0; dd < dstn; dd++) {
                float c = 0;
                if (dstn == 1) c = gain;
                else if (dd < 2) { float th = (pan + 1) * 0.78539816f; c = gain * (dd == 0 ? cosf(th) : sinf(th)); }
                wrf(m + 4 * ((uint64_t)dd * srcn + s), c);
            }
        }
    }
    if ((flags & 2) && rd_ptr(D + P)) for (uint32_t i = 0; i < dstn; i++) wrf(rd_ptr(D + P) + 4 * (uint64_t)i, 0);
    if (flags & 4) wrf(df, 1.0f);       // LPF direct
    if (flags & 8) wrf(df + 4, 1.0f);   // LPF reverb
    if (flags & 0x10) wrf(df + 8, 0.0f); // reverb level
    float ve = 0, vl = 0, dop = 1;
    if (dist > 1e-6f) {
        float dir[3] = { d[0] / dist, d[1] / dist, d[2] / dist };
        vl = dot3(lv, dir); ve = dot3(ev, dir);
        if (flags & 0x20) {
            float c = rdf(h + 4);
            float a = vl * doppler_scaler, b = ve * doppler_scaler;
            if (a > 0.95f * c) a = 0.95f * c;
            if (a < -0.95f * c) a = -0.95f * c;
            if (b > 0.95f * c) b = 0.95f * c;
            if (b < -0.95f * c) b = -0.95f * c;
            dop = (c + a) / (c + b); // velocities along the line from the listener to the emitter
        }
    }
    if (flags & 0x20) { wrf(df + 12, dop); wrf(df + 24, ve); wrf(df + 28, vl); }
    if (flags & 0x40) { // angle between the emitter's front and the direction to the listener
        float a = 0;
        if (dist > 1e-6f) { float nf[3] = { ef[0], ef[1], ef[2] }; norm3(nf); float c = -dot3(nf, d) / dist; a = acosf(c > 1 ? 1 : c < -1 ? -1 : c); }
        wrf(df + 16, a);
    }
    RET(S_OK_);
}

const HleEntry hle_xaudio2_table[] = {
    { "XAudio2Create", xa_XAudio2Create, NULL }, { "XAudio2CreateWithVersionInfo", xa_XAudio2CreateWithVersionInfo, NULL },
    { "CreateAudioReverb", xa_CreateAudioEffect, NULL }, { "CreateAudioVolumeMeter", xa_CreateAudioEffect, NULL }, { "CreateFX", xa_CreateFX, NULL },
    { "X3DAudioInitialize", x3d_Initialize, NULL }, { "X3DAudioCalculate", x3d_Calculate, NULL },
    { NULL, NULL, NULL }
};
