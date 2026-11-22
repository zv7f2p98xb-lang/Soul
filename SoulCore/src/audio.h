// SoulCore - internal "stream voice" API of the sound mixer (audio.c), used by xaudio2.c.
//
// A stream engine groups the voices of one XAudio2 instance. Voices are source voices (a queue of
// buffers with a format), submix voices and the mastering voice (one per engine); a source or
// submix sends its output to submix / mastering voices. Everything is mixed into the 44.1 kHz stereo
// output of the mixer. Events (buffer start / end, loop end, stream end, processing passes) are
// delivered in order on the process's audio callback thread: method `midx` of the guest callback
// object (vtable index, no IUnknown methods) is called with the object as first argument.
#pragma once
#include <stdint.h>
#include "soul_internal.h"

enum { SA_SOURCE = 0, SA_SUBMIX = 1, SA_MASTER = 2 };

// Voice callback methods (IXAudio2VoiceCallback vtable order)
enum { SA_CB_PASS_START, SA_CB_PASS_END, SA_CB_STREAM_END, SA_CB_BUFFER_START, SA_CB_BUFFER_END, SA_CB_LOOP_END, SA_CB_ERROR };

typedef struct SAudioFormat {
    int tag;            // 1 PCM, 3 IEEE float, 2 MS-ADPCM (decoded when a buffer is submitted)
    int channels, rate, bits, block;
    int spb;            // ADPCM: samples per block
    int ncoef;
    int16_t coef[16][2];
} SAudioFormat;

typedef struct SAudioBuffer {
    const uint8_t *data;
    uint32_t bytes, play_begin, play_length, loop_begin, loop_length, loop_count; // positions in sample frames
    uint64_t context;
    int eos;            // XAUDIO2_END_OF_STREAM
} SAudioBuffer;

// WAVEFORMATEX / WAVEFORMATEXTENSIBLE in host memory; 0 = usable, -1 = unsupported format
int saudio_format_parse(const uint8_t *wfx, SAudioFormat *f);

int saudio_engine_new(void);                                   // engine id or -1
void saudio_engine_free(int e);                                // destroys its voices too
void saudio_engine_run(int e, int on);                         // StartEngine / StopEngine
int saudio_engine_callback(int e, uint64_t obj, int add);      // IXAudio2EngineCallback: (Un)RegisterForCallbacks
void saudio_engine_stats(int e, uint32_t *active_src, uint32_t *total_src, uint32_t *submix);
void saudio_ensure_callbacks(X86State *st);                    // starts the process's callback thread

// Returns the voice id or -1 (a second mastering voice, no free slot). fmt: source voices only.
int saudio_voice_new(int e, int type, const SAudioFormat *fmt, int channels, int rate, float max_ratio, uint64_t cb_obj, int stage);
void saudio_voice_free(int v);
int saudio_voice_master(int e);                                // the engine's mastering voice or -1
// Output voices. n < 0: default (the mastering voice). 0 = ok, -1 = invalid
int saudio_voice_sends(int v, int n, const int *dst);
int saudio_voice_start(int v);
int saudio_voice_stop(int v);
// 0 = ok, -1 = invalid call, -2 = out of memory
int saudio_voice_submit(int v, const SAudioBuffer *b);
void saudio_voice_flush(int v);
void saudio_voice_exit_loop(int v);
void saudio_voice_state(int v, uint64_t *context, uint32_t *queued, uint64_t *samples);
void saudio_voice_set_ratio(int v, float r);
float saudio_voice_get_ratio(int v);
void saudio_voice_set_source_rate(int v, int rate);
void saudio_voice_set_volume(int v, float x);
float saudio_voice_get_volume(int v);
int saudio_voice_set_chvol(int v, int n, const float *x);      // -1: wrong channel count
int saudio_voice_get_chvol(int v, int n, float *x);
// dst < 0: the default output (mastering voice). Matrix index = dst channel * src channels + src channel.
int saudio_voice_set_matrix(int v, int dst, int src_ch, int dst_ch, const float *m);
int saudio_voice_get_matrix(int v, int dst, int src_ch, int dst_ch, float *m);
