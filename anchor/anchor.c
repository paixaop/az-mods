/* SPDX-License-Identifier: GPL-3.0-only
 *
 * anchor: an AZ-MODS community mod that aligns a transition without touching the decks.
 *
 * It reads the phrase map the deck already parsed for each loaded track (see loaded.c),
 * watches the two decks' positions and the outgoing deck's loop, and paints a strip between
 * the waveforms: which phrase A's target ends on, when to start B, and - when B is looping -
 * the pass in which to press EXIT so that B's drop lands on A's boundary.
 *
 * It never starts, stops, loops, cues or seeks a deck. It only reads:
 *   - deck_info/deck_grid equivalents through the core (deck state and clock),
 *   - the loaded TrackPhrase and BeatGrid in EP147's memory (read-only, pread),
 *   - the ANLZ files on the USB stick when the deck has no phrase map (for hot cues) or on
 *     firmware whose loaded-state layout is not verified,
 *   - the two decks' positions, to detect a loop from the playhead wrapping.
 *
 * The strip is a surface on the deck view. Tap it to open the target chips; tap a chip to
 * latch that phrase end or cue for the current transition.
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "anlz.h"
#include "azmod-mod.h"
#include "guide.h"
#include "loaded.h"
#include "resolve.h"
#include "settings.h"
#include "display.h"
#include "mod.h"

#define MOD_ID ANCHOR_MOD_ID
#define MAX_CAND 256
#define LOAD_SETTLE_MS 250u

static const char *const TAGS[] = { "any", NULL };

/* The settings page (settings.c) is the mod's page under Community Mods > Installed and a
 * SCREEN row in the Mod Menu; the values live in the core's settings file. */

/* ------------------------------------------------------------------------------------------
 * state
 */
static const az_core_v1 *C;
static az_surface *strip;
static az_surface *sidebar_tab;
static int cur_layout = ANCHOR_LAYOUT_CARD;
static int tab_shown, tab_open = -1;
static int drawer_width = 190, drawer_from = 190, drawer_target = 190;
static uint64_t drawer_since;

static int loaded_supported;
static int prepared_reader_ok;

enum { ST_NONE = 0, ST_PENDING = 1, ST_OK = 2, ST_FAILED = 3 };

typedef struct {
    uint64_t last_ms;
    uint32_t last_pos;
    int      have_last;
    int      prev_playing;
    double   rate;
    int      rate_ok;
    int      confirmed;   /* a loop was seen wrapping */
    uint32_t in_ms, out_ms, len_ms;
    int      candidate;   /* one wrap seen, waiting for the second */
    uint32_t cand_in, cand_out, cand_len, wrap_pos;
    uint32_t in_low, in_high;
    uint64_t last_wrap_ms;
    int      released;
    int      native_loop; /* valid engaged state and boundaries from the core */
    int      loop_pending; /* native reader has not published a fresh result for this load */
} deck_rt_t;

typedef struct {
    int      deck;
    int      want_cues;
    uint32_t content_id;
    uint64_t seq;
    uint64_t not_before_ms;
    unsigned retries; /* deferred readiness retry, rather than a new track load */
    char     title[128];
    char     artist[128];
} request_t;

static pthread_mutex_t anlz_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t queue_mu = PTHREAD_MUTEX_INITIALIZER;
/* At most one waiting request per deck. A newer load replaces the waiting work rather
 * than filling a FIFO with obsolete tracks during rapid browsing/loading. */
static request_t queue[4];
static int queued[4];
static int q_next;
static int worker_ok;

/* Analysis belongs to this deck's current load, never to a title/content-id cache key.
 * Retired allocations are freed by the worker on replacement, not on the UI thread. */
static an_anlz_t deck_analysis[4];
static int status[4];
static char fail_why[4][160];
static uint64_t latest_seq[4];

static deck_rt_t rt[4];
static struct {
    an_loaded_loop_t state;
    uint64_t seq, observed_ms;
} prepared_loop[4]; /* protected by anlz_mu; native reads happen only on the worker */
static int last_loaded[4];
static uint32_t last_cid[4];
static char last_title[4][128];
static char last_artist[4][128];
static int load_notified[4];
static int cues_requested[4];
static int deck_gen[4];

/* UI thread state */
static int cur_a = -1, cur_b = -1;
static int role_gen_a = -1, role_gen_b = -1;
static an_target_opts_t cur_opts = {
    AN_RULE_PHRASE, -1, AN_POINT_END, 1, 1, 1
};
static int cur_deck_setting = -1;
static int setting_changed;

static an_plan_t plan;
static char shown_line2[96];
static an_display_t shown_display;
static int shown_display_valid;
static int strip_shown;
static uint64_t last_redraw_ms;

static int sel_open;
static int sel_page;
static int sel_n;
static int sel_per = 4;
static an_cand_t sel_cand[MAX_CAND];

static uint32_t latched_ms;
static char latched_label[40];
static int latched_gen = -1;

static az_rect chip_r[4];
static az_rect arrow_l_r, arrow_r_r, close_r;
static int layout_ok;
static int sel_deck = -1, sel_gen = -1;
static int sel_b, sel_kind; /* B picker: phrase starts or hot cues */
static int sel_wait_cues;
static az_rect kind_r[3], target_controls[2];
static int controls_ok;
typedef struct {
    int valid, gen, suggested_valid;
    an_cand_t target, suggested;
} incoming_target_t;
static incoming_target_t b_targets[4];

static int force_refresh;
static uint64_t worker_clock_ms; /* UI publishes time; worker scheduling is an atomic read */

/* ------------------------------------------------------------------------------------------
 * small helpers
 */
static uint64_t mono_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t accent_rgb(int accent)
{
    switch (accent) {
    case 4: return 0x34c759; /* green: aligned / window open */
    case 3: return 0xff453a; /* red: missed */
    case 2: return 0xffb340; /* amber: warning */
    case 1: return C ? C->color(AZ_COL_TEXT) : 0xffffff;
    default: return C ? C->color(AZ_COL_TEXT_DIM) : 0x888888;
    }
}

static void reset_runtime(int d)
{
    memset(&rt[d], 0, sizeof rt[d]);
}

static int selected_layout(void)
{
    int value = settings_get(SET_DISPLAY);
    return value >= 0 && value < 3 ? value + 1 : ANCHOR_LAYOUT_CARD;
}

static int near(uint32_t a, uint32_t b, uint32_t tol)
{
    uint32_t d = a > b ? a - b : b - a;
    return d <= tol;
}

static an_anlz_t *slot_anlz(int d)
{
    if (d < 0 || d >= 4 || status[d] != ST_OK || !deck_analysis[d].valid) return NULL;
    return &deck_analysis[d];
}

static void clear_deck_analysis(int d)
{
    pthread_mutex_lock(&anlz_mu);
    latest_seq[d]++; /* Cancel a read even if an eject is followed by no new load. */
    memset(&prepared_loop[d],0,sizeof prepared_loop[d]);
    status[d] = ST_NONE;
    fail_why[d][0] = 0;
    pthread_mutex_unlock(&anlz_mu);
    pthread_mutex_lock(&queue_mu);
    queued[d] = 0;
    pthread_mutex_unlock(&queue_mu);
}

static void invalidate_deck(int d)
{
    memset(&b_targets[d], 0, sizeof b_targets[d]);
    controls_ok = 0;
    clear_deck_analysis(d);
    cues_requested[d] = 0;
    reset_runtime(d);
    deck_gen[d]++;
    if (d == cur_a || d == cur_b) {
        latched_ms = 0;
        latched_gen = -1;
        sel_open = sel_n = layout_ok = 0;
        memset(&plan, 0, sizeof plan);
        shown_display_valid = 0;
        role_gen_a = role_gen_b = -1;
        if (strip && strip_shown) {
            C->surface_show(strip, 0);
            strip_shown = 0;
        }
    }
}

static void on_deck_event(int d, int what, void *user)
{
    (void)user;
    if (d < 0 || d >= 4 || (what != AZ_DECK_LOADED && what != AZ_DECK_EJECTED)) return;
    invalidate_deck(d);
    load_notified[d] = what == AZ_DECK_LOADED;
    if (what == AZ_DECK_EJECTED) last_loaded[d] = 0;
}

static void enqueue_request(int d, const az_deck_info *info, int want_cues)
{
    request_t r;
    memset(&r, 0, sizeof r);
    r.deck = d;
    r.want_cues = want_cues;
    cues_requested[d] = want_cues;
    r.content_id = info->content_id;
    uint64_t now = mono_ms();
    __atomic_store_n(&worker_clock_ms, now, __ATOMIC_RELEASE);
    r.not_before_ms = now + LOAD_SETTLE_MS;
    snprintf(r.title, sizeof r.title, "%s", info->title);
    snprintf(r.artist, sizeof r.artist, "%s", info->artist);

    pthread_mutex_lock(&anlz_mu);
    status[d] = ST_PENDING;
    fail_why[d][0] = 0;
    latest_seq[d]++;
    r.seq = latest_seq[d];
    pthread_mutex_unlock(&anlz_mu);

    if (!worker_ok) {
        pthread_mutex_lock(&anlz_mu);
        status[d] = ST_FAILED;
        snprintf(fail_why[d], sizeof fail_why[d], "the core has no thread service");
        pthread_mutex_unlock(&anlz_mu);
        return;
    }
    pthread_mutex_lock(&queue_mu);
    queue[d] = r;
    queued[d] = 1;
    pthread_mutex_unlock(&queue_mu);
}

static void note_track_change(int d, const az_deck_info *info)
{
    if (info->loaded < 0) return; /* Unknown is not an eject. */
    int loaded = info->loaded > 0;
    if (!loaded) {
        if (last_loaded[d] || load_notified[d]) {
            last_loaded[d] = 0;
            last_cid[d] = 0;
            last_title[d][0] = 0;
            last_artist[d][0] = 0;
            load_notified[d] = 0;
            invalidate_deck(d);
        }
        return;
    }
    int changed = load_notified[d] || !last_loaded[d] || info->content_id != last_cid[d] ||
                  strcmp(info->title, last_title[d]) != 0 ||
                  strcmp(info->artist, last_artist[d]) != 0;
    if (!changed) return;
    if (!load_notified[d]) invalidate_deck(d);
    load_notified[d] = 0;
    last_loaded[d] = 1;
    last_cid[d] = info->content_id;
    snprintf(last_title[d], sizeof last_title[d], "%s", info->title);
    snprintf(last_artist[d], sizeof last_artist[d], "%s", info->artist);
    enqueue_request(d, info, settings_get(SET_RULE) == AN_RULE_HOTCUE);
}

/* ------------------------------------------------------------------------------------------
 * the resolver thread
 */
static int take_request(request_t *out)
{
    uint64_t now = __atomic_load_n(&worker_clock_ms, __ATOMIC_ACQUIRE);
    pthread_mutex_lock(&queue_mu);
    for (int i = 0; i < 4; i++) {
        int d = (q_next + i) % 4;
        if (!queued[d]) continue;
        if (queue[d].retries && now < queue[d].not_before_ms) continue;
        *out = queue[d];
        queued[d] = 0;
        q_next = (d + 1) % 4;
        pthread_mutex_unlock(&queue_mu);
        return 1;
    }
    pthread_mutex_unlock(&queue_mu);
    return 0;
}

static int request_current(const request_t *r)
{
    pthread_mutex_lock(&anlz_mu);
    int current = r->seq == latest_seq[r->deck] && status[r->deck] == ST_PENDING;
    pthread_mutex_unlock(&anlz_mu);
    return current;
}

static int request_matches_deck(const request_t *r)
{
    if (!r->content_id && !r->title[0]) return 0;
    az_deck_info info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    return C->deck_info(r->deck, &info) == 0 && info.loaded > 0 &&
           info.content_id == r->content_id && !strcmp(info.title, r->title) &&
           !strcmp(info.artist, r->artist);
}

static int wait_for_request_deck(const request_t *r)
{
    /* SDK fields are individually atomic, not a transactional snapshot. A short unknown
     * or mixed snapshot must not permanently mark an otherwise loaded track as failed. */
    for (int attempt = 0; attempt < 5 && request_current(r); attempt++) {
        if (request_matches_deck(r)) return 1;
        if (attempt < 4) {
            struct timespec wait = {0, 50000000};
            nanosleep(&wait, NULL);
        }
    }
    return 0;
}

/* Read one deck's analysis: the deck's own copy first (milliseconds), and the USB ANLZ
 * files only when the copy is not there, or for the cues the deck's loaded state does not
 * carry. The caller holds no lock: this runs on the resolver thread. */
static int fetch_analysis(const request_t *req, an_anlz_t *parsed, char *why, size_t why_n,
                          int *from_loaded)
{
    memset(parsed, 0, sizeof *parsed);
    if (why && why_n) why[0] = 0;
    *from_loaded = 0;
    int rc = -1;
    if (loaded_supported) {
        /* The deck builds its gui::LoadState objects with the deck view. On the very first
         * load after a restart the worker can get here a moment before they exist; give the
         * deck a second rather than falling back to the USB for the whole track. */
        for (int attempt = 0; attempt < 5; attempt++) {
            if (!request_current(req)) return -1;
            rc = an_loaded_read(req->deck, parsed, why, why_n);
            if (rc == AN_LOADED_RETRY) return rc;
            if (rc == 0 && (parsed->has_phrases || attempt == 4)) break;
            if (attempt == 4) break;
            an_anlz_free(parsed); /* The grid can arrive before the phrase provider. */
            struct timespec wait = { 0, 250000000 };
            nanosleep(&wait, NULL);
        }
    }
    if (rc == 0) *from_loaded = 1;

    int need_cues = req->want_cues || (rc == 0 && !parsed->has_phrases);
    if (rc != 0 || need_cues) {
        if (!request_current(req)) return -1;
        an_anlz_t usb;
        char why2[160];
        memset(&usb, 0, sizeof usb);
        why2[0] = 0;
        int r2 = an_resolve_analysis(C, MOD_ID, req->content_id, req->title, req->artist, &usb,
                                     why2, sizeof why2);
        if (r2 == 0 && usb.valid) {
            if (rc == 0) { /* keep the loaded phrase map; take the cues */
                if (!parsed->cues && usb.cues) {
                    parsed->cues = usb.cues;
                    parsed->ncues = usb.ncues;
                    usb.cues = NULL;
                    usb.ncues = 0;
                }
            } else {
                *parsed = usb;
                memset(&usb, 0, sizeof usb);
                rc = 0;
                *from_loaded = 0;
            }
        } else {
            if (req->want_cues) rc = -1; /* Required-but-unread cues are a read failure. */
            if (why && why_n && (need_cues || !why[0]))
                snprintf(why, why_n, "%s", why2[0] ? why2 : "no analysis for this track");
        }
        an_anlz_free(&usb);
    }
    return rc;
}

static void process_request(const request_t *req)
{
    while (request_current(req) && __atomic_load_n(&worker_clock_ms, __ATOMIC_ACQUIRE) < req->not_before_ms) {
        struct timespec settle = {0, 25000000};
        nanosleep(&settle, NULL);
    }
    if (!request_current(req)) return;
    an_anlz_t parsed, retired;
    memset(&parsed, 0, sizeof parsed);
    memset(&retired, 0, sizeof retired);
    char why[160] = "the track changed before its analysis could be read";
    int from_loaded = 0;
    int rc = -1;
    if (wait_for_request_deck(req))
        rc = fetch_analysis(req, &parsed, why, sizeof why, &from_loaded);
    /* deck_info's fields and the analysis provider can change independently. Check the
     * requested metadata again, and the load sequence under the publication lock. */
    if (rc == 0 && !wait_for_request_deck(req)) {
        rc = -1;
        snprintf(why, sizeof why, "the track changed while its analysis was read");
    }
    if (rc == AN_LOADED_RETRY && request_current(req)) {
        request_t retry = *req;
        if (retry.retries < 3) retry.retries++;
        retry.not_before_ms = __atomic_load_n(&worker_clock_ms, __ATOMIC_ACQUIRE) + (UINT64_C(250) << retry.retries);
        pthread_mutex_lock(&queue_mu);
        /* Never replace a more recent load/refresh waiting for this deck. A stale
         * deferred job is harmless: process_request rechecks the publication sequence. */
        if (!queued[req->deck] || queue[req->deck].seq <= retry.seq) {
            queue[req->deck] = retry;
            queued[req->deck] = 1;
        }
        pthread_mutex_unlock(&queue_mu);
        an_anlz_free(&parsed);
        return;
    }
    int published = 0;
    pthread_mutex_lock(&anlz_mu);
    if (req->seq == latest_seq[req->deck] && status[req->deck] == ST_PENDING) {
        if (rc == 0 && parsed.valid) {
            published = 1;
            retired = deck_analysis[req->deck];
            deck_analysis[req->deck] = parsed;
            memset(&parsed, 0, sizeof parsed);
            status[req->deck] = ST_OK;
            fail_why[req->deck][0] = 0;
        } else {
            status[req->deck] = ST_FAILED;
            snprintf(fail_why[req->deck], sizeof fail_why[req->deck], "%s", why[0] ? why : "no analysis");
        }
    }
    pthread_mutex_unlock(&anlz_mu);
    if (published && AZ_HAS(C, log)) {
        char message[160];
        snprintf(message, sizeof message, "Deck %d analysis ready: %d beats, %d phrases (%s)",
                 req->deck + 1, deck_analysis[req->deck].nbeats, deck_analysis[req->deck].nphrases,
                 from_loaded ? "loaded state" : "ANLZ");
        C->log(MOD_ID, "%s", message);
    }
    an_anlz_free(&retired);
    an_anlz_free(&parsed);
}

static void *worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        request_t req;
        if (take_request(&req)) {
            process_request(&req);
            continue;
        }

        /* No request: sleep briefly, then make one harmless core call. The core call is
         * what lets a restart park this thread instead of leaving it in the old lifetime. */
        struct timespec idle = { 0, 250000000 };
        nanosleep(&idle, NULL);
        if (C && C->deck_info) {
            az_deck_info tmp;
            memset(&tmp, 0, sizeof tmp);
            tmp.size = sizeof tmp;
            C->deck_info(0, &tmp);
        }
    }
    return NULL;
}

/* A separate worker keeps prepared loops fresh even while USB analysis is busy.
 * No native pointers escape loaded.c, and no /proc I/O runs on tick/paint. */
static void poll_prepared_loops(void)
{
    for(int d=0;d<4;d++) {
        az_deck_info before={.size=sizeof before},after={.size=sizeof after};
        if(C->deck_info(d,&before) || before.loaded<=0) continue;
        pthread_mutex_lock(&anlz_mu);
        uint64_t seq=latest_seq[d];
        pthread_mutex_unlock(&anlz_mu);
        an_loaded_loop_t value={0};
        if(an_loaded_loop_read(d,&value)) value.available=0;
        if(C->deck_info(d,&after) || after.loaded!=before.loaded ||
           after.content_id!=before.content_id || strcmp(after.title,before.title) ||
           strcmp(after.artist,before.artist)) continue;
        uint64_t now=mono_ms();
        pthread_mutex_lock(&anlz_mu);
        if(latest_seq[d]==seq) {
            prepared_loop[d].state=value;
            prepared_loop[d].seq=seq;
            prepared_loop[d].observed_ms=now;
        }
        pthread_mutex_unlock(&anlz_mu);
    }
}
static void *loop_worker(void *unused)
{
    (void)unused;
    for(;;) {
        poll_prepared_loops();
        struct timespec interval={0,125000000};
        nanosleep(&interval,NULL);
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------
 * runtime sampling: rate and loop detection
 */
/* Optional authoritative state. Old cores, and cores that leave loop.valid empty, use
 * observation below. A saved loop entry alone never proves that the deck is looping. */
static int native_loop_state(int d, const az_deck_info *info, uint32_t pos, uint64_t now)
{
    deck_rt_t *r = &rt[d];
    (void)pos;
    int was_native=r->native_loop;
    r->native_loop = 0;
    r->loop_pending = 0;
    if(prepared_reader_ok) {
        an_loaded_loop_t value;
        pthread_mutex_lock(&anlz_mu);
        uint64_t observed=prepared_loop[d].observed_ms;
        int fresh=prepared_loop[d].seq==latest_seq[d] && observed &&
                  observed<=now+10 && now<=observed+750;
        value=prepared_loop[d].state;
        pthread_mutex_unlock(&anlz_mu);
        if(fresh && value.available) {
            if(value.engaged) {
                r->in_ms=value.in_ms; r->out_ms=value.out_ms;
                r->len_ms=value.out_ms-value.in_ms;
                r->confirmed=r->native_loop=1;
                r->candidate=r->released=0;
            } else {
                r->released=r->confirmed && info->playing>0;
                r->confirmed=r->candidate=0;
            }
            return 1;
        }
        r->loop_pending=1;
        if(was_native) r->confirmed=r->candidate=r->released=0;
    }
#ifdef AZ_LOOP_DEFINED
    if (!AZ_HAS(C, deck_snapshot)) return 0;
    az_deck_snapshot snap;
    memset(&snap, 0, sizeof snap);
    snap.size = sizeof snap;
    unsigned need = AZ_LIVE_LOADED | AZ_LIVE_POSITION;
    if (C->deck_snapshot(d, &snap) != AZ_OK || snap.version != AZ_DATA_VERSION ||
        snap.deck != (unsigned)d || (snap.valid & need) != need || snap.loaded != 1 ||
        snap.loop.size < sizeof snap.loop || snap.loop.version != AZ_DATA_VERSION ||
        !snap.observed_ns || snap.observed_ns / 1000000u > now + 10 ||
        now > snap.observed_ns / 1000000u + 500 ||
        !isfinite(snap.position_s) || fabs(snap.position_s - info->pos_s) > .25 ||
        (snap.loop.valid & (AZ_LOOP_DEFINED | AZ_LOOP_ENGAGED)) != (AZ_LOOP_DEFINED | AZ_LOOP_ENGAGED)) return 0;
    if ((snap.valid & AZ_LIVE_TRACK) && info->content_id && snap.track.track_id != info->content_id) return 0;
    r->loop_pending=0;
    if (!snap.loop.engaged || !snap.loop.defined) {
        if (r->confirmed && info->playing > 0) r->released = 1;
        r->confirmed = r->candidate = 0;
        return 1;
    }
    double in = snap.loop.in_s * 1000.0, out = snap.loop.out_s * 1000.0;
    if (!isfinite(in) || !isfinite(out) || in < 0 || out > UINT32_MAX - 1000.0 ||
        out - in < 125 || out - in > 120000) {
        r->confirmed = r->candidate = r->released = 0;
        return 1;
    }
    r->in_ms = (uint32_t)llround(in);
    r->out_ms = (uint32_t)llround(out);
    r->len_ms = r->out_ms - r->in_ms;
    r->confirmed = r->native_loop = 1;
    r->candidate = r->released = 0;
    return 1;
#else
    (void)info; (void)pos; (void)now;
    return 0;
#endif
}

static void update_runtime(int d, const az_deck_info *info, uint64_t now)
{
    deck_rt_t *r = &rt[d];
    if (info->loaded <= 0 || !isfinite(info->pos_s) || info->pos_s < 0 || info->pos_s > 4000000) {
        reset_runtime(d);
        return;
    }
    uint32_t pos = (uint32_t)(info->pos_s * 1000.0 + 0.5);
    int native = native_loop_state(d, info, pos, now);
    if (info->playing <= 0) {
        r->have_last = r->prev_playing = 0;
        if (!native) r->confirmed = r->candidate = r->released = 0;
        return;
    }
    if (r->have_last && now > r->last_ms) {
        uint64_t dt = now - r->last_ms;
        int64_t dp = (int64_t)pos - r->last_pos;
        if (dt >= 4 && dt <= 250) {
            double rate = r->rate_ok ? r->rate : 1.0;
            double step = rate * dt;
            if (dp < -25 && !native) {
                /* gap alone omits all progress between the samples around a wrap. */
                uint32_t length = (uint32_t)llround(-dp + step);
                uint32_t tol = (uint32_t)fmax(30.0, step * .3);
                uint32_t low = pos > step ? pos - (uint32_t)ceil(step) : 0;
                int repeat = r->candidate && near(length, r->cand_len, tol) &&
                             now > r->last_wrap_ms && now - r->last_wrap_ms <= 400000;
                /* The time and source-position distance between two wrap observations
                 * independently measures one complete pass, even with sampling jitter. */
                double period = rate * (double)(now - r->last_wrap_ms) + r->wrap_pos - (double)pos;
                repeat = repeat && fabs(period - length) <= fmax(2 * step, length * .01);
                if (repeat && low <= r->in_high && pos >= r->in_low) {
                    if (low > r->in_low) r->in_low = low;
                    if (pos < r->in_high) r->in_high = pos;
                    r->len_ms = (r->cand_len + length + 1) / 2;
                    r->in_ms = r->in_low + (r->in_high - r->in_low) / 2;
                    r->out_ms = r->in_ms + r->len_ms;
                    r->confirmed = 1;
                    r->released = 0;
                } else {
                    r->confirmed = r->released = 0;
                    r->in_low = low; r->in_high = pos;
                }
                r->candidate = length >= 125 && length <= 120000;
                if (!r->candidate) r->confirmed = 0;
                r->cand_len = length;
                r->cand_in = pos; r->cand_out = r->last_pos;
                r->wrap_pos = pos; r->last_wrap_ms = now;
            } else if (dp >= 0 && dp <= 3 * (int64_t)dt + 25) {
                double observed = (double)dp / dt;
                if (observed >= .3 && observed <= 3) {
                    r->rate = r->rate_ok ? .75 * r->rate + .25 * observed : observed;
                    r->rate_ok = 1;
                }
                if (!native && (r->confirmed || r->candidate)) {
                    uint32_t out = r->confirmed ? r->out_ms : r->in_high + r->cand_len;
                    if (pos > out + (uint32_t)fmax(100, step)) {
                        r->released = r->confirmed;
                        r->confirmed = r->candidate = 0;
                    }
                }
            } else if (!native) {
                /* A forward seek, scratch or irregular movement invalidates old evidence. */
                r->confirmed = r->candidate = r->released = 0;
                r->rate_ok = 0;
            }
        } else if (!native) {
            r->confirmed = r->candidate = r->released = 0;
        }
    }
    r->last_pos = pos; r->last_ms = now;
    r->have_last = r->prev_playing = 1;
}

/* Prefer a unique grid beat inside the observed wrap bracket. This does not quantize an
 * arbitrary manual loop to the nearest beat: if no beat is in the bracket, keep the
 * bounded midpoint estimate. Binary search keeps the UI-thread work bounded. */
static uint32_t observed_loop_in(const deck_rt_t *r, const an_anlz_t *analysis)
{
    if (r->native_loop || !analysis || !analysis->beats || analysis->nbeats < 1) return r->in_ms;
    int lo = 0, hi = analysis->nbeats;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (analysis->beats[mid].time_ms < r->in_low) lo = mid + 1;
        else hi = mid;
    }
    if (lo < analysis->nbeats && analysis->beats[lo].time_ms <= r->in_high &&
        (lo + 1 >= analysis->nbeats || analysis->beats[lo + 1].time_ms > r->in_high))
        return analysis->beats[lo].time_ms;
    return r->in_ms;
}

/* ------------------------------------------------------------------------------------------
 * the plan
 */
static void fallback_plan_message(an_plan_t *p, int a, int b)
{
    memset(p, 0, sizeof *p);
    p->valid = 1;
    int sa = status[a], sb = status[b];
    if (sa == ST_PENDING || sb == ST_PENDING) {
        p->notice = AN_NOTICE_READING;
        p->state = AN_ST_NO_ANALYSIS;
        p->accent = 0;
        snprintf(p->line1, sizeof p->line1, "READING THE LOADED ANALYSIS");
        snprintf(p->line2, sizeof p->line2, "READING FRESH ANALYSIS FOR THE CURRENT LOAD");
        return;
    }
    p->state = AN_ST_NO_ANALYSIS;
    p->accent = 2;
    if (sa == ST_FAILED) {
        p->notice = AN_NOTICE_ANALYSIS_A;
        snprintf(p->line1, sizeof p->line1, "TRACK A NEEDS ANALYSIS");
    } else if (sb == ST_FAILED) {
        p->notice = AN_NOTICE_ANALYSIS_B;
        snprintf(p->line1, sizeof p->line1, "TRACK B NEEDS ANALYSIS");
    } else
        snprintf(p->line1, sizeof p->line1, "WAITING FOR TWO DECKS");
    if (sa == ST_FAILED && fail_why[a][0])
        snprintf(p->line2, sizeof p->line2, "A: %.70s", fail_why[a]);
    else if (sb == ST_FAILED && fail_why[b][0])
        snprintf(p->line2, sizeof p->line2, "B: %.70s", fail_why[b]);
    else
        snprintf(p->line2, sizeof p->line2, "ANALYSE IT IN REKORDBOX, THEN RELOAD IT");
}

static void compute_plan(uint64_t now, const az_deck_info inf[4], int a, int b,
                         const an_target_opts_t *opts)
{
    an_plan_t p;
    memset(&p, 0, sizeof p);

    an_deck_t A, B;
    memset(&A, 0, sizeof A);
    memset(&B, 0, sizeof B);
    A.loaded = 1;
    A.playing = 1;
    A.pos_ms = (uint32_t)(inf[a].pos_s * 1000.0 + 0.5);
    A.bpm = inf[a].bpm;
    B.loaded = 1;
    B.playing = inf[b].playing > 0;
    B.pos_ms = (uint32_t)(inf[b].pos_s * 1000.0 + 0.5);
    B.bpm = inf[b].bpm;

    pthread_mutex_lock(&anlz_mu);
    an_anlz_t *aa = slot_anlz(a);
    an_anlz_t *ba = slot_anlz(b);

    /* The playback rate of a deck is current BPM / the analysed BPM at that point in the
     * track: the pitch factor, which is also how fast the audio playhead advances. The
     * measured rate is only a fallback when the analysis has no grid. */
    if (aa && A.bpm >= 20.0 && A.bpm <= 400.0) {
        double native = an_anlz_bpm_at(aa, A.pos_ms);
        if (native > 20.0 && native < 400.0) A.rate = A.bpm / native;
        else A.rate = (rt[a].rate_ok && rt[a].rate > 0.3) ? rt[a].rate : 1.0;
    } else {
        A.rate = (rt[a].rate_ok && rt[a].rate > 0.3) ? rt[a].rate : 1.0;
    }
    if (ba && B.bpm >= 20.0 && B.bpm <= 400.0) {
        double native = an_anlz_bpm_at(ba, B.pos_ms);
        if (native > 20.0 && native < 400.0) B.rate = B.bpm / native;
        else B.rate = (rt[b].rate_ok && rt[b].rate > 0.3) ? rt[b].rate : 1.0;
    } else {
        B.rate = (rt[b].rate_ok && rt[b].rate > 0.3) ? rt[b].rate : 1.0;
    }

    incoming_target_t *bt = &b_targets[b];
    if (bt->gen != deck_gen[b]) memset(bt, 0, sizeof *bt);
    bt->gen = deck_gen[b];
    if (ba && !bt->suggested_valid)
        bt->suggested_valid = an_suggest_phrase(ba, B.pos_ms, &bt->suggested) == 0;
    if (ba && !bt->valid)
        bt->valid = an_find_drop(ba, B.pos_ms, &bt->target) == 0;
    if (!aa || !aa->valid || ((!ba || !ba->valid) && !bt->valid)) {
        fallback_plan_message(&p, a, b);
        pthread_mutex_unlock(&anlz_mu);
        plan = p;
        return;
    }
    int have_latched = latched_ms && latched_gen == deck_gen[a] &&
                       (int64_t)latched_ms > (int64_t)A.pos_ms + 150;

    /* A track can be loaded without a phrase map (a grid-only analysis, or one the deck
     * could not parse). Then the cue points are the guide's only usable markers; if there
     * are none either, say so instead of guessing. */
    an_target_opts_t use = *opts;
    int hotcue_fallback = 0;
    if (use.rule == AN_RULE_PHRASE && !aa->has_phrases) {
        use.rule = AN_RULE_HOTCUE;
        hotcue_fallback = 1;
    }

    an_loop_t loop;
    memset(&loop, 0, sizeof loop);
    an_cand_t drop;
    int have_drop = bt->valid;
    drop = bt->target;
    if (!have_drop && ba && !ba->has_phrases && ba->ncues == 0) {
        p.notice = AN_NOTICE_ANALYSIS_B;
        p.valid = 1;
        p.state = AN_ST_NO_ANALYSIS;
        p.accent = 2;
        snprintf(p.line1, sizeof p.line1, "TRACK B NEEDS ANALYSIS");
        snprintf(p.line2, sizeof p.line2, "NO PHRASES OR HOT CUES - ANALYSE IT IN REKORDBOX");
        pthread_mutex_unlock(&anlz_mu);
        plan = p;
        return;
    }
    if (!have_latched && use.rule == AN_RULE_HOTCUE && aa->ncues == 0) {
        p.valid = 1;
        p.state = AN_ST_NO_ANALYSIS;
        p.accent = 2;
        if (hotcue_fallback) {
            p.notice = AN_NOTICE_ANALYSIS_A;
            snprintf(p.line1, sizeof p.line1, "TRACK A NEEDS ANALYSIS");
            snprintf(p.line2, sizeof p.line2, "NO PHRASES OR HOT CUES - ANALYSE IT IN REKORDBOX");
        } else {
            p.notice = AN_NOTICE_NO_CUES;
            snprintf(p.line1, sizeof p.line1, "TRACK A HAS NO HOT CUES");
            snprintf(p.line2, sizeof p.line2, "PICK A PHRASE TARGET OR ADD CUES IN REKORDBOX");
        }
        pthread_mutex_unlock(&anlz_mu);
        plan = p;
        return;
    }

    if (rt[b].released) loop.released = 1;
    else if (rt[b].confirmed && rt[b].len_ms >= 125) {
        loop.valid = 1;
        loop.in_ms = observed_loop_in(&rt[b], ba);
        loop.len_ms = rt[b].len_ms;
        loop.out_ms = loop.in_ms + loop.len_ms;
    }

    an_cand_t target;
    int have_target = 0;
    if (have_latched) {
        memset(&target, 0, sizeof target);
        target.ms = latched_ms;
        snprintf(target.label, sizeof target.label, "%s", latched_label);
        have_target = 1;
    } else if (an_auto_target(aa, A.pos_ms, &use, &target) == 0) {
        have_target = 1;
    }

    if (!have_drop) {
        p.notice = AN_NOTICE_NO_DROP;
        p.valid = 1;
        p.state = AN_ST_NO_ANALYSIS;
        p.accent = 2;
        snprintf(p.line1, sizeof p.line1, "B HAS NO DROP AHEAD");
        snprintf(p.line2, sizeof p.line2, "CUE B BEFORE ITS CHORUS OR LOAD ANOTHER TRACK");
    } else if (!have_target) {
        p.valid = 1;
        p.state = AN_ST_NONE;
        p.accent = 0;
        snprintf(p.line1, sizeof p.line1, "NO TARGET AHEAD");
        snprintf(p.line2, sizeof p.line2, "%s",
                 use.rule == AN_RULE_HOTCUE ? "NO HOT CUE AHEAD - TAP TO PICK ONE"
                                            : "TAP TO PICK A PHRASE OR CUE");
    } else {
        an_plan_target(&A, aa, &B, ba, &loop, target.ms, target.label, &drop, now, &p);
        if (rt[b].loop_pending && !rt[b].confirmed) {
            p.notice=AN_NOTICE_LOOP_UNKNOWN; p.state=AN_ST_OFF;
            p.action=p.play_now=0; p.bars_action=0; p.accent=0;
            snprintf(p.line2,sizeof p.line2,"Waiting for fresh native loop state");
        }
        if (B.playing && rt[b].candidate && !rt[b].confirmed) {
            p.notice = AN_NOTICE_LOOP_DETECTING;
            p.state = AN_ST_OFF;
            p.bars_action = 0;
            p.play_now = 0;
            p.accent = 0;
            snprintf(p.line2, sizeof p.line2, "Confirming a repeated wrap; keep the loop playing");
        }
        if (p.valid && A.bpm > 0 && B.bpm > 0 && fabs(A.bpm - B.bpm) > 0.15 &&
            (p.state == AN_ST_SINGLE || p.state == AN_ST_START || p.state == AN_ST_WAIT ||
             p.state == AN_ST_OPEN)) {
            p.tempo_mismatch = 1;
            p.accent = 2;
            size_t n = strlen(p.line2);
            if (n < sizeof p.line2 - 8) strncat(p.line2, " · SYNC", sizeof p.line2 - n - 1);
        }
        if (!p.valid) {
            p.state = AN_ST_NONE;
            p.valid = 1;
            snprintf(p.line1, sizeof p.line1, "NO TARGET AHEAD");
            snprintf(p.line2, sizeof p.line2, "TAP TO PICK A PHRASE OR CUE");
        }
        if (hotcue_fallback && p.valid) {
            /* say where the target came from, when the line has room left */
            size_t n = strlen(p.line2);
            if (n < sizeof p.line2 - 16) strncat(p.line2, " · USING HOT CUES", sizeof p.line2 - n - 1);
        }
    }
    pthread_mutex_unlock(&anlz_mu);
    if (bt->valid) {
        p.b_target_valid = 1;
        p.b_target_ms = bt->target.ms;
        snprintf(p.b_target, sizeof p.b_target, "%s", bt->target.label);
    }
    plan = p;
}

/* ------------------------------------------------------------------------------------------
 * drawing
 */
static int text_w(const char *s, int px)
{
    if (!C || !s || !s[0]) return 0;
    int w = C->text_width(s, px);
    return w > 0 ? w : (int)strlen(s) * px * 3 / 5;
}

/* Fit s into maxw, cutting at a character and ending with "...". Never returns text wider
 * than maxw, so a long state or a long title cannot run into the next element. */
static void fit_text(char *out, size_t n, const char *s, int px, int maxw)
{
    if (!n) return;
    out[0] = 0;
    if (!s || maxw <= px) return;
    if (text_w(s, px) <= maxw) {
        snprintf(out, n, "%s", s);
        return;
    }
    size_t len = strlen(s);
    if (len > n - 1) len = n - 1;
    while (len > 0) {
        char tmp[160];
        snprintf(tmp, sizeof tmp, "%.*s...", (int)len, s);
        if (text_w(tmp, px) <= maxw) {
            snprintf(out, n, "%s", tmp);
            return;
        }
        len--;
    }
    snprintf(out, n, "...");
}

static void draw_fit(az_draw *d, int x, int y, int w, const char *s, int px, uint32_t rgb)
{
    char buf[160];
    if (!C || w < px || !s || px < 8) return;
    fit_text(buf, sizeof buf, s, px, w);
    if (buf[0]) C->draw_text(d, x, y, buf, px, rgb);
}

static void draw_plan(az_draw *d, az_rect a)
{
    an_display_t m;
    an_display_model(&plan, cur_a, cur_b, &m);
    an_display_draw(C, d, a, cur_layout, &m);
    an_display_controls(a, cur_layout, target_controls);
    controls_ok = target_controls[0].w > 0;
}

static void draw_chip(az_draw *d, az_rect r, const char *label, int selected)
{
    uint32_t border = selected ? accent_rgb(4) : C->color(AZ_COL_TEXT_DIM);
    C->fill_rect(d, r.x, r.y, r.w, r.h, C->color(AZ_COL_BG));
    C->draw_rect(d, r.x, r.y, r.w, r.h, selected ? 2 : 1, border);
    int px = r.h >= 32 ? 13 : 11;
    if (r.w < 30 || r.h < 16) return;
    while (px > 9 && text_w(label, px) > r.w - 10) px--;
    int tw = text_w(label, px);
    int x = r.x + 5;
    if (tw < r.w - 10) x = r.x + (r.w - tw) / 2;
    int y = r.y + (r.h - px * 3 / 2) / 2;
    draw_fit(d, x, y, r.w - 10, label, px, C->color(AZ_COL_TEXT));
}

static uint32_t selected_ms(void)
{
    return sel_b && cur_b >= 0 ? b_targets[cur_b].target.ms : latched_ms;
}

static void draw_selector_list(az_draw *d, az_rect a)
{
    /* Invalidate old hit rectangles before every layout, including tiny fallbacks. */
    memset(chip_r, 0, sizeof chip_r);
    memset(&arrow_l_r, 0, sizeof arrow_l_r);
    memset(&arrow_r_r, 0, sizeof arrow_r_r);
    memset(&close_r, 0, sizeof close_r);
    layout_ok = 0;
    C->fill_rect(d, a.x, a.y, a.w, a.h, C->color(AZ_COL_BG));
    C->draw_rect(d, a.x, a.y, a.w, a.h, 1, C->color(AZ_COL_TEXT_DIM));
    if (cur_layout == ANCHOR_LAYOUT_SIDEBAR && a.w >= 140 && a.h >= 260) {
        sel_per = 4;
        int pages = (sel_n + sel_per - 1) / sel_per;
        if (pages < 1) pages = 1;
        if (sel_page >= pages) sel_page = pages - 1;
        if (sel_page < 0) sel_page = 0;
        char hint[64];
        snprintf(hint, sizeof hint, "%c: DECK %d  %d/%d", sel_b ? 'B' : 'A', sel_deck + 1, sel_page + 1, pages);
        draw_fit(d, a.x + 8, a.y + 8, a.w - 16, hint, 11, C->color(AZ_COL_TEXT));
        for (int i = 0; i < sel_per; i++) {
            int idx = sel_page * sel_per + i;
            if (idx >= sel_n) break;
            az_rect r = {a.x + 8, a.y + 38 + i * 49, a.w - 16, 44};
            chip_r[i] = r;
            int selected = sel_cand[idx].ms == selected_ms();
            C->draw_rect(d, r.x, r.y, r.w, r.h, selected ? 2 : 1,
                         C->color(selected ? AZ_COL_HI : AZ_COL_TEXT_DIM));
            draw_fit(d, r.x + 5, r.y + 4, r.w - 10, sel_cand[idx].label, 11, C->color(AZ_COL_TEXT));
            unsigned sec = sel_cand[idx].ms / 1000u;
            snprintf(hint, sizeof hint, "%u:%02u", sec / 60u, sec % 60u);
            draw_fit(d, r.x + 5, r.y + 23, r.w - 10, hint, 10, C->color(AZ_COL_TEXT_DIM));
        }
        if (!sel_n) draw_fit(d, a.x + 8, a.y + 80, a.w - 16,
                             sel_wait_cues ? "READING HOT CUES" : "NO TARGETS", 12, C->color(AZ_COL_TEXT));
        snprintf(hint, sizeof hint, "%d loop pass%s", plan.loops, plan.loops == 1 ? "" : "es");
        draw_fit(d, a.x + 8, a.y + a.h - 62, a.w - 16, hint, 10, C->color(AZ_COL_TEXT_DIM));
        int bw = (a.w - 24) / 3;
        int by = a.y + a.h - 40;
        arrow_l_r = (az_rect){a.x + 8, by, bw, 32};
        arrow_r_r = (az_rect){a.x + 12 + bw, by, bw, 32};
        close_r = (az_rect){a.x + 16 + 2 * bw, by, bw, 32};
        draw_chip(d, arrow_l_r, "<", 0);
        draw_chip(d, arrow_r_r, ">", 0);
        draw_chip(d, close_r, "X", 0);
        layout_ok = 1;
        return;
    }
    if (a.w < 260 || a.h < (sel_b ? 20 : 42)) {
        draw_fit(d, a.x + 8, a.y + (a.h - 12 * 3 / 2) / 2, a.w - 16, "TAP TARGET", 12,
                 C->color(AZ_COL_TEXT_DIM));
        layout_ok = 1;
        return;
    }

    int pad = 6;
    int hint_px = 9;
    int hint_h = sel_b ? 0 : 12;

    int arrow_px = 16;
    int close_w = text_w("X", arrow_px) + 16;
    int arrow_w = text_w("<", arrow_px) + 14;
    int gap = 4;
    int chips_x = a.x + pad + arrow_w;
    int chips_w = a.w - 2 * pad - close_w - 2 * arrow_w;
    int cy = a.y + hint_h + 2;
    int ch = a.y + a.h - cy - 2;
    int max_chip_h = a.h >= 90 ? 44 : 34;
    if (ch > max_chip_h) ch = max_chip_h;
    if (ch < 12) ch = 12;

    arrow_l_r = (az_rect){ a.x + pad, cy, arrow_w - 2, ch };
    arrow_r_r = (az_rect){ a.x + a.w - pad - close_w - arrow_w, cy, arrow_w - 2, ch };
    close_r = (az_rect){ a.x + a.w - pad - close_w, cy, close_w, ch };

    /* How many chips fit is measured, not fixed: a long label ("BRIDGE END · 12:34")
     * shrinks the page, so no chip text is ever drawn over its neighbour. */
    int widest = 30;
    for (int i = 0; i < sel_n; i++) {
        char disp[64];
        unsigned sec = sel_cand[i].ms / 1000u;
        snprintf(disp, sizeof disp, "%s · %u:%02u", sel_cand[i].label, sec / 60u, sec % 60u);
        int w = text_w(disp, 11) + 12;
        if (w > widest) widest = w;
    }
    int per = chips_w / (widest + gap);
    if (per > 4) per = 4;
    if (per < 1) per = 1;
    sel_per = per;
    int pages = (sel_n + per - 1) / per;
    if (pages < 1) pages = 1;
    if (sel_page >= pages) sel_page = pages - 1;
    if (sel_page < 0) sel_page = 0;
    if (!sel_b) {
        char hint[96];
        if (a.w >= 620)
            snprintf(hint, sizeof hint, "TAP A TARGET  ·  < > PAGES  ·  X CLOSE  ·  %d/%d",
                     sel_page + 1, pages);
        else if (a.w >= 380)
            snprintf(hint, sizeof hint, "TAP A TARGET · %d/%d", sel_page + 1, pages);
        else
            snprintf(hint, sizeof hint, "TARGETS %d/%d", sel_page + 1, pages);
        draw_fit(d, a.x + pad, a.y + 1, a.w - 2 * pad, hint, hint_px,
                 C->color(AZ_COL_TEXT_DIM));
    }
    int cw = (chips_w - (per - 1) * gap) / per;
    if (cw < 20) cw = 20;

    for (int i = 0; i < per; i++) {
        int idx = sel_page * per + i;
        az_rect r = { chips_x + i * (cw + gap), cy, cw, ch };
        chip_r[i] = r;
        if (idx < sel_n) {
            char disp[64];
            unsigned sec = sel_cand[idx].ms / 1000u;
            snprintf(disp, sizeof disp, "%s · %u:%02u", sel_cand[idx].label, sec / 60u, sec % 60u);
            draw_chip(d, r, disp, sel_cand[idx].ms == selected_ms());
        } else {
            draw_chip(d, r, "", 0);
        }
    }
    C->draw_text(d, arrow_l_r.x + 4, cy + (ch - arrow_px * 3 / 2) / 2, "<", arrow_px,
                 C->color(AZ_COL_TEXT_DIM));
    C->draw_text(d, arrow_r_r.x + 4, cy + (ch - arrow_px * 3 / 2) / 2, ">", arrow_px,
                 C->color(AZ_COL_TEXT_DIM));
    C->draw_text(d, close_r.x + 5, cy + (ch - arrow_px * 3 / 2) / 2, "X", arrow_px,
                 C->color(AZ_COL_TEXT_DIM));

    if (sel_n == 0)
        draw_fit(d, chips_x, cy + (ch - 12 * 3 / 2) / 2, chips_w,
                 sel_wait_cues ? "READING HOT CUES" : sel_b ? "NO MARKERS - USE SET HERE" : "NO TARGETS AHEAD - ANALYSE TRACK A", 12, C->color(AZ_COL_TEXT_DIM));
    if (a.h >= 90) {
        char detail[128];
        snprintf(detail, sizeof detail, "%s | %d loop pass%s", plan.line2, plan.loops,
                 plan.loops == 1 ? "" : "es");
        draw_fit(d, a.x + pad, a.y + a.h - 22, a.w - 2 * pad, detail, 11, C->color(AZ_COL_TEXT_DIM));
    }
    layout_ok = 1;
}

static void draw_selector(az_draw *d, az_rect a)
{
    memset(kind_r, 0, sizeof kind_r);
    controls_ok = 0;
    if (!sel_b || a.w < 140 || a.h < 50) { draw_selector_list(d, a); return; }
    C->fill_rect(d, a.x, a.y, a.w, a.h, C->color(AZ_COL_BG));
    int width = cur_layout == ANCHOR_LAYOUT_SIDEBAR ? (a.w - 12) / 3 : 108;
    int height = a.h >= 100 ? 32 : 24;
    const char *names[] = {"PHRASE", "HOT CUE", "SET HERE"};
    for (int i = 0; i < 3; i++) {
        kind_r[i] = (az_rect){a.x + 4 + i * (width + 2), a.y + 2, width, height};
        draw_chip(d, kind_r[i], names[i], i == sel_kind);
    }
    if (a.w > 420) draw_fit(d, a.x + 340, a.y + 4, a.w - 348,
                           "B TARGET / DECK DESTINATION", 11, C->color(AZ_COL_TEXT_DIM));
    a.y += height + 4; a.h -= height + 4;
    draw_selector_list(d, a);
}

static void paint(az_draw *d, az_rect a, void *user)
{
    (void)d;
    (void)user;
    if (!C || a.w < 80 || a.h < 24) return;
    if (sel_open)
        draw_selector(d, a);
    else
        draw_plan(d, a);
}

static az_rect sidebar_tab_area(void)
{
    az_rect a = an_sidebar_tab_rect(drawer_width > 0);
    if (drawer_width > 0) a.x = 1280 - drawer_width - a.w - AN_SIDEBAR_HANDLE_GAP;
    return a;
}

static void tab_paint(az_draw *d, az_rect a, void *user)
{
    (void)user;
    an_sidebar_tab_draw(C, d, a, drawer_width > 0);
}

static int tab_input(const az_event *ev, void *user)
{
    (void)user;
    az_rect r = sidebar_tab_area();
    if (!C || !ev || ev->kind != AZ_EV_TAP || ev->x < r.x || ev->y < r.y ||
        ev->x >= r.x + r.w || ev->y >= r.y + r.h) return 0;
    settings_set(SET_SHOW, !settings_get(SET_SHOW));
    sel_open = layout_ok = 0;
    if (sidebar_tab) C->surface_redraw(sidebar_tab);
    return 1;
}

static int animate_drawer(uint64_t now, int open)
{
    az_rect full = an_display_rect(ANCHOR_LAYOUT_SIDEBAR);
    int target = open ? full.w : 0;
    if (target != drawer_target) {
        drawer_from = drawer_width; drawer_target = target; drawer_since = now;
    }
    double t = now >= drawer_since ? (double)(now - drawer_since) / 180.0 : 1.0;
    if (t > 1) t = 1;
    double eased = 1 - (1 - t) * (1 - t);
    int width = (int)llround(drawer_from + (drawer_target - drawer_from) * eased);
    int changed = width != drawer_width;
    drawer_width = width;
    if (changed) {
        layout_ok = controls_ok = 0;
        if (width > 0) C->surface_place(strip, (az_rect){full.x + full.w - width, full.y, width, full.h});
    }
    return changed;
}

/* ------------------------------------------------------------------------------------------
 * input
 */
static int in_rect(const az_rect *r, int x, int y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static void open_selector(void)
{
    sel_b = sel_wait_cues = 0;
    controls_ok = 0;
    sel_open = 1;
    sel_page = 0;
    sel_n = 0;
    layout_ok = 0;
    sel_deck = cur_a;
    sel_gen = cur_a >= 0 ? deck_gen[cur_a] : -1;
    if (cur_a >= 0 && C->deck_info) {
        az_deck_info info;
        memset(&info, 0, sizeof info);
        info.size = sizeof info;
        if (C->deck_info(cur_a, &info) == 0) {
            uint32_t pos = info.pos_s > 0.0 ? (uint32_t)(info.pos_s * 1000.0 + 0.5) : 0;
            pthread_mutex_lock(&anlz_mu);
            an_anlz_t *aa = slot_anlz(cur_a);
            if (aa) sel_n = an_candidates(aa, pos, &cur_opts, sel_cand, MAX_CAND);
            pthread_mutex_unlock(&anlz_mu);
        }
    }
    if (strip) C->surface_redraw(strip);
}

static void open_b_selector(int kind)
{
    sel_b = sel_open = 1;
    sel_kind = kind;
    sel_page = sel_n = layout_ok = controls_ok = sel_wait_cues = 0;
    sel_deck = cur_b;
    sel_gen = cur_b >= 0 ? deck_gen[cur_b] : -1;
    if (cur_b < 0) return;
    az_deck_info info = {.size = sizeof info};
    if (!AZ_HAS(C, deck_info) || C->deck_info(cur_b, &info) != 0 || info.loaded <= 0) return;
    pthread_mutex_lock(&anlz_mu);
    sel_n = an_b_candidates(slot_anlz(cur_b), kind == 1, sel_cand, MAX_CAND);
    pthread_mutex_unlock(&anlz_mu);
    if (kind == 1 && !cues_requested[cur_b]) {
        enqueue_request(cur_b, &info, 1);
        sel_wait_cues = 1;
    }
    if (strip) C->surface_redraw(strip);
}

static void set_b_target(const an_cand_t *target)
{
    if (cur_b < 0 || sel_deck != cur_b || sel_gen != deck_gen[cur_b]) return;
    incoming_target_t *bt = &b_targets[cur_b];
    bt->target = *target; bt->valid = 1; bt->gen = deck_gen[cur_b];
    rt[cur_b].released = 0;
    sel_open = layout_ok = controls_ok = 0;
    shown_display_valid = 0;
    if (strip) C->surface_redraw(strip);
}

static void set_b_here(void)
{
    az_deck_info info = {.size = sizeof info};
    if (cur_b < 0 || sel_deck != cur_b || sel_gen != deck_gen[cur_b] ||
        !AZ_HAS(C, deck_info) || C->deck_info(cur_b, &info) != 0 || info.loaded <= 0 ||
        !isfinite(info.pos_s) || info.pos_s < 0 || info.pos_s > 4000000) return;
    if (last_loaded[cur_b] && (info.content_id != last_cid[cur_b] ||
        strcmp(info.title, last_title[cur_b]) || strcmp(info.artist, last_artist[cur_b]))) return;
    an_cand_t target = {.ms = (uint32_t)llround(info.pos_s * 1000)};
    snprintf(target.label, sizeof target.label, "MARKED POINT");
    set_b_target(&target);
}

static void latch_candidate(int idx)
{
    if (sel_b) {
        if (idx >= 0 && idx < sel_n) set_b_target(&sel_cand[idx]);
        return;
    }
    if (cur_a < 0 || sel_deck != cur_a || sel_gen != deck_gen[cur_a] ||
        idx < 0 || idx >= sel_n) return;
    latched_ms = sel_cand[idx].ms;
    snprintf(latched_label, sizeof latched_label, "%s", sel_cand[idx].label);
    latched_gen = deck_gen[cur_a];
    if (cur_b >= 0) rt[cur_b].released = 0;
    sel_open = 0;
    if (strip) C->surface_redraw(strip);
}

static int input(const az_event *ev, void *user)
{
    (void)user;
    if (!C || !ev) return 0;
    if (cur_layout == ANCHOR_LAYOUT_SIDEBAR &&
        (drawer_width != an_display_rect(cur_layout).w || !settings_get(SET_SHOW))) return 0;
    if (ev->kind == AZ_EV_BACK) {
        if (sel_open) {
            sel_open = 0;
            if (strip) C->surface_redraw(strip);
            return 1;
        }
        return 0;
    }
    if (ev->kind != AZ_EV_TAP) return 0;
    if (!sel_open) {
        if (cur_a < 0 || cur_b < 0 || !plan.valid) return 1;
        if (controls_ok && in_rect(&target_controls[1], ev->x, ev->y)) {
            open_b_selector(0); return 1;
        }
        open_selector();
        return 1;
    }
    int x = ev->x, y = ev->y;
    if (!layout_ok) {
        sel_open = 0;
        return 1;
    }
    if (sel_deck != (sel_b ? cur_b : cur_a) || sel_deck < 0 || sel_gen != deck_gen[sel_deck]) {
        sel_open = layout_ok = controls_ok = 0; return 1;
    }
    if (sel_b) {
        for (int i = 0; i < 3; i++) if (in_rect(&kind_r[i], x, y)) {
            if (i == 2) set_b_here(); else open_b_selector(i);
            return 1;
        }
    }
    for (int i = 0; i < 4 && i < sel_per; i++) {
        if (in_rect(&chip_r[i], x, y)) {
            latch_candidate(sel_page * sel_per + i);
            return 1;
        }
    }
    if (in_rect(&arrow_l_r, x, y)) {
        if (sel_page > 0) sel_page--;
        if (strip) C->surface_redraw(strip);
        return 1;
    }
    if (in_rect(&arrow_r_r, x, y)) {
        int pages = (sel_n + sel_per - 1) / sel_per;
        if (sel_page + 1 < pages) sel_page++;
        if (strip) C->surface_redraw(strip);
        return 1;
    }
    if (in_rect(&close_r, x, y)) {
        sel_open = 0;
        if (strip) C->surface_redraw(strip);
        return 1;
    }
    sel_open = 0;
    if (strip) C->surface_redraw(strip);
    return 1;
}

/* ------------------------------------------------------------------------------------------
 * init and tick
 */
static int init(const az_core_v1 *core)
{
    C = core;
    if (!AZ_HAS(C, deck_info) || !AZ_HAS(C, view) || !AZ_HAS(C, surface_create) ||
        !AZ_HAS(C, surface_place) || !AZ_HAS(C, surface_show) || !AZ_HAS(C, surface_redraw) ||
        !AZ_HAS(C, draw_text) || !AZ_HAS(C, fill_rect) || !AZ_HAS(C, draw_rect) ||
        !AZ_HAS(C, text_width) || !AZ_HAS(C, color) || !AZ_HAS(C, on_deck_event)) {
        C->log(MOD_ID, "this core has no surfaces or deck state; the guide stays off");
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        status[i] = ST_NONE;
    }

    if (C->on_deck_event(MOD_ID, on_deck_event, NULL) != 0) {
        C->log(MOD_ID, "load notifications refused; analysis cannot be kept current");
        return 1;
    }

    settings_init(C);
    if (AZ_HAS(C, register_screen)) C->register_screen(MOD_ID, &settings_screen);
    if (AZ_HAS(C, register_row)) {
        /* One row that opens the page. The settings themselves are on the page because a
         * row's value buttons are the core's fixed-size squares, which cannot hold labels
         * such as "16-bar grid" or a list of eight phrase types without them running into
         * each other. On the page every widget is sized from its own text. */
        static const az_row_v1 setup_row = {
            .size = sizeof(az_row_v1),
            .key = "settings",
            .label = ANCHOR_MOD_NAME " settings",
            .kind = AZ_ROW_SCREEN,
            .screen = &settings_screen,
            .help = "How to use the guide, and every transition setting",
        };
        C->register_row(MOD_ID, &setup_row);
    }

    static const az_surface_v1 desc = {
        .size = sizeof(az_surface_v1),
        .layer = AZ_LAYER_OVER_EP147,
        .views = AZ_VIEW_BIT(AZ_VIEW_DECK),
        .on_paint = paint,
        .on_input = input,
    };
    strip = C->surface_create(MOD_ID, &desc);
    if (!strip) {
        C->log(MOD_ID, "surface refused");
        return 1;
    }
    cur_layout = selected_layout();
    C->surface_place(strip, an_display_rect(cur_layout));
    C->surface_show(strip, 0);
    static const az_surface_v1 tab_desc = {
        .size = sizeof(az_surface_v1), .layer = AZ_LAYER_OVER_EP147,
        .views = AZ_VIEW_BIT(AZ_VIEW_DECK), .on_paint = tab_paint, .on_input = tab_input,
    };
    sidebar_tab = C->surface_create(MOD_ID, &tab_desc);
    if (!sidebar_tab) { C->log(MOD_ID, "sidebar tab refused"); return 1; }
    C->surface_place(sidebar_tab, an_sidebar_tab_rect(0));
    C->surface_show(sidebar_tab, 0);
    drawer_width = drawer_from = drawer_target = settings_get(SET_SHOW) ? 190 : 0;
    drawer_since = 0;
    tab_shown = 0; tab_open = -1;

    /* The deck's own copy of the analysis: the phrase map and the grid it parsed when the
     * track loaded. Only the firmware whose object layout was verified is used; anything
     * else keeps the ANLZ file resolver so the guide still works. */
    loaded_supported = 0;
    if (AZ_HAS(C, fw_tag)) {
        const char *tag = C->fw_tag();
        if (tag && strcmp(tag, ANCHOR_LOADED_FW) == 0) {
            if (an_loaded_open(0) == 0)
                loaded_supported = 1;
            else
                C->log(MOD_ID, "cannot read /proc/self/mem; using the USB analysis");
        } else {
            C->log(MOD_ID, "firmware %s has no verified loaded-state layout; using the USB analysis",
                   tag ? tag : "(unknown)");
        }
    }

    worker_ok = 0;
    if (AZ_HAS(C, thread_start)) {
        int rc = C->thread_start(MOD_ID, "anchor-resolver", worker_main, NULL);
        if (rc == 0) {
            worker_ok = 1;
        } else {
            C->log(MOD_ID, "resolver thread refused (%d): analysis is unavailable", rc);
        }
    } else {
        C->log(MOD_ID, "the core has no thread service: analysis is unavailable");
    }
    prepared_reader_ok=0;
    if(loaded_supported && AZ_HAS(C,thread_start) &&
       C->thread_start(MOD_ID,"anchor-loop-state",loop_worker,NULL)==0) prepared_reader_ok=1;
    C->log(MOD_ID, "loaded; phrases from the deck's loaded analysis; prepared-loop reader %s",
           prepared_reader_ok ? "enabled" : "unavailable");
    return 0;
}

static int choose_decks(const az_deck_info inf[4], int deck_setting, int *pa, int *pb)
{
    *pa = *pb = -1;
    if (deck_setting == 1 || deck_setting == 2) {
        int b = deck_setting - 1;
        int a = 1 - b;
        if (inf[a].loaded > 0 && inf[b].loaded > 0 && inf[a].playing > 0) {
            *pa = a;
            *pb = b;
            return 0;
        }
        return -1;
    }
    int l0 = inf[0].loaded > 0, l1 = inf[1].loaded > 0;
    int p0 = inf[0].playing > 0, p1 = inf[1].playing > 0;
    if (!l0 || !l1) return -1;
    if (p0 != p1) {
        *pa = p0 ? 0 : 1;
        *pb = p0 ? 1 : 0;
        return 0;
    }
    if (p0 && p1) {
        /* Both playing: only the looping one is unambiguous as B. Otherwise the DJ has to
         * pick the incoming deck in the settings. */
        int loop0 = rt[0].confirmed && rt[0].len_ms >= 125;
        int loop1 = rt[1].confirmed && rt[1].len_ms >= 125;
        if (loop0 != loop1) {
            *pb = loop0 ? 0 : 1;
            *pa = 1 - *pb;
            return 0;
        }
        /* Keep roles established while one deck was paused. Otherwise a successful PLAY
         * (or loop release) hides the cue as soon as both heads move. Never infer new roles
         * from two already-playing tracks, and never carry them across a track change. */
        if (cur_a >= 0 && cur_a < 2 && cur_b == 1 - cur_a &&
            role_gen_a == deck_gen[cur_a] && role_gen_b == deck_gen[cur_b]) {
            *pa = cur_a;
            *pb = cur_b;
            return 0;
        }
    }
    return -1;
}

static void tick(void)
{
    if (!C || !strip) return;
    uint64_t now = mono_ms();
    __atomic_store_n(&worker_clock_ms, now, __ATOMIC_RELEASE);
    az_deck_info inf[4];
    for (int d = 0; d < 4; d++) {
        memset(&inf[d], 0, sizeof inf[d]);
        inf[d].size = sizeof inf[d];
        C->deck_info(d, &inf[d]);
        note_track_change(d, &inf[d]);
        update_runtime(d, &inf[d], now);
    }

    if (settings_take_refresh()) force_refresh = 1;
    int layout = selected_layout();
    if (layout != cur_layout) {
        cur_layout = layout;
        sel_open = 0;
        layout_ok = controls_ok = 0;
        C->surface_place(strip, an_display_rect(cur_layout));
        shown_display_valid = 0;
        last_redraw_ms = 0;
        if (strip_shown) C->surface_redraw(strip);
    }
    if (force_refresh) {
        force_refresh = 0;
        latched_ms = 0;
        latched_gen = -1;
        sel_open = layout_ok = 0;
        for (int d = 0; d < 4; d++)
            if (inf[d].loaded > 0) enqueue_request(d, &inf[d], cues_requested[d] || settings_get(SET_RULE) == 1);
    }

    an_target_opts_t opts;
    memset(&opts, 0, sizeof opts);
    opts.rule = settings_get(SET_RULE);
    if (opts.rule < 0 || opts.rule > AN_RULE_GRID32) opts.rule = AN_RULE_PHRASE;
    int pcat = settings_get(SET_PHRASE);
    if (pcat < 0 || pcat > AN_CAT_BRIDGE) pcat = 0;
    opts.phrase_cat = pcat == 0 ? -1 : pcat;
    opts.phrase_point = settings_get(SET_POINT) == AN_POINT_START ? AN_POINT_START : AN_POINT_END;
    opts.consolidate = settings_get(SET_CONSOL) != 0;
    opts.hotcue_any = settings_get(SET_HCANY) != 0;
    int hs = settings_get(SET_HCSLOT);
    if (hs < 0 || hs > 7) hs = 0;
    opts.hotcue_slot = hs + 1;
    int deck_setting = settings_get(SET_DECK);
    if (deck_setting < 0 || deck_setting > 2) deck_setting = 0;
    int show = settings_get(SET_SHOW);
    int want_cues = opts.rule == AN_RULE_HOTCUE;

    /* Switching to a hot-cue target needs the cues: the deck's loaded phrase map does not
     * carry them, so fetch them once for both decks. */
    if (want_cues) {
        for (int d = 0; d < 4; d++)
            if (inf[d].loaded > 0 && !cues_requested[d]) enqueue_request(d, &inf[d], 1);
    }
    if (memcmp(&opts, &cur_opts, sizeof opts) != 0 ||
        deck_setting != cur_deck_setting) {
        setting_changed = 1;
        cur_opts = opts;
        cur_deck_setting = deck_setting;
        latched_ms = 0;
        latched_gen = -1;
        sel_open = layout_ok = 0;
        for (int d = 0; d < 4; d++) rt[d].released = 0;
    }

    int a = -1, b = -1;
    int prev_a = cur_a;
    choose_decks(inf, deck_setting, &a, &b);
    cur_a = a;
    cur_b = b;
    role_gen_a = a >= 0 ? deck_gen[a] : -1;
    role_gen_b = b >= 0 ? deck_gen[b] : -1;

    if (a != prev_a) {
        latched_ms = 0;
        latched_gen = -1;
    }
    if (a >= 0 && latched_ms) {
        uint32_t apos = inf[a].pos_s > 0.0 ? (uint32_t)(inf[a].pos_s * 1000.0 + 0.5) : 0;
        if (latched_gen != deck_gen[a] || (int64_t)latched_ms <= (int64_t)apos + 150) {
            latched_ms = 0;
            latched_gen = -1;
        }
    }

    if (sel_open && (a < 0 || b < 0 || !plan.valid ||
        sel_deck != (sel_b ? b : a) || sel_gen != deck_gen[sel_deck])) {
        sel_open = layout_ok = controls_ok = 0;
    }
    if (sel_open && sel_b && sel_wait_cues) {
        pthread_mutex_lock(&anlz_mu);
        if (status[b] != ST_PENDING) {
            sel_wait_cues = 0;
            sel_n = an_b_candidates(slot_anlz(b), sel_kind == 1, sel_cand, MAX_CAND);
            layout_ok = 0;
            if (strip) C->surface_redraw(strip);
        }
        pthread_mutex_unlock(&anlz_mu);
    }

    int ndecks = AZ_HAS(C, deck_count) ? C->deck_count() : 2;
    int view = C->view();
    int deck_view = view == AZ_VIEW_DECK && ndecks == 2;
    int sidebar_mode = cur_layout == ANCHOR_LAYOUT_SIDEBAR;
    int want_tab = sidebar_mode && deck_view;
    if (sidebar_tab) {
        if (want_tab != tab_shown) { C->surface_show(sidebar_tab, want_tab); tab_shown = want_tab; }
    }
    int frame_changed = sidebar_mode && deck_view ? animate_drawer(now, show) : 0;
    if (sidebar_tab && want_tab) {
        C->surface_place(sidebar_tab, sidebar_tab_area());
        int expanded = drawer_width > 0;
        if (tab_open != expanded) { C->surface_redraw(sidebar_tab); tab_open = expanded; }
    }
    int showable = show && deck_view && (sidebar_mode || (a >= 0 && b >= 0));
    if (!showable && sel_open) sel_open = 0;
    if (showable) {
        if (a >= 0 && b >= 0) compute_plan(now, inf, a, b, &opts);
        else {
            memset(&plan, 0, sizeof plan);
            plan.valid = 1;
            if (inf[0].loaded <= 0 || inf[1].loaded <= 0) plan.state = AN_ST_NEED_TRACKS;
            else if (inf[0].playing <= 0 && inf[1].playing <= 0) plan.notice = AN_NOTICE_START_OUTGOING;
            else plan.accent = 2; /* two unrelated playing tracks: choose incoming deck */
        }
    }
    int body_visible = sidebar_mode ? deck_view && drawer_width > 0 : showable;

    if (body_visible && plan.valid) {
        an_display_t current_display;
        an_display_model(&plan, a, b, &current_display);
        int changed = !shown_display_valid ||
                      memcmp(&shown_display, &current_display, sizeof shown_display) != 0 ||
                      (sel_open && strcmp(shown_line2, plan.line2) != 0);
        int redrew = 0;
        if (!strip_shown) {
            C->surface_show(strip, 1);
            strip_shown = 1;
            C->surface_redraw(strip);
            last_redraw_ms = now;
            redrew = 1;
        } else if ((changed || frame_changed) && (frame_changed || plan.play_now || now - last_redraw_ms >= 180u)) {
            C->surface_redraw(strip);
            last_redraw_ms = now;
            redrew = 1;
        }
        if (changed && redrew) {
            snprintf(shown_line2, sizeof shown_line2, "%s", plan.line2);
            shown_display = current_display;
            shown_display_valid = 1;
        }
    } else if (strip_shown) {
        C->surface_show(strip, 0);
        strip_shown = 0;
        sel_open = 0;
        shown_line2[0] = 0;
        shown_display_valid = 0;
    }
    (void)setting_changed;
}

static const az_mod_v1 MOD = {
    AZ_MOD_ABI, sizeof(az_mod_v1), MOD_ID, TAGS, init, tick, NULL,
};

AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void)
{
    return &MOD;
}
