/* SPDX-License-Identifier: GPL-3.0-only */
#include "deck-mcp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

struct topic {
    size_t offset;
    unsigned id;
};
#define TOPIC(member, id) { offsetof(az_deck_status_view_v1, member), id }
enum { LOAD_GROUP, METADATA_GROUP, KEY_GROUP, TRANSPORT_GROUP, POSITION_GROUP,
       TEMPO_GROUP, LOOP_GROUP, SYNC_GROUP, SETTINGS_GROUP, PHASE_GROUP, STEMS_GROUP, CUES_GROUP };
static const struct topic topics[] = {
    TOPIC(load, AZ_STATUS_LOAD), TOPIC(metadata, AZ_STATUS_METADATA), TOPIC(key, AZ_STATUS_KEY),
    TOPIC(transport, AZ_STATUS_TRANSPORT), TOPIC(position, AZ_STATUS_POSITION),
    TOPIC(tempo, AZ_STATUS_TEMPO), TOPIC(loop, AZ_STATUS_LOOP), TOPIC(sync, AZ_STATUS_SYNC),
    TOPIC(settings, AZ_STATUS_SETTINGS), TOPIC(phase, AZ_STATUS_PHASE),
    TOPIC(stems, AZ_STATUS_STEMS), TOPIC(cues, AZ_STATUS_CUES)
};
#undef TOPIC

struct field {
    const char *name;
    unsigned topic;
    enum mcp_value_kind kind;
    size_t value, receipt;
};
#define FIELD(label, group, type, member, kind) \
    { label, group, kind, offsetof(type, member), offsetof(type, member ## _info) }
#define LOAD(label, member) FIELD(label, LOAD_GROUP, az_status_load_v1, member, MCP_INT)
#define META(label, member, kind) FIELD(label, METADATA_GROUP, az_status_metadata_v1, member, kind)
#define KEY(label, member, kind) FIELD(label, KEY_GROUP, az_status_key_v1, member, kind)
#define TRANSPORT_FIELD(label, member, kind) FIELD(label, TRANSPORT_GROUP, az_status_transport_v1, member, kind)
#define POS(label, member, kind) FIELD(label, POSITION_GROUP, az_status_position_v1, member, kind)
#define TEMPO(label, member, kind) FIELD(label, TEMPO_GROUP, az_status_tempo_v1, member, kind)
#define LOOP(label, member, kind) FIELD(label, LOOP_GROUP, az_status_loop_v1, member, kind)
#define SYNC(label, member) FIELD(label, SYNC_GROUP, az_status_sync_v1, member, MCP_INT)
#define SETTINGS(label, member) FIELD(label, SETTINGS_GROUP, az_status_settings_v1, member, MCP_INT)
#define PHASE(label, member) FIELD(label, PHASE_GROUP, az_status_phase_v1, member, MCP_NUM)
#define STEMS(label, member) FIELD(label, STEMS_GROUP, az_status_stems_v1, member, MCP_INT)
static const struct field fields[MCP_FIELDS] = {
    [LOAD_STATE] = LOAD("loadState", state),
    [LOAD_SOURCE] = LOAD("source", source),
    [TITLE] = META("title", title, MCP_TEXT),
    [ARTIST] = META("artist", artist, MCP_TEXT),
    [ALBUM] = META("album", album, MCP_TEXT),
    [DURATION] = META("durationSeconds", duration_s, MCP_NUM),
    [ORIGINAL_KEY] = KEY("originalKey", original, MCP_TEXT),
    [EFFECTIVE_KEY] = KEY("effectiveKey", effective, MCP_TEXT),
    [KEY_SHIFT] = KEY("keyShiftSemitones", shift_semitones, MCP_INT),
    [TRANSPORT] = TRANSPORT_FIELD("transport", state, MCP_INT),
    [DIRECTION] = TRANSPORT_FIELD("direction", direction, MCP_INT),
    [PLAYBACK_RATE] = TRANSPORT_FIELD("playbackRate", rate, MCP_NUM),
    [HEAD] = POS("head", head, MCP_COORD),
    [SLIP_HEAD] = POS("slipHead", slip_head, MCP_COORD),
    [REMAINING] = POS("remainingSeconds", remaining_s, MCP_NUM),
    [BPM] = TEMPO("trackBpm", original_bpm, MCP_NUM),
    [CURRENT_BPM] = TEMPO("currentBpm", current_bpm, MCP_NUM),
    [TEMPO_SLIDER] = TEMPO("tempoSliderPercent", slider_percent, MCP_NUM),
    [TEMPO_LOCKED] = TEMPO("tempoSliderLocked", slider_locked, MCP_INT),
    [LOOP_DEFINED] = LOOP("loopDefined", defined, MCP_INT),
    [LOOP_ENGAGED] = LOOP("loopEngaged", engaged, MCP_INT),
    [LOOP_IN] = LOOP("retainedLoopIn", retained_in, MCP_COORD),
    [LOOP_OUT] = LOOP("retainedLoopOut", retained_out, MCP_COORD),
    [WORKING_IN] = LOOP("workingLoopIn", working_in, MCP_COORD),
    [WORKING_OUT] = LOOP("workingLoopOut", working_out, MCP_COORD),
    [RELOOP] = LOOP("reloopAvailable", reloop_available, MCP_INT),
    [LOOP_MODE] = LOOP("loopMode", mode, MCP_INT),
    [SYNC_ENABLED] = SYNC("syncEnabled", enabled),
    [SYNC_LOCKED] = SYNC("syncLocked", locked),
    [SYNC_FOLLOWING] = SYNC("syncFollowing", following),
    [SYNC_MASTER] = SYNC("syncMaster", master),
    [MASTER_TEMPO] = SETTINGS("masterTempo", master_tempo),
    [KEY_LOCK] = SETTINGS("keyLock", key_lock),
    [SLIP] = SETTINGS("slip", slip),
    [QUANTIZE] = SETTINGS("quantize", quantize),
    [PAD_MODE] = SETTINGS("padMode", pad_mode),
    [BEAT_PHASE] = PHASE("beatPhase", beat),
    [BAR_PHASE] = PHASE("barPhase", bar),
    [STEMS_AVAILABLE] = STEMS("stemsAvailable", available),
    [STEMS_ENABLED] = STEMS("stemsEnabled", enabled)
};

const char *const mcp_cue_names[MCP_CUE_BANKS] = {
    "main", "liveHot", "liveMemory", "savedHot", "savedMemory"
};
static const struct field cue_fields[MCP_CUE_BANKS] = {
    FIELD("main", CUES_GROUP, az_status_cues_v1, main, MCP_INT),
    FIELD("liveHot", CUES_GROUP, az_status_cues_v1, live_hot, MCP_INT),
    FIELD("liveMemory", CUES_GROUP, az_status_cues_v1, live_memory, MCP_INT),
    FIELD("savedHot", CUES_GROUP, az_status_cues_v1, saved_hot, MCP_INT),
    FIELD("savedMemory", CUES_GROUP, az_status_cues_v1, saved_memory, MCP_INT)
};

uint64_t mcp_watch_topics(void)
{
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof topics / sizeof topics[0]; ++i)
        mask |= AZ_STATUS_TOPIC(topics[i].id);
    return mask | AZ_STATUS_TOPIC(AZ_STATUS_GRID) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASES);
}

static const az_status_topic_v1 *read_topic(const az_deck_status_view_v1 *view, unsigned group)
{
    const az_status_topic_v1 *header;
    size_t offset = topics[group].offset;
    if (view->size < offset + sizeof header)
        return NULL;
    memcpy(&header, (const char *)view + offset, sizeof header);
    return header;
}

static az_status_field_info_v1 read_receipt(const az_status_topic_v1 *header, size_t offset)
{
    az_status_field_info_v1 receipt = {0};
    if (header && header->size >= offset + sizeof receipt)
        memcpy(&receipt, (const char *)header + offset, sizeof receipt);
    return receipt;
}

static void copy_value(struct mcp_value *out, const void *value, enum mcp_value_kind kind)
{
    const char *text;
    switch (kind) {
    case MCP_INT: memcpy(&out->value.integer, value, sizeof out->value.integer); break;
    case MCP_NUM: memcpy(&out->value.number, value, sizeof out->value.number); break;
    case MCP_COORD: memcpy(&out->value.coord, value, sizeof out->value.coord); break;
    case MCP_TEXT:
        memcpy(&text, value, sizeof text);
        if (text)
            snprintf(out->value.text, sizeof out->value.text, "%.*s",
                     (int)sizeof out->value.text - 1, text);
        break;
    }
}

static void copy_field(struct mcp_value *out, const struct field *field,
                       const az_deck_status_view_v1 *view)
{
    const az_status_topic_v1 *header = read_topic(view, field->topic);
    out->receipt = read_receipt(header, field->receipt);
    if (out->receipt.info.availability == AZ_AVAIL_READY)
        copy_value(out, (const char *)header + field->value, field->kind);
}

static void copy_cues(struct mcp_cues *out, const struct field *field,
                      const az_status_topic_v1 *header)
{
    const az_cue_entry *entries;
    out->receipt = read_receipt(header, field->receipt);
    if (out->receipt.info.availability != AZ_AVAIL_READY)
        return;
    memcpy(&entries, (const char *)header + field->value, sizeof entries);
    if (!entries)
        return;
    out->count = out->receipt.count <= AZ_NATIVE_CUE_LIMIT ? out->receipt.count : AZ_NATIVE_CUE_LIMIT;
    memcpy(out->entries, entries, out->count * sizeof *entries);
}

static void copy_track(struct mcp_deck *deck, const az_deck_status_view_v1 *view)
{
    const az_status_topic_v1 *header = read_topic(view, LOAD_GROUP);
    const az_status_load_v1 *load = (const az_status_load_v1 *)header;
    const az_status_field_info_v1 receipt = read_receipt(header, offsetof(az_status_load_v1, track_info));
    if (deck->load_epoch && receipt.info.availability == AZ_AVAIL_READY &&
        receipt.scope == AZ_STATUS_LOAD_SCOPE && receipt.scope_id == deck->load_epoch)
        deck->track = load->track;
}

static void copy_snapshot(struct mcp_deck *deck, const az_deck_status_event_v1 *event)
{
    *deck = (struct mcp_deck){ .sequence = event->sequence, .load_epoch = event->load_epoch,
                              .observed_ns = event->observed_ns, .lost_topics = event->lost_topics };
    const az_status_topic_v1 *cues = read_topic(event->view, CUES_GROUP);
    for (unsigned i = 0; i < MCP_FIELDS; ++i)
        copy_field(&deck->fields[i], &fields[i], event->view);
    for (unsigned i = 0; i < MCP_CUE_BANKS; ++i)
        copy_cues(&deck->cues[i], &cue_fields[i], cues);
    copy_track(deck, event->view);
}

void mcp_publish(struct mcp_runtime *runtime)
{
    if (!runtime->pending || pthread_mutex_trylock(&runtime->lock))
        return;
    for (unsigned i = 0; i < MCP_DECKS; ++i)
        if (runtime->pending & (1u << i))
            runtime->published[i] = runtime->ui[i];
    runtime->pending = 0;
    mcp_live_publish();
    pthread_mutex_unlock(&runtime->lock);
}

void mcp_capture(const az_deck_status_event_v1 *event, void *user)
{
    struct mcp_runtime *runtime = user;
    if (!event || event->size < offsetof(az_deck_status_event_v1, occurrence) ||
        event->deck >= MCP_DECKS || !event->view || atomic_load(&runtime->stopping))
        return;
    copy_snapshot(&runtime->ui[event->deck], event);
    mcp_live_capture(event);
    runtime->pending |= 1u << event->deck;
    mcp_publish(runtime);
}

void mcp_set(cmcp_json_t *object, const char *key, cmcp_json_t *value)
{
    if (cmcp_json_object_set(object, key, value) != 0)
        cmcp_json_free(value);
}

cmcp_json_t *mcp_id(uint64_t value)
{
    char text[24];
    snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    return cmcp_json_new_string(text);
}

static cmcp_json_t *number(double value)
{
    return isfinite(value) ? cmcp_json_new_double(value) : cmcp_json_new_null();
}

static cmcp_json_t *coordinate(const az_status_coord_v1 *value)
{
    char samples[24];
    snprintf(samples, sizeof samples, "%lld", (long long)value->samples);
    cmcp_json_t *out = cmcp_json_new_object();
    mcp_set(out, "seconds", number(value->seconds));
    mcp_set(out, "samples", value->rate_hz ? cmcp_json_new_string(samples) : cmcp_json_new_null());
    mcp_set(out, "sampleRateHz", cmcp_json_new_int(value->rate_hz));
    return out;
}

static cmcp_json_t *bounded_text(const char *text, size_t capacity)
{
    return cmcp_json_new_string_n(text, mcp_utf8_prefix(text, strnlen(text, capacity)));
}

static cmcp_json_t *field_value(const struct mcp_value *value, enum mcp_value_kind kind)
{
    switch (kind) {
    case MCP_INT: return cmcp_json_new_int(value->value.integer);
    case MCP_NUM: return number(value->value.number);
    case MCP_COORD: return coordinate(&value->value.coord);
    case MCP_TEXT: return bounded_text(value->value.text, sizeof value->value.text);
    }
    return NULL;
}

cmcp_json_t *mcp_receipt_json(const az_status_field_info_v1 *receipt)
{
    static const char *const availability[] = { "unsupported", "loading", "ready", "empty", "failed", "stale" };
    const unsigned status = receipt->info.availability;
    cmcp_json_t *out = cmcp_json_new_object();
    mcp_set(out, "availability", cmcp_json_new_string(status < sizeof availability / sizeof availability[0]
                                                       ? availability[status] : "unknown"));
    mcp_set(out, "provenance", cmcp_json_new_int(receipt->info.provenance));
    mcp_set(out, "revision", mcp_id(receipt->info.revision));
    mcp_set(out, "observedNs", mcp_id(receipt->info.observed_ns));
    mcp_set(out, "providerId", mcp_id(receipt->provider_id));
    mcp_set(out, "scopeId", mcp_id(receipt->scope_id));
    return out;
}

static cmcp_json_t *track_json(const az_track_ref *track)
{
    if (!track->size)
        return cmcp_json_new_null();
    cmcp_json_t *out = cmcp_json_new_object();
    mcp_set(out, "sourceId", mcp_id(track->source_id));
    mcp_set(out, "sourceGeneration", mcp_id(track->source_generation));
    mcp_set(out, "libraryId", mcp_id(track->library_id));
    mcp_set(out, "trackId", mcp_id(track->track_id));
    return out;
}

static cmcp_json_t *deck_header(const struct mcp_deck *deck, int number)
{
    cmcp_json_t *out = cmcp_json_new_object();
    mcp_set(out, "deck", cmcp_json_new_int(number));
    mcp_set(out, "observed", cmcp_json_new_bool(deck->sequence != 0));
    mcp_set(out, "sequence", mcp_id(deck->sequence));
    mcp_set(out, "loadEpoch", mcp_id(deck->load_epoch));
    mcp_set(out, "observedNs", mcp_id(deck->observed_ns));
    return out;
}

cmcp_json_t *mcp_deck_json(const struct mcp_deck *deck, int number)
{
    cmcp_json_t *out = deck_header(deck, number);
    cmcp_json_t *values = cmcp_json_new_object();
    mcp_set(out, "lostTopics", mcp_id(deck->lost_topics));
    mcp_set(out, "track", track_json(&deck->track));
    for (unsigned i = 0; i < MCP_FIELDS; ++i) {
        const struct mcp_value *field = &deck->fields[i];
        cmcp_json_t *entry = mcp_receipt_json(&field->receipt);
        if (field->receipt.info.availability == AZ_AVAIL_READY)
            mcp_set(entry, "value", field_value(field, fields[i].kind));
        mcp_set(values, fields[i].name, entry);
    }
    mcp_set(out, "fields", values);
    return out;
}

static void cue_details(cmcp_json_t *out, const az_cue_entry *cue)
{
    if (cue->valid & AZ_CUE_COLOR)
        mcp_set(out, "rgb", cmcp_json_new_int(cue->rgb));
    if (cue->valid & AZ_CUE_COLOR_ID)
        mcp_set(out, "colorId", cmcp_json_new_int(cue->color_id));
    if (cue->valid & AZ_CUE_COMMENT)
        mcp_set(out, "comment", bounded_text(cue->comment, sizeof cue->comment));
    if (cue->valid & AZ_CUE_AUTO_ACTIVE)
        mcp_set(out, "autoActive", cmcp_json_new_bool(cue->auto_active));
    if (cue->valid & AZ_CUE_SAVED) {
        mcp_set(out, "saved", cmcp_json_new_bool(cue->saved));
        mcp_set(out, "modified", cmcp_json_new_int(cue->modified));
    }
}

cmcp_json_t *mcp_cue_entry_json(const az_cue_entry *cue)
{
    cmcp_json_t *out = cmcp_json_new_object();
    mcp_set(out, "entryId", cmcp_json_new_int(cue->entry_id));
    mcp_set(out, "slot", cmcp_json_new_int(cue->slot));
    mcp_set(out, "type", cmcp_json_new_int(cue->type));
    mcp_set(out, "classification", cmcp_json_new_int(cue->classification));
    if (cue->valid & AZ_CUE_START)
        mcp_set(out, "startSeconds", number(cue->start_s));
    if (cue->valid & AZ_CUE_END)
        mcp_set(out, "endSeconds", number(cue->end_s));
    cue_details(out, cue);
    return out;
}

cmcp_json_t *mcp_cues_json(const struct mcp_deck *deck, int bank, size_t offset, size_t limit)
{
    const struct mcp_cues *cues = &deck->cues[bank];
    cmcp_json_t *out = mcp_receipt_json(&cues->receipt);
    cmcp_json_t *entries = cmcp_json_new_array();
    size_t end = offset + limit < cues->count ? offset + limit : cues->count;
    for (size_t i = offset; i < end; ++i) {
        cmcp_json_t *entry = mcp_cue_entry_json(&cues->entries[i]);
        if (cmcp_json_array_append(entries, entry) != 0)
            cmcp_json_free(entry);
    }
    mcp_set(out, "bank", cmcp_json_new_string(mcp_cue_names[bank]));
    mcp_set(out, "count", cmcp_json_new_int(cues->receipt.count));
    mcp_set(out, "loadEpoch", mcp_id(deck->load_epoch));
    mcp_set(out, "sequence", mcp_id(deck->sequence));
    mcp_set(out, "entries", entries);
    mcp_set(out, "nextOffset", end < cues->count ? cmcp_json_new_int((long long)end) : cmcp_json_new_null());
    mcp_set(out, "truncated", cmcp_json_new_bool(cues->count < cues->receipt.count));
    return out;
}
