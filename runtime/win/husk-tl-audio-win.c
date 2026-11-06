/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The game's sound to the speakers on Windows: the Darwin file (husk-tl-audio.c) with
 * waveOut in place of AudioQueue. The ring, the pacing and the 2x upsampling of low
 * rates are the same code; a feeder thread here plays the part of AudioQueue's callback,
 * refilling each buffer the device hands back.
 */
#include "husk-tl-audio.h"

#include <windows.h>
#include <mmsystem.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "husk-tl-bionic.h"
#include "husk-tl-cocos.h"

#define NBUF 4
#define FRAMES_PER_BUF 1024
#define RING_FRAMES 8192

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    HWAVEOUT q;
    WAVEHDR hdr[NBUF];
    int16_t *bufs[NBUF];
    HANDLE event;
    int rate, channels;
    int16_t *ring;
    size_t head, count;
    bool paused, failed;
    bool stalled;
} A = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

/* A buffer the device is done with: refilled from the ring (silence when it is empty) and queued again. */
static void refill(WAVEHDR *h)
{
    int16_t *out = (int16_t *)h->lpData;
    size_t want = FRAMES_PER_BUF, ch = (size_t)A.channels;
    pthread_mutex_lock(&A.mu);
    size_t take = A.paused ? 0 : (A.count < want ? A.count : want);
    for (size_t i = 0; i < take; i++)
        for (size_t c = 0; c < ch; c++) out[i * ch + c] = A.ring[((A.head + i) % RING_FRAMES) * ch + c];
    A.head = (A.head + take) % RING_FRAMES;
    A.count -= take;
    A.stalled = false;
    pthread_cond_broadcast(&A.cv);
    pthread_mutex_unlock(&A.mu);
    if (take < want) memset(out + take * ch, 0, (want - take) * ch * sizeof(int16_t));
    h->dwBufferLength = (DWORD)(want * ch * sizeof(int16_t));
    h->dwFlags &= ~WHDR_DONE;
    waveOutWrite(A.q, h, sizeof(*h));
}

static DWORD WINAPI feeder(void *arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        WaitForSingleObject(A.event, INFINITE);
        for (int i = 0; i < NBUF; i++) if (A.hdr[i].dwFlags & WHDR_DONE) refill(&A.hdr[i]);
    }
    return 0;
}

static bool open_queue(int rate, int channels)
{
    WAVEFORMATEX f = { 0 };
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = (WORD)channels; f.nSamplesPerSec = (DWORD)rate;
    f.wBitsPerSample = 16; f.nBlockAlign = (WORD)(channels * 2); f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    A.event = CreateEventA(NULL, FALSE, FALSE, NULL);
    MMRESULT r = waveOutOpen(&A.q, WAVE_MAPPER, &f, (DWORD_PTR)A.event, 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) { tl_log_line("audio: waveOutOpen failed (%u)", (unsigned)r); A.q = NULL; return false; }
    A.ring = calloc((size_t)RING_FRAMES * (size_t)channels, sizeof(int16_t));
    A.rate = rate; A.channels = channels;
    for (int i = 0; i < NBUF; i++) {
        A.bufs[i] = calloc((size_t)FRAMES_PER_BUF * (size_t)channels, sizeof(int16_t));
        memset(&A.hdr[i], 0, sizeof(A.hdr[i]));
        A.hdr[i].lpData = (LPSTR)A.bufs[i];
        A.hdr[i].dwBufferLength = FRAMES_PER_BUF * (DWORD)channels * 2;
        waveOutPrepareHeader(A.q, &A.hdr[i], sizeof(A.hdr[i]));
        waveOutWrite(A.q, &A.hdr[i], sizeof(A.hdr[i]));
    }
    CloseHandle(CreateThread(NULL, 0, feeder, NULL, 0, NULL));
    tl_log_line("audio: output running, %d Hz, %d channel(s)", rate, channels);
    return true;
}

static void sleep_for(int frames, int rate)
{
    struct timespec ts = { 0, (long)((double)frames * 1e9 / rate) };
    nanosleep(&ts, NULL);
}

static void ring_write(const int16_t *samples, int frames, int channels, int rate);

/*
 * The mixer's write. A game mixing at a low rate (Unity's FMOD runs at 24 kHz) is played at twice that: each frame, then the
 * point halfway to the next. The iPhone played Subway Surfers' 24 kHz output high-pitched and broken while the same samples
 * were right on a Mac, and every game that sounds right there runs at 44.1 or 48 kHz -- so the queue is only ever opened
 * at one of those.
 */
static void host_write(const int16_t *samples, int frames, int channels, int rate)
{
    if (rate > 0 && rate * 2 <= 48000 && channels > 0 && channels <= 2 && frames > 0) {
        static int16_t prev[2];
        static bool have_prev;
        int16_t *up = malloc((size_t)frames * 2 * (size_t)channels * sizeof(int16_t));
        if (up) {
            for (int f = 0; f < frames; f++)
                for (int c = 0; c < channels; c++) {
                    int16_t cur = samples[(size_t)f * channels + c];
                    int16_t before = f > 0 ? samples[(size_t)(f - 1) * channels + c] : (have_prev ? prev[c] : cur);
                    up[(size_t)(2 * f) * channels + c] = (int16_t)(((int)before + cur) / 2);
                    up[(size_t)(2 * f + 1) * channels + c] = cur;
                }
            for (int c = 0; c < channels; c++) prev[c] = samples[(size_t)(frames - 1) * channels + c];
            have_prev = true;
            ring_write(up, frames * 2, channels, rate * 2);
            free(up);
            return;
        }
    }
    ring_write(samples, frames, channels, rate);
}

/*
 * Copy into the ring, blocking while it is full (that is the pacing).
 *
 * The wait is never unbounded. While the output is paused (the app is going
 * away) or stalled (the system stopped the queue: a lock, a call), the samples
 * are dropped and the writer is paced by the clock instead. The writer is the
 * game's audio thread, and the game stops that thread on its way out
 * (Native_NotifyAudioLostFocus, its resign handler): a writer parked here
 * forever is a join that never returns, and on the iPad that held the guest's
 * background handler until the app came back -- the lifecycle request timed
 * out, and the game's sound effects stayed off after the trip.
 */
static void ring_write(const int16_t *samples, int frames, int channels, int rate)
{
    pthread_mutex_lock(&A.mu);
    if (!A.q && !A.failed) { if (!open_queue(rate, channels)) A.failed = true; }
    if (A.failed || channels != A.channels) { pthread_mutex_unlock(&A.mu); sleep_for(frames, rate); return; }
    int done = 0;
    while (done < frames) {
        if (A.count >= RING_FRAMES && !A.paused && !A.stalled) {
            /* the ring drains in RING_FRAMES / rate seconds; twice that, and the output has stopped */
            struct timespec dl;
            clock_gettime(CLOCK_REALTIME, &dl);
            long ns = (long)(2.0 * RING_FRAMES * 1e9 / (rate > 0 ? rate : 48000));
            dl.tv_sec += (dl.tv_nsec + ns) / 1000000000L;
            dl.tv_nsec = (dl.tv_nsec + ns) % 1000000000L;
            while (A.count >= RING_FRAMES && !A.paused)
                if (pthread_cond_timedwait(&A.cv, &A.mu, &dl) != 0) {
                    A.stalled = true;
                    tl_log_line("audio: the output stopped draining; the samples are dropped until it runs again");
                    break;
                }
        }
        if (A.paused || A.count >= RING_FRAMES) {
            pthread_mutex_unlock(&A.mu);
            sleep_for(frames - done, rate);
            return;
        }
        size_t room = RING_FRAMES - A.count, n = (size_t)(frames - done) < room ? (size_t)(frames - done) : room;
        size_t tail = (A.head + A.count) % RING_FRAMES;
        for (size_t i = 0; i < n; i++)
            memcpy(&A.ring[((tail + i) % RING_FRAMES) * (size_t)channels], &samples[(size_t)(done + (int)i) * (size_t)channels], (size_t)channels * sizeof(int16_t));
        A.count += n;
        done += (int)n;
    }
    pthread_mutex_unlock(&A.mu);
}

/* TL_AUDIO_MUTE: pace like a device but make no sound (for test runs on a Mac). TL_AUDIO_STATS=1 also logs, about once a second,
 * how loud the game's output was, so a silent run still shows whether the game is making sound. */
static void muted_write(const int16_t *samples, int frames, int channels, int rate)
{
    static int stats = -1, seen, peak;
    static long loud, total;
    if (stats < 0) stats = getenv("TL_AUDIO_STATS") != NULL;
    if (stats) {
        for (int i = 0; i < frames * channels; i++) {
            int v = samples[i] < 0 ? -samples[i] : samples[i];
            if (v > peak) peak = v;
            if (v > 64) loud++;
        }
        total += frames * channels;
        if ((seen += frames) >= rate) {
            tl_log_line("audio: %d Hz x %d, peak %d, %.0f%% of samples audible", rate, channels, peak, 100.0 * loud / (total ? total : 1));
            seen = 0; peak = 0; loud = 0; total = 0;
        }
    }
    sleep_for(frames, rate);
}

void tl_audio_install(void) { tl_cocos_audio_hook = getenv("TL_AUDIO_MUTE") ? muted_write : host_write; }

void tl_audio_restart(void)
{
    pthread_mutex_lock(&A.mu);
    if (A.q) {
        A.count = 0;
        A.stalled = false;
        pthread_cond_broadcast(&A.cv);
        waveOutRestart(A.q);
        tl_log_line("audio: the output is started again");
    }
    pthread_mutex_unlock(&A.mu);
}

void tl_audio_set_paused(bool paused)
{
    pthread_mutex_lock(&A.mu);
    A.paused = paused;
    if (paused) A.count = 0;
    pthread_cond_broadcast(&A.cv);
    pthread_mutex_unlock(&A.mu);
}
