/* SPDX-License-Identifier: GPL-3.0-only */
#include "deck.h"

#include <math.h>
#include <stdio.h>

#include "azmod-data-helpers.h"

/* Bounds of the SDK's deck-status collections. Hot cues occupy slots 1 through 8. */
enum { MAX_BEATS = 32768 };
static struct {
    an_beat_t beats[MAX_BEATS];
    an_phrase_t phrases[AN_MAX_PHRASES];
    an_cue_t cues[AN_MAX_HOT_CUES + AN_MAX_MEMORY_CUES];
} owned[AN_DECKS];

#define TOPIC_PRESENT(t) (az_data_covers((t), sizeof *(t)) && (t)->header.version == AZ_DATA_VERSION)
#define MAP_TOPICS (AZ_STATUS_TOPIC(AZ_STATUS_GRID) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASES))

static int time_ms(double seconds, uint32_t *out)
{
    double ms = seconds * 1000;
    if (!isfinite(ms) || ms < 0 || ms > UINT32_MAX - 1000.0) return 0;
    *out = (uint32_t)llround(ms);
    return 1;
}

int an_availability_pending(int availability)
{
    return availability == AZ_AVAIL_LOADING || availability == AZ_AVAIL_STALE;
}

static int unit_or_load_ready(const az_deck_status_event_v1 *e, const az_status_topic_v1 *header,
                              unsigned field, const az_status_field_info_v1 *info)
{
    if (info->scope != AZ_STATUS_UNIT) return az_data_event_scalar_ready(e, header, field, info);
    return (header->supported & header->valid & az_status_field_bit(field)) &&
           info->info.availability == AZ_AVAIL_READY && info->count == 1 && info->scope_id;
}

static int load_availability(const az_deck_status_event_v1 *e, const az_status_topic_v1 *header,
                             unsigned field, const az_status_field_info_v1 *info)
{
    uint64_t bit = az_status_field_bit(field);
    if (!(header->supported & bit)) return AZ_AVAIL_UNSUPPORTED;
    if (info->scope != AZ_STATUS_LOAD_SCOPE || info->scope_id != e->load_epoch) {
        /* A producer can advertise support before observing the first load. */
        return info->info.availability == AZ_AVAIL_LOADING ? AZ_AVAIL_LOADING : AZ_AVAIL_STALE;
    }
    if (info->info.availability == AZ_AVAIL_READY && !(header->valid & bit)) return AZ_AVAIL_STALE;
    return (int)info->info.availability;
}

typedef struct {
    const void *rows;
    const az_status_field_info_v1 *info;
    int availability;
} collection_t;

static collection_t collection(const az_deck_status_event_v1 *e, const az_status_topic_v1 *header,
                               unsigned field, const az_status_field_info_v1 *info,
                               const void *rows, unsigned max)
{
    int availability = load_availability(e, header, field, info);
    if (availability == AZ_AVAIL_READY &&
        (!rows || !info->provider_id || !info->info.revision || !info->count || info->count > max))
        availability = AZ_AVAIL_FAILED;
    return (collection_t){rows, info, availability};
}

static void copy_grid(an_deck_status_t *deck, collection_t grid, an_beat_t *beats)
{
    deck->analysis.nbeats = 0;
    deck->grid_pending = an_availability_pending(grid.availability);
    if (grid.availability != AZ_AVAIL_READY) return;
    const az_beat *rows = grid.rows;
    for (unsigned i = 0; i < grid.info->count; i++) {
        uint32_t ms;
        if (!time_ms(rows[i].time_s, &ms) || (i && ms < beats[i - 1].time_ms)) return;
        double bpm = rows[i].bpm;
        beats[i] = (an_beat_t){ms, bpm >= 20 && bpm <= 400 ? (uint16_t)lround(bpm * 100) : 0,
                               rows[i].beat_in_bar <= UINT8_MAX ? (uint8_t)rows[i].beat_in_bar : 0};
    }
    deck->analysis.beats = beats;
    deck->analysis.nbeats = (int)grid.info->count;
}

static void copy_phrases(an_deck_status_t *deck, collection_t phrases, collection_t grid,
                          const az_status_grid_ref_v1 *ref, an_phrase_t *out)
{
    deck->analysis.nphrases = 0;
    deck->phrases_pending = an_availability_pending(phrases.availability);
    if (phrases.availability != AZ_AVAIL_READY) return;
    if (grid.availability != AZ_AVAIL_READY || !ref || ref->load_epoch != deck->epoch ||
        ref->provider_id != grid.info->provider_id || ref->revision != grid.info->info.revision ||
        ref->provenance != grid.info->info.provenance) {
        deck->phrases_pending = 1;
        return;
    }
    const az_phrase *rows = phrases.rows;
    const az_beat *beats = grid.rows;
    for (unsigned i = 0; i < phrases.info->count; i++) {
        const az_phrase *p = &rows[i];
        uint32_t start, end;
        if (p->start_beat >= p->end_beat || p->end_beat >= grid.info->count ||
            (i && p->start_beat < rows[i - 1].end_beat) ||
            !time_ms(beats[p->start_beat].time_s, &start) || !time_ms(beats[p->end_beat].time_s, &end) ||
            end <= start) return;
        out[i] = (an_phrase_t){start, end, an_phrase_category(p->raw_mood, p->raw_kind),
                               i && p->start_beat == rows[i - 1].end_beat};
    }
    deck->analysis.phrases = out;
    deck->analysis.nphrases = (int)phrases.info->count;
}

static void update_maps(an_deck_status_t *deck, const az_deck_status_event_v1 *e,
                        const az_deck_status_view_v1 *v)
{
    const az_status_grid_v1 *g = v->grid;
    const az_status_phrases_v1 *p = v->phrases;
    collection_t grid[2] = {{0}}, phrases[2] = {{0}};
    if (TOPIC_PRESENT(g)) {
        grid[0] = collection(e, &g->header, AZ_STATUS_GRID_CURRENT, &g->current_info, g->current, MAX_BEATS);
        grid[1] = collection(e, &g->header, AZ_STATUS_GRID_SAVED, &g->saved_info, g->saved, MAX_BEATS);
    }
    int current_grid = grid[0].availability == AZ_AVAIL_READY || an_availability_pending(grid[0].availability);
    copy_grid(deck, grid[current_grid ? 0 : 1], owned[e->deck].beats);

    const az_status_grid_ref_v1 *ref = NULL;
    int selected = 0;
    if (TOPIC_PRESENT(p)) {
        phrases[0] = collection(e, &p->header, AZ_STATUS_PHRASES_CURRENT, &p->current_info, p->current, AN_MAX_PHRASES);
        phrases[1] = collection(e, &p->header, AZ_STATUS_PHRASES_SAVED, &p->saved_info, p->saved, AN_MAX_PHRASES);
        selected = phrases[0].availability != AZ_AVAIL_READY && !an_availability_pending(phrases[0].availability);
        if (!selected && az_data_event_scalar_ready(e, &p->header, AZ_STATUS_PHRASES_CURRENT_GRID, &p->current_grid_info) &&
            p->current_grid.grid_field == AZ_STATUS_GRID_CURRENT) ref = &p->current_grid;
        if (selected && az_data_event_scalar_ready(e, &p->header, AZ_STATUS_PHRASES_SAVED_GRID, &p->saved_grid_info) &&
            p->saved_grid.grid_field == AZ_STATUS_GRID_SAVED) ref = &p->saved_grid;
    }
    copy_phrases(deck, phrases[selected], grid[selected], ref, owned[e->deck].phrases);
}

static int copy_cues(collection_t cues, int hot, an_cue_t *out, int *count)
{
    if (cues.availability != AZ_AVAIL_READY) return cues.availability;
    const az_cue_entry *rows = cues.rows;
    unsigned slots = 0;
    int before = *count;
    for (unsigned i = 0; i < cues.info->count; i++) {
        const az_cue_entry *c = &rows[i];
        an_cue_t cue = {0};
        if (!az_data_covers(c, sizeof *c) || c->version != AZ_DATA_VERSION || !(c->valid & AZ_CUE_START) ||
            c->classification != (hot ? AZ_CUE_HOT : AZ_CUE_MEMORY) || !time_ms(c->start_s, &cue.time_ms)) continue;
        if (hot) {
            if (!c->slot || c->slot > AN_MAX_HOT_CUES || (slots & (1u << c->slot))) continue;
            slots |= 1u << c->slot;
            cue.slot = c->slot;
        }
        if (c->valid & AZ_CUE_COMMENT) snprintf(cue.label, sizeof cue.label, "%.39s", c->comment);
        /* Keep the merged list in time order for automatic cue selection. */
        int at = (*count)++;
        while (at > 0 && out[at - 1].time_ms > cue.time_ms) {
            out[at] = out[at - 1];
            at--;
        }
        out[at] = cue;
    }
    return *count > before ? AZ_AVAIL_READY : AZ_AVAIL_FAILED;
}

static void update_cues(an_deck_status_t *deck, const az_deck_status_event_v1 *e, const az_status_cues_v1 *cues)
{
    deck->analysis.ncues = 0;
    deck->hot_cues = deck->memory_cues = AZ_AVAIL_UNSUPPORTED;
    if (!TOPIC_PRESENT(cues)) return;
    collection_t hot = collection(e, &cues->header, AZ_STATUS_CUES_LIVE_HOT, &cues->live_hot_info, cues->live_hot, 64);
    collection_t memory = collection(e, &cues->header, AZ_STATUS_CUES_LIVE_MEMORY, &cues->live_memory_info, cues->live_memory, AN_MAX_MEMORY_CUES);
    /* A confirmed empty live bank takes precedence over its saved edition. */
    if (hot.availability == AZ_AVAIL_UNSUPPORTED)
        hot = collection(e, &cues->header, AZ_STATUS_CUES_SAVED_HOT, &cues->saved_hot_info, cues->saved_hot, 256);
    if (memory.availability == AZ_AVAIL_UNSUPPORTED)
        memory = collection(e, &cues->header, AZ_STATUS_CUES_SAVED_MEMORY, &cues->saved_memory_info, cues->saved_memory, AN_MAX_MEMORY_CUES);
    an_cue_t *out = owned[e->deck].cues;
    deck->analysis.cues = out;
    deck->hot_cues = copy_cues(hot, 1, out, &deck->analysis.ncues);
    deck->memory_cues = copy_cues(memory, 0, out, &deck->analysis.ncues);
}

static void update_transport(an_deck_status_t *deck, const az_deck_status_event_v1 *e,
                             const az_deck_status_view_v1 *v)
{
    const az_status_transport_v1 *t = v->transport;
    deck->playback = AZ_PLAYBACK_UNKNOWN;
    deck->direction = AZ_STATUS_DIRECTION_STATIONARY;
    deck->rate = deck->bpm = deck->original_bpm = 0;
    if (TOPIC_PRESENT(t)) {
        if (az_data_event_scalar_ready(e, &t->header, AZ_STATUS_TRANSPORT_STATE, &t->state_info))
            deck->playback = t->state;
        if (az_data_event_scalar_ready(e, &t->header, AZ_STATUS_TRANSPORT_DIRECTION, &t->direction_info))
            deck->direction = t->direction;
        if (az_data_event_scalar_ready(e, &t->header, AZ_STATUS_TRANSPORT_RATE, &t->rate_info))
            deck->rate = t->rate;
    }
    const az_status_tempo_v1 *tempo = v->tempo;
    if (TOPIC_PRESENT(tempo)) {
        if (unit_or_load_ready(e, &tempo->header, AZ_STATUS_TEMPO_CURRENT_BPM, &tempo->current_bpm_info))
            deck->bpm = tempo->current_bpm;
        if (az_data_event_scalar_ready(e, &tempo->header, AZ_STATUS_TEMPO_ORIGINAL_BPM, &tempo->original_bpm_info))
            deck->original_bpm = tempo->original_bpm;
    }
    const az_status_position_v1 *pos = v->position;
    deck->position_valid = TOPIC_PRESENT(pos) &&
        az_data_event_scalar_ready(e, &pos->header, AZ_STATUS_POSITION_HEAD, &pos->head_info) &&
        time_ms(pos->head.seconds, &deck->pos_ms);
    deck->head_ns = deck->position_valid ? pos->head_info.info.observed_ns : 0;
    deck->sample_rate = deck->position_valid ? pos->head.rate_hz : 0;
}

static void update_loop(an_deck_status_t *deck, const az_deck_status_event_v1 *e)
{
    int engaged = 0, was_engaged = deck->loop_engaged, released = deck->loop.released;
    deck->loop = (an_loop_t){0};
    deck->loop_engaged = 0;
    deck->loop_availability = AZ_AVAIL_UNSUPPORTED;
    const az_status_loop_v1 *loop;
    if (!az_read_event_loop(e, &loop)) return;
    deck->loop_availability = load_availability(e, &loop->header, AZ_STATUS_LOOP_ENGAGED, &loop->engaged_info);
    if (deck->loop_availability != AZ_AVAIL_READY) return;
    if (!az_read_event_loop_engaged(e, &engaged)) {
        deck->loop_availability = AZ_AVAIL_FAILED;
        return;
    }
    deck->loop_engaged = engaged;
    if (!engaged) {
        const az_status_occurrence_v1 *action = e->occurrence;
        int repositioned = az_data_covers(action, sizeof *action) && action->version == AZ_DATA_VERSION &&
                           (action->kind == AZ_STATUS_SEEK || action->kind == AZ_STATUS_JUMP);
        deck->loop.released = e->kind == AZ_STATUS_CHANGE && (released || was_engaged) &&
                              deck->playback == AZ_PLAYBACK_PLAYING && !repositioned;
        return;
    }
    double in, out;
    if (!az_read_event_loop_point(e, AZ_STATUS_LOOP_WORKING_IN, &in) ||
        !az_read_event_loop_point(e, AZ_STATUS_LOOP_WORKING_OUT, &out)) {
        int a = load_availability(e, &loop->header, AZ_STATUS_LOOP_WORKING_IN, &loop->working_in_info);
        int b = load_availability(e, &loop->header, AZ_STATUS_LOOP_WORKING_OUT, &loop->working_out_info);
        deck->loop_availability = an_availability_pending(a) || an_availability_pending(b) ? AZ_AVAIL_LOADING : AZ_AVAIL_FAILED;
        return;
    }
    uint32_t in_ms, out_ms;
    uint32_t rate = loop->working_out.rate_hz;
    if (!rate) rate = loop->working_in.rate_hz;
    if (!rate) rate = deck->sample_rate;
    int order = compare_timestamps(out, in, 1, rate, 1);
    if (order == AZ_TIME_INVALID || order != AZ_TIME_AFTER ||
        (loop->working_in.rate_hz && loop->working_in.rate_hz != rate) ||
        (deck->sample_rate && deck->sample_rate != rate) ||
        !time_ms(in, &in_ms) || !time_ms(out, &out_ms) || out_ms - in_ms < 125) {
        deck->loop_availability = AZ_AVAIL_FAILED;
        return;
    }
    deck->sample_rate = rate;
    deck->loop = (an_loop_t){.valid = 1, .in_ms = in_ms, .out_ms = out_ms, .len_ms = out_ms - in_ms};
}

unsigned an_deck_update(an_deck_status_t decks[AN_DECKS], const az_deck_status_event_v1 *e)
{
    const az_deck_status_view_v1 *v = az_data_event_view(e, AZ_STATUS_TOPIC(AZ_STATUS_LOAD), sizeof *v);
    if (!v || e->deck >= AN_DECKS || !az_data_covers(e, sizeof *e)) return 0;
    an_deck_status_t *deck = &decks[e->deck];
    unsigned changed = AN_DECK_UPDATED;
    if (e->load_epoch != deck->epoch) {
        *deck = (an_deck_status_t){.epoch = e->load_epoch};
        changed |= AN_DECK_REPLACED;
    }
    const az_status_load_v1 *load = v->load;
    int rebuild = !deck->loaded || e->kind != AZ_STATUS_CHANGE;
    deck->loaded = e->load_epoch && TOPIC_PRESENT(load) &&
                   unit_or_load_ready(e, &load->header, AZ_STATUS_LOAD_STATE, &load->state_info) &&
                   load->state == AZ_STATUS_LOAD_LOADED;
    if (!deck->loaded) {
        deck->loop_engaged = 0;
        deck->loop = (an_loop_t){0};
        return changed;
    }
    if (rebuild || (e->changed_topics & MAP_TOPICS)) {
        update_maps(deck, e, v);
        changed |= AN_DECK_ANALYSIS;
    }
    if (rebuild || (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_CUES))) {
        update_cues(deck, e, v->cues);
        changed |= AN_DECK_ANALYSIS;
    }
    update_transport(deck, e, v);
    update_loop(deck, e);
    return changed;
}
