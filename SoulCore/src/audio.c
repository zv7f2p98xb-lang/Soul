// SoulCore - sound: a mixer for waveOut, PlaySound / Beep and DirectSound.
//
// Every playing sound is a voice. The host pulls the mix (44.1 kHz stereo float) from its audio
// render thread with soul_audio_render(); when no host pulls, a pump thread plays the sound out in
// real time into nothing, so programs waiting for their buffers to finish keep going. Notifications
// (WOM_DONE, events, DirectSound notify positions, waveOutProc callbacks) are delivered by a
// notifier thread, so the render thread never waits for the guest.
#include "hle.h"
#include "gui.h"
#include "audio.h"
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

// kind 1: waveOut message, 2: DirectSound notify event, 3: call of a method of a guest callback object (XAudio2)
typedef struct Note { int kind; uint32_t pid; int cb_type; uint64_t target, instance, handle, p1; uint32_t msg; HObj *event;
                      uint64_t obj; int midx, nargs; uint64_t a1, a2; int *flag; uint32_t owner; } Note;
static Note g_notes[1024];
static int g_nnotes;
// waveOutProc calls wait here for the process's audio callback thread
typedef struct Call { uint32_t pid; uint64_t fn; int nargs; uint64_t args[6]; uint64_t obj; int midx; int *flag; uint32_t owner; } Call;
static Call g_calls[512];
static int g_ncalls;
// XAudio2 voices / engines have a unique non-zero owner number; the callback thread publishes the owner of the
// call it is running, so DestroyVoice can wait for it
static struct { uint32_t owner; SoulThread *thr; } g_exec[16];
static int owner_alive(uint32_t owner);

static void note_push(Note n) { // g_mu held
    if (g_nnotes < 1024) g_notes[g_nnotes++] = n;
    pthread_cond_signal(&g_note_cv);
}
static void deliver(Note *n) { // GIL held
    SoulProcess *p = soul_process_by_pid(n->pid);
    if (!p || p->released) return; // the program ended meanwhile
    if (n->kind == 2) { if (n->event) n->event->signaled = 1; return; } // DirectSound notify position
    if (n->kind == 3) { // method of a guest callback object, run on the process's audio callback thread
        pthread_mutex_lock(&g_mu);
        if (!owner_alive(n->owner)) { /* destroyed while the note waited */ }
        else if (g_ncalls < 512) g_calls[g_ncalls++] = (Call){ n->pid, 0, n->nargs, { n->obj, n->a1, n->a2 }, n->obj, n->midx, n->flag, n->owner };
        else if (n->flag) *n->flag = 0;
        pthread_mutex_unlock(&g_mu);
        return;
    }
    switch (n->cb_type) {
    case 1: win_post(n->target, n->msg, n->handle, n->p1); break;
    case 2: win_post_thread((uint32_t)n->target, n->msg, n->handle, n->p1); break;
    case 5: if (n->event) n->event->signaled = 1; break;
    case 3:
        pthread_mutex_lock(&g_mu);
        if (g_ncalls < 512) g_calls[g_ncalls++] = (Call){ n->pid, n->target, 5, { n->handle, n->msg, n->instance, n->p1, 0 }, 0, 0, NULL, 0 };
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
static int stream_mix(float *out, int frames);
static void stream_process_exit(uint32_t pid);
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
    if (stream_mix(out, frames)) any = 1;
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
    stream_process_exit(pid);
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
        int slot = -1;
        if (have && c.owner) for (int i = 0; i < 16 && slot < 0; i++) if (!g_exec[i].owner) { slot = i; g_exec[i].owner = c.owner; g_exec[i].thr = soul_cur_thread(); }
        pthread_mutex_unlock(&g_mu);
        if (!have) { gil_wait(200); continue; }
        uint64_t fn = c.fn;
        if (c.obj) fn = rd_ptr(rd_ptr(c.obj) + (uint64_t)c.midx * (uint64_t)PSZ); // guest callback object: method by vtable index
        if (fn) soul_guest_call(st, fn, c.nargs, c.args);
        pthread_mutex_lock(&g_mu);
        if (slot >= 0) g_exec[slot].owner = 0;
        if (c.flag && (!c.owner || owner_alive(c.owner))) *c.flag = 0;
        pthread_mutex_unlock(&g_mu);
    }
}
void saudio_ensure_callbacks(X86State *st);
static void ensure_cb_thread(X86State *st) {
    if (g_cb_thread) return;
    g_cb_thread = 1;
    uint32_t id = hle_thunk_for(g_proc, mm_SoulAudioThread, "winmm", "SoulAudioThread");
    g_proc->thunks[id].argbytes = g_mode32 ? 4 : 0;
    soul_thread_create(st, g_proc->thunk_base + (uint64_t)id * SOUL_THUNK_STRIDE, 0, 1u << 20, 0);
}
void saudio_ensure_callbacks(X86State *st) { ensure_cb_thread(st); }
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

// ---- stream voices (XAudio2) --------------------------------------------------------------------------------
// See audio.h. Everything here runs under g_mu; events are pushed as notes of kind 3 and run on the audio
// callback thread, in the order they happened. A voice's "processing pass" events are paced at 10 ms and at most
// one pair per voice / engine is pending, so a slow guest cannot be flooded.

#define SA_MAXV 256
#define SA_MAXE 8
#define SA_BLK 512
#define SA_QMAX 64
#define SA_PASS 441
#define SA_MAXCB 4

typedef struct SBuf {
    const uint8_t *data;
    uint8_t *own;                  // decoded ADPCM
    uint32_t frames, begin, end, lbegin, lend, loops_left;
    int loop_on, infinite, eos, started;
    uint64_t ctx;
} SBuf;
typedef struct SVoice {
    int used, type, eng, adpcm, running, stage;
    uint32_t pid, gen;
    int in_ch, rate, bits, block, is_float;
    SAudioFormat fmt;
    float volume, chvol[8], ratio, maxratio;
    int route_default, nsend, send[16];
    float mat[16][64];             // per send: index = dst channel * in_ch + src channel
    SBuf q[SA_QMAX];
    int qh, qn;
    uint32_t pos;                  // frame in the head buffer
    double frac;
    uint64_t played;
    uint64_t cb;
    int pass_pending;
    uint32_t pass_acc;
    float *acc;                    // submix / mastering: input of the current block
} SVoice;
typedef struct SEngine {
    int used, running, master, pass_pending, ncb;
    uint32_t pid, pass_acc, gen;
    uint64_t cbs[SA_MAXCB];
} SEngine;
static SVoice g_sv[SA_MAXV];
static SEngine g_se[SA_MAXE];
static uint32_t g_sgen;

static int owner_alive(uint32_t owner) { // g_mu held
    if (!owner) return 1;
    for (int i = 0; i < SA_MAXV; i++) if (g_sv[i].used && g_sv[i].gen == owner) return 1;
    for (int i = 0; i < SA_MAXE; i++) if (g_se[i].used && g_se[i].gen == owner) return 1;
    return 0;
}
// g_mu held: drops the notes and calls of a destroyed voice / engine that were not delivered yet (order kept)
static void purge_owner(uint32_t o) {
    int k = 0;
    for (int i = 0; i < g_nnotes; i++) if (g_notes[i].owner != o) g_notes[k++] = g_notes[i];
    g_nnotes = k;
    k = 0;
    for (int i = 0; i < g_ncalls; i++) if (g_calls[i].owner != o) g_calls[k++] = g_calls[i];
    g_ncalls = k;
}
// Waits (GIL released) until the callback thread is no longer running a callback of these owners. Not when
// called from inside such a callback (XAudio2 forbids that anyway).
static void wait_owners(const uint32_t *o, int n) {
    SoulThread *t = soul_cur_thread();
    for (;;) {
        int busy = 0;
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < 16; i++) if (g_exec[i].owner && g_exec[i].thr != t) for (int k = 0; k < n; k++) if (g_exec[i].owner == o[k]) busy = 1;
        pthread_mutex_unlock(&g_mu);
        if (!busy) return;
        gil_unlock(t);
        usleep(1000);
        gil_lock(t);
    }
}

static uint32_t cur_pid(void) { return g_proc ? g_proc->pid : 0; }
static SVoice *sv_get(int v) { return v >= 0 && v < SA_MAXV && g_sv[v].used && g_sv[v].pid == cur_pid() ? &g_sv[v] : NULL; }
static SEngine *se_get(int e) { return e >= 0 && e < SA_MAXE && g_se[e].used && g_se[e].pid == cur_pid() ? &g_se[e] : NULL; }

static void sv_call(SVoice *s, int midx, int nargs, uint64_t a1, int *flag) {
    if (!s->cb) return;
    Note n;
    memset(&n, 0, sizeof n);
    n.kind = 3; n.pid = s->pid; n.owner = s->gen; n.obj = s->cb; n.midx = midx; n.nargs = nargs; n.a1 = a1; n.flag = flag;
    note_push(n);
}
static void sv_release_buf(SBuf *b) { free(b->own); b->own = NULL; }
static void sv_free(SVoice *s) { // g_mu held
    for (int i = 0; i < s->qn; i++) sv_release_buf(&s->q[(s->qh + i) % SA_QMAX]);
    free(s->acc);
    memset(s, 0, sizeof *s);
}

int saudio_format_parse(const uint8_t *w, SAudioFormat *f) {
    memset(f, 0, sizeof *f);
    uint16_t tag, ch, align, bits, cb;
    uint32_t rate;
    memcpy(&tag, w, 2); memcpy(&ch, w + 2, 2); memcpy(&rate, w + 4, 4); memcpy(&align, w + 12, 2); memcpy(&bits, w + 14, 2);
    cb = tag == 1 || tag == 3 ? 0 : (uint16_t)(w[16] | w[17] << 8);
    if (tag == 0xFFFE) {
        memcpy(&cb, w + 16, 2);
        if (cb < 22) return -1;
        memcpy(&tag, w + 24, 2); // first word of the SubFormat GUID
    }
    if (!ch || ch > 8 || rate < 1000 || rate > 200000) return -1;
    f->tag = tag; f->channels = ch; f->rate = (int)rate; f->bits = bits;
    if (tag == 1) { if (bits != 8 && bits != 16 && bits != 24 && bits != 32) return -1; f->block = ch * bits / 8; }
    else if (tag == 3) { if (bits != 32) return -1; f->block = ch * 4; }
    else if (tag == 2) { // MS-ADPCM: samples per block, coefficient count, coefficient pairs
        if (ch > 2 || bits != 4 || align < 7 * (uint16_t)ch || cb < 4) return -1;
        uint16_t spb, nc;
        memcpy(&spb, w + 18, 2); memcpy(&nc, w + 20, 2);
        if (nc > 16 || (uint32_t)nc * 4 + 4 > cb || !spb) return -1;
        for (int i = 0; i < nc; i++) { int16_t a, b; memcpy(&a, w + 22 + 4 * i, 2); memcpy(&b, w + 24 + 4 * i, 2); f->coef[i][0] = a; f->coef[i][1] = b; }
        f->ncoef = nc; f->spb = spb; f->block = align;
    } else return -1; // xWMA and others
    return 0;
}

// MS-ADPCM -> 16-bit PCM. Returns frames, or -1.
static int64_t adpcm_decode(const SAudioFormat *f, const uint8_t *d, uint32_t bytes, int16_t **out) {
    static const int adapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };
    int ch = f->channels, blk = f->block;
    if (blk <= 0 || bytes % (uint32_t)blk) return -1;
    uint32_t nb = bytes / (uint32_t)blk;
    int spb = (blk - 7 * ch) * 2 / ch + 2;
    int16_t *o = malloc((size_t)nb * (size_t)spb * (size_t)ch * 2 + 2);
    if (!o) return -1;
    for (uint32_t b = 0; b < nb; b++) {
        const uint8_t *p = d + (size_t)b * (size_t)blk;
        int16_t *op = o + (size_t)b * (size_t)spb * (size_t)ch;
        int pred[2], delta[2], s1[2], s2[2], c1[2], c2[2];
        for (int c = 0; c < ch; c++) {
            int idx = p[c];
            if (idx >= (f->ncoef ? f->ncoef : 7)) idx = 0;
            static const int16_t std[7][2] = { { 256, 0 }, { 512, -256 }, { 0, 0 }, { 192, 64 }, { 240, 0 }, { 460, -208 }, { 392, -232 } };
            c1[c] = f->ncoef ? f->coef[idx][0] : std[idx][0];
            c2[c] = f->ncoef ? f->coef[idx][1] : std[idx][1];
        }
        const uint8_t *q = p + ch;
        for (int c = 0; c < ch; c++, q += 2) delta[c] = (int16_t)(q[0] | q[1] << 8);
        for (int c = 0; c < ch; c++, q += 2) s1[c] = (int16_t)(q[0] | q[1] << 8);
        for (int c = 0; c < ch; c++, q += 2) s2[c] = (int16_t)(q[0] | q[1] << 8);
        for (int c = 0; c < ch; c++) { op[c] = (int16_t)s2[c]; op[ch + c] = (int16_t)s1[c]; }
        int n = 2;
        const uint8_t *end = p + blk;
        int nib = 0; // nibble counter over interleaved samples
        for (; q < end && n < spb; q++) {
            for (int h = 0; h < 2 && n < spb; h++, nib++) {
                int c = ch == 2 ? h : 0;
                int v = h == 0 ? q[0] >> 4 : q[0] & 15;
                int sv = v >= 8 ? v - 16 : v;
                pred[c] = ((s1[c] * c1[c] + s2[c] * c2[c]) >> 8) + sv * delta[c];
                if (pred[c] > 32767) pred[c] = 32767;
                if (pred[c] < -32768) pred[c] = -32768;
                s2[c] = s1[c]; s1[c] = pred[c];
                delta[c] = (adapt[v] * delta[c]) >> 8;
                if (delta[c] < 16) delta[c] = 16;
                op[(size_t)n * (size_t)ch + (size_t)c] = (int16_t)pred[c];
                if (ch == 1 || h == 1) n++;
            }
        }
    }
    *out = o;
    return (int64_t)nb * spb;
}

// ---- engines and voices
int saudio_engine_new(void) {
    pthread_mutex_lock(&g_mu);
    int r = -1;
    for (int i = 0; i < SA_MAXE; i++) if (!g_se[i].used) {
        memset(&g_se[i], 0, sizeof g_se[i]);
        g_se[i].used = 1; g_se[i].gen = ++g_sgen; g_se[i].running = 1; g_se[i].master = -1; g_se[i].pid = cur_pid();
        ensure_threads();
        r = i;
        break;
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}
void saudio_engine_free(int e) {
    uint32_t owners[SA_MAXV + 1];
    int no = 0;
    pthread_mutex_lock(&g_mu);
    if (se_get(e)) {
        for (int i = 0; i < SA_MAXV; i++) if (g_sv[i].used && g_sv[i].eng == e) { owners[no++] = g_sv[i].gen; purge_owner(g_sv[i].gen); sv_free(&g_sv[i]); }
        owners[no++] = g_se[e].gen;
        purge_owner(g_se[e].gen);
        g_se[e].used = 0;
    }
    pthread_mutex_unlock(&g_mu);
    wait_owners(owners, no);
}
void saudio_engine_run(int e, int on) { pthread_mutex_lock(&g_mu); SEngine *x = se_get(e); if (x) x->running = on != 0; pthread_mutex_unlock(&g_mu); }
int saudio_engine_callback(int e, uint64_t obj, int add) {
    pthread_mutex_lock(&g_mu);
    SEngine *x = se_get(e);
    int r = -1;
    if (x) {
        r = 0;
        for (int i = 0; i < x->ncb; i++) if (x->cbs[i] == obj) { memmove(&x->cbs[i], &x->cbs[i + 1], sizeof(uint64_t) * (size_t)(x->ncb - i - 1)); x->ncb--; break; }
        if (add) { if (x->ncb < SA_MAXCB) x->cbs[x->ncb++] = obj; else r = -1; }
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}
void saudio_engine_stats(int e, uint32_t *active, uint32_t *total, uint32_t *sub) {
    *active = *total = *sub = 0;
    pthread_mutex_lock(&g_mu);
    if (se_get(e)) for (int i = 0; i < SA_MAXV; i++) {
        SVoice *s = &g_sv[i];
        if (!s->used || s->eng != e) continue;
        if (s->type == SA_SOURCE) { (*total)++; if (s->running && s->qn) (*active)++; }
        else if (s->type == SA_SUBMIX) (*sub)++;
    }
    pthread_mutex_unlock(&g_mu);
}
int saudio_voice_master(int e) { pthread_mutex_lock(&g_mu); SEngine *x = se_get(e); int m = x ? x->master : -1; pthread_mutex_unlock(&g_mu); return m; }

int saudio_voice_new(int e, int type, const SAudioFormat *fmt, int channels, int rate, float max_ratio, uint64_t cb_obj, int stage) {
    pthread_mutex_lock(&g_mu);
    SEngine *x = se_get(e);
    int r = -1;
    if (!x || (type == SA_MASTER && x->master >= 0)) goto out;
    if (type != SA_SOURCE && (channels < 1 || channels > 8)) goto out;
    for (int i = 0; i < SA_MAXV; i++) if (!g_sv[i].used) {
        SVoice *s = &g_sv[i];
        memset(s, 0, sizeof *s);
        s->used = 1; s->gen = ++g_sgen; s->type = type; s->eng = e; s->pid = x->pid; s->stage = stage; s->cb = cb_obj;
        s->volume = 1; s->ratio = 1; s->route_default = 1;
        for (int c = 0; c < 8; c++) s->chvol[c] = 1;
        if (type == SA_SOURCE) {
            s->fmt = *fmt; s->in_ch = fmt->channels; s->rate = fmt->rate; s->bits = fmt->bits; s->block = fmt->block; s->is_float = fmt->tag == 3;
            if (fmt->tag == 2) { s->adpcm = 1; s->bits = 16; s->block = fmt->channels * 2; }
            s->maxratio = max_ratio < 1.0f / 1024 ? 2.0f : max_ratio > 1024 ? 1024 : max_ratio;
        } else {
            s->in_ch = channels; s->rate = rate;
            s->acc = calloc((size_t)SA_BLK * 8, sizeof(float));
            if (!s->acc) { s->used = 0; goto out; }
            if (type == SA_MASTER) x->master = i;
        }
        r = i;
        break;
    }
out:
    pthread_mutex_unlock(&g_mu);
    return r;
}
void saudio_voice_free(int v) {
    uint32_t owner = 0;
    pthread_mutex_lock(&g_mu);
    SVoice *s = sv_get(v);
    if (s) {
        owner = s->gen;
        purge_owner(owner);
        SEngine *x = se_get(s->eng);
        if (x && x->master == v) x->master = -1;
        for (int i = 0; i < SA_MAXV; i++) { // nothing may send to a destroyed voice
            SVoice *o = &g_sv[i];
            if (!o->used) continue;
            for (int k = 0; k < o->nsend;) {
                if (o->send[k] == v) { o->send[k] = o->send[o->nsend - 1]; memcpy(o->mat[k], o->mat[o->nsend - 1], sizeof o->mat[k]); o->nsend--; } else k++;
            }
        }
        sv_free(s);
    }
    pthread_mutex_unlock(&g_mu);
    if (owner) wait_owners(&owner, 1);
}

static void def_matrix(int sch, int dch, float *m) {
    memset(m, 0, 64 * sizeof(float));
    if (sch == dch) { for (int i = 0; i < sch; i++) m[i * sch + i] = 1; return; }
    if (sch == 1) { for (int d = 0; d < dch && d < 2; d++) m[d] = 1; return; }
    if (dch == 1) { for (int s = 0; s < sch; s++) m[s] = sch == 2 ? 0.5f : 1.0f / (float)sch; return; }
    for (int s = 0; s < sch; s++) {
        if (s < dch && !(dch == 2 && sch > 2 && s > 1)) m[s * sch + s] = 1;
        else if (dch == 2 && sch > 2) { // downmix of surround channels to stereo
            if (s == 2) { m[0 * sch + s] = m[1 * sch + s] = 0.7071f; }
            else if (s >= 4) m[(s & 1 ? 1 : 0) * sch + s] = 0.7071f;
        }
    }
}
int saudio_voice_sends(int v, int n, const int *dst) {
    pthread_mutex_lock(&g_mu);
    SVoice *s = sv_get(v);
    int r = -1;
    if (!s || s->type == SA_MASTER || n > 16) goto out;
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        SVoice *d = sv_get(dst[i]);
        if (!d || d->type == SA_SOURCE || d->eng != s->eng || d == s) ok = 0;
        else if (d->type == SA_SUBMIX && d->stage <= s->stage) ok = 0; // a send must go to a later processing stage
        for (int k = 0; k < i && ok; k++) if (dst[k] == dst[i]) ok = 0;
    }
    if (!ok) goto out;
    if (n < 0) { s->route_default = 1; s->nsend = 0; }
    else {
        s->route_default = 0; s->nsend = n;
        for (int i = 0; i < n; i++) { s->send[i] = dst[i]; def_matrix(s->in_ch, g_sv[dst[i]].in_ch, s->mat[i]); }
    }
    r = 0;
out:
    pthread_mutex_unlock(&g_mu);
    return r;
}
int saudio_voice_start(int v) { pthread_mutex_lock(&g_mu); SVoice *s = sv_get(v); if (s) s->running = 1; pthread_mutex_unlock(&g_mu); return s ? 0 : -1; }
int saudio_voice_stop(int v) { pthread_mutex_lock(&g_mu); SVoice *s = sv_get(v); if (s) s->running = 0; pthread_mutex_unlock(&g_mu); return s ? 0 : -1; }

int saudio_voice_submit(int v, const SAudioBuffer *in) {
    pthread_mutex_lock(&g_mu);
    SVoice *s = sv_get(v);
    int r = -1;
    if (!s || s->type != SA_SOURCE) goto out;
    if (s->qn >= SA_QMAX) goto out;
    SBuf b;
    memset(&b, 0, sizeof b);
    uint32_t frames;
    if (s->adpcm) {
        int16_t *dec = NULL;
        int64_t n = in->bytes ? adpcm_decode(&s->fmt, in->data, in->bytes, &dec) : 0;
        if (n < 0) goto out;
        frames = (uint32_t)n;
        b.own = (uint8_t *)dec; b.data = (const uint8_t *)dec;
    } else {
        if (in->bytes % (uint32_t)s->block || in->bytes > 0x80000000u) goto out;
        frames = in->bytes / (uint32_t)s->block;
        b.data = in->data;
    }
    b.frames = frames; b.begin = in->play_begin;
    b.end = in->play_length ? in->play_begin + in->play_length : frames;
    if (in->play_begin > frames || b.end > frames || b.end < b.begin || (frames && !b.data)) { sv_release_buf(&b); goto out; }
    if (in->loop_count) {
        if (in->loop_count > 255) { sv_release_buf(&b); goto out; }
        b.lbegin = in->loop_begin;
        b.lend = in->loop_length ? in->loop_begin + in->loop_length : b.end;
        if (b.lbegin < b.begin || b.lend > b.end || b.lend <= b.lbegin) { sv_release_buf(&b); goto out; }
        b.loop_on = 1; b.infinite = in->loop_count == 255; b.loops_left = in->loop_count;
    }
    b.eos = in->eos != 0; b.ctx = in->context;
    s->q[(s->qh + s->qn) % SA_QMAX] = b;
    s->qn++;
    r = 0;
out:
    pthread_mutex_unlock(&g_mu);
    return r;
}
void saudio_voice_flush(int v) {
    pthread_mutex_lock(&g_mu);
    SVoice *s = sv_get(v);
    if (s) {
        int keep = s->running && s->qn && s->q[s->qh].started; // the buffer being played stays while the voice runs
        int n = s->qn;
        for (int i = keep ? 1 : 0; i < n; i++) {
            SBuf *b = &s->q[(s->qh + i) % SA_QMAX];
            sv_call(s, SA_CB_BUFFER_END, 2, b->ctx, NULL);
            sv_release_buf(b);
        }
        s->qn = keep ? 1 : 0;
        if (!keep) { s->qh = 0; s->pos = 0; }
    }
    pthread_mutex_unlock(&g_mu);
}
void saudio_voice_exit_loop(int v) {
    pthread_mutex_lock(&g_mu);
    SVoice *s = sv_get(v);
    if (s && s->qn) { SBuf *b = &s->q[s->qh]; b->infinite = 0; b->loops_left = 0; }
    pthread_mutex_unlock(&g_mu);
}
void saudio_voice_state(int v, uint64_t *ctx, uint32_t *queued, uint64_t *samples) {
    pthread_mutex_lock(&g_mu);
    SVoice *s = sv_get(v);
    *ctx = s && s->qn ? s->q[s->qh].ctx : 0;
    *queued = s ? (uint32_t)s->qn : 0;
    *samples = s ? s->played : 0;
    pthread_mutex_unlock(&g_mu);
}
void saudio_voice_set_ratio(int v, float r) {
    pthread_mutex_lock(&g_mu);
    SVoice *s = sv_get(v);
    if (s && s->type == SA_SOURCE) s->ratio = r != r ? 1.0f : r < 1.0f / 1024 ? 1.0f / 1024 : r > s->maxratio ? s->maxratio : r;
    pthread_mutex_unlock(&g_mu);
}
float saudio_voice_get_ratio(int v) { pthread_mutex_lock(&g_mu); SVoice *s = sv_get(v); float r = s ? s->ratio : 1; pthread_mutex_unlock(&g_mu); return r; }
void saudio_voice_set_source_rate(int v, int rate) { pthread_mutex_lock(&g_mu); SVoice *s = sv_get(v); if (s && s->type == SA_SOURCE && rate >= 1000 && rate <= 200000) s->rate = rate; pthread_mutex_unlock(&g_mu); }
void saudio_voice_set_volume(int v, float x) { pthread_mutex_lock(&g_mu); SVoice *s = sv_get(v); if (s) s->volume = x; pthread_mutex_unlock(&g_mu); }
float saudio_voice_get_volume(int v) { pthread_mutex_lock(&g_mu); SVoice *s = sv_get(v); float r = s ? s->volume : 1; pthread_mutex_unlock(&g_mu); return r; }
int saudio_voice_set_chvol(int v, int n, const float *x) {
    pthread_mutex_lock(&g_mu);
    SVoice *s = sv_get(v);
    int r = s && n == s->in_ch ? 0 : -1;
    if (r == 0) for (int i = 0; i < n; i++) s->chvol[i] = x[i];
    pthread_mutex_unlock(&g_mu);
    return r;
}
int saudio_voice_get_chvol(int v, int n, float *x) {
    pthread_mutex_lock(&g_mu);
    SVoice *s = sv_get(v);
    int r = s && n == s->in_ch ? 0 : -1;
    if (r == 0) for (int i = 0; i < n; i++) x[i] = s->chvol[i];
    pthread_mutex_unlock(&g_mu);
    return r;
}
// Index of the send to dst (dst < 0 or the default output = the mastering voice); makes a default route explicit. -1 if none.
static int send_index(SVoice *s, int dst, int create) {
    if (s->route_default) {
        SEngine *x = se_get(s->eng);
        int m = x ? x->master : -1;
        if (m < 0 || (dst >= 0 && dst != m)) return -1;
        if (!create) return -2; // default matrix
        s->route_default = 0; s->nsend = 1; s->send[0] = m;
        def_matrix(s->in_ch, g_sv[m].in_ch, s->mat[0]);
        return 0;
    }
    for (int k = 0; k < s->nsend; k++) if (dst < 0 ? s->nsend == 1 : s->send[k] == dst) return k;
    return -1;
}
int saudio_voice_set_matrix(int v, int dst, int sch, int dch, const float *m) {
    pthread_mutex_lock(&g_mu);
    SVoice *s = sv_get(v);
    int r = -1;
    if (!s || s->type == SA_MASTER || sch != s->in_ch) goto out;
    int k = send_index(s, dst, 1);
    if (k < 0 || g_sv[s->send[k]].in_ch != dch) goto out;
    for (int i = 0; i < sch * dch; i++) s->mat[k][i] = m[i];
    r = 0;
out:
    pthread_mutex_unlock(&g_mu);
    return r;
}
int saudio_voice_get_matrix(int v, int dst, int sch, int dch, float *m) {
    pthread_mutex_lock(&g_mu);
    SVoice *s = sv_get(v);
    int r = -1;
    if (!s || s->type == SA_MASTER || sch != s->in_ch) goto out;
    int k = send_index(s, dst, 0);
    if (k == -2) { SEngine *x = se_get(s->eng); if (g_sv[x->master].in_ch != dch) goto out; float t[64]; def_matrix(sch, dch, t); memcpy(m, t, sizeof(float) * (size_t)(sch * dch)); r = 0; goto out; }
    if (k < 0 || g_sv[s->send[k]].in_ch != dch) goto out;
    memcpy(m, s->mat[k], sizeof(float) * (size_t)(sch * dch));
    r = 0;
out:
    pthread_mutex_unlock(&g_mu);
    return r;
}

// ---- mixing
static void sv_start_head(SVoice *s) { SBuf *b = &s->q[s->qh]; b->started = 1; s->pos = b->begin; sv_call(s, SA_CB_BUFFER_START, 2, b->ctx, NULL); }
// Buffers whose play region has been played are finished: buffer end, then stream end for the last one.
static void sv_finish_done(SVoice *s) {
    while (s->qn && s->q[s->qh].started && s->pos >= s->q[s->qh].end) {
        SBuf *b = &s->q[s->qh];
        sv_call(s, SA_CB_BUFFER_END, 2, b->ctx, NULL);
        if (b->eos) sv_call(s, SA_CB_STREAM_END, 1, 0, NULL);
        sv_release_buf(b);
        s->qh = (s->qh + 1) % SA_QMAX; s->qn--; s->pos = 0;
    }
}
static int sv_settle(SVoice *s) { // 1 when a frame can be read
    for (;;) {
        if (!s->qn) return 0;
        if (!s->q[s->qh].started) sv_start_head(s);
        sv_finish_done(s);
        if (s->qn && s->q[s->qh].started) return 1;
    }
}
static void sv_advance(SVoice *s) {
    SBuf *b = &s->q[s->qh];
    s->pos++; s->played++;
    if (b->loop_on && s->pos >= b->lend) {
        if (b->infinite || b->loops_left > 0) {
            if (!b->infinite) b->loops_left--;
            s->pos = b->lbegin;
            sv_call(s, SA_CB_LOOP_END, 2, b->ctx, NULL);
        } else b->loop_on = 0;
    }
    sv_finish_done(s);
}
static void read_frame(const SVoice *s, const SBuf *b, uint32_t f, float *o) {
    const uint8_t *p = b->data + (size_t)f * (size_t)s->block;
    int bps = s->bits / 8;
    for (int c = 0; c < s->in_ch; c++) o[c] = sample_at(p + c * bps, s->bits, s->is_float);
}
// Source voice -> blk frames of in_ch channels (linear resampling to the output rate). 1 when it produced sound.
static int src_render(SVoice *s, float *out, int blk) {
    int ch = s->in_ch, any = 0;
    double step = (double)s->rate * (double)s->ratio / OUT_RATE;
    for (int i = 0; i < blk; i++) {
        float *o = out + (size_t)i * (size_t)ch;
        if (!sv_settle(s)) { for (int c = 0; c < ch; c++) o[c] = 0; continue; }
        SBuf *b = &s->q[s->qh];
        float a[8] = { 0 }, n[8] = { 0 };
        read_frame(s, b, s->pos, a);
        uint32_t limit = b->loop_on ? b->lend : b->end;
        if (s->frac > 0 && s->pos + 1 < limit) {
            read_frame(s, b, s->pos + 1, n);
            float t = (float)s->frac;
            for (int c = 0; c < ch; c++) a[c] += (n[c] - a[c]) * t;
        }
        for (int c = 0; c < ch; c++) o[c] = a[c];
        any = 1;
        s->frac += step;
        while (s->frac >= 1.0) {
            s->frac -= 1.0;
            sv_advance(s);
            if (!s->qn || !s->q[s->qh].started) { s->frac = 0; break; }
        }
    }
    return any;
}
static void route(const float *in, int sch, int blk, SVoice *d, const float *m) {
    int dch = d->in_ch;
    for (int i = 0; i < blk; i++) {
        const float *x = in + (size_t)i * (size_t)sch;
        float *y = d->acc + (size_t)i * (size_t)dch;
        for (int c = 0; c < dch; c++) {
            float sum = 0;
            for (int k = 0; k < sch; k++) sum += m[c * sch + k] * x[k];
            y[c] += sum;
        }
    }
}
static void send_out(SVoice *s, const float *x, int blk) {
    if (s->route_default) {
        SEngine *e = &g_se[s->eng];
        if (e->master >= 0) { float m[64]; def_matrix(s->in_ch, g_sv[e->master].in_ch, m); route(x, s->in_ch, blk, &g_sv[e->master], m); }
        return;
    }
    for (int k = 0; k < s->nsend; k++) if (g_sv[s->send[k]].used) route(x, s->in_ch, blk, &g_sv[s->send[k]], s->mat[k]);
}
static void apply_gain(SVoice *s, float *x, int blk) {
    int ch = s->in_ch;
    for (int i = 0; i < blk; i++) for (int c = 0; c < ch; c++) x[(size_t)i * (size_t)ch + (size_t)c] *= s->volume * s->chvol[c];
}
static void to_stereo(const float *v, int ch, float *l, float *r) {
    switch (ch) {
    case 1: *l = *r = v[0]; return;
    case 2: *l = v[0]; *r = v[1]; return;
    default: break;
    }
    *l = v[0]; *r = v[1];
    if (ch == 4) { *l += 0.7071f * v[2]; *r += 0.7071f * v[3]; return; }
    *l += 0.7071f * v[2]; *r += 0.7071f * v[2]; // front centre
    if (ch > 4) *l += 0.7071f * v[4];
    if (ch > 5) *r += 0.7071f * v[5];
    if (ch > 6) *l += 0.7071f * v[6];
    if (ch > 7) *r += 0.7071f * v[7];
}
static uint32_t frames_left(SVoice *s) {
    uint64_t n = 0;
    for (int i = 0; i < s->qn; i++) {
        SBuf *b = &s->q[(s->qh + i) % SA_QMAX];
        if (b->loop_on && b->infinite) return 0xFFFFFFFFu;
        uint32_t from = i == 0 && b->started ? s->pos : b->begin;
        n += b->end > from ? b->end - from : 0;
    }
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}
static int engine_mix(int ei, float *out, int blk) {
    static float tmp[SA_BLK * 8];
    SEngine *e = &g_se[ei];
    int any = 0;
    for (int i = 0; i < SA_MAXV; i++) { SVoice *s = &g_sv[i]; if (s->used && s->eng == ei && s->acc) memset(s->acc, 0, sizeof(float) * (size_t)blk * (size_t)s->in_ch); }
    for (int i = 0; i < SA_MAXV; i++) {
        SVoice *s = &g_sv[i];
        if (!s->used || s->eng != ei || s->type != SA_SOURCE || !s->running) continue;
        if (s->qn && src_render(s, tmp, blk)) {
            any = 1;
            apply_gain(s, tmp, blk);
            send_out(s, tmp, blk);
        }
        if (s->cb) { // processing pass every 10 ms: bytes the voice needs for the next pass
            s->pass_acc += (uint32_t)blk;
            if (s->pass_acc >= SA_PASS) {
                s->pass_acc -= SA_PASS;
                if (!s->pass_pending) {
                    uint64_t need = (uint64_t)((double)SA_PASS * s->rate * s->ratio / OUT_RATE + 1);
                    uint64_t have = frames_left(s);
                    uint64_t bytes = have >= need ? 0 : (need - have) * (uint64_t)s->block;
                    s->pass_pending = 1;
                    sv_call(s, SA_CB_PASS_START, 2, bytes, NULL);
                    sv_call(s, SA_CB_PASS_END, 1, 0, &s->pass_pending);
                }
            }
        }
    }
    int order[SA_MAXV], n = 0;
    for (int i = 0; i < SA_MAXV; i++) if (g_sv[i].used && g_sv[i].eng == ei && g_sv[i].type == SA_SUBMIX) {
        int k = n++;
        while (k > 0 && g_sv[order[k - 1]].stage > g_sv[i].stage) { order[k] = order[k - 1]; k--; }
        order[k] = i;
    }
    for (int j = 0; j < n; j++) {
        SVoice *s = &g_sv[order[j]];
        memcpy(tmp, s->acc, sizeof(float) * (size_t)blk * (size_t)s->in_ch);
        apply_gain(s, tmp, blk);
        send_out(s, tmp, blk);
    }
    if (e->master >= 0) {
        SVoice *m = &g_sv[e->master];
        int ch = m->in_ch;
        for (int i = 0; i < blk; i++) {
            float v[8] = { 0 }, l, r;
            for (int c = 0; c < ch; c++) v[c] = m->acc[(size_t)i * (size_t)ch + (size_t)c] * m->volume * m->chvol[c];
            to_stereo(v, ch, &l, &r);
            out[2 * i] += l;
            out[2 * i + 1] += r;
        }
    }
    if (e->ncb) {
        e->pass_acc += (uint32_t)blk;
        if (e->pass_acc >= SA_PASS) {
            e->pass_acc -= SA_PASS;
            if (!e->pass_pending) {
                e->pass_pending = 1;
                for (int i = 0; i < e->ncb; i++) { Note nt; memset(&nt, 0, sizeof nt); nt.kind = 3; nt.pid = e->pid; nt.owner = e->gen; nt.obj = e->cbs[i]; nt.midx = 0; nt.nargs = 1; note_push(nt); }
                for (int i = 0; i < e->ncb; i++) { Note nt; memset(&nt, 0, sizeof nt); nt.kind = 3; nt.pid = e->pid; nt.owner = e->gen; nt.obj = e->cbs[i]; nt.midx = 1; nt.nargs = 1; nt.flag = i == e->ncb - 1 ? &e->pass_pending : NULL; note_push(nt); }
            }
        }
    }
    return any;
}
static int stream_mix(float *out, int frames) { // g_mu held
    int any = 0;
    for (int done = 0; done < frames; done += SA_BLK) {
        int blk = frames - done < SA_BLK ? frames - done : SA_BLK;
        for (int e = 0; e < SA_MAXE; e++) if (g_se[e].used && g_se[e].running && engine_mix(e, out + 2 * done, blk)) any = 1;
    }
    return any;
}
static void stream_process_exit(uint32_t pid) { // g_mu held
    for (int i = 0; i < SA_MAXV; i++) if (g_sv[i].used && g_sv[i].pid == pid) sv_free(&g_sv[i]);
    for (int i = 0; i < SA_MAXE; i++) if (g_se[i].used && g_se[i].pid == pid) g_se[i].used = 0;
}

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
