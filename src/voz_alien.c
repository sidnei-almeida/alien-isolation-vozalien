/*
 * VozAlien — voice detection for Alien: Isolation.
 * https://github.com/sidnei-almeida/alien-isolation-vozalien  (MIT License)
 *
 * Written from scratch against the Steam AI.exe (32-bit, sha256 dde66521...).
 * Every game address is byte-checked at startup; if anything differs the mod
 * stays disabled and says so in VozAlien.log.
 *
 * Natural mode (default): while you speak, every ~250 ms the mod posts a type-0
 * "movement noise" stimulus (the same kind footsteps and the motion tracker use)
 * to the game's StimulusManager, at your position, with a loudness derived from
 * your voice. From there the game decides: every character measures the walkable
 * path distance (straight line x2 when a vent is involved), compares it with its
 * hearing range (50 m), fills its HEARD_MOVEMENT meter, and the behaviour tree
 * chooses the reaction (investigate, sweep vents, killtrap, break out, attack...).
 *
 * Forced mode (fallback): an NPC_FakeSense-style event handed straight to the
 * Alien's perception, with certainty and positional error computed by the mod.
 *
 * Comments below are in Portuguese (the author's language); docs are in English.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define THISCALL __attribute__((thiscall))
#define VOZALIEN_VERSION "0.2.2"

/* ---------- AI.exe (Steam), RVAs ---------- */
#define RVA_STIM_CTOR      0x000468B0u /* ExtStimulus::ctor (0x3C bytes) */
#define RVA_STIM_ADD       0x000574D0u /* StimulusManager::AddExtended */
#define RVA_STIM_TICK      0x003A5310u /* StimulusManager::Tick */
#define RVA_TICK_CALLSITE  0x003B41A3u /* call StimulusManager::Tick (só roda com a IA ativa) */
#define RVA_FAKESENSE_CTOR 0x000AB520u /* evento de sentido forçado (NPC_FakeSense) */
#define RVA_ENQUEUE_SENSE  0x000E1C20u
#define RVA_CHAR_GET       0x0038A770u /* tem o endereço da raiz do mundo no 2º byte */
#define RVA_GAMETIME       0x012410A8u /* double: tempo de jogo */
#define RVA_SENSE_SCALAR   0x00DF70E4u /* float 0.03125 */

#define OFF_HANDLE     0x54
#define OFF_CLASSMETA  0x64
#define OFF_POS        0x70
#define OFF_BACKSTAGE  0x2DA /* byte: dentro do duto; +1 = entrando */
#define OFF_PERCEPTION 0x3BA8

#define CLASS_PLAYER 0
#define CLASS_ALIEN  1

enum { LVL_TRACE = 0, LVL_LOWER = 1, LVL_ACTIVATED = 2, LVL_UPPER = 3 };
static const char *LVL_NAME[] = {"TRACE", "LOWER", "ACTIVATED", "UPPER"};

typedef void *(THISCALL *stim_ctor_t)(void *self, int type, const float *pos, float time,
                                      const void *handle, float loudness, const uint8_t *flags6,
                                      int extra);
typedef void(THISCALL *stim_add_t)(void *mgr, const void *stim, int replace);
typedef void(THISCALL *tick_fn_t)(void *mgr);
typedef void *(THISCALL *fs_ctor_t)(void *self, int kind, uint32_t h0, uint32_t h1, const float *pos,
                                    float one, int flag, int threshold, float time, float scalar);
typedef void(THISCALL *enqueue_t)(void *perception, void *ev, int zero);

/* ---------- configuração ---------- */
static struct {
    int enabled, log_level, forced;
    char device[128];
    int calib_ms, attack_ms, release_ms, min_utt_ms, eval_ms, stale_ms, max_utt_ms;
    float start_db, stop_db;
    /* natural */
    float loud20, loud_max, loud_min;
    int fuzz_on;
    float fuzz_start, fuzz_frac, fuzz_max, bs_dist_mult;
    /* forçado */
    float radius20, max_radius, vertical_factor, upper_ratio, activated_ratio, lower_ratio;
    int memory_ms, repeat_cooldown_ms, max_per_min, bs_max_level;
    float bs_near_ratio;
    float frac[3], fmin[3], fmax[3];
    /* teste */
    int hotkeys, key_low, key_high;
    float key_low_db, key_high_db;
} C;

static char g_dir[MAX_PATH], g_ini[MAX_PATH + 32];

static float ini_f(const char *s, const char *k, float d) {
    char b[64], def[32];
    snprintf(def, sizeof def, "%g", d);
    GetPrivateProfileStringA(s, k, def, b, sizeof b, g_ini);
    return (float)atof(b);
}
static int ini_i(const char *s, const char *k, int d) {
    char b[64], def[32];
    snprintf(def, sizeof def, "%d", d);
    GetPrivateProfileStringA(s, k, def, b, sizeof b, g_ini);
    return (int)strtol(b, NULL, 0);
}

static void load_config(void) {
    char modo[32];
    C.enabled = ini_i("Geral", "Enabled", 1);
    C.log_level = ini_i("Geral", "LogLevel", 1);
    GetPrivateProfileStringA("Geral", "Modo", "natural", modo, sizeof modo, g_ini);
    C.forced = (_stricmp(modo, "forcado") == 0);

    GetPrivateProfileStringA("Microfone", "Device", "", C.device, sizeof C.device, g_ini);
    C.calib_ms = ini_i("Microfone", "CalibrationMs", 3000);
    C.start_db = ini_f("Microfone", "StartDb", 10.0f);
    C.stop_db = ini_f("Microfone", "StopDb", 6.0f);
    C.attack_ms = ini_i("Microfone", "AttackMs", 100);
    C.release_ms = ini_i("Microfone", "ReleaseMs", 350);
    C.min_utt_ms = ini_i("Microfone", "MinUtteranceMs", 300);
    C.eval_ms = ini_i("Microfone", "PostMs", 250);
    C.max_utt_ms = ini_i("Microfone", "MaxUtteranceMs", 8000);
    C.stale_ms = ini_i("Microfone", "StaleMs", 800);

    C.loud20 = ini_f("Voz", "Loudness20dB", 0.30f);
    C.loud_max = ini_f("Voz", "LoudnessMax", 1.0f);
    C.loud_min = ini_f("Voz", "LoudnessMin", 0.03f);

    C.fuzz_on = ini_i("Imprecisao", "Enabled", 1);
    C.fuzz_start = ini_f("Imprecisao", "StartDistance", 8.0f);
    C.fuzz_frac = ini_f("Imprecisao", "Fraction", 0.30f);
    C.fuzz_max = ini_f("Imprecisao", "Max", 15.0f);
    C.bs_dist_mult = ini_f("Imprecisao", "VentDistanceMultiplier", 2.0f);

    C.radius20 = ini_f("ModoForcado", "Radius20dB", 8.0f);
    C.max_radius = ini_f("ModoForcado", "MaxRadius", 40.0f);
    C.vertical_factor = ini_f("ModoForcado", "VerticalFactor", 2.0f);
    C.upper_ratio = ini_f("ModoForcado", "UpperRatio", 0.20f);
    C.activated_ratio = ini_f("ModoForcado", "ActivatedRatio", 0.45f);
    C.lower_ratio = ini_f("ModoForcado", "LowerRatio", 0.70f);
    C.memory_ms = ini_i("ModoForcado", "MemoryMs", 20000);
    C.repeat_cooldown_ms = ini_i("ModoForcado", "RepeatCooldownMs", 2500);
    C.max_per_min = ini_i("ModoForcado", "MaxEventsPerMinute", 20);
    C.bs_max_level = ini_i("ModoForcado", "VentMaxLevel", LVL_ACTIVATED);
    C.bs_near_ratio = ini_f("ModoForcado", "VentNearRatio", 0.35f);
    C.frac[0] = 0.50f; C.fmin[0] = 6; C.fmax[0] = 20;
    C.frac[1] = 0.33f; C.fmin[1] = 4; C.fmax[1] = 14;
    C.frac[2] = 0.15f; C.fmin[2] = 2; C.fmax[2] = 6;

    C.hotkeys = ini_i("Teste", "Hotkeys", 0);
    C.key_low = ini_i("Teste", "KeyLow", VK_F7);
    C.key_high = ini_i("Teste", "KeyHigh", VK_F8);
    C.key_low_db = ini_f("Teste", "KeyLowDb", 18.0f);
    C.key_high_db = ini_f("Teste", "KeyHighDb", 32.0f);
}

/* ---------- log ---------- */
static FILE *g_log;
static CRITICAL_SECTION g_log_cs;
static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    EnterCriticalSection(&g_log_cs);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_cs);
}
#define LOG(...) logf_(__VA_ARGS__)
#define DBG(...) do { if (C.log_level >= 2) logf_(__VA_ARGS__); } while (0)

/* ---------- utilidades ---------- */
static uint32_t g_rng = 0x9E3779B9u;
static float frand(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (g_rng >> 8) * (1.0f / 16777216.0f);
}
static int readable(const void *p, size_t n) { return p && !IsBadReadPtr(p, n); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
typedef struct { float x, y, z; } vec3;
static float dist3(vec3 a, vec3 b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
static float dist_xz(vec3 a, vec3 b) { float dx = a.x - b.x, dz = a.z - b.z; return sqrtf(dx * dx + dz * dz); }
static int pos_ok(vec3 p) {
    return isfinite(p.x) && isfinite(p.y) && isfinite(p.z) && fabsf(p.x) < 1e5f &&
           fabsf(p.y) < 1e5f && fabsf(p.z) < 1e5f && (p.x != 0 || p.y != 0 || p.z != 0);
}

/* ---------- microfone ---------- */
#define SR 16000
#define FRAME_SAMPLES 320 /* 20 ms */
#define NBUF 8
#define SILENCE_DB -110.0f /* abaixo disso é zero digital, não ambiente */

typedef struct { float peak_db; int dur_ms; uint32_t utt; int final; ULONGLONG t; } voice_pkt;
#define QN 32
static voice_pkt g_q[QN];
static volatile LONG g_qhead, g_qtail;
static CRITICAL_SECTION g_q_cs;
static volatile float g_floor_db = -60.0f;

static void q_push(voice_pkt p) {
    EnterCriticalSection(&g_q_cs);
    LONG n = (g_qhead + 1) % QN;
    if (n != g_qtail) { g_q[g_qhead] = p; g_qhead = n; }
    LeaveCriticalSection(&g_q_cs);
}
static int q_pop(voice_pkt *p) {
    int ok = 0;
    EnterCriticalSection(&g_q_cs);
    if (g_qtail != g_qhead) { *p = g_q[g_qtail]; g_qtail = (g_qtail + 1) % QN; ok = 1; }
    LeaveCriticalSection(&g_q_cs);
    return ok;
}

static int cmpf(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static UINT pick_device(void) {
    if (!C.device[0]) return WAVE_MAPPER;
    UINT n = waveInGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        WAVEINCAPSA caps;
        if (waveInGetDevCapsA(i, &caps, sizeof caps) == MMSYSERR_NOERROR && strstr(caps.szPname, C.device)) {
            LOG("MIC device %u: %s", i, caps.szPname);
            return i;
        }
    }
    LOG("MIC device \"%s\" not found; using the default one", C.device);
    return WAVE_MAPPER;
}

static DWORD WINAPI mic_thread(LPVOID unused) {
    (void)unused;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 1, SR, SR * 2, 2, 16, 0};
    HWAVEIN wi = NULL;
    static short buf[NBUF][FRAME_SAMPLES];
    static WAVEHDR hdr[NBUF];
    MMRESULT r;
    for (;;) {
        r = waveInOpen(&wi, pick_device(), &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
        if (r == MMSYSERR_NOERROR) break;
        LOG("MIC waveInOpen failed (%u); retrying in 5 s", r);
        Sleep(5000);
    }
    for (int i = 0; i < NBUF; i++) {
        memset(&hdr[i], 0, sizeof hdr[i]);
        hdr[i].lpData = (LPSTR)buf[i];
        hdr[i].dwBufferLength = sizeof buf[i];
        waveInPrepareHeader(wi, &hdr[i], sizeof hdr[i]);
        waveInAddBuffer(wi, &hdr[i], sizeof hdr[i]);
    }
    waveInStart(wi);
    LOG("MIC open; calibrating for %d ms (stay quiet)", C.calib_ms);

    int ncal = C.calib_ms / 20 > 0 ? C.calib_ms / 20 : 1, ical = 0;
    float *cal = (float *)calloc(ncal, sizeof(float));
    float floor_db = -60.0f, smooth = -90.0f;
    int speaking = 0, attack = 0, release = 0, dur = 0, since_push = 0, pushed_any = 0;
    float win_peak = 0, utt_peak = 0, utt_min = 0;
    int nosig_ms = 0;
    uint32_t utt = 0;
    DWORD last_status = GetTickCount();

    for (;;) {
        WaitForSingleObject(ev, 200);
        for (int i = 0; i < NBUF; i++) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            int n = hdr[i].dwBytesRecorded / 2;
            double acc = 0;
            for (int k = 0; k < n; k++) acc += (double)buf[i][k] * buf[i][k];
            float rms = n ? (float)sqrt(acc / n) : 0;
            float db = 20.0f * log10f(rms / 32768.0f + 1e-7f);
            hdr[i].dwFlags &= ~WHDR_DONE;
            waveInAddBuffer(wi, &hdr[i], sizeof hdr[i]);
            if (n == 0) continue;

            /* Silêncio digital (microfone mudo/desconectado): não é "ruído do quarto". */
            if (db < SILENCE_DB) {
                nosig_ms += 20;
                if (nosig_ms == 2000) LOG("MIC has sent digital silence for 2 s: is the microphone muted? Waiting for it to come back.");
                if (speaking) release = C.release_ms; /* encerra a fala em curso */
                else continue;
            } else {
                if (nosig_ms >= 2000) {
                    LOG("MIC is back; recalibrating for %d ms (stay quiet)", C.calib_ms);
                    ical = 0; speaking = 0; attack = 0;
                }
                nosig_ms = 0;
                smooth = (smooth < SILENCE_DB) ? db : 0.6f * smooth + 0.4f * db;
            }
            if (ical < ncal) {
                if (db < SILENCE_DB) continue;
                cal[ical++] = db;
                if (ical == ncal) {
                    qsort(cal, ncal, sizeof(float), cmpf);
                    floor_db = cal[ncal * 3 / 10];
                    g_floor_db = floor_db;
                    LOG("MIC calibrated: room noise %.1f dBFS (median %.1f)", floor_db, cal[ncal / 2]);
                }
                continue;
            }
            float A = smooth - floor_db;
            if (A < 0) A = 0;
            if (speaking && smooth < utt_min) utt_min = smooth;
            if (speaking && dur >= C.max_utt_ms) { /* ruído do quarto mudou (ventilador, música...) */
                LOG("VOICE #%u lasted over %d ms without a pause: treating it as background noise; room noise %.1f -> %.1f dBFS",
                    utt, C.max_utt_ms, floor_db, utt_min);
                floor_db = utt_min; g_floor_db = floor_db;
                speaking = 0; attack = 0;
                continue;
            }
            if (!speaking) {
                floor_db += (db - floor_db) * (db < floor_db ? 0.05f : 0.002f);
                g_floor_db = floor_db;
                attack = (A >= C.start_db) ? attack + 20 : 0;
                if (attack >= C.attack_ms) {
                    speaking = 1; utt++; dur = attack; since_push = 0; pushed_any = 0;
                    win_peak = utt_peak = A; release = 0; utt_min = smooth;
                    DBG("VOICE start #%u A=%.1f dB", utt, A);
                }
            } else {
                dur += 20; since_push += 20;
                if (A > win_peak) win_peak = A;
                if (A > utt_peak) utt_peak = A;
                release = (A < C.stop_db) ? release + 20 : 0;
                int ending = release >= C.release_ms;
                if (dur >= C.min_utt_ms && (!pushed_any || since_push >= C.eval_ms)) {
                    voice_pkt p = {win_peak, dur, utt, ending, GetTickCount64()};
                    q_push(p);
                    pushed_any = 1; since_push = 0; win_peak = 0;
                }
                if (ending) {
                    speaking = 0; attack = 0;
                    if (pushed_any || C.log_level >= 2)
                        LOG("VOICE end #%u: %d ms, peak %.1f dB above room noise", utt, dur, utt_peak);
                }
            }
        }
        DWORD now = GetTickCount();
        if (C.log_level >= 2 && now - last_status > 10000) {
            last_status = now;
            DBG("MIC level=%.1f dBFS room=%.1f", smooth, floor_db);
        }
    }
    return 0;
}

/* ---------- estado do jogo ---------- */
static uint8_t *g_base;
static void **g_root_pp;
static stim_ctor_t g_stim_ctor;
static stim_add_t g_stim_add;
static tick_fn_t g_tick_orig;
static fs_ctor_t g_fs_ctor;
static enqueue_t g_enqueue;
static double *g_gametime;
static float g_scalar;

typedef struct { uint8_t *actor; vec3 pos; int backstage; } alien_t;
#define MAX_ALIENS 4
static uint8_t *g_player;
static vec3 g_player_pos;
static alien_t g_aliens[MAX_ALIENS];
static int g_naliens;

#define TRAIL_N 128
static vec3 g_trail[TRAIL_N];
static int g_trail_n, g_trail_i;

static int read_actor(uint8_t *a, int *cls, vec3 *pos) {
    if (!readable(a, OFF_BACKSTAGE + 2) || !readable(a + OFF_POS, 12)) return 0;
    uint8_t *meta = *(uint8_t **)(a + OFF_CLASSMETA);
    if (!readable(meta, 8)) return 0;
    *cls = *(int *)(meta + 4);
    memcpy(pos, a + OFF_POS, sizeof *pos);
    return 1;
}

static void scan_characters(void) {
    g_player = NULL;
    g_naliens = 0;
    if (!readable(g_root_pp, 4)) return;
    uint8_t *root = (uint8_t *)*g_root_pp;
    if (!readable(root, 12)) return;
    uint8_t *mgr = *(uint8_t **)(root + 8);
    if (!readable(mgr, 0xD8)) return;
    struct { uint8_t **arr; uint32_t n; } lists[2] = {
        {*(uint8_t ***)(mgr + 0x80), *(uint32_t *)(mgr + 0x84)},
        {*(uint8_t ***)(mgr + 0xD0), *(uint32_t *)(mgr + 0xD4)}};
    for (int l = 0; l < 2; l++) {
        if (lists[l].n == 0 || lists[l].n > 256 || !readable(lists[l].arr, lists[l].n * 4)) continue;
        for (uint32_t i = 0; i < lists[l].n; i++) {
            uint8_t *a = lists[l].arr[i];
            int cls;
            vec3 p;
            if (!read_actor(a, &cls, &p) || !pos_ok(p)) continue;
            if (cls == CLASS_PLAYER && !g_player) {
                g_player = a;
                g_player_pos = p;
            } else if (cls == CLASS_ALIEN && g_naliens < MAX_ALIENS) {
                g_aliens[g_naliens].actor = a;
                g_aliens[g_naliens].pos = p;
                g_aliens[g_naliens].backstage = a[OFF_BACKSTAGE] || a[OFF_BACKSTAGE + 1];
                g_naliens++;
            }
        }
    }
}

static void trail_update(void) {
    if (!g_player) return;
    if (g_trail_n) {
        vec3 last = g_trail[(g_trail_i + TRAIL_N - 1) % TRAIL_N];
        if (dist_xz(last, g_player_pos) < 1.0f && fabsf(last.y - g_player_pos.y) < 1.0f) return;
    }
    g_trail[g_trail_i] = g_player_pos;
    g_trail_i = (g_trail_i + 1) % TRAIL_N;
    if (g_trail_n < TRAIL_N) g_trail_n++;
}

/* Ponto onde "pareceu" vir a voz: um lugar por onde você passou há pouco, a ~r m
 * de você (chão onde dá pra andar), nunca mais perto do Alien do que você está. */
static vec3 fuzz_point(float r, const vec3 *alien, const char **how) {
    if (r <= 0.05f) { *how = "exact"; return g_player_pos; }
    float my_d = alien ? dist3(*alien, g_player_pos) : 0;
    int cand[TRAIL_N], nc = 0, best = -1;
    float bestdiff = 1e9f;
    for (int i = 0; i < g_trail_n; i++) {
        vec3 t = g_trail[i];
        if (fabsf(t.y - g_player_pos.y) > 3.0f) continue;
        if (alien && dist3(*alien, t) < my_d - 1.0f) continue;
        float d = dist_xz(t, g_player_pos);
        if (d >= 0.6f * r && d <= 1.4f * r) cand[nc++] = i;
        if (d >= 0.4f * r && fabsf(d - r) < bestdiff) { bestdiff = fabsf(d - r); best = i; }
    }
    if (nc) { *how = "trail"; return g_trail[cand[(int)(frand() * nc) % nc]]; }
    if (best >= 0) { *how = "trail-near"; return g_trail[best]; }
    *how = "exact(no-trail)";
    return g_player_pos;
}

static alien_t *nearest_alien(float *d_eff) {
    alien_t *best = NULL;
    float bd = 1e9f;
    for (int i = 0; i < g_naliens; i++) {
        float d = dist3(g_aliens[i].pos, g_player_pos);
        if (g_aliens[i].backstage) d *= C.bs_dist_mult;
        if (d < bd) { bd = d; best = &g_aliens[i]; }
    }
    *d_eff = bd;
    return best;
}

/* ---------- modo natural ---------- */
static uint32_t g_fuzz_utt = 0xFFFFFFFF;
static vec3 g_fuzz_pos, g_fuzz_base;
static const char *g_fuzz_how = "exact";
static float g_fuzz_r;

static void post_voice(void *mgr, float A, uint32_t utt, const char *src) {
    if (!g_player) { DBG("VOICE %s #%u: no player", src, utt); return; }
    float L = clampf(C.loud20 * powf(2.0f, (A - 20.0f) / 6.0f), 0.0f, C.loud_max);
    if (L < C.loud_min) { DBG("VOICE %s #%u A=%.1f: too quiet (%.3f)", src, utt, A, L); return; }

    float d_eff = 0;
    alien_t *al = nearest_alien(&d_eff);
    /* Erro permitido AGORA, pela distância atual do Alien. A posição fica estável
     * durante a fala, mas é refeita se mudou de fala, se você andou, ou se o Alien
     * chegou perto (aí a voz passa a vir de onde você está de verdade). */
    float r_now = 0;
    if (C.fuzz_on && al && d_eff > C.fuzz_start) r_now = clampf(C.fuzz_frac * d_eff, 0, C.fuzz_max);
    if (utt != g_fuzz_utt || utt == 0 || dist3(g_fuzz_base, g_player_pos) > 2.5f ||
        dist3(g_fuzz_pos, g_player_pos) > r_now * 1.4f + 0.5f) {
        g_fuzz_utt = utt;
        g_fuzz_base = g_player_pos;
        g_fuzz_r = r_now;
        g_fuzz_pos = fuzz_point(g_fuzz_r, al ? &al->pos : NULL, &g_fuzz_how);
    }

    uint8_t stim[0x40] __attribute__((aligned(16)));
    static const uint8_t flags[6] = {0, 0, 0, 0, 0, 0};
    memset(stim, 0, sizeof stim);
    float t = (float)*g_gametime;
    g_stim_ctor(stim, 0, &g_fuzz_pos.x, t, g_player + OFF_HANDLE, L, flags, 0);
    g_stim_add(mgr, stim, 0);

    char who[96] = "no Alien in this area";
    if (al) snprintf(who, sizeof who, "alien at %.1f m%s", dist3(al->pos, g_player_pos), al->backstage ? " IN VENT" : "");
    LOG("VOICE %s #%u A=%.1f dB -> loudness %.2f (range ~%.0f m; through vents ~%.0f m) | %s | position %s, offset %.1f m",
        src, utt, A, L, L * 50.0f, L * 25.0f, who, g_fuzz_how, dist3(g_fuzz_pos, g_player_pos));
}

/* ---------- modo forçado (reserva) ---------- */
typedef struct { uint8_t *actor; int level; ULONGLONG last_heard, last_emit; } alien_mem;
static alien_mem g_mem[MAX_ALIENS];
static ULONGLONG g_emits[64];
static int g_emit_i;

static alien_mem *mem_for(uint8_t *actor) {
    for (int i = 0; i < MAX_ALIENS; i++) if (g_mem[i].actor == actor) return &g_mem[i];
    for (int i = 0; i < MAX_ALIENS; i++)
        if (!g_mem[i].actor) { g_mem[i].actor = actor; g_mem[i].level = -1; return &g_mem[i]; }
    memset(&g_mem[0], 0, sizeof g_mem[0]);
    g_mem[0].actor = actor; g_mem[0].level = -1;
    return &g_mem[0];
}

static void forced_eval(float A, uint32_t utt, const char *src) {
    ULONGLONG now = GetTickCount64();
    float R = clampf(C.radius20 * powf(2.0f, (A - 20.0f) / 6.0f), 0.0f, C.max_radius);
    if (!g_player || !g_naliens) { DBG("FORCED %s #%u: no player/alien", src, utt); return; }
    for (int i = 0; i < g_naliens; i++) {
        alien_t *al = &g_aliens[i];
        float dx = al->pos.x - g_player_pos.x, dy = al->pos.y - g_player_pos.y, dz = al->pos.z - g_player_pos.z;
        float d = sqrtf(dx * dx + dz * dz + dy * dy * C.vertical_factor * C.vertical_factor);
        int bs = al->backstage;
        float deff = bs ? d * C.bs_dist_mult : d, q = R > 0 ? deff / R : 99.0f;
        alien_mem *m = mem_for(al->actor);
        if (m->level >= 0 && now - m->last_heard > (ULONGLONG)C.memory_ms) m->level = -1;
        if (q > 1.0f) { LOG("FORCED %s #%u A=%.1f radius=%.1f | alien d=%.1f%s: not heard", src, utt, A, R, d, bs ? " vent" : ""); continue; }
        int cap = q < C.upper_ratio ? LVL_UPPER : q < C.activated_ratio ? LVL_ACTIVATED : q < C.lower_ratio ? LVL_LOWER : LVL_TRACE;
        if (bs && cap > C.bs_max_level) cap = C.bs_max_level;
        int level;
        if (cap == LVL_UPPER) level = LVL_UPPER;
        else if (bs && q < C.bs_near_ratio) level = cap;
        else if (m->level >= 0) level = m->level + 1 > cap ? cap : m->level + 1;
        else level = cap - 1 < LVL_TRACE ? LVL_TRACE : cap - 1;
        if (level <= m->level && now - m->last_emit < (ULONGLONG)C.repeat_cooldown_ms) { m->last_heard = now; continue; }
        int n = 0;
        for (int k = 0; k < 64; k++) if (g_emits[k] && now - g_emits[k] < 60000) n++;
        if (n >= C.max_per_min) { LOG("FORCED: per-minute limit reached"); continue; }
        float r = level < LVL_UPPER ? clampf(C.frac[level] * deff, C.fmin[level], C.fmax[level]) : 0;
        const char *how;
        vec3 p = fuzz_point(r, &al->pos, &how);
        uint8_t ev[0x40] __attribute__((aligned(16)));
        memset(ev, 0, sizeof ev);
        uint32_t h[2];
        memcpy(h, g_player + OFF_HANDLE, 8);
        g_fs_ctor(ev, 11, h[0], h[1], &p.x, 1.0f, 0, level, (float)*g_gametime, g_scalar);
        g_enqueue(al->actor + OFF_PERCEPTION, ev, 0);
        g_emits[g_emit_i] = now; g_emit_i = (g_emit_i + 1) % 64;
        m->level = level; m->last_heard = m->last_emit = now;
        LOG("FORCED %s #%u A=%.1f radius=%.1f | alien d=%.1f q=%.2f%s -> %s offset %.1f m (%s)", src, utt, A, R, d, q,
            bs ? " vent" : "", LVL_NAME[level], dist3(p, g_player_pos), how);
    }
}

/* ---------- tique (thread do jogo, dentro do StimulusManager::Tick) ---------- */
static ULONGLONG g_last_scan, g_last_status;
static int g_logged_player;
static int g_key_prev[2], g_synth_left;
static float g_synth_db;
static ULONGLONG g_synth_next;
static uint32_t g_synth_utt = 0x80000000u;

static void handle_voice(void *mgr, float A, uint32_t utt, const char *src) {
    if (C.forced) forced_eval(A, utt, src);
    else post_voice(mgr, A, utt, src);
}

static void game_tick(void *mgr) {
    ULONGLONG now = GetTickCount64();
    if (now - g_last_scan >= 250) {
        g_last_scan = now;
        scan_characters();
        trail_update();
        if (g_player && !g_logged_player) {
            uint32_t *h = (uint32_t *)(g_player + OFF_HANDLE);
            LOG("PLAYER found: pos=(%.2f,%.2f,%.2f) handle=(%08X,%08X)", g_player_pos.x, g_player_pos.y,
                g_player_pos.z, h[0], h[1]);
            g_logged_player = 1;
        }
        if (!g_player) g_logged_player = 0;
        if (g_player && now - g_last_status >= 15000) {
            g_last_status = now;
            char b[256] = "";
            for (int i = 0; i < g_naliens; i++) {
                vec3 a = g_aliens[i].pos;
                snprintf(b + strlen(b), sizeof b - strlen(b), " | alien%d (%.1f,%.1f,%.1f) at %.1f m%s", i, a.x,
                         a.y, a.z, dist3(a, g_player_pos), g_aliens[i].backstage ? " IN VENT" : "");
            }
            LOG("STATUS me=(%.1f,%.1f,%.1f)%s trail=%d room_noise=%.1f dBFS", g_player_pos.x, g_player_pos.y,
                g_player_pos.z, b[0] ? b : " | no Alien", g_trail_n, g_floor_db);
        }
    }

    voice_pkt p;
    while (q_pop(&p)) {
        if (now - p.t > (ULONGLONG)C.stale_ms) continue; /* estava pausado/carregando */
        handle_voice(mgr, p.peak_db, p.utt, "voice");
    }

    if (C.hotkeys) {
        int keys[2] = {C.key_low, C.key_high};
        float dbs[2] = {C.key_low_db, C.key_high_db};
        for (int k = 0; k < 2; k++) {
            int down = (GetAsyncKeyState(keys[k]) & 0x8000) != 0;
            if (down && !g_key_prev[k]) { /* simula 1 s de fala */
                g_synth_db = dbs[k];
                g_synth_left = C.forced ? 1 : 1000 / (C.eval_ms > 0 ? C.eval_ms : 250);
                g_synth_next = now;
                g_synth_utt++;
            }
            g_key_prev[k] = down;
        }
        if (g_synth_left > 0 && now >= g_synth_next) {
            g_synth_left--;
            g_synth_next = now + C.eval_ms;
            handle_voice(mgr, g_synth_db, g_synth_utt, "key");
        }
    }
}

static volatile LONG g_in_tick;
static void THISCALL tick_hook(void *mgr) {
    g_tick_orig(mgr);
    if (InterlockedExchange(&g_in_tick, 1)) return;
    game_tick(mgr);
    InterlockedExchange(&g_in_tick, 0);
}

/* ---------- instalação ---------- */
static int bytes_eq(const uint8_t *p, const char *hex) { /* "8B 54 ?? 04" */
    while (*hex) {
        while (*hex == ' ') hex++;
        if (!*hex) break;
        if (hex[0] == '?') { p++; hex += 2; continue; }
        unsigned v;
        if (sscanf(hex, "%2x", &v) != 1 || *p != (uint8_t)v) return 0;
        p++; hex += 2;
    }
    return 1;
}

static int install(void) {
    g_base = (uint8_t *)GetModuleHandleA(NULL);
    uint8_t *site = g_base + RVA_TICK_CALLSITE;
    struct { const char *name; uint32_t rva; const char *sig; } checks[] = {
        {"stim_ctor", RVA_STIM_CTOR, "8B C1 8B 4C 24 04 66 0F EF C0 66 0F D6 40 04 89 48 0C"},
        {"stim_add", RVA_STIM_ADD, "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 3C 8B 44 24 4C 56 8B F1"},
        {"stim_tick", RVA_STIM_TICK, "83 EC 0C FF 41 20 53 55 8D 41 04"},
        {"tick_call", RVA_TICK_CALLSITE - 5, "8B 49 18 56 57 E8"},
        {"fs_ctor", RVA_FAKESENSE_CTOR, "8B 54 24 04 8B C1 F3 0F 10 44 24 14 33 C9 89 48 04"},
        {"enqueue", RVA_ENQUEUE_SENSE, "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 3C"},
        {"chars", RVA_CHAR_GET, "A1 ?? ?? ?? ?? 8B 48 08 8B 91 84 00 00 00 8B 44 24 04 3B C2"},
    };
    for (unsigned i = 0; i < sizeof checks / sizeof checks[0]; i++) {
        uint8_t *p = g_base + checks[i].rva;
        if (!readable(p, 40) || !bytes_eq(p, checks[i].sig)) {
            LOG("ERROR: AI.exe is not the expected build (%s, RVA %08X). Mod disabled.", checks[i].name, checks[i].rva);
            return 0;
        }
    }
    uint8_t *target = site + 5 + *(int32_t *)(site + 1);
    if (target != g_base + RVA_STIM_TICK) {
        LOG("ERROR: the StimulusManager call is already redirected by another mod (RVA %08X). Mod disabled.",
            (unsigned)(target - g_base));
        return 0;
    }
    g_scalar = *(float *)(g_base + RVA_SENSE_SCALAR);
    if (g_scalar != 0.03125f) { LOG("ERROR: unexpected constant (%g). Mod disabled.", g_scalar); return 0; }
    g_root_pp = *(void ***)(g_base + RVA_CHAR_GET + 1);
    g_gametime = (double *)(g_base + RVA_GAMETIME);
    g_stim_ctor = (stim_ctor_t)(g_base + RVA_STIM_CTOR);
    g_stim_add = (stim_add_t)(g_base + RVA_STIM_ADD);
    g_fs_ctor = (fs_ctor_t)(g_base + RVA_FAKESENSE_CTOR);
    g_enqueue = (enqueue_t)(g_base + RVA_ENQUEUE_SENSE);
    g_tick_orig = (tick_fn_t)target;

    DWORD old;
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) { LOG("ERROR: VirtualProtect failed"); return 0; }
    *(int32_t *)(site + 1) = (int32_t)((uint8_t *)tick_hook - (site + 5));
    VirtualProtect(site + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    LOG("Hook on StimulusManager::Tick installed (base %p). Mode: %s", g_base, C.forced ? "forced" : "natural");
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    GetModuleFileNameA(h, g_dir, sizeof g_dir);
    char *s = strrchr(g_dir, '\\');
    if (s) s[1] = 0;
    snprintf(g_ini, sizeof g_ini, "%sVozAlien.ini", g_dir);
    load_config();
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_q_cs);
    char logp[MAX_PATH + 32], prev[MAX_PATH + 32];
    snprintf(logp, sizeof logp, "%sVozAlien.log", g_dir);
    snprintf(prev, sizeof prev, "%sVozAlien.previous.log", g_dir);
    MoveFileExA(logp, prev, MOVEFILE_REPLACE_EXISTING);
    g_log = fopen(logp, "w");
    g_rng ^= GetTickCount();
    LOG("VozAlien " VOZALIEN_VERSION " loaded");
    if (!C.enabled) { LOG("Enabled=0 in VozAlien.ini; nothing to do"); return TRUE; }
    if (!install()) return TRUE;
    CloseHandle(CreateThread(NULL, 0, mic_thread, NULL, 0, NULL));
    return TRUE;
}
