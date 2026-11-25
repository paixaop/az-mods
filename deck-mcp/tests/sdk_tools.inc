/* SPDX-License-Identifier: GPL-3.0-only */
static pthread_t library_worker;
static int setting_value;
static unsigned panel_presses, batch_submissions;
static void (*confirmation)(int, void *);
static void *confirmation_user;

static int official_read(uint32_t index, az_official_setting_v1 *out)
{
    if (index == 0)
        return AZ_EMPTY;
    if (index != 1)
        return AZ_NOT_FOUND;
    *out = (az_official_setting_v1){ .size = sizeof *out, .version = AZ_DATA_VERSION,
        .kind = AZ_ROW_TOGGLE, .available = 1, .configured = setting_value, .effective = setting_value,
        .key = "test-setting", .label = "Test Setting" };
    return AZ_OK;
}

static int official_write(const char *id, const char *key, int expected, int desired)
{
    assert(!strcmp(id, MCP_ID) && !strcmp(key, "test-setting"));
    if (expected != setting_value)
        return AZ_STALE;
    setting_value = desired;
    return AZ_OK;
}

static int deck_view(void) { return AZ_VIEW_DECK; }
static int visible_decks(void) { return 4; }
static int theme(void) { return 2; }

static int button_press(const char *id, uint32_t button, uint32_t hold, uint64_t *token)
{
    assert(!strcmp(id, MCP_ID) && button == AZ_PANEL_BROWSE && hold == 80);
    *token = ++panel_presses;
    return AZ_OK;
}

static int button_poll(const char *id, uint64_t token)
{
    assert(!strcmp(id, MCP_ID));
    return token == panel_presses ? AZ_OK : AZ_STALE;
}

static int native_bank(int deck, az_native_cue_snapshot *out)
{
    *out = (az_native_cue_snapshot){ .size = sizeof *out, .version = AZ_DATA_VERSION,
        .deck = deck, .count = 1, .native_load_generation = epochs[deck],
        .revision = 17, .source_rate_hz = 48000,
        .entries = { { .entry_id = 1, .classification = AZ_CUE_HOT, .slot = 1,
                      .type = AZ_CUE_POINT, .valid = AZ_CUE_START, .start_s = 4 } } };
    return AZ_OK;
}

static int confirm(const char *id, const char *title, const char *message, const char *yes,
                   void (*callback)(int, void *), void *user)
{
    assert(!strcmp(id, MCP_ID) && title[0] && message[0] && yes[0] && callback);
    confirmation = callback;
    confirmation_user = user;
    return AZ_OK;
}

static int submit_batch(const char *id, const az_cue_batch_request *request,
                        az_command_completion_fn callback, void *user, az_command_result *out)
{
    assert(!strcmp(id, MCP_ID) && request->request_id > last_request && out->size == sizeof *out);
    assert(request->mode == AZ_CUE_BATCH_REPLACE && request->count == 1 && request->expected_revision == 17);
    assert(request->entries[0].classification == AZ_CUE_HOT && request->entries[0].slot == 6);
    complete_command = callback;
    completion_user = user;
    last_request = request->request_id;
    ++batch_submissions;
    return AZ_OK;
}

static int lib_playlists(const char *id, int stick, int (*fn)(int, int, const char *, int, void *), void *user)
{
    assert(!strcmp(id, MCP_ID) && stick == 0);
    (void)fn(91, 0, "Fixture Playlist", 0, user);
    return AZ_OK;
}

static int lib_tracks(const char *id, int stick, int playlist, int (*fn)(const az_track *, void *), void *user)
{
    assert(!strcmp(id, MCP_ID) && stick == 0 && playlist == 0);
    az_track track = { .size = sizeof track, .id = 42, .title = "Jazz", .artist = "Fixture",
        .key = "Am", .path = "/private path", .bpm = 120, .len_s = 180 };
    (void)fn(&track, user);
    return AZ_OK;
}

static az_track_ref library_ref(void)
{
    return (az_track_ref){ .size = sizeof(az_track_ref), .version = AZ_DATA_VERSION,
        .track_id = 42, .source_id = 1, .source_generation = 5, .library_id = 1 };
}

static int track_resolve(const char *id, int stick, uint32_t track, az_track_ref *out)
{
    assert(!strcmp(id, MCP_ID) && stick == 0 && track == 42);
    *out = library_ref();
    return AZ_OK;
}

static int track_request(const char *id, const az_track_ref *track, uint32_t flags, uint64_t *ticket)
{
    assert(!strcmp(id, MCP_ID) && track->track_id == 42 && flags);
    *ticket = 44;
    return AZ_OK;
}

static int track_acquire(const char *id, uint64_t ticket, const az_track_snapshot **out)
{
    static const az_beat beat[] = { { .time_s = 1, .bpm = 120, .beat_in_bar = 1 } };
    static const az_phrase phrase[] = { { .start_beat = 0, .end_beat = 1 } };
    static const az_cue_entry cue[] = { { .entry_id = 1 } };
    static const uint8_t wave[] = { 2, 3, 4 };
    static const az_track_snapshot value = { .size = sizeof value, .version = AZ_DATA_VERSION,
        .grid_info = { .availability = AZ_AVAIL_READY, .revision = 9 }, .beats = beat, .beat_count = 1,
        .metadata_info = { .availability = AZ_AVAIL_READY },
        .metadata = { .valid = AZ_META_TITLE | AZ_META_ARTIST | AZ_META_ALBUM | AZ_META_KEY | AZ_META_DURATION | AZ_META_TEMPO,
                      .title = "Jazz", .artist = "Fixture", .album = "Test", .key = "Am",
                      .duration_s = 180, .original_bpm = 120 },
        .cues_info = { .availability = AZ_AVAIL_READY }, .cues = cue, .cue_count = 1,
        .phrases_info = { .availability = AZ_AVAIL_READY }, .phrases = phrase, .phrase_count = 1,
        .waveform_info = { .availability = AZ_AVAIL_READY },
        .waveform = { .samples = wave, .sample_count = sizeof wave } };
    assert(!strcmp(id, MCP_ID) && ticket == 44);
    *out = &value;
    return AZ_OK;
}

static int ticket_release(const char *id, uint64_t ticket)
{
    assert(!strcmp(id, MCP_ID) && (ticket == 44 || ticket == 45));
    return AZ_OK;
}

static int wave_request(const char *id, const az_track_ref *track, const az_wave_query *query, uint64_t *ticket)
{
    assert(!strcmp(id, MCP_ID) && track->track_id == 42 && query->encoding == 1 && query->column_count == 3);
    *ticket = 45;
    return AZ_OK;
}

static int wave_acquire(const char *id, uint64_t ticket, const az_wave_snapshot **out)
{
    static const uint8_t bytes[] = { 1, 2, 3 };
    static const az_wave_snapshot wave = { .size = sizeof wave, .version = AZ_DATA_VERSION,
        .encoding = 1, .column_count = 3, .total_columns = 3, .bytes_per_column = 1,
        .info = { .availability = AZ_AVAIL_READY }, .columns = bytes, .data_bytes = sizeof bytes };
    assert(!strcmp(id, MCP_ID) && ticket == 45);
    *out = &wave;
    return AZ_OK;
}

static int lib_order(const char *id, int stick, uint32_t playlist, uint32_t offset,
                      az_playlist_entry_v1 *rows, uint32_t capacity, az_playlist_order_v1 *out)
{
    assert(!strcmp(id, MCP_ID) && stick == 0 && playlist == 5 && capacity);
    assert(out->size == sizeof *out && out->version == AZ_DATA_VERSION);
    out->source_generation = 17;
    out->library_id = 18;
    out->revision = 19;
    out->total = 3;
    for (uint32_t i = offset; i < out->total && out->returned < capacity; ++i)
        rows[out->returned++] = (az_playlist_entry_v1){ 100 + i, 23, 1 + i };
    return AZ_OK;
}

static int lib_move(const char *id, const az_playlist_move_v1 *move, uint64_t *receipt)
{
    assert(!strcmp(id, MCP_ID) && move->size == sizeof *move && move->version == AZ_DATA_VERSION);
    assert(move->stick == 0 && move->playlist_id == 5 && move->from == 2 && move->to == 3);
    assert(move->source_generation == 17 && move->library_id == 18 && move->revision == 19);
    assert(move->entry_id == 101);
    *receipt = 42;
    return AZ_PENDING;
}

static int lib_move_status(const char *id, uint64_t receipt, az_playlist_move_result_v1 *out)
{
    assert(!strcmp(id, MCP_ID) && receipt == 42 && out->size == sizeof *out && out->version == AZ_DATA_VERSION);
    out->status = AZ_OK;
    out->outcome = AZ_PLAYLIST_MOVED;
    out->device_library = AZ_PDB_FAILED;
    return AZ_OK;
}

static const az_library_v1 *library_api(uint32_t abi)
{
    static const az_library_v1 api = { .abi = AZ_LIBRARY_ABI, .size = sizeof api,
        .lib_playlists = lib_playlists, .lib_tracks = lib_tracks, .track_resolve = track_resolve,
        .track_request = track_request, .track_acquire = track_acquire, .track_release = ticket_release,
        .track_cancel = ticket_release, .waveform_request = wave_request, .waveform_acquire = wave_acquire,
        .playlist_order = lib_order, .playlist_move = lib_move, .playlist_move_status = lib_move_status };
    return abi == api.abi ? &api : NULL;
}

static int vocal_intervals(const char *id, const az_track_ref *track, az_vocal_snapshot_v1 *out,
                            az_vocal_interval *rows, uint32_t max)
{
    assert(!strcmp(id, MCP_ID) && track->track_id == 42);
    out->interval_count = 1;
    out->info.revision = 31;
    if (!max)
        return AZ_LIMIT;
    rows[0] = (az_vocal_interval){ .start_s = 1, .stop_s = 2 };
    return AZ_OK;
}

static int vocal_loaded(const char *id, int deck, uint64_t epoch, az_vocal_snapshot_v1 *out,
                         az_vocal_interval *rows, uint32_t max)
{
    if (epochs[deck] != epoch)
        return AZ_STALE;
    az_track_ref track = library_ref();
    return vocal_intervals(id, &track, out, rows, max);
}

static int analysis_request(const char *id, const az_analysis_request_v1 *request, uint64_t *job)
{
    assert(!strcmp(id, MCP_ID) && request->load_epoch == 106 && request->deck == 3);
    *job = 91;
    return AZ_OK;
}

static int analysis_status(const char *id, uint64_t job, az_analysis_status_v1 *out)
{
    assert(!strcmp(id, MCP_ID) && job == 91);
    out->state = AZ_ANALYSIS_COMPLETE;
    return AZ_OK;
}

static int analysis_result(const char *id, uint64_t job, az_analysis_result_v1 *out,
                            az_analysis_phrase_v1 *phrases, uint32_t p, az_vocal_frame_v1 *vocals,
                            uint32_t v)
{
    assert(!strcmp(id, MCP_ID) && job == 91);
    out->phrases.count = out->vocals.count = 1;
    if (!p || !v)
        return AZ_LIMIT;
    phrases[0].start_beat = 7;
    vocals[0].activity = 3;
    return AZ_OK;
}

static int analysis_control(const char *id, uint64_t job)
{
    assert(!strcmp(id, MCP_ID) && job == 91);
    return AZ_OK;
}

static const az_analysis_v1 *analysis_api(uint32_t abi)
{
    static const az_analysis_v1 api = { .abi = AZ_ANALYSIS_ABI, .size = sizeof api,
        .vocal_intervals_loaded = vocal_loaded, .vocal_intervals = vocal_intervals,
        .request = analysis_request, .status = analysis_status, .result = analysis_result,
        .cancel = analysis_control, .release = analysis_control };
    return abi == api.abi ? &api : NULL;
}
