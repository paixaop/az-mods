/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "cues.h"
#include "azmod-data-helpers.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_BEATS 32768u
#define MAX_PHRASES 1024u
#define TOPIC(t) AZ_STATUS_TOPIC(AZ_STATUS_##t)
#define TOPICS                                                                 \
    (TOPIC(LOAD) | TOPIC(METADATA) | TOPIC(KEY) | TOPIC(TEMPO) | TOPIC(CUES) | \
     TOPIC(GRID) | TOPIC(PHRASES) | TOPIC(POSITION))
#define WRITE_FLAGS \
    (AZ_COMMAND_PERSIST | AZ_COMMAND_NATIVE_LOAD | AZ_COMMAND_NATIVE_CUE_IDS)
#define WRITE_COMMANDS                       \
    ((UINT64_C(1) << AZ_CMD_CUE_POINT_SET) | \
     (UINT64_C(1) << AZ_CMD_CUE_LOOP_SET))
/* All SDK calls and commands stay on UI. A worker only evaluates immutable,
 * bounded copies. Every callback context lives for the lifetime of the mod. */
static const az_core_v1 *C;
static const az_observation_v1 *O;
static const az_commands_v1 *Q;
static az_deck_subscription subscription;
static az_surface *deck_button, *deck_picker;
static int picker_open, picker_selected;
static uint64_t config_revision = 1, next_request, next_check;
static struct deck_state {
    uint64_t epoch, revision, finished_revision, finished_config;
    uint64_t grid_revision, phrase_revision, grid_provider, phrase_provider;
    uint32_t head_rate, plan_rate;
    int loaded;
    int cues_known, live_count, manual_requested, plan_manual, clearing,
        confirming, confirmed, deleting;
    uint64_t auto_done_epoch, cue_serial, confirm_revision, confirm_cue_serial;
    az_cue_entry live_cues[AZ_NATIVE_CUE_LIMIT];
    az_track_snapshot track;
    az_beat beats[MAX_BEATS];
    az_phrase phrases[MAX_PHRASES];
    cues_plan plan;
    int plan_ready, next, saved, retries;
    uint64_t plan_revision, plan_config, plan_epoch, native_load,
        native_revision, uncertain_load;
    uint64_t request;
    uint64_t view_deadline;
    int pending, terminal, obsolete, acknowledge;
    az_command_result result;
} decks[4];
static struct plan_job {
    atomic_int phase; /* 0 UI owns, 1 ready, 2 worker owns, 3 result ready */
    cues_config config;
    az_track_snapshot track;
    az_beat beats[MAX_BEATS];
    az_phrase phrases[MAX_PHRASES];
    az_native_cue_snapshot existing;
    uint32_t rate;
    uint64_t epoch, revision, config_revision;
    int manual;
    cues_plan result;
    int status;
} jobs[4];

static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0)
        return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static int same_track(const az_track_ref *a, const az_track_ref *b)
{
    return a->source_id && a->source_generation && a->library_id &&
           a->track_id && a->source_id == b->source_id &&
           a->source_generation == b->source_generation &&
           a->library_id == b->library_id && a->track_id == b->track_id;
}
static void status(int deck, const char *message)
{
    static char last[4][128];
    static uint64_t epoch[4];
    char text[128];
    snprintf(text, sizeof text, "Deck %d: %s", deck + 1, message);
    if (epoch[deck] == decks[deck].epoch && !strcmp(last[deck], text))
        return;
    epoch[deck] = decks[deck].epoch;
    snprintf(last[deck], sizeof last[deck], "%s", text);
    cues_settings_status(text);
    if (AZ_HAS(C, banner))
        C->banner(CUES_ID, text, 2400);
    if (deck_button && AZ_HAS(C, surface_redraw))
        C->surface_redraw(deck_button);
}
static int topic_valid(const az_status_topic_v1 *h, size_t size)
{
    return az_data_covers(h, size) && h->version == AZ_DATA_VERSION;
}
static int ready(const az_deck_status_event_v1 *e, const az_status_topic_v1 *h,
                 unsigned field, const az_status_field_info_v1 *info)
{
    return az_data_event_scalar_ready(e, h, field, info);
}
static int unit_ready(const az_status_topic_v1 *h, unsigned field,
                      const az_status_field_info_v1 *info)
{
    return (h->supported & h->valid & az_status_field_bit(field)) &&
           info->info.availability == AZ_AVAIL_READY && info->count == 1 &&
           info->scope == AZ_STATUS_UNIT && info->scope_id && info->provider_id;
}
static int text_ready(const az_deck_status_event_v1 *e,
                      const az_status_topic_v1 *h, unsigned field,
                      const az_status_field_info_v1 *info, int allow_unit)
{
    return (h->supported & h->valid & az_status_field_bit(field)) &&
           info->info.availability == AZ_AVAIL_READY && info->count &&
           info->scope_id &&
           ((info->scope == AZ_STATUS_LOAD_SCOPE &&
             info->scope_id == e->load_epoch) ||
            (allow_unit && info->scope == AZ_STATUS_UNIT && info->provider_id));
}
static void copy_text(char *out, size_t capacity, const char *text,
                      uint32_t count)
{
    size_t limit = count < capacity - 1 ? count : capacity - 1;
    size_t n = strnlen(text, limit);
    if (n == limit && n < count) {
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80)
            --n;
    }
    memcpy(out, text, n);
    out[n] = 0;
}
static int collection_ready(const az_deck_status_event_v1 *e,
                            const az_status_topic_v1 *h, unsigned field,
                            const az_status_field_info_v1 *info,
                            const void *rows, uint32_t maximum)
{
    uint64_t bit = az_status_field_bit(field);
    return (h->supported & h->valid & bit) &&
           info->scope == AZ_STATUS_LOAD_SCOPE &&
           info->scope_id == e->load_epoch && info->provider_id &&
           info->info.revision && info->info.availability == AZ_AVAIL_READY &&
           info->count <= maximum && (!info->count || rows);
}
static void invalidate_plan(struct deck_state *d)
{
    d->plan_ready = 0;
    if (d->pending) {
        d->obsolete = 1;
        if (AZ_COMMANDS_HAS(Q, command_cancel)) {
            az_command_result out = { .size = sizeof out };
            (void)Q->command_cancel(CUES_ID, d->request, &out);
        }
    }
}
static void config_changed(void)
{
    ++config_revision;
    for (int i = 0; i < 4; ++i) {
        invalidate_plan(&decks[i]);
        decks[i].retries = 0;
        decks[i].manual_requested = 0;
    }
    if (!cues_settings_config()->enabled)
        cues_settings_status("Automatic placement off");
}
static void receive_epoch(const az_deck_status_event_v1 *e, struct deck_state *d)
{
    if (d->epoch != e->load_epoch) {
        invalidate_plan(d);
        if (d->epoch != e->load_epoch)
            d->auto_done_epoch = 0;
        d->epoch = e->load_epoch;
        d->loaded = 0;
        d->cues_known = d->live_count = d->manual_requested = d->confirming = 0;
        d->head_rate = 0;
        d->grid_revision = d->phrase_revision = 0;
        d->track = (az_track_snapshot){ .size = sizeof d->track,
                                        .version = AZ_DATA_VERSION };
        ++d->revision;
        d->retries = 0;
    }
}

static int native_load_ready(const az_deck_status_event_v1 *e, const az_status_load_v1 *load)
{
    return !(!e->load_epoch ||
             !topic_valid(load ? &load->header : NULL, sizeof *load) ||
             !(ready(e, &load->header, AZ_STATUS_LOAD_STATE, &load->state_info) ||
               unit_ready(&load->header, AZ_STATUS_LOAD_STATE, &load->state_info)) ||
             load->state_info.info.provenance != AZ_PROV_NATIVE ||
             load->state != AZ_STATUS_LOAD_LOADED);
}

static int receive_reference(const az_deck_status_event_v1 *e, const az_status_load_v1 *load, struct deck_state *d, int *qualified)
{
    *qualified = ready(e, &load->header, AZ_STATUS_LOAD_TRACK,
                       &load->track_info) &&
                 same_track(&load->track, &load->track);
    az_track_ref track = *qualified ? load->track : (az_track_ref){ 0 };
    int reference_changed = (*qualified || d->track.track.track_id) &&
                            !same_track(&d->track.track, &track);
    if (reference_changed) d->track.track = track;
    return reference_changed;
}

static int collection_empty(const az_deck_status_event_v1 *e, const az_status_topic_v1 *header, unsigned field, const az_status_field_info_v1 *info)
{
    return (header->supported & az_status_field_bit(field)) && info->provider_id && info->info.revision &&
           info->scope == AZ_STATUS_LOAD_SCOPE && info->scope_id == e->load_epoch &&
           info->info.availability == AZ_AVAIL_EMPTY && info->count == 0;
}

static int copy_live_cues(const az_deck_status_event_v1 *e, const az_status_cues_v1 *live, struct deck_state *d)
{
    int live_count = 0;
    const az_status_field_info_v1 *info[] = { &live->live_hot_info,
                                              &live->live_memory_info };
    const az_cue_entry *rows[] = { live->live_hot, live->live_memory };
    unsigned field[] = { AZ_STATUS_CUES_LIVE_HOT,
                         AZ_STATUS_CUES_LIVE_MEMORY };
    for (int k = 0; k < 2; ++k) {
        int empty = collection_empty(e, &live->header, field[k], info[k]);
        int populated =
            collection_ready(e, &live->header, field[k], info[k], rows[k],
                             AZ_NATIVE_CUE_LIMIT);
        if ((!empty && !populated) ||
            live_count + (int)info[k]->count > (int)AZ_NATIVE_CUE_LIMIT) {
            return -1;
        }
        if (info[k]->count)
            memcpy(d->live_cues + live_count, rows[k],
                   info[k]->count * sizeof(az_cue_entry));
        live_count += (int)info[k]->count;
    }
    return live_count;
}

static void receive_cues(const az_deck_status_event_v1 *e, const az_status_cues_v1 *live, struct deck_state *d)
{
    int count = -1;
    if (topic_valid(live ? &live->header : NULL, sizeof *live)) count = copy_live_cues(e, live, d);
    d->cues_known = count >= 0;
    d->live_count = count >= 0 ? count : 0;
    if (e->changed_topics & TOPIC(CUES)) ++d->cue_serial;
}

static void metadata_text(const az_deck_status_event_v1 *e, const az_status_metadata_v1 *m, int unit_metadata, az_track_metadata *meta)
{
    const struct {
        unsigned field, valid;
        const az_status_field_info_v1 *info;
        const char *source;
        char *target;
        size_t capacity;
    } columns[] = {
        { AZ_STATUS_METADATA_TITLE, AZ_META_TITLE, &m->title_info, m->title, meta->title, sizeof meta->title },
        { AZ_STATUS_METADATA_ARTIST, AZ_META_ARTIST, &m->artist_info, m->artist, meta->artist, sizeof meta->artist },
        { AZ_STATUS_METADATA_ALBUM, AZ_META_ALBUM, &m->album_info, m->album, meta->album, sizeof meta->album },
        { AZ_STATUS_METADATA_PATH, AZ_META_PATH, &m->path_info, m->path, meta->path, sizeof meta->path },
        { AZ_STATUS_METADATA_FORMAT, AZ_META_FORMAT, &m->format_info, m->format, meta->format, sizeof meta->format },
    };
    for (unsigned i = 0; i < sizeof columns / sizeof *columns; ++i) {
        if (text_ready(e, &m->header, columns[i].field, columns[i].info, unit_metadata) && columns[i].source) {
            copy_text(columns[i].target, columns[i].capacity, columns[i].source, columns[i].info->count);
            meta->valid |= columns[i].valid;
        }
    }
}

static void metadata_key_tempo(const az_deck_status_event_v1 *e, const az_deck_status_view_v1 *v, az_track_metadata *meta)
{
    const az_status_key_v1 *key = v->key;
    if (topic_valid(key ? &key->header : NULL, sizeof *key) &&
        text_ready(e, &key->header, AZ_STATUS_KEY_ORIGINAL, &key->original_info,
                   0) &&
        key->original) {
        copy_text(meta->key, sizeof meta->key, key->original,
                  key->original_info.count);
        meta->valid |= AZ_META_KEY;
    }
    const az_status_tempo_v1 *tempo = v->tempo;
    if (topic_valid(tempo ? &tempo->header : NULL, sizeof *tempo) &&
        ready(e, &tempo->header, AZ_STATUS_TEMPO_ORIGINAL_BPM,
              &tempo->original_bpm_info)) {
        meta->original_bpm = tempo->original_bpm;
        meta->valid |= AZ_META_TEMPO;
    }
}

static int metadata_text_changed(const az_track_metadata *meta, const az_track_metadata *old)
{
    return meta->valid != old->valid || strcmp(meta->title, old->title) ||
           strcmp(meta->artist, old->artist) || strcmp(meta->album, old->album) ||
           strcmp(meta->key, old->key) || strcmp(meta->path, old->path) ||
           strcmp(meta->format, old->format);
}
static int metadata_changed_from(const az_track_metadata *meta, const az_track_metadata *old, uint32_t rate)
{
    int duration = rate
                       ? compare_timestamps(meta->duration_s, old->duration_s, 1,
                                            rate, 1)
                       : AZ_TIME_INVALID;
    return metadata_text_changed(meta, old) ||
           meta->original_bpm != old->original_bpm ||
           ((meta->valid & AZ_META_DURATION) && (duration == AZ_TIME_INVALID || duration != AZ_TIME_EQUAL));
}

static void read_metadata(const az_deck_status_event_v1 *e, const az_deck_status_view_v1 *v, int qualified, az_track_metadata *meta)
{
    const az_status_metadata_v1 *m = v->metadata;
    if (topic_valid(m ? &m->header : NULL, sizeof *m)) {
        int unit_metadata = qualified &&
                            unit_ready(&m->header, AZ_STATUS_METADATA_CONTENT_ID,
                                       &m->content_id_info) &&
                            m->content_id == v->load->track.track_id;
        metadata_text(e, m, unit_metadata, meta);
        if (ready(e, &m->header, AZ_STATUS_METADATA_DURATION_S,
                  &m->duration_s_info)) {
            meta->duration_s = m->duration_s;
            meta->valid |= AZ_META_DURATION;
        }
    }
    metadata_key_tempo(e, v, meta);
}

static void receive_metadata(const az_deck_status_event_v1 *e, const az_deck_status_view_v1 *v, struct deck_state *d, int qualified, int reference_changed)
{
    az_track_metadata meta = { .size = sizeof meta, .version = AZ_DATA_VERSION };
    read_metadata(e, v, qualified, &meta);
    /* Compare numeric metadata as values, and duration through its source clock
     * after the rate is available. No struct-padding or timestamp equality. */
    az_track_metadata *old = &d->track.metadata;
    int metadata_changed = metadata_changed_from(&meta, old, d->head_rate);
    if (metadata_changed || reference_changed) {
        *old = meta;
        ++d->revision;
        /* Metadata can be enriched after the first native write. Finish that
         * selected plan on this same load instead of abandoning a partial
         * bank. Map, cue, configuration and native-load fences still apply. */
        if (d->plan_ready && !d->obsolete && (d->pending || d->saved))
            d->plan_revision = d->revision;
    }
    d->track.metadata_info.availability = AZ_AVAIL_READY;
}

static int receive_grid(const az_deck_status_event_v1 *e, const az_status_grid_v1 *g, struct deck_state *d)
{
    int grid_ready = topic_valid(g ? &g->header : NULL, sizeof *g) &&
                     collection_ready(e, &g->header, AZ_STATUS_GRID_CURRENT,
                                      &g->current_info, g->current, MAX_BEATS);
    if (!grid_ready) {
        if (d->track.beat_count)
            ++d->revision;
        d->track.beat_count = 0;
        d->grid_revision = 0;
        d->track.grid_info.availability = AZ_AVAIL_STALE;
    } else {
        if (d->grid_revision != g->current_info.info.revision ||
            d->grid_provider != g->current_info.provider_id ||
            d->track.beat_count != g->current_info.count) {
            memcpy(d->beats, g->current,
                   g->current_info.count * sizeof *d->beats);
            d->track.beat_count = g->current_info.count;
            d->grid_revision = g->current_info.info.revision;
            d->grid_provider = g->current_info.provider_id;
            ++d->revision;
        }
        d->track.grid_info = g->current_info.info;
    }
    return grid_ready;
}

static int phrase_grid_matches(const az_deck_status_event_v1 *e, const az_status_grid_ref_v1 *ref, const az_status_grid_v1 *g)
{
    return ref->load_epoch == e->load_epoch &&
           ref->provider_id == g->current_info.provider_id &&
           ref->revision == g->current_info.info.revision &&
           ref->provenance == g->current_info.info.provenance;
}
static int phrases_ready(const az_deck_status_event_v1 *e, const az_status_phrases_v1 *p, const az_status_grid_v1 *g)
{
    return topic_valid(p ? &p->header : NULL, sizeof *p) &&
           collection_ready(e, &p->header, AZ_STATUS_PHRASES_CURRENT,
                            &p->current_info, p->current, MAX_PHRASES) &&
           ready(e, &p->header, AZ_STATUS_PHRASES_CURRENT_GRID,
                 &p->current_grid_info) &&
           phrase_grid_matches(e, &p->current_grid, g);
}

static int empty_phrases(const az_deck_status_event_v1 *e, const az_status_phrases_v1 *p)
{
    return topic_valid(p ? &p->header : NULL, sizeof *p) &&
           collection_empty(e, &p->header, AZ_STATUS_PHRASES_CURRENT, &p->current_info);
}

static void receive_phrases(const az_deck_status_event_v1 *e, const az_deck_status_view_v1 *v, struct deck_state *d, int grid_ready)
{
    const az_status_phrases_v1 *p = v->phrases;
    int phrases_empty = empty_phrases(e, p);
    int phrase_ready = grid_ready && phrases_ready(e, p, v->grid);
    if (!phrase_ready) {
        if (d->track.phrase_count)
            ++d->revision;
        d->track.phrase_count = 0;
        d->phrase_revision = 0;
        d->track.phrases_info.availability =
            phrases_empty ? AZ_AVAIL_EMPTY : AZ_AVAIL_STALE;
    } else {
        if (d->phrase_revision != p->current_info.info.revision ||
            d->phrase_provider != p->current_info.provider_id ||
            d->track.phrase_count != p->current_info.count) {
            memcpy(d->phrases, p->current,
                   p->current_info.count * sizeof *d->phrases);
            d->track.phrase_count = p->current_info.count;
            d->phrase_revision = p->current_info.info.revision;
            d->phrase_provider = p->current_info.provider_id;
            ++d->revision;
        }
        d->track.phrases_info = p->current_info.info;
    }
}

static void receive_rate(const az_deck_status_event_v1 *e, const az_status_position_v1 *pos, struct deck_state *d)
{
    uint32_t rate =
        topic_valid(pos ? &pos->header : NULL, sizeof *pos) &&
                ready(e, &pos->header, AZ_STATUS_POSITION_HEAD, &pos->head_info)
            ? pos->head.rate_hz
            : 0;
    if (rate != d->head_rate) {
        d->head_rate = rate;
        ++d->revision;
    }
}

static void deck_status(const az_deck_status_event_v1 *e, void *user)
{
    (void)user;
    const az_deck_status_view_v1 *v =
        az_data_event_view(e, TOPIC(LOAD), sizeof *v);
    if (!v || !az_data_covers(e, sizeof *e))
        return;
    struct deck_state *d = &decks[e->deck];
    /* INITIAL/RESYNC deliver a full current image. A same-load resync can
     * follow our own cue edit; it must not retire confirmed work. Validate
     * the fresh track and fields below, retaining the native revision fence. */
    receive_epoch(e, d);
    const az_status_load_v1 *load = v->load;
    if (!native_load_ready(e, load)) {
        if (d->loaded) {
            invalidate_plan(d);
            ++d->revision;
        }
        d->loaded = 0;
        return;
    }
    /* Native commands target the load epoch, including streaming loads with
     * no USB library reference. A qualified reference is only needed to join
     * unit-scoped metadata; never infer one from a native content ID. */
    int qualified;
    int reference_changed = receive_reference(e, load, d, &qualified);
    d->loaded = 1;
    receive_cues(e, v->cues, d);
    receive_metadata(e, v, d, qualified, reference_changed);
    int grid_ready = receive_grid(e, v->grid, d);
    receive_phrases(e, v, d, grid_ready);
    receive_rate(e, v->position, d);
    d->track.beats = d->beats;
    d->track.phrases = d->phrases;
    if (d->plan_ready && d->plan_revision != d->revision)
        invalidate_plan(d);
}
static int worker_step(void)
{
    int worked = 0;
    for (int i = 0; i < 4; ++i) {
        struct plan_job *j = &jobs[i];
        int expected = 1;
        if (!atomic_compare_exchange_strong_explicit(&j->phase, &expected, 2,
                                                     memory_order_acquire,
                                                     memory_order_relaxed))
            continue;
        j->status = cues_build_plan(&j->config, &j->track, &j->existing,
                                    j->rate, &j->result);
        atomic_store_explicit(&j->phase, 3, memory_order_release);
        worked = 1;
    }
    return worked;
}
static void *plan_worker(void *unused)
{
    (void)unused;
    for (;;) {
        worker_step();
        struct timespec delay = { 0, 100000000 };
        nanosleep(&delay, NULL);
    }
    return NULL;
}
static void finish_plan(int deck, const char *reason)
{
    struct deck_state *d = &decks[deck];
    d->finished_revision = d->plan_revision;
    d->finished_config = d->plan_config;
    d->plan_ready = 0;
    d->auto_done_epoch = d->epoch;
    d->manual_requested = d->clearing = d->confirming = 0;
    if (reason)
        status(deck, reason);
    else {
        char line[100];
        snprintf(line, sizeof line, "saved %d, skipped %d", d->saved,
                 d->plan.skipped);
        status(deck, line);
    }
}
static int completion_applied(const az_command_completion *c)
{
    return c->result.outcome == AZ_COMMAND_APPLIED && c->settled && c->result.reason == AZ_OK &&
           c->result.revision && c->result.observed_ns;
}
static int completion_settled(const az_command_completion *c)
{
    if (c->settled > 1) return 0;
    if (c->result.outcome != AZ_COMMAND_UNCERTAIN && c->settled != 1) return 0;
    return c->result.outcome != AZ_COMMAND_APPLIED || completion_applied(c);
}
static int completion_valid(const az_command_completion *c, const struct deck_state *d)
{
    return d->pending && az_data_covers(c, sizeof *c) && c->version == AZ_DATA_VERSION &&
           az_command_result_matches(&c->result, d->request) && c->result.outcome != AZ_COMMAND_ACCEPTED && completion_settled(c);
}

static int completion_releases(const az_command_completion *c, const struct deck_state *d)
{
    int observed = compare_timestamps(c->result.observed_ns,
                                      d->result.observed_ns, 1000000000,
                                      1000000000, 1);
    if (c->settled != 1 ||
        c->result.outcome != d->result.outcome ||
        c->result.reason != d->result.reason ||
        c->result.revision != d->result.revision ||
        observed == AZ_TIME_INVALID || observed != AZ_TIME_EQUAL)
        return 0;
    return 1;
}

static void complete_plan(int deck, struct deck_state *d, const az_command_completion *completion)
{
    if (d->obsolete || d->plan_epoch != d->epoch ||
        d->plan_config != config_revision) {
            /* A retired request only owns its acknowledgement/cleanup. Its
             * result cannot finish or consume the replacement track's load. */
    } else if (completion_applied(completion)) {
        d->native_revision = d->result.revision;
        d->view_deadline = now_ns() + UINT64_C(2000000000);
        if (!d->deleting) {
            ++d->next;
            ++d->saved;
        }
    } else {
            /* Uncertain writes are never retried: a saved effect may exist. */
        finish_plan(
            deck, d->result.outcome == AZ_COMMAND_UNCERTAIN
                      ? "placement uncertain; reload track before retrying"
                      : "placement stopped; partial changes may remain");
    }
}

static void receive_terminal(int deck, struct deck_state *d, const az_command_completion *completion)
{
    if (d->terminal)
        return;
    d->result = completion->result;
    d->terminal = 1;
    d->acknowledge = 1;
    if (d->result.outcome == AZ_COMMAND_UNCERTAIN)
        d->uncertain_load = d->native_load;
    complete_plan(deck, d, completion);
    int rc = Q->command_acknowledge(CUES_ID, d->request);
    if (rc == AZ_OK)
        d->acknowledge = 0;
}

static void command_complete(const az_command_completion *completion,
                             void *user)
{
    struct deck_state *d = user;
    if (!completion_valid(completion, d)) return;
    if (completion->kind == AZ_COMMAND_COMPLETION_TERMINAL) {
        receive_terminal((int)(d - decks), d, completion);
    } else if (completion->kind == AZ_COMMAND_COMPLETION_RELEASED &&
               d->terminal) {
        if (!completion_releases(completion, d)) return;
        d->pending = d->terminal = d->acknowledge = 0;
        d->request = 0;
        if (d->obsolete) {
            d->obsolete = 0;
            d->plan_ready = 0;
        }
    }
}
static int native_state(int deck, uint64_t now, az_deck_snapshot *s,
                        az_native_cue_snapshot *cues)
{
    (void)now;
    az_command_target_v1 target = { .size = sizeof target };
    if (O->command_target(deck, decks[deck].epoch, &target) != AZ_OK) return 0;
    if (target.size < sizeof target || target.version != AZ_DATA_VERSION) return 0;
    *s = target.deck;
    *cues = target.cues;
    return 1;
}
static int filter_text_ready(const cues_group *g, const az_track_metadata *meta)
{
    return !((g->artist[0] && !(meta->valid & AZ_META_ARTIST)) ||
             (g->key[0] && !(meta->valid & AZ_META_KEY)));
}

static int group_metadata_ready(const cues_config *config, const cues_group *g, const az_track_metadata *meta)
{
    if (!filter_text_ready(g, meta)) return 0;
    double low = g->min_bpm > 0 ? g->min_bpm : config->min_bpm;
    double high = g->max_bpm > 0 ? g->max_bpm : config->max_bpm;
    if ((low > 0 || high > 0) && !(meta->valid & AZ_META_TEMPO)) return 0;
    return 1;
}

static int filter_metadata_ready(const cues_config *config,
                                 const az_track_metadata *meta)
{
    for (int i = 0; i < config->group_count; ++i) {
        const cues_group *g = &config->groups[i];
        if (!g->enabled || !g->rule_count) continue;
        if (!group_metadata_ready(config, g, meta)) return 0;
    }
    return 1;
}
static void schedule_plan(int deck, const az_deck_snapshot *native,
                          const az_native_cue_snapshot *existing)
{
    struct deck_state *d = &decks[deck];
    struct plan_job *j = &jobs[deck];
    if (atomic_load_explicit(&j->phase, memory_order_acquire) != 0)
        return;
    j->config = *cues_settings_config();
    j->manual = d->manual_requested;
    j->config.enabled = 1;
    j->track = d->track;
    memcpy(j->beats, d->beats, d->track.beat_count * sizeof *j->beats);
    memcpy(j->phrases, d->phrases, d->track.phrase_count * sizeof *j->phrases);
    j->track.beats = j->beats;
    j->track.phrases = j->phrases;
    j->existing = *existing;
    if (j->manual)
        j->existing.count =
            0; /* Evaluate replacement before asking or deleting. */
    j->rate = existing->source_rate_hz;
    j->epoch = d->epoch;
    j->revision = d->revision;
    j->config_revision = config_revision;
    d->native_load = native->native_load_generation;
    atomic_store_explicit(&j->phase, 1, memory_order_release);
}
static void manual_apply(int deck)
{
    struct deck_state *d = &decks[deck];
    if (!d->loaded) {
        status(deck, "load a track first");
        return;
    }
    if (d->pending || d->confirming || d->plan_ready ||
        atomic_load_explicit(&jobs[deck].phase, memory_order_acquire)) {
        status(deck, "cue placement is already in progress");
        return;
    }
    d->manual_requested = 1;
    d->retries = 0;
    status(deck, "waiting for analysis to apply rules");
}
static int cue_identity_matches(const az_cue_entry *a, const az_cue_entry *b)
{
    return a->classification == b->classification && a->slot == b->slot && a->type == b->type && (b->valid & AZ_CUE_START);
}
static int cue_address_matches(const az_cue_entry *a, const az_cue_entry *b, uint32_t rate)
{
    if (!cue_identity_matches(a, b)) return 0;
    int start = compare_timestamps(a->start_s, b->start_s, 1, rate, 1);
    int end = a->type == AZ_CUE_LOOP ? compare_timestamps(a->end_s, b->end_s, 1, rate, 1) : AZ_TIME_EQUAL;
    if (start == AZ_TIME_INVALID || end == AZ_TIME_INVALID) return -1;
    return start == AZ_TIME_EQUAL && end == AZ_TIME_EQUAL;
}
static int live_cue_addressable(const az_cue_entry *cue, const az_native_cue_snapshot *snapshot)
{
    for (uint32_t j = 0; j < snapshot->count; ++j) {
        int match = cue_address_matches(cue, &snapshot->entries[j], snapshot->source_rate_hz);
        if (match < 0) return 0;
        if (match) return 1;
    }
    return 0;
}

static int live_addressable(const struct deck_state *d,
                            const az_native_cue_snapshot *s)
{
    uint32_t rate = s->source_rate_hz;
    if (!rate || rate >= 0x7fffffffu)
        return 0;
    for (int i = 0; i < d->live_count; ++i) {
        const az_cue_entry *a = &d->live_cues[i];
        int found = 0;
        if (!(a->valid & AZ_CUE_START))
            return 0;
        found = live_cue_addressable(a, s);
        if (!found)
            return 0;
    }
    return 1;
}
static int replacement_state_valid(const struct deck_state *d)
{
    return !(!d->plan_ready || d->plan_epoch != d->epoch ||
             d->plan_config != config_revision || d->plan_revision != d->revision ||
             !d->cues_known || d->cue_serial != d->confirm_cue_serial);
}

static void confirm_replace(int yes, void *user)
{
    struct deck_state *d = user;
    int deck = (int)(d - decks);
    d->confirming = 0;
    if (!yes) {
        finish_plan(deck, "replacement cancelled");
        return;
    }
    az_deck_snapshot native;
    az_native_cue_snapshot existing;
    if (!replacement_state_valid(d) ||
        !native_state(deck, now_ns(), &native, &existing) ||
        native.native_load_generation != d->native_load ||
        existing.revision != d->confirm_revision ||
        !live_addressable(d, &existing)) {
        finish_plan(deck, "track or cues changed; press Cues again");
        return;
    }
    d->clearing = 1;
    d->confirmed = 1;
}
static void button_paint(az_draw *draw, az_rect area, void *user)
{
    (void)user;
    C->fill_rect(draw, area.x, area.y, area.w, area.h, C->color(AZ_COL_BG));
    C->draw_rect(draw, area.x, area.y, area.w, area.h, 1, C->color(AZ_COL_HI));
    C->draw_text(draw, area.x + 13, area.y + 10, "CUES", 14,
                 C->color(AZ_COL_HI));
}
static void picker_paint(az_draw *draw, az_rect area, void *user)
{
    (void)user;
    C->fill_rect(draw, area.x, area.y, area.w, area.h, C->color(AZ_COL_BG));
    for (int i = 0; i < 4; ++i) {
        int y = area.y + 4 + 48 * i;
        C->draw_rect(draw, area.x + 4, y, area.w - 8, 44,
                     i == picker_selected ? 2 : 1, C->color(AZ_COL_HI));
        char line[48];
        snprintf(line, sizeof line, "Deck %d  %s", i + 1,
                 decks[i].loaded ? "Apply cues" : "Not loaded");
        C->draw_text(draw, area.x + 14, y + 10, line, 14,
                     C->color(decks[i].loaded ? AZ_COL_TEXT : AZ_COL_TEXT_DIM));
    }
}
static int button_input(const az_event *event, void *user)
{
    (void)user;
    if (event->kind != AZ_EV_TAP && event->kind != AZ_EV_CLICK)
        return 0;
    picker_open = !picker_open;
    C->surface_show(deck_picker, picker_open);
    if (AZ_HAS(C, surface_focus))
        C->surface_focus(deck_picker, picker_open);
    return 1;
}
static void close_picker(void)
{
    picker_open = 0;
    C->surface_show(deck_picker, 0);
    if (AZ_HAS(C, surface_focus))
        C->surface_focus(deck_picker, 0);
}

static int picker_input(const az_event *event, void *user)
{
    (void)user;
    if (event->kind == AZ_EV_KNOB) {
        picker_selected = (picker_selected + (event->dir < 0 ? 3 : 1)) % 4;
        C->surface_redraw(deck_picker);
        return 1;
    }
    int deck = picker_selected;
    if (event->kind == AZ_EV_TAP) {
        deck = (event->y - 60) / 48;
        if (deck < 0 || deck > 3)
            return 0;
    } else if (event->kind != AZ_EV_CLICK && event->kind != AZ_EV_BACK)
        return 0;
    close_picker();
    if (event->kind != AZ_EV_BACK)
        manual_apply(deck);
    return 1;
}
static int plan_job_current(const struct deck_state *d, const struct plan_job *j)
{
    return d->epoch == j->epoch && d->revision == j->revision && config_revision == j->config_revision;
}
static void install_plan(struct deck_state *d, const struct plan_job *j)
{
    d->plan = j->result;
    d->plan_revision = j->revision;
    d->plan_config = j->config_revision;
    d->plan_epoch = j->epoch;
    d->native_revision = j->existing.revision;
    d->view_deadline = 0;
    d->next = d->saved = 0;
    d->plan_ready = 1;
    d->plan_manual = j->manual;
    d->plan_rate = j->rate;
    d->clearing = d->confirming = d->confirmed = 0;
}
static void acknowledge_command(struct deck_state *d)
{
    if (d->pending && d->acknowledge &&
        Q->command_acknowledge(CUES_ID, d->request) == AZ_OK)
        d->acknowledge = 0;
}

static void receive_plan_job(int i)
{
    struct deck_state *d = &decks[i];
    acknowledge_command(d);
    struct plan_job *j = &jobs[i];
    if (atomic_load_explicit(&j->phase, memory_order_acquire) == 3) {
        if (j->status == AZ_OK && plan_job_current(d, j)) {
            install_plan(d, j);
        } else if (j->status != AZ_OK && plan_job_current(d, j)) {
            d->finished_revision = j->revision;
            d->finished_config = j->config_revision;
            status(i,
                   j->status == AZ_PENDING
                       ? "waiting for track analysis"
                       : "rule evaluation unavailable for this analysis");
        }
        atomic_store_explicit(&j->phase, 0, memory_order_release);
    }
}

static int tick_due(uint64_t clock)
{
    if (!clock)
        return 0;
    if (next_check) {
        int order =
            compare_timestamps(clock, next_check, 1000000000, 1000000000, 1);
        if (order == AZ_TIME_INVALID || order == AZ_TIME_BEFORE)
            return 0;
    }
    next_check = clock + UINT64_C(250000000);
    return 1;
}

static int preserve_existing(int i, const az_native_cue_snapshot *existing)
{
    struct deck_state *d = &decks[i];
    if (!d->manual_requested && (existing->count || d->live_count)) {
        d->auto_done_epoch = d->epoch;
        status(i, "existing cues preserved; use Cues to replace");
        return 1;
    }
    return 0;
}
static int phrase_analysis_available(const struct deck_state *d)
{
    return d->track.phrases_info.availability == AZ_AVAIL_READY || d->track.phrases_info.availability == AZ_AVAIL_EMPTY;
}

static void schedule_ready_plan(int i, const cues_config *config, const az_deck_snapshot *native, const az_native_cue_snapshot *existing)
{
    struct deck_state *d = &decks[i];
    if (!d->cues_known)
        return;
    if (preserve_existing(i, existing)) return;
    if (!d->manual_requested &&
        !phrase_analysis_available(d))
        return;
    if (!filter_metadata_ready(config, &d->track.metadata)) {
        status(i, "waiting for rule filter metadata");
        return;
    }
    if (d->finished_revision != d->revision ||
        d->finished_config != config_revision || d->manual_requested)
        schedule_plan(i, native, existing);
    return;
}

static int plan_view_ready(int i, uint64_t clock, const az_native_cue_snapshot *existing)
{
    struct deck_state *d = &decks[i];
    if (d->native_revision != existing->revision) {
        if (d->view_deadline) {
            int order = compare_timestamps(clock, d->view_deadline,
                                           1000000000, 1000000000, 1);
            if (order != AZ_TIME_INVALID && order == AZ_TIME_BEFORE)
                return 0;
        }
        finish_plan(i, "cue collection changed; press Cues again");
        return 0;
    }
    d->view_deadline = 0;
    return 1;
}

static void request_replacement_confirmation(int i, struct deck_state *d, const az_native_cue_snapshot *existing)
{
    char prompt[256];
    snprintf(prompt, sizeof prompt,
             "Deck %d: replace all %u saved hot/memory cues with "
             "%d rule "
             "cues? %.80s",
             i + 1, existing->count, d->plan.count,
             d->track.metadata.title);
    d->confirm_revision = existing->revision;
    d->confirm_cue_serial = d->cue_serial;
    d->confirming = 1;
    if (C->confirm(CUES_ID, "Replace track cues?", prompt,
                   "Replace", confirm_replace, d))
        finish_plan(i,
                    "confirmation unavailable; press Cues again");
}
static int plan_replacement_confirmed(int i, const az_native_cue_snapshot *existing)
{
    struct deck_state *d = &decks[i];
    if (d->plan_manual && !d->confirmed) {
        if (!d->cues_known)
            return 0;
        if (existing->count || d->live_count) {
            if (!live_addressable(d, existing)) {
                finish_plan(i, "save existing cues before replacing them");
                return 0;
            }
            request_replacement_confirmation(i, d, existing);
            return 0;
        }
        d->confirmed = 1;
    }
    return 1;
}

static void submit_plan_point(int i, uint64_t clock, const az_native_cue_snapshot *existing)
{
    struct deck_state *d = &decks[i];
    const cues_point *p = &d->plan.points[d->next];
    az_command_request request = {
        .size = sizeof request,
        .version = AZ_DATA_VERSION,
        .operation = p->type == AZ_CUE_LOOP ? AZ_CMD_CUE_LOOP_SET
                                            : AZ_CMD_CUE_POINT_SET,
        .deck = (uint32_t)i,
        .request_id = ++next_request,
        .expected_load_generation = d->native_load,
        .expected_revision = existing->revision,
        .deadline_ns = clock + UINT64_C(10000000000),
        .quantization = AZ_QUANTIZE_NATIVE,
        .flags = WRITE_FLAGS,
        .classification = p->classification,
        .entry_id = p->slot,
        .position_s = p->start_s,
        .end_s = p->end_s,
        .rgb = p->rgb
    };
    snprintf(request.comment, sizeof request.comment, "%s", p->comment);
    d->deleting = d->clearing;
    if (d->clearing) {
        const az_cue_entry *entry = &existing->entries[0];
        request.operation = entry->classification == AZ_CUE_HOT
                                ? AZ_CMD_HOT_CLEAR
                                : AZ_CMD_MEMORY_DELETE;
        request.classification = entry->classification;
        request.entry_id = entry->classification == AZ_CUE_HOT
                               ? entry->slot
                               : entry->entry_id;
        request.position_s = request.end_s = 0;
        request.rgb = 0;
        request.comment[0] = 0;
    }
    az_command_result result = { .size = sizeof result };
    d->request = request.request_id;
    d->pending = 1;
    d->terminal = d->obsolete = d->acknowledge = 0;
    int rc = Q->command_submit_async(CUES_ID, &request, command_complete, d,
                                     &result);
    if (rc != AZ_OK) {
        d->pending = 0;
        d->request = 0;
        if (rc != AZ_BUSY && rc != AZ_PENDING)
            finish_plan(i, "SDK refused cue creation");
    }
}

static int deck_eligible(const struct deck_state *d, const cues_config *config)
{
    if (!d->loaded || !d->head_rate || d->pending || d->confirming)
        return 0;
    if (!d->manual_requested && !d->plan_ready &&
        (!config->enabled || d->auto_done_epoch == d->epoch))
        return 0;
    return 1;
}

static int plan_target_current(const struct deck_state *d, const az_deck_snapshot *native, const az_native_cue_snapshot *existing)
{
    return !(d->plan_config != config_revision || d->plan_epoch != d->epoch ||
             d->plan_revision != d->revision ||
             native->native_load_generation != d->native_load ||
             existing->source_rate_hz != d->plan_rate);
}

static int automatic_has_cues(const struct deck_state *d, const az_native_cue_snapshot *existing)
{
    return !d->plan_manual && !d->next && (existing->count || d->live_count);
}

static void apply_plan(int i, uint64_t clock, const az_native_cue_snapshot *existing)
{
    struct deck_state *d = &decks[i];
    if (!d->plan.count) {
        finish_plan(i, "no rules matched; existing cues preserved");
        return;
    }
    if (!plan_replacement_confirmed(i, existing)) return;
    if (automatic_has_cues(d, existing)) {
        finish_plan(i, "existing cues preserved; use Cues to replace");
        return;
    }
    if (d->clearing && !existing->count)
        d->clearing = 0;
    if (d->next >= d->plan.count) {
        finish_plan(i, NULL);
        return;
    }
    if (next_request == UINT64_MAX) {
        finish_plan(i, "request limit reached; restart required");
        return;
    }
    submit_plan_point(i, clock, existing);
}
static void advance_deck(int i, uint64_t clock, const cues_config *config)
{
    struct deck_state *d = &decks[i];
    if (!deck_eligible(d, config)) return;
    az_deck_snapshot native;
    az_native_cue_snapshot existing;
    if (!native_state(i, clock, &native, &existing))
        return;
    if (d->uncertain_load == native.native_load_generation)
        return;
    if (!d->plan_ready) {
        schedule_ready_plan(i, config, &native, &existing);
        return;
    }
    if (!plan_target_current(d, &native, &existing)) {
        invalidate_plan(d);
        return;
    }
    if (!plan_view_ready(i, clock, &existing)) return;
    apply_plan(i, clock, &existing);
}

static void tick(void)
{
    cues_settings_tick();
    for (int i = 0; i < 4; ++i) receive_plan_job(i);
    uint64_t clock = now_ns();
    if (!tick_due(clock)) return;
    const cues_config *config = cues_settings_config();
    az_data_capabilities caps = { .size = sizeof caps };
    if (O->data_capabilities(&caps) != AZ_OK ||
        (caps.commands & WRITE_COMMANDS) != WRITE_COMMANDS) {
        cues_settings_status("Waiting for SDK cue creation support");
        return;
    }
    for (int i = 0; i < 4; ++i) advance_deck(i, clock, config);
}
static int surface_services_valid(const az_core_v1 *core)
{
    return AZ_HAS(core, surface_create) && AZ_HAS(core, surface_place) && AZ_HAS(core, surface_show) && AZ_HAS(core, surface_redraw);
}
static int core_services_valid(const az_core_v1 *core)
{
    if (!core || core->abi != AZ_MOD_ABI || !AZ_HAS(core, observation_service) || !AZ_HAS(core, command_service) ||
        !AZ_HAS(core, register_screen) || !AZ_HAS(core, thread_start) ||
        !AZ_HAS(core, confirm) || !surface_services_valid(core))
        return 0;
    return 1;
}

static int library_services_valid(void)
{
    if (!AZ_OBSERVATION_HAS(O, on_deck_status) || !AZ_OBSERVATION_HAS(O, deck_status_release) ||
        !AZ_OBSERVATION_HAS(O, command_target) ||
        !AZ_OBSERVATION_HAS(O, data_capabilities) ||
        !AZ_COMMANDS_HAS(Q, command_submit_async) ||
        !AZ_COMMANDS_HAS(Q, command_acknowledge))
        return 0;
    return 1;
}

static int create_surfaces(void)
{
    const az_surface_v1 button = { .size = sizeof button,
                                   .layer = AZ_LAYER_OVER_EP147,
                                   .views = AZ_VIEW_BIT(AZ_VIEW_DECK),
                                   .on_paint = button_paint,
                                   .on_input = button_input };
    const az_surface_v1 picker = { .size = sizeof picker,
                                   .layer = AZ_LAYER_OVER_EP147,
                                   .views = AZ_VIEW_BIT(AZ_VIEW_DECK),
                                   .on_paint = picker_paint,
                                   .on_input = picker_input };
    deck_button = C->surface_create(CUES_ID, &button);
    deck_picker = C->surface_create(CUES_ID, &picker);
    if (!deck_button || !deck_picker)
        return 0;
    C->surface_place(deck_button, (az_rect){ 866, 4, 76, 44 });
    C->surface_place(deck_picker, (az_rect){ 866, 56, 244, 196 });
    C->surface_show(deck_button, 1);
    C->surface_show(deck_picker, 0);
    return 1;
}

static int init(const az_core_v1 *core)
{
    if (!core_services_valid(core)) return -1;
    C = core;
    O = core->observation_service(AZ_OBSERVATION_ABI);
    Q = core->command_service(AZ_COMMANDS_ABI);
    if (!library_services_valid()) return -2;
    const az_screen_v1 *screen = cues_settings_init(core, config_changed);
    if (!screen || C->register_screen(CUES_ID, screen))
        return -3;
    if (!create_surfaces()) return -6;
    if (C->thread_start(CUES_ID, "cues-plan", plan_worker, NULL))
        return -4;
    az_deck_watch_v1 watch = { .size = sizeof watch,
                               .version = AZ_DATA_VERSION,
                               .deck_mask = AZ_STATUS_ALL_DECKS,
                               .position_hz = 1,
                               .topics = TOPICS };
    if (O->on_deck_status(CUES_ID, &watch, deck_status, NULL, &subscription) !=
        AZ_OK)
        return -5;
    cues_settings_status(cues_settings_config()->enabled
                             ? "Waiting for loaded tracks"
                             : "Automatic placement off");
    return 0;
}
static const char *const firmware[] = { "any", NULL };
static const az_mod_v1 mod = { .abi = AZ_MOD_ABI,
                               .size = sizeof mod,
                               .id = CUES_ID,
                               .fw_tags = firmware,
                               .init = init,
                               .tick = tick };
AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void) { return &mod; }
