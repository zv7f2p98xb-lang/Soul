// SoulCore - sound: a mixer for waveOut, PlaySound / Beep and DirectSound.
//
// Every playing sound is a voice. The host pulls the mix (44.1 kHz stereo float) from its audio
// render thread with soul_audio_render(); when no host pulls, a pump thread plays the sound out in
// real time into nothing, so programs waiting for their buffers to finish keep going. Notifications
// (WOM_DONE, events, DirectSound notify positions, waveOutProc callbacks) are delivered by a
// notifier thread, so the render thread never waits for the guest.
#include "hle.h"
#include "gui.h"
#include <math.h>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>

#define OUT_RATE 44100
#define MAX_VOICES 64
#define WOM_OPEN 0x3BB
#define WOM_CLOSE 0x3BC
#define WOM_DONE 0x3BD

typedef struct WBuf { uint8_t *data; uint32_t len; uint8_t *hdr; uint64_t hdr_g; uint32_t loops; } WBuf;
enum { V_WAVEOUT = 1, V_DSOUND, V_ONESHOT };
typedef struct Voice {
    int used, kind, playing, paused, looping, from_playsound, buf_owner;
    uint32_t pid, serial;
    int rate, channels, bits, is_float, block;
    double frac;               // fractional source frame position
    float gain_l, gain_r, volume;
    // waveOut: queue of program buffers (host pointers into the program's memory)
    WBuf q[128];
    int qhead, qn;
    uint32_t qpos;             // byte offset in the current buffer
    uint64_t played;           // bytes played (waveOutGetPosition)
    int cb_type;               // CALLBACK_*: 0 none, 1 window, 2 thread, 3 function, 5 event
    uint64_t cb_target, cb_instance, handle;
    HObj *cb_event;
    // DirectSound: circular buffer in the program's memory
    uint8_t *buf;
    uint32_t buf_len, play_pos;
    struct { uint32_t offset; HObj *event; } notify[32];
    int nnotify;
    uint64_t freq_override;
    // one-shot sounds owned by Soul (PlaySound, Beep)
    uint8_t *own;
    uint32_t own_len, own_pos;
} Voice;

static Voice g_v[MAX_VOICES];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_host_attached;
static volatile double g_last_pull;
static pthread_t g_pump, g_notifier;
static int g_threads_on;
static pthread_cond_t g_note_cv = PTHREAD_COND_INITIALIZER;

// ---- notifications ----------------------------------------------------------------------------------------

typedef struct Note { int kind; uint32_t pid; int cb_type; uint64_t target, instance, handle, p1; uint32_t msg; HObj *event; } Note;
static Note g_notes[1024];
static int g_nnotes;
// waveOutProc calls wait here for the process's audio callback thread
typedef struct Call { uint32_t pid; uint64_t fn, handle, instance, p1; uint32_t msg; } Call;
static Call g_calls[512];
static int g_ncalls;

static void note_push(Note n) { // g_mu held
    if (g_nnotes < 1024) g_notes[g_nnotes++] = n;
    pthread_cond_signal(&g_note_cv);
}
static void deliver(Note *n) { // GIL held
    SoulProcess *p = soul_process_by_pid(n->pid);
    if (!p || p->released) return; // the program ended meanwhile
    if (n->kind == 2) { if (n->event) n->event->signaled = 1; return; } // DirectSound notify position
    switch (n->cb_type) {
    case 1: win_post(n->target, n->msg, n->handle, n->p1); break;
    case 2: win_post_thread((uint32_t)n->target, n->msg, n->handle, n->p1); break;
    case 5: if (n->event) n->event->signaled = 1; break;
    case 3:
        pthread_mutex_lock(&g_mu);
        if (g_ncalls < 512) g_calls[g_ncalls++] = (Call){ n->pid, n->target, n->handle, n->instance, n->p1, n->msg };
        pthread_mutex_unlock(&g_mu);
        break;
    default: break;
    }
}
static void *notifier_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        while (!g_nnotes) pthread_cond_wait(&g_note_cv, &g_mu);
        Note batch[1024];
        int n = g_nnotes;
        memcpy(batch, g_notes, sizeof(Note) * (size_t)n);
        g_nnotes = 0;
        pthread_mutex_unlock(&g_mu);
        gil_lock(NULL);
        for (int i = 0; i < n; i++) deliver(&batch[i]);
        gil_notify();
        gil_unlock(NULL);
        pthread_mutex_lock(&g_mu);
    }
    return NULL;
}

// ---- mixing ---------------------------------------------------------------------------------------------------

static float sample_at(const uint8_t *p, int bits, int is_float) {
    if (is_float) { float f; memcpy(&f, p, 4); return f; }
    switch (bits) {
    case 8: return ((int)p[0] - 128) / 128.0f;
    case 16: { int16_t s; memcpy(&s, p, 2); return s / 32768.0f; }
    case 24: { int32_t s = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8; return s / 8388608.0f; }
    case 32: { int32_t s; memcpy(&s, p, 4); return s / 2147483648.0f; }
    default: return 0;
    }
}
static void frame_at(Voice *v, const uint8_t *p, float *l, float *r) {
    int bps = v->bits / 8;
    *l = sample_at(p, v->bits, v->is_float);
    *r = v->channels > 1 ? sample_at(p + bps, v->bits, v->is_float) : *l;
}
// Source bytes for the voice's current position: pointer and bytes left in the current block.
static const uint8_t *cursor(Voice *v, uint32_t *left) {
    switch (v->kind) {
    case V_WAVEOUT:
        while (v->qn) {
            WBuf *b = &v->q[v->qhead];
            if (v->qpos + (uint32_t)v->block <= b->len) { *left = b->len - v->qpos; return b->data + v->qpos; }
            return NULL; // the buffer is finished (handled by advance)
        }
        return NULL;
    case V_DSOUND:
        if (!v->buf || v->play_pos + (uint32_t)v->block > v->buf_len) return NULL;
        *left = v->buf_len - v->play_pos;
        return v->buf + v->play_pos;
    case V_ONESHOT:
        if (v->own_pos + (uint32_t)v->block > v->own_len) return NULL;
        *left = v->own_len - v->own_pos;
        return v->own + v->own_pos;
    }
    return NULL;
}
// Moves one source frame on; returns 0 when the voice ran out.
static int advance(Voice *v) {
    uint32_t blk = (uint32_t)v->block;
    switch (v->kind) {
    case V_WAVEOUT: {
        WBuf *b = &v->q[v->qhead];
        v->qpos += blk;
        v->played += blk;
        if (v->qpos + blk > b->len) {
            // buffer finished: mark it done and notify
            if (b->hdr) { uint32_t f; memcpy(&f, b->hdr, 4); f = (f | 1u) & ~0x10u; memcpy(b->hdr, &f, 4); } // WHDR_DONE, !WHDR_INQUEUE
            note_push((Note){ 1, v->pid, v->cb_type, v->cb_target, v->cb_instance, v->handle, b->hdr_g, WOM_DONE, v->cb_event });
            v->qhead = (v->qhead + 1) % 128;
            v->qn--;
            v->qpos = 0;
            if (!v->qn) return 0;
        }
        return 1;
    }
    case V_DSOUND: {
        uint32_t old = v->play_pos;
        v->play_pos += blk;
        if (v->play_pos + blk > v->buf_len) {
            if (v->looping) v->play_pos = 0;
            else { v->play_pos = 0; v->playing = 0; }
        }
        for (int i = 0; i < v->nnotify; i++) { // notify positions crossed
            uint32_t o = v->notify[i].offset;
            if (o == 0xFFFFFFFFu) { if (!v->playing) note_push((Note){ 2, v->pid, 0, 0, 0, 0, 0, 0, v->notify[i].event }); continue; }
            int crossed = v->play_pos > old ? (o >= old && o < v->play_pos) : (o >= old || o < v->play_pos);
            if (crossed) note_push((Note){ 2, v->pid, 0, 0, 0, 0, 0, 0, v->notify[i].event });
        }
        return v->playing;
    }
    case V_ONESHOT:
        v->own_pos += blk;
        if (v->own_pos + blk > v->own_len) {
            if (v->looping) v->own_pos = 0;
            else { v->playing = 0; free(v->own); v->own = NULL; v->used = 0; v->serial = 0; return 0; }
        }
        return 1;
    }
    return 0;
}
// Mixes frames into out (interleaved stereo). g_mu held.
static int mix(float *out, int frames) {
    memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    int any = 0;
    for (int k = 0; k < MAX_VOICES; k++) {
        Voice *v = &g_v[k];
        if (!v->used || !v->playing || v->paused || v->block <= 0) continue;
        double rate = (double)(v->freq_override ? v->freq_override : (uint64_t)v->rate);
        double step = rate / OUT_RATE;
        for (int i = 0; i < frames; i++) {
            uint32_t left;
            const uint8_t *p = cursor(v, &left);
            if (!p) break;
            float l, r;
            frame_at(v, p, &l, &r);
            if (left >= 2 * (uint32_t)v->block && v->frac > 0) { // linear interpolation with the next frame
                float l2, r2;
                frame_at(v, p + v->block, &l2, &r2);
                float t = (float)v->frac;
                l += (l2 - l) * t;
                r += (r2 - r) * t;
            }
            out[2 * i] += l * v->gain_l * v->volume;
            out[2 * i + 1] += r * v->gain_r * v->volume;
            any = 1;
            v->frac += step;
            int ended = 0;
            while (v->frac >= 1.0) { v->frac -= 1.0; if (!advance(v)) { ended = 1; break; } }
            if (ended || !v->used || !v->playing) break;
        }
    }
    for (int i = 0; i < 2 * frames; i++) out[i] = out[i] > 1 ? 1 : out[i] < -1 ? -1 : out[i];
    return any;
}

int soul_audio_render(float *interleaved, int frames) {
    pthread_mutex_lock(&g_mu);
    g_last_pull = soul_now();
    int any = mix(interleaved, frames);
    pthread_mutex_unlock(&g_mu);
    return any;
}
void soul_audio_set_host(int attached) { g_host_attached = attached; }

// Plays the mix into nothing while the host does not pull it (soulrun, app in the background).
static void *pump_main(void *arg) {
    (void)arg;
    static float buf[2 * 4410];
    double last = soul_now();
    for (;;) {
        usleep(10000);
        double now = soul_now();
        int frames = (int)((now - last) * OUT_RATE);
        if (frames <= 0) continue;
        last += (double)frames / OUT_RATE;
        if (frames > 4410) { frames = 4410; last = now; }
        if (g_host_attached && now - g_last_pull < 0.25) continue;
        pthread_mutex_lock(&g_mu);
        mix(buf, frames);
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}
static void ensure_threads(void) { // g_mu held
    if (g_threads_on) return;
    g_threads_on = 1;
    pthread_create(&g_pump, NULL, pump_main, NULL);
    pthread_detach(g_pump);
    pthread_create(&g_notifier, NULL, notifier_main, NULL);
    pthread_detach(g_notifier);
}
static uint32_t g_serial;
static Voice *voice_new(int kind) { // g_mu held
    for (int i = 0; i < MAX_VOICES; i++) if (!g_v[i].used) {
        Voice *v = &g_v[i];
        memset(v, 0, sizeof *v);
        v->used = 1; v->kind = kind; v->gain_l = v->gain_r = 1; v->volume = 1;
        v->serial = ++g_serial ? g_serial : ++g_serial;
        v->pid = g_proc ? g_proc->pid : 0;
        ensure_threads();
        return v;
    }
    return NULL;
}
static int set_format(Voice *v, const uint8_t *wfx) { // WAVEFORMATEX(TENSIBLE)
    uint16_t tag, ch, align, bits;
    uint32_t rate;
    memcpy(&tag, wfx, 2); memcpy(&ch, wfx + 2, 2); memcpy(&rate, wfx + 4, 4); memcpy(&align, wfx + 12, 2); memcpy(&bits, wfx + 14, 2);
    if (tag == 0xFFFE) { uint16_t sub; memcpy(&sub, wfx + 24, 2); tag = sub; } // SubFormat GUID's first word
    if ((tag != 1 && tag != 3) || !ch || ch > 8 || !rate || rate > 384000) return -1;
    if (tag == 3 && bits != 32) return -1;
    if (tag == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32) return -1;
    v->rate = (int)rate; v->channels = ch; v->bits = bits; v->is_float = tag == 3;
    v->block = align ? align : ch * bits / 8;
    return 0;
}
// Stops a process's voices when it ends.
void audio_process_exit(uint32_t pid) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_VOICES; i++) if (g_v[i].used && g_v[i].pid == pid) { free(g_v[i].own); g_v[i].own = NULL; g_v[i].used = 0; g_v[i].serial = 0; }
    for (int i = 0; i < g_ncalls;) { if (g_calls[i].pid == pid) g_calls[i] = g_calls[--g_ncalls]; else i++; }
    pthread_mutex_unlock(&g_mu);
}

// ---- waveOut -----------------------------------------------------------------------------------------------

#define WAVE_BASE 0x5A0000u
static Voice *wave_of(uint64_t h) { if (h < WAVE_BASE || h >= WAVE_BASE + MAX_VOICES) return NULL; Voice *v = &g_v[h - WAVE_BASE]; return v->used && v->kind == V_WAVEOUT && v->pid == g_proc->pid ? v : NULL; }
#define MMSYSERR_INVALHANDLE 5
#define MMSYSERR_BADDEVICEID 2
#define WAVERR_BADFORMAT 32
#define WAVERR_STILLPLAYING 33
#define L_WAVEHDR "P44P44PP"

static int g_cb_thread; // per process: the thread that runs waveOutProc callbacks was started
SOUL_PROCESS_VAR(g_cb_thread)
HLE(mm_SoulAudioThread) { // the program's "winmm" thread: runs waveOutProc callbacks
    for (;;) {
        if (g_proc->terminate) soul_exit_thread(st, 0);
        Call c;
        int have = 0;
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < g_ncalls; i++) if (g_calls[i].pid == g_proc->pid) { c = g_calls[i]; memmove(&g_calls[i], &g_calls[i + 1], sizeof(Call) * (size_t)(g_ncalls - i - 1)); g_ncalls--; have = 1; break; }
        pthread_mutex_unlock(&g_mu);
        if (!have) { gil_wait(200); continue; }
        uint64_t args[5] = { c.handle, c.msg, c.instance, c.p1, 0 };
        soul_guest_call(st, c.fn, 5, args);
    }
}
static void ensure_cb_thread(X86State *st) {
    if (g_cb_thread) return;
    g_cb_thread = 1;
    uint32_t id = hle_thunk_for(g_proc, mm_SoulAudioThread, "winmm", "SoulAudioThread");
    g_proc->thunks[id].argbytes = g_mode32 ? 4 : 0;
    soul_thread_create(st, g_proc->thunk_base + (uint64_t)id * SOUL_THUNK_STRIDE, 0, 1u << 20, 0);
}
static void notify_now(Voice *v, uint32_t msg, uint64_t p1) { // the program's own thread: open / close messages
    Note n = { 1, v->pid, v->cb_type, v->cb_target, v->cb_instance, v->handle, p1, msg, v->cb_event };
    pthread_mutex_lock(&g_mu);
    note_push(n);
    pthread_mutex_unlock(&g_mu);
}

HLE(mm_waveOutGetNumDevs) { RET(1); }
static void wave_caps(X86State *st, int wide) { // WAVEOUTCAPS: mid, pid, version, name[32], formats, channels, reserved, support
    uint64_t c = ARG(1);
    size_t name = wide ? 64 : 32, sz = 8 + name + 12;
    if (!c || (uint32_t)ARG(2) < sz) { RET(11); return; } // MMSYSERR_INVALPARAM
    memset(G2H(c), 0, sz);
    mem_wr(c, 2, 1); mem_wr(c + 2, 2, 100); mem_wr(c + 4, 4, 0x0100);
    const char *n = "Soul Hoparlör";
    if (wide) u2w(n, -1, (uint16_t *)G2H(c + 8), 32); else snprintf(G2H(c + 8), 32, "Soul Hoparlor");
    mem_wr(c + 8 + name, 4, 0xFFFFF);     // every WAVE_FORMAT_*
    mem_wr(c + 8 + name + 4, 2, 2);
    mem_wr(c + 8 + name + 8, 4, 0x2C);    // VOLUME | LRVOLUME | SAMPLEACCURATE
    RET(0);
}
HLE(mm_waveOutGetDevCapsA) { wave_caps(st, 0); }
HLE(mm_waveOutGetDevCapsW) { wave_caps(st, 1); }
HLE(mm_waveOutOpen) { // (&hwo, device, format, callback, instance, flags)
    uint32_t flags = (uint32_t)hle_arg(st, 5);
    int dev = (int)(int32_t)ARG(1);
    if (dev != 0 && dev != -1) { RET(MMSYSERR_BADDEVICEID); return; } // the one device or WAVE_MAPPER
    Voice tmp;
    memset(&tmp, 0, sizeof tmp);
    if (!ARG(2) || set_format(&tmp, PTR(2)) != 0) { RET(WAVERR_BADFORMAT); return; }
    if (flags & 1) { RET(0); return; } // WAVE_FORMAT_QUERY
    pthread_mutex_lock(&g_mu);
    Voice *v = voice_new(V_WAVEOUT);
    if (!v) { pthread_mutex_unlock(&g_mu); RET(4); return; } // MMSYSERR_ALLOCATED
    v->rate = tmp.rate; v->channels = tmp.channels; v->bits = tmp.bits; v->is_float = tmp.is_float; v->block = tmp.block;
    v->playing = 1;
    v->handle = WAVE_BASE + (uint64_t)(v - g_v);
    uint32_t cb = flags & 0x70000;
    v->cb_type = cb == 0x10000 ? 1 : cb == 0x20000 ? 2 : cb == 0x30000 ? 3 : cb == 0x50000 ? 5 : 0;
    v->cb_target = ARG(3);
    v->cb_instance = hle_arg(st, 4);
    if (v->cb_type == 5) v->cb_event = h_get(ARG(3));
    if (v->cb_type == 2) v->cb_target = (uint32_t)ARG(3);
    uint64_t h = v->handle;
    pthread_mutex_unlock(&g_mu);
    if (ARG(0)) wr_ptr(ARG(0), h);
    if (v->cb_type == 3) ensure_cb_thread(st);
    notify_now(v, WOM_OPEN, 0);
    RET(0);
}
HLE(mm_waveOutPrepareHeader) { uint64_t hd = ARG(1); if (!hd) { RET(11); return; } gs_set(hd, L_WAVEHDR, 4, (gs_get(hd, L_WAVEHDR, 4) | 2) & ~1ull); RET(0); } // WHDR_PREPARED
HLE(mm_waveOutUnprepareHeader) {
    uint64_t hd = ARG(1);
    if (!hd) { RET(11); return; }
    if (gs_get(hd, L_WAVEHDR, 4) & 0x10) { RET(WAVERR_STILLPLAYING); return; }
    gs_set(hd, L_WAVEHDR, 4, gs_get(hd, L_WAVEHDR, 4) & ~2ull);
    RET(0);
}
HLE(mm_waveOutWrite) {
    uint64_t hd = ARG(1);
    if (!hd) { RET(11); return; }
    if (!(gs_get(hd, L_WAVEHDR, 4) & 2)) { RET(34); return; } // WAVERR_UNPREPARED
    pthread_mutex_lock(&g_mu);
    Voice *v = wave_of(ARG(0));
    if (!v) { pthread_mutex_unlock(&g_mu); RET(MMSYSERR_INVALHANDLE); return; }
    if (v->qn >= 128) { pthread_mutex_unlock(&g_mu); RET(7); return; } // MMSYSERR_NOMEM
    uint32_t len = (uint32_t)gs_get(hd, L_WAVEHDR, 1);
    len -= len % (uint32_t)v->block;
    uint8_t *flags = (uint8_t *)G2H(hd + gs_off(L_WAVEHDR, 4)); // dwFlags, written by the mixer
    if (len == 0) { // nothing to play: done at once
        uint32_t f; memcpy(&f, flags, 4); f = (f | 1u) & ~0x10u; memcpy(flags, &f, 4);
        note_push((Note){ 1, v->pid, v->cb_type, v->cb_target, v->cb_instance, v->handle, hd, WOM_DONE, v->cb_event });
        pthread_mutex_unlock(&g_mu);
        RET(0);
        return;
    }
    uint32_t f; memcpy(&f, flags, 4); f = (f | 0x10u) & ~1u; memcpy(flags, &f, 4); // WHDR_INQUEUE, !WHDR_DONE
    WBuf *b = &v->q[(v->qhead + v->qn) % 128];
    b->data = G2H(gs_get(hd, L_WAVEHDR, 0));
    b->len = len;
    b->hdr = flags;
    b->hdr_g = hd;
    v->qn++;
    pthread_mutex_unlock(&g_mu);
    RET(0);
}
static void wave_flush(Voice *v) { // g_mu held: every queued buffer is done
    while (v->qn) {
        WBuf *b = &v->q[v->qhead];
        uint32_t f; memcpy(&f, b->hdr, 4); f = (f | 1u) & ~0x10u; memcpy(b->hdr, &f, 4);
        note_push((Note){ 1, v->pid, v->cb_type, v->cb_target, v->cb_instance, v->handle, b->hdr_g, WOM_DONE, v->cb_event });
        v->qhead = (v->qhead + 1) % 128;
        v->qn--;
    }
    v->qpos = 0;
}
HLE(mm_waveOutReset) {
    pthread_mutex_lock(&g_mu);
    Voice *v = wave_of(ARG(0));
    if (v) { wave_flush(v); v->played = 0; v->paused = 0; }
    pthread_mutex_unlock(&g_mu);
    RET(v ? 0 : MMSYSERR_INVALHANDLE);
}
HLE(mm_waveOutClose) {
    pthread_mutex_lock(&g_mu);
    Voice *v = wave_of(ARG(0));
    if (!v) { pthread_mutex_unlock(&g_mu); RET(MMSYSERR_INVALHANDLE); return; }
    if (v->qn) { pthread_mutex_unlock(&g_mu); RET(WAVERR_STILLPLAYING); return; }
    Note n = { 1, v->pid, v->cb_type, v->cb_target, v->cb_instance, v->handle, 0, WOM_CLOSE, v->cb_event };
    note_push(n);
    v->used = 0;
    v->serial = 0;
    pthread_mutex_unlock(&g_mu);
    RET(0);
}
HLE(mm_waveOutPause) { pthread_mutex_lock(&g_mu); Voice *v = wave_of(ARG(0)); if (v) v->paused = 1; pthread_mutex_unlock(&g_mu); RET(v ? 0 : MMSYSERR_INVALHANDLE); }
HLE(mm_waveOutRestart) { pthread_mutex_lock(&g_mu); Voice *v = wave_of(ARG(0)); if (v) v->paused = 0; pthread_mutex_unlock(&g_mu); RET(v ? 0 : MMSYSERR_INVALHANDLE); }
HLE(mm_waveOutBreakLoop) { RET(0); }
HLE(mm_waveOutGetPosition) { // (hwo, MMTIME*, size): { UINT type; union { ms, sample, cb ... } }
    pthread_mutex_lock(&g_mu);
    Voice *v = wave_of(ARG(0));
    uint64_t bytes = v ? v->played : 0;
    int block = v && v->block ? v->block : 1, rate = v ? v->rate : 1;
    pthread_mutex_unlock(&g_mu);
    if (!v) { RET(MMSYSERR_INVALHANDLE); return; }
    uint64_t t = ARG(1);
    uint32_t type = (uint32_t)mem_rd(t, 4);
    if (type == 2) mem_wr(t + 4, 4, (uint32_t)(bytes / (uint64_t)block));          // TIME_SAMPLES
    else if (type == 1) mem_wr(t + 4, 4, (uint32_t)(bytes * 1000 / (uint64_t)block / (uint64_t)rate)); // TIME_MS
    else { mem_wr(t, 4, 4); mem_wr(t + 4, 4, (uint32_t)bytes); }                     // TIME_BYTES
    RET(0);
}
HLE(mm_waveOutSetVolume) { // low word left, high word right (0..0xFFFF)
    pthread_mutex_lock(&g_mu);
    Voice *v = wave_of(ARG(0));
    if (v) { v->gain_l = (float)(ARG(1) & 0xFFFF) / 65535.0f; v->gain_r = (float)((ARG(1) >> 16) & 0xFFFF) / 65535.0f; }
    pthread_mutex_unlock(&g_mu);
    RET(0);
}
HLE(mm_waveOutGetVolume) {
    pthread_mutex_lock(&g_mu);
    Voice *v = wave_of(ARG(0));
    uint32_t vol = v ? (uint32_t)(v->gain_l * 65535) | (uint32_t)(v->gain_r * 65535) << 16 : 0xFFFFFFFFu;
    pthread_mutex_unlock(&g_mu);
    if (ARG(1)) mem_wr(ARG(1), 4, vol);
    RET(0);
}
HLE(mm_waveOutGetID) { if (ARG(1)) mem_wr(ARG(1), 4, 0); RET(0); }
HLE(mm_waveOutMessage) { RET(0); }
HLE(mm_waveInGetNumDevs) { RET(0); }
HLE(mm_waveInOpen) { RET(MMSYSERR_BADDEVICEID); }

// ---- PlaySound / Beep -------------------------------------------------------------------------------------------

// Parses a RIFF WAVE file in memory; returns a one-shot voice (not yet playing) or NULL.
static Voice *voice_from_wav(const uint8_t *d, size_t n) {
    if (n < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return NULL;
    const uint8_t *fmt = NULL, *data = NULL;
    uint32_t data_len = 0;
    for (size_t o = 12; o + 8 <= n;) {
        uint32_t sz;
        memcpy(&sz, d + o + 4, 4);
        if (!memcmp(d + o, "fmt ", 4)) fmt = d + o + 8;
        else if (!memcmp(d + o, "data", 4)) { data = d + o + 8; data_len = sz; if (o + 8 + sz > n) data_len = (uint32_t)(n - o - 8); }
        o += 8 + sz + (sz & 1);
    }
    if (!fmt || !data) return NULL;
    pthread_mutex_lock(&g_mu);
    Voice *v = voice_new(V_ONESHOT);
    if (v && set_format(v, fmt) != 0) { v->used = 0; v = NULL; }
    if (v) { v->own = malloc(data_len ? data_len : 1); memcpy(v->own, data, data_len); v->own_len = data_len; }
    pthread_mutex_unlock(&g_mu);
    return v;
}
static void stop_playsound(uint32_t pid) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &g_v[i];
        if (v->used && v->kind == V_ONESHOT && v->from_playsound && v->pid == pid) { free(v->own); v->own = NULL; v->used = 0; v->serial = 0; }
    }
    pthread_mutex_unlock(&g_mu);
}
static int playsound_busy(uint32_t pid) {
    int busy = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_VOICES; i++) if (g_v[i].used && g_v[i].kind == V_ONESHOT && g_v[i].from_playsound && g_v[i].pid == pid && g_v[i].playing) busy = 1;
    pthread_mutex_unlock(&g_mu);
    return busy;
}
// A tone (Beep, MessageBeep, system sounds): 16-bit mono sine with soft edges; starts playing.
static Voice *tone(int freq, int ms) {
    if (freq < 37) freq = 37;
    if (freq > 32767) freq = 32767;
    if (ms <= 0) return NULL;
    if (ms > 60000) ms = 60000;
    uint32_t frames = (uint32_t)((int64_t)OUT_RATE * ms / 1000);
    int16_t *s = malloc(frames * 2 + 2);
    for (uint32_t i = 0; i < frames; i++) {
        double env = i < 441 ? i / 441.0 : (frames - i < 441 ? (frames - i) / 441.0 : 1.0);
        s[i] = (int16_t)(sin(2 * 3.14159265358979323846 * freq * i / OUT_RATE) * 9000 * env);
    }
    pthread_mutex_lock(&g_mu);
    Voice *v = voice_new(V_ONESHOT);
    if (v) { v->rate = OUT_RATE; v->channels = 1; v->bits = 16; v->block = 2; v->own = (uint8_t *)s; v->own_len = frames * 2; v->playing = 1; }
    else free(s);
    pthread_mutex_unlock(&g_mu);
    return v;
}
static int wait_voice(Voice *v, uint32_t serial) { // until the one-shot ends (the GIL is released)
    SoulThread *t = soul_cur_thread();
    for (;;) {
        pthread_mutex_lock(&g_mu);
        int alive = v->used && v->serial == serial && v->playing;
        pthread_mutex_unlock(&g_mu);
        if (!alive || g_proc->terminate) return 0;
        gil_unlock(t);
        usleep(10000);
        gil_lock(t);
    }
}
static Voice *wav_file(const char *win) {
    char host[2048];
    if (path_w2h(win, host, sizeof host) != 0) return NULL;
    FILE *f = fopen(host, "rb");
    if (!f) return NULL;
    Voice *v = NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 && n < (256 << 20) ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) == (size_t)n) v = voice_from_wav(d, (size_t)n);
    free(d);
    fclose(f);
    return v;
}
static int system_tone(const char *alias) { // pitch of the beep standing in for a system sound (0 = not one)
    static const struct { const char *name; int freq; } k[] = {
        { "SystemHand", 440 }, { "SystemExclamation", 660 }, { "SystemAsterisk", 990 }, { "SystemQuestion", 770 },
        { "SystemDefault", 880 }, { ".Default", 880 }, { "SystemStart", 660 }, { "SystemExit", 523 }, { "SystemNotification", 990 },
        { "MailBeep", 880 }, { "Notification.Default", 990 },
    };
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) if (!strcasecmp(alias, k[i].name)) return k[i].freq;
    return 0;
}
#define SND_ASYNC 0x1
#define SND_NODEFAULT 0x2
#define SND_MEMORY 0x4
#define SND_LOOP 0x8
#define SND_NOSTOP 0x10
#define SND_PURGE 0x40
#define SND_ALIAS 0x10000
#define SND_FILENAME 0x20000
#define SND_RESOURCE 0x40004
#define SND_ALIAS_ID 0x110000
static int play_core(uint64_t name, uint64_t module, uint32_t flags, int wide) {
    uint32_t pid = g_proc->pid;
    if ((flags & SND_NOSTOP) && name && playsound_busy(pid)) return 0;
    stop_playsound(pid); // a new sound replaces the previous one
    if (!name || (flags & SND_PURGE)) return 1;
    Voice *v = NULL;
    if ((flags & SND_ALIAS_ID) == SND_ALIAS_ID) v = tone(880, 150);
    else if ((flags & SND_RESOURCE) == SND_RESOURCE) { // a "WAVE" resource of the module
        uint32_t len = 0;
        void *type = wide ? (void *)u2w_gdup("WAVE") : (void *)g_strdup("WAVE");
        const uint8_t *r = pe_resource(module, H2G(type), name, wide, &len);
        g_free(type);
        if (r) v = voice_from_wav(r, len);
    } else if (flags & SND_MEMORY) { // a WAV image in memory
        const uint8_t *d = G2H(name);
        uint32_t riff;
        memcpy(&riff, d + 4, 4);
        v = voice_from_wav(d, (size_t)riff + 8);
    } else {
        char *s = wide ? w2u_dup((const uint16_t *)G2H(name)) : strdup((const char *)G2H(name));
        int freq = (flags & SND_FILENAME) ? 0 : system_tone(s);
        if (freq) v = tone(freq, 150);
        else if (!(flags & SND_ALIAS)) v = wav_file(s);
        if (!v && !(flags & SND_NODEFAULT)) v = tone(880, 150);
        free(s);
    }
    if (!v) return 0;
    pthread_mutex_lock(&g_mu);
    v->from_playsound = 1;
    v->looping = (flags & SND_LOOP) != 0;
    v->playing = 1;
    uint32_t serial = v->serial;
    pthread_mutex_unlock(&g_mu);
    if (!(flags & SND_ASYNC) && !v->looping) wait_voice(v, serial);
    return 1;
}
HLE(mm_PlaySoundA) { RET(play_core(ARG(0), ARG(1), (uint32_t)ARG(2), 0)); }
HLE(mm_PlaySoundW) { RET(play_core(ARG(0), ARG(1), (uint32_t)ARG(2), 1)); }
HLE(mm_sndPlaySoundA) { RET(play_core(ARG(0), 0, (uint32_t)ARG(1) & ~0x40000u, 0)); } // flags: ASYNC NODEFAULT MEMORY LOOP NOSTOP
HLE(mm_sndPlaySoundW) { RET(play_core(ARG(0), 0, (uint32_t)ARG(1) & ~0x40000u, 1)); }
HLE(k32_Beep) { // (frequency, ms): plays and waits like Windows
    Voice *v = tone((int)ARG(0), (int)ARG(1));
    if (v) { pthread_mutex_lock(&g_mu); uint32_t serial = v->serial; pthread_mutex_unlock(&g_mu); wait_voice(v, serial); }
    RET(1);
}
HLE(u_MessageBeep) {
    uint32_t t = (uint32_t)ARG(0);
    tone(t == 0x10 ? 440 : t == 0x30 ? 660 : t == 0x40 ? 990 : t == 0x20 ? 770 : 880, 120);
    RET(1);
}

// ---- DirectSound ----------------------------------------------------------------------------------------------
// Objects follow the COM layout of hle_com.c: [vtable][refs:4][pad:4][state index:8].

typedef struct ComMethod { const char *name; HleFn fn; int nargs; } ComMethod;
enum { VT_DS, VT_DSB, VT_DSN, VT_COUNT };
static uint64_t g_vt[VT_COUNT];
SOUL_PROCESS_VAR(g_vt)
static uint64_t vtable(int which, const char *iface, const ComMethod *m, int n) {
    if (g_vt[which]) return g_vt[which];
    uint8_t *t = g_calloc((size_t)n, (size_t)PSZ);
    for (int i = 0; i < n; i++) {
        char name[96];
        snprintf(name, sizeof name, "%s::%s", iface, m[i].name);
        uint32_t id = hle_thunk_for(g_proc, m[i].fn, "dsound", name);
        g_proc->thunks[id].argbytes = g_mode32 ? (uint16_t)(4 * m[i].nargs) : 0;
        wr_ptr(H2G(t) + (uint64_t)i * PSZ, g_proc->thunk_base + (uint64_t)id * SOUL_THUNK_STRIDE);
    }
    g_vt[which] = H2G(t);
    return g_vt[which];
}
static uint64_t obj_new(uint64_t vt, uint64_t state) {
    uint8_t *o = g_calloc(1, (size_t)PSZ + 16);
    wr_ptr(H2G(o), vt);
    mem_wr(H2G(o) + (uint64_t)PSZ, 4, 1);
    mem_wr(H2G(o) + (uint64_t)PSZ + 8, 8, state);
    return H2G(o);
}
#define OBJ_REFS(o) ((o) + (uint64_t)PSZ)
#define OBJ_STATE(o) mem_rd((o) + (uint64_t)PSZ + 8, 8)
#define DS_OK 0
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_NOINTERFACE 0x80004002u
#define DSERR_BADFORMAT 0x88780064u

static Voice *dsb_voice(uint64_t self) { uint64_t i = OBJ_STATE(self); return i < MAX_VOICES && g_v[i].used && g_v[i].kind == V_DSOUND && g_v[i].pid == g_proc->pid ? &g_v[i] : NULL; }
static int is_guid(uint64_t p, uint32_t d1) { return p && (uint32_t)mem_rd(p, 4) == d1; }

HLE(ds_AddRef) { uint32_t r = (uint32_t)mem_rd(OBJ_REFS(ARG(0)), 4) + 1; mem_wr(OBJ_REFS(ARG(0)), 4, r); RET(r); }
HLE(ds_ReleaseDevice) { uint32_t r = (uint32_t)mem_rd(OBJ_REFS(ARG(0)), 4) - 1; mem_wr(OBJ_REFS(ARG(0)), 4, r); RET(r); }
HLE(dsb_Release) {
    uint32_t r = (uint32_t)mem_rd(OBJ_REFS(ARG(0)), 4) - 1;
    mem_wr(OBJ_REFS(ARG(0)), 4, r);
    if (r == 0) {
        pthread_mutex_lock(&g_mu);
        Voice *v = dsb_voice(ARG(0));
        void *mem = v && v->buf_owner ? v->buf : NULL;
        int shared = 0;
        if (v) {
            v->used = 0;
            v->serial = 0;
            for (int i = 0; i < MAX_VOICES && mem; i++) if (g_v[i].used && g_v[i].buf == mem) { g_v[i].buf_owner = 1; shared = 1; break; } // a duplicate keeps it
        }
        pthread_mutex_unlock(&g_mu);
        if (mem && !shared) g_free(mem);
        mem_wr(ARG(0) + (uint64_t)PSZ + 8, 8, ~0ull);
    }
    RET(r);
}
HLE(ds_QueryInterface) { // IUnknown, IDirectSound, IDirectSound8
    uint32_t d1 = ARG(1) ? (uint32_t)mem_rd(ARG(1), 4) : 1;
    if (d1 != 0 && d1 != 0x279AFA83 && d1 != 0xC50A7E93) { if (ARG(2)) wr_ptr(ARG(2), 0); RET(DSERR_NOINTERFACE); return; }
    if (ARG(2)) wr_ptr(ARG(2), ARG(0));
    ds_AddRef(st);
    RET(DS_OK);
}
static const ComMethod k_dsb[];
static const ComMethod k_dsn[];
static uint64_t make_buffer(X86State *st, uint64_t desc, int primary);
HLE(ds_CreateSoundBuffer) { // (self, DSBUFFERDESC*, &buffer, outer)
    uint64_t d = ARG(1);
    if (!d || !ARG(2)) { RET(DSERR_INVALIDPARAM); return; }
    uint32_t flags = (uint32_t)mem_rd(d + 4, 4);
    uint64_t b = make_buffer(st, d, (flags & 1) != 0); // DSBCAPS_PRIMARYBUFFER
    if (!b) { RET(DSERR_BADFORMAT); return; }
    wr_ptr(ARG(2), b);
    RET(DS_OK);
}
HLE(ds_GetCaps) { if (ARG(1)) { uint32_t sz = (uint32_t)mem_rd(ARG(1), 4); memset(G2H(ARG(1) + 4), 0, sz > 4 ? sz - 4 : 0); mem_wr(ARG(1) + 4, 4, 0xF5F); mem_wr(ARG(1) + 8, 4, 100); mem_wr(ARG(1) + 12, 4, 200000); } RET(DS_OK); }
HLE(ds_Ok) { RET(DS_OK); }
HLE(ds_GetSpeakerConfig) { if (ARG(1)) mem_wr(ARG(1), 4, 4); RET(DS_OK); } // DSSPEAKER_STEREO
HLE(ds_DuplicateSoundBuffer) { // (self, original, &duplicate): a new voice on the same memory
    pthread_mutex_lock(&g_mu);
    Voice *src = dsb_voice(ARG(1));
    Voice *v = src ? voice_new(V_DSOUND) : NULL;
    if (v) { uint32_t pid = v->pid, serial = v->serial; *v = *src; v->pid = pid; v->serial = serial; v->playing = 0; v->play_pos = 0; v->nnotify = 0; v->buf_owner = 0; }
    pthread_mutex_unlock(&g_mu);
    if (!v) { RET(DSERR_INVALIDPARAM); return; }
    wr_ptr(ARG(2), obj_new(vtable(VT_DSB, "IDirectSoundBuffer", k_dsb, 24), (uint64_t)(v - g_v)));
    RET(DS_OK);
}
static const ComMethod k_ds[] = {
    { "QueryInterface", ds_QueryInterface, 3 }, { "AddRef", ds_AddRef, 1 }, { "Release", ds_ReleaseDevice, 1 },
    { "CreateSoundBuffer", ds_CreateSoundBuffer, 4 }, { "GetCaps", ds_GetCaps, 2 }, { "DuplicateSoundBuffer", ds_DuplicateSoundBuffer, 3 },
    { "SetCooperativeLevel", ds_Ok, 3 }, { "Compact", ds_Ok, 1 }, { "GetSpeakerConfig", ds_GetSpeakerConfig, 2 },
    { "SetSpeakerConfig", ds_Ok, 2 }, { "Initialize", ds_Ok, 2 }, { "VerifyCertification", ds_GetSpeakerConfig, 2 },
};

// IDirectSoundBuffer
HLE(dsb_QueryInterface) {
    if (is_guid(ARG(1), 0xB0210783)) { // IID_IDirectSoundNotify
        wr_ptr(ARG(2), obj_new(vtable(VT_DSN, "IDirectSoundNotify", k_dsn, 4), OBJ_STATE(ARG(0))));
        RET(DS_OK);
        return;
    }
    uint32_t d1 = ARG(1) ? (uint32_t)mem_rd(ARG(1), 4) : 1;
    if (d1 != 0 && d1 != 0x279AFA85 && d1 != 0x6825A449) { if (ARG(2)) wr_ptr(ARG(2), 0); RET(DSERR_NOINTERFACE); return; } // 3D interfaces: not offered
    if (ARG(2)) wr_ptr(ARG(2), ARG(0));
    ds_AddRef(st);
    RET(DS_OK);
}
HLE(dsb_GetCaps) { // DSBCAPS { size, flags, bufferBytes, unlockTransferRate, cpuOverhead }
    pthread_mutex_lock(&g_mu);
    Voice *v = dsb_voice(ARG(0));
    uint32_t len = v ? v->buf_len : 0;
    pthread_mutex_unlock(&g_mu);
    if (ARG(1)) { mem_wr(ARG(1) + 4, 4, 0x80); mem_wr(ARG(1) + 8, 4, len); mem_wr(ARG(1) + 12, 4, 0); mem_wr(ARG(1) + 16, 4, 0); }
    RET(DS_OK);
}
// The write cursor runs about 15 ms ahead of the play cursor, like a DirectSound driver.
static uint32_t write_cursor(const Voice *v) {
    if (!v->buf_len || !v->block) return 0;
    uint32_t ahead = (uint32_t)(v->rate * 15 / 1000) * (uint32_t)v->block;
    return v->playing ? (v->play_pos + ahead) % v->buf_len : v->play_pos;
}
HLE(dsb_GetCurrentPosition) { // (self, &play, &write)
    pthread_mutex_lock(&g_mu);
    Voice *v = dsb_voice(ARG(0));
    uint32_t play = v ? v->play_pos : 0, w = v ? write_cursor(v) : 0;
    pthread_mutex_unlock(&g_mu);
    if (ARG(1)) mem_wr(ARG(1), 4, play);
    if (ARG(2)) mem_wr(ARG(2), 4, w);
    RET(DS_OK);
}
HLE(dsb_GetFormat) { // (self, WAVEFORMATEX*, size, &written)
    pthread_mutex_lock(&g_mu);
    Voice *v = dsb_voice(ARG(0));
    Voice c = v ? *v : (Voice){ 0 };
    pthread_mutex_unlock(&g_mu);
    if (ARG(1) && (uint32_t)ARG(2) >= 18) {
        uint8_t *w = PTR(1);
        uint16_t tag = c.is_float ? 3 : 1, ch = (uint16_t)c.channels, al = (uint16_t)c.block, bits = (uint16_t)c.bits;
        uint32_t rate = (uint32_t)c.rate, avg = rate * al;
        memcpy(w, &tag, 2); memcpy(w + 2, &ch, 2); memcpy(w + 4, &rate, 4); memcpy(w + 8, &avg, 4); memcpy(w + 12, &al, 2); memcpy(w + 14, &bits, 2);
        memset(w + 16, 0, 2);
    }
    if (hle_arg(st, 3)) mem_wr(hle_arg(st, 3), 4, 18);
    RET(DS_OK);
}
HLE(dsb_GetVolume) { pthread_mutex_lock(&g_mu); Voice *v = dsb_voice(ARG(0)); float vol = v ? v->volume : 1; pthread_mutex_unlock(&g_mu); if (ARG(1)) mem_wr(ARG(1), 4, (uint32_t)(int32_t)(vol <= 0 ? -10000 : 2000 * log10f(vol))); RET(DS_OK); }
HLE(dsb_GetPan) { if (ARG(1)) mem_wr(ARG(1), 4, 0); RET(DS_OK); }
HLE(dsb_GetFrequency) { pthread_mutex_lock(&g_mu); Voice *v = dsb_voice(ARG(0)); uint32_t f = v ? (uint32_t)(v->freq_override ? v->freq_override : (uint64_t)v->rate) : 0; pthread_mutex_unlock(&g_mu); if (ARG(1)) mem_wr(ARG(1), 4, f); RET(DS_OK); }
HLE(dsb_GetStatus) { pthread_mutex_lock(&g_mu); Voice *v = dsb_voice(ARG(0)); uint32_t s = v && v->playing ? (1 | (v->looping ? 4 : 0)) : 0; pthread_mutex_unlock(&g_mu); if (ARG(1)) mem_wr(ARG(1), 4, s); RET(DS_OK); }
HLE(dsb_Lock) { // (self, offset, bytes, &p1, &n1, &p2, &n2, flags)
    pthread_mutex_lock(&g_mu);
    Voice *v = dsb_voice(ARG(0));
    uint32_t len = v ? v->buf_len : 0, wc = v ? write_cursor(v) : 0;
    uint64_t base = v && v->buf ? H2G(v->buf) : 0;
    pthread_mutex_unlock(&g_mu);
    if (!v || !len) { RET(DSERR_INVALIDPARAM); return; }
    uint32_t flags = (uint32_t)hle_arg(st, 7);
    uint32_t off = (flags & 1) ? wc : (uint32_t)ARG(1) % len;     // DSBLOCK_FROMWRITECURSOR
    uint32_t n = (flags & 2) ? len : (uint32_t)ARG(2);            // DSBLOCK_ENTIREBUFFER
    if (n > len) n = len;
    uint32_t first = len - off < n ? len - off : n;
    wr_ptr(ARG(3), base + off);
    mem_wr(hle_arg(st, 4), 4, first);
    if (hle_arg(st, 5)) wr_ptr(hle_arg(st, 5), n > first ? base : 0);
    if (hle_arg(st, 6)) mem_wr(hle_arg(st, 6), 4, n > first ? n - first : 0);
    RET(DS_OK);
}
HLE(dsb_Play) { // (self, reserved, priority, flags)
    pthread_mutex_lock(&g_mu);
    Voice *v = dsb_voice(ARG(0));
    if (v && v->buf) { v->playing = 1; v->looping = ((uint32_t)hle_arg(st, 3) & 1) != 0; }
    pthread_mutex_unlock(&g_mu);
    RET(DS_OK);
}
HLE(dsb_SetCurrentPosition) { pthread_mutex_lock(&g_mu); Voice *v = dsb_voice(ARG(0)); if (v && v->buf_len) v->play_pos = (uint32_t)ARG(1) % v->buf_len / (uint32_t)(v->block ? v->block : 1) * (uint32_t)(v->block ? v->block : 1); pthread_mutex_unlock(&g_mu); RET(DS_OK); }
HLE(dsb_SetFormat) { // primary buffer: the output format (the mixer converts anyway)
    RET(DS_OK);
}
HLE(dsb_SetVolume) { // hundredths of a decibel, -10000 .. 0
    int32_t db = (int32_t)ARG(1);
    pthread_mutex_lock(&g_mu);
    Voice *v = dsb_voice(ARG(0));
    if (v) v->volume = db <= -10000 ? 0 : powf(10.0f, (float)db / 2000.0f);
    pthread_mutex_unlock(&g_mu);
    RET(DS_OK);
}
HLE(dsb_SetPan) { // -10000 (left) .. 10000 (right)
    int32_t p = (int32_t)ARG(1);
    pthread_mutex_lock(&g_mu);
    Voice *v = dsb_voice(ARG(0));
    if (v) {
        v->gain_l = p > 0 ? powf(10.0f, (float)-p / 2000.0f) : 1;
        v->gain_r = p < 0 ? powf(10.0f, (float)p / 2000.0f) : 1;
    }
    pthread_mutex_unlock(&g_mu);
    RET(DS_OK);
}
HLE(dsb_SetFrequency) { pthread_mutex_lock(&g_mu); Voice *v = dsb_voice(ARG(0)); if (v) v->freq_override = (uint32_t)ARG(1); pthread_mutex_unlock(&g_mu); RET(DS_OK); }
HLE(dsb_Stop) {
    pthread_mutex_lock(&g_mu);
    Voice *v = dsb_voice(ARG(0));
    if (v) {
        v->playing = 0;
        for (int i = 0; i < v->nnotify; i++) if (v->notify[i].offset == 0xFFFFFFFFu) note_push((Note){ 2, v->pid, 0, 0, 0, 0, 0, 0, v->notify[i].event }); // DSBPN_OFFSETSTOP
    }
    pthread_mutex_unlock(&g_mu);
    RET(DS_OK);
}
static const ComMethod k_dsb[] = {
    { "QueryInterface", dsb_QueryInterface, 3 }, { "AddRef", ds_AddRef, 1 }, { "Release", dsb_Release, 1 }, { "GetCaps", dsb_GetCaps, 2 },
    { "GetCurrentPosition", dsb_GetCurrentPosition, 3 }, { "GetFormat", dsb_GetFormat, 4 }, { "GetVolume", dsb_GetVolume, 2 },
    { "GetPan", dsb_GetPan, 2 }, { "GetFrequency", dsb_GetFrequency, 2 }, { "GetStatus", dsb_GetStatus, 2 }, { "Initialize", ds_Ok, 3 },
    { "Lock", dsb_Lock, 8 }, { "Play", dsb_Play, 4 }, { "SetCurrentPosition", dsb_SetCurrentPosition, 2 }, { "SetFormat", dsb_SetFormat, 2 },
    { "SetVolume", dsb_SetVolume, 2 }, { "SetPan", dsb_SetPan, 2 }, { "SetFrequency", dsb_SetFrequency, 2 }, { "Stop", dsb_Stop, 1 },
    { "Unlock", ds_Ok, 5 }, { "Restore", ds_Ok, 1 }, { "SetFX", ds_Ok, 4 }, { "AcquireResources", ds_Ok, 4 }, { "GetObjectInPath", ds_Ok, 5 },
};
HLE(dsn_SetNotificationPositions) { // (self, count, DSBPOSITIONNOTIFY[] { offset, event })
    pthread_mutex_lock(&g_mu);
    uint64_t i = OBJ_STATE(ARG(0));
    Voice *v = i < MAX_VOICES && g_v[i].used ? &g_v[i] : NULL;
    uint32_t n = (uint32_t)ARG(1);
    if (v) {
        v->nnotify = 0;
        const char *L = "4P";
        for (uint32_t k = 0; k < n && k < 32; k++) {
            uint64_t e = ARG(2) + (uint64_t)k * gs_size(L);
            v->notify[k].offset = (uint32_t)gs_get(e, L, 0);
            v->notify[k].event = h_get(gs_get(e, L, 1));
            v->nnotify++;
        }
    }
    pthread_mutex_unlock(&g_mu);
    RET(v ? DS_OK : DSERR_INVALIDPARAM);
}
static const ComMethod k_dsn[] = {
    { "QueryInterface", ds_QueryInterface, 3 }, { "AddRef", ds_AddRef, 1 }, { "Release", ds_ReleaseDevice, 1 },
    { "SetNotificationPositions", dsn_SetNotificationPositions, 3 },
};
static uint64_t make_buffer(X86State *st, uint64_t d, int primary) {
    (void)st;
    // DSBUFFERDESC { size, flags, bufferBytes, reserved, WAVEFORMATEX*, guid3D }
    uint32_t bytes = (uint32_t)mem_rd(d + 8, 4);
    uint64_t fmt = rd_ptr(d + 16);
    Voice f;
    memset(&f, 0, sizeof f);
    if (primary || !fmt) { f.rate = 44100; f.channels = 2; f.bits = 16; f.block = 4; }
    else if (set_format(&f, G2H(fmt)) != 0) return 0;
    if (primary) bytes = 0;
    else {
        if (bytes < 4 || bytes > (64u << 20)) return 0; // DSBSIZE_MIN .. DSBSIZE_MAX
        bytes -= bytes % (uint32_t)f.block;
    }
    uint8_t *mem = bytes ? g_calloc(1, bytes) : NULL; // in the program's memory: Lock hands it out
    if (mem && f.bits == 8) memset(mem, 0x80, bytes); // 8-bit silence
    pthread_mutex_lock(&g_mu);
    Voice *v = voice_new(V_DSOUND);
    if (v) {
        v->rate = f.rate; v->channels = f.channels; v->bits = f.bits; v->is_float = f.is_float; v->block = f.block;
        v->buf = mem; v->buf_len = bytes; v->buf_owner = 1;
    }
    pthread_mutex_unlock(&g_mu);
    if (!v) { if (mem) g_free(mem); return 0; }
    return obj_new(vtable(VT_DSB, "IDirectSoundBuffer", k_dsb, 24), (uint64_t)(v - g_v));
}
HLE(ds_DirectSoundCreate) { // (guid, &ds, outer)
    if (!ARG(1)) { RET(DSERR_INVALIDPARAM); return; }
    wr_ptr(ARG(1), obj_new(vtable(VT_DS, "IDirectSound8", k_ds, 12), 0));
    RET(DS_OK);
}
static void ds_enumerate(X86State *st, int wide) { // (callback, context): (guid, description, module, context)
    uint64_t desc = wide ? H2G(u2w_gdup("Soul Hoparlör")) : H2G(g_strdup("Soul Hoparlor"));
    uint64_t mod = wide ? H2G(u2w_gdup("")) : H2G(g_strdup(""));
    uint64_t args[4] = { 0, desc, mod, ARG(1) };
    soul_guest_call(st, ARG(0), 4, args);
    g_free(G2H(desc)); g_free(G2H(mod));
    RET(DS_OK);
}
HLE(ds_DirectSoundEnumerateA) { ds_enumerate(st, 0); }
HLE(ds_DirectSoundEnumerateW) { ds_enumerate(st, 1); }

// mixerGetNumDevs / auxiliary
HLE(mm_Zero) { RET(0); }

const HleEntry hle_audio_table[] = {
    { "waveOutGetNumDevs", mm_waveOutGetNumDevs, NULL }, { "waveOutGetDevCapsA", mm_waveOutGetDevCapsA, NULL },
    { "waveOutGetDevCapsW", mm_waveOutGetDevCapsW, NULL }, { "waveOutOpen", mm_waveOutOpen, NULL },
    { "waveOutPrepareHeader", mm_waveOutPrepareHeader, NULL }, { "waveOutUnprepareHeader", mm_waveOutUnprepareHeader, NULL },
    { "waveOutWrite", mm_waveOutWrite, NULL }, { "waveOutReset", mm_waveOutReset, NULL }, { "waveOutClose", mm_waveOutClose, NULL },
    { "waveOutPause", mm_waveOutPause, NULL }, { "waveOutRestart", mm_waveOutRestart, NULL }, { "waveOutBreakLoop", mm_waveOutBreakLoop, NULL },
    { "waveOutGetPosition", mm_waveOutGetPosition, NULL }, { "waveOutSetVolume", mm_waveOutSetVolume, NULL },
    { "waveOutGetVolume", mm_waveOutGetVolume, NULL }, { "waveOutGetID", mm_waveOutGetID, NULL }, { "waveOutMessage", mm_waveOutMessage, NULL },
    { "waveInGetNumDevs", mm_waveInGetNumDevs, NULL }, { "waveInOpen", mm_waveInOpen, NULL },
    { "PlaySoundA", mm_PlaySoundA, NULL }, { "PlaySoundW", mm_PlaySoundW, NULL }, { "PlaySound", mm_PlaySoundA, NULL },
    { "sndPlaySoundA", mm_sndPlaySoundA, NULL }, { "sndPlaySoundW", mm_sndPlaySoundW, NULL },
    { "Beep", k32_Beep, NULL }, { "MessageBeep", u_MessageBeep, NULL },
    { "DirectSoundCreate", ds_DirectSoundCreate, NULL }, { "DirectSoundCreate8", ds_DirectSoundCreate, NULL },
    { "DirectSoundEnumerateA", ds_DirectSoundEnumerateA, NULL }, { "DirectSoundEnumerateW", ds_DirectSoundEnumerateW, NULL },
    { "midiOutGetNumDevs", mm_Zero, NULL }, { "mixerGetNumDevs", mm_Zero, NULL }, { "auxGetNumDevs", mm_Zero, NULL },
    { NULL, NULL, NULL }
};
