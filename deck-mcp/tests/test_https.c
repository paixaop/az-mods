/* SPDX-License-Identifier: GPL-3.0-only */
#include "deck-mcp.h"
#include "http.h"
#if !defined(MCP_TEST_SHARED) || defined(MCP_TEST_PROVIDER_CLIENT)
#include "provider/az-mcp-provider.h"
#endif

#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#ifdef MCP_TEST_SHARED
#include <dlfcn.h>
#endif
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LEGACY_HEADERS "MCP-Protocol-Version: 2025-11-25"
#define MODERN_HEADERS "MCP-Protocol-Version: 2026-07-28"
#define META "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\",\"io.modelcontextprotocol/clientCapabilities\":{}}"
#define REQ(method, params) "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"" method "\",\"params\":" params "}"
#define INIT REQ("initialize", "{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}")
#define DISCOVER REQ("server/discover", "{" META "}")
#define TOOLS REQ("tools/list", "{}")
#define CALL(name, args) REQ("tools/call", "{\"name\":\"" name "\",\"arguments\":" args "}")
#define TEST_TOKEN "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static const char *fixture;
static unsigned port;
static char fixture_address[16] = "127.0.0.1";
static const char *fixture_certificate = "server.crt";
static az_deck_status_fn observe;
static void *observation_user;
static pthread_t worker;
static atomic_int serving;
static unsigned releases, submissions, acknowledgements, cancellations;
static az_command_completion_fn complete_command;
static void *completion_user;
static uint64_t last_request;
static void set_arming(const char *arm, const char *disarm, bool enabled);
static uint64_t epochs[MCP_DECKS];
static bool cancel_busy;
static const az_screen_v1 *token_screen;
static char drawn_token[MCP_SECRET_SIZE];
static char drawn_url[MCP_URL_SIZE];
static char drawn_pair_url[MCP_URL_SIZE], drawn_code[MCP_PAIR_CODE_SIZE];
static char drawn_fingerprint[MCP_FINGERPRINT_SIZE];
static bool drawn_trust_note, drawn_harness_label, drawn_token_note, drawn_qr, drawn_save_error;
static bool drawn_pair_used;
static unsigned drawn_token_lines;
static struct drawn_button {
    const char *label;
    int x, y;
    bool visible;
} buttons[] = {
    { .label = "Pair with Codex" }, { .label = "New setup code" },
    { .label = "Show token and manual setup" }, { .label = "Reset token" }, { .label = "Back" },
    { .label = "Arm live controls" }, { .label = "Disarm live controls" },
    { .label = "Arm saved edits" }, { .label = "Disarm saved edits" }
};
static unsigned redraws;


#include "sdk_tools.inc"

static void pause_ms(long ms)
{
    struct timespec delay = { .tv_nsec = ms * 1000000 };
    nanosleep(&delay, NULL);
}

static void fake_log(const char *id, const char *fmt, ...)
{
    assert(!strcmp(id, MCP_ID));
    if (strstr(fmt, "Serving"))
        atomic_store(&serving, 1);
    else
        atomic_store(&serving, -1);
}

static int start(const char *id, const char *name, void *(*fn)(void *), void *user)
{
    assert(!strcmp(id, MCP_ID));
    if (!strcmp(name, "deck-library"))
        return pthread_create(&library_worker, NULL, fn, user);
    assert(!strcmp(name, "deck-mcp"));
    return pthread_create(&worker, NULL, fn, user);
}

static const char *data_dir(const char *id)
{
    assert(!strcmp(id, MCP_ID));
    return fixture;
}

static int watch(const char *id, const az_deck_watch_v1 *query, az_deck_status_fn fn,
                 void *user, az_deck_subscription *subscription)
{
    assert(!strcmp(id, MCP_ID) && query->deck_mask == AZ_STATUS_ALL_DECKS && query->position_hz == 5);
    assert(query->topics & AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
    observe = fn;
    observation_user = user;
    *subscription = 1;
    return AZ_OK;
}

static int release(const char *id, az_deck_subscription subscription)
{
    assert(!strcmp(id, MCP_ID) && subscription == 1);
    ++releases;
    return AZ_OK;
}

static int target(int deck, uint64_t epoch, az_command_target_v1 *out)
{
    assert(deck >= 0 && deck < MCP_DECKS);
    if (epoch != epochs[deck])
        return AZ_STALE;
    out->version = AZ_DATA_VERSION;
    out->commands = (UINT64_C(1) << AZ_CMD_LOOP_EXIT) | (UINT64_C(1) << AZ_CMD_HOT_TRIGGER) |
                    (UINT64_C(1) << AZ_CMD_LOOP_LENGTH) | (UINT64_C(1) << AZ_CMD_LOOP_ADJUST_OUT) |
                    (UINT64_C(1) << AZ_CMD_CUE_POINT_SET);
    out->cues.revision = 17;
    out->cues.native_load_generation = epoch;
    return AZ_OK;
}

static int submit_command(const char *id, const az_command_request *request,
                          az_command_completion_fn callback, void *user, az_command_result *out)
{
    assert(!strcmp(id, MCP_ID) && request->request_id > last_request && out->size == sizeof *out);
    assert(request->quantization == AZ_QUANTIZE_NATIVE);
    if (request->operation == AZ_CMD_CUE_POINT_SET) {
        assert(request->flags == (AZ_COMMAND_NATIVE_LOAD | AZ_COMMAND_NATIVE_CUE_IDS | AZ_COMMAND_PERSIST));
        assert(request->classification == AZ_CUE_HOT && request->expected_revision == 17 && request->entry_id == 5);
    } else
        assert(request->flags == AZ_COMMAND_NATIVE_LOAD);
    assert(request->expected_load_generation == epochs[request->deck] && request->deadline_ns);
    if (request->operation == AZ_CMD_HOT_TRIGGER)
        assert(request->entry_id == 8);
    if (request->operation == AZ_CMD_LOOP_LENGTH)
        assert(request->beats == 1.5);
    if (request->operation == AZ_CMD_LOOP_ADJUST_OUT)
        assert(request->end_s == -0.125);
    complete_command = callback;
    completion_user = user;
    last_request = request->request_id;
    ++submissions;
    /* The mod must wait for the terminal callback even with an incomplete reply. */
    return AZ_OK;
}

static int acknowledge(const char *id, uint64_t request_id)
{
    assert(!strcmp(id, MCP_ID) && request_id == last_request);
    ++acknowledgements;
    return AZ_OK;
}

static int cancel(const char *id, uint64_t request_id, az_command_result *out)
{
    assert(!strcmp(id, MCP_ID) && request_id == last_request && out->size == sizeof *out);
    ++cancellations;
    if (cancel_busy) {
        cancel_busy = false;
        return AZ_BUSY;
    }
    return AZ_PENDING;
}

static const az_commands_v1 *commands(uint32_t abi)
{
    static const az_commands_v1 api = {
        .abi = AZ_COMMANDS_ABI, .size = sizeof api, .command_submit_async = submit_command,
        .command_acknowledge = acknowledge, .command_cancel = cancel, .cue_batch_submit_async = submit_batch
    };
    return abi == api.abi ? &api : NULL;
}

static int register_row(const char *id, const az_row_v1 *row)
{
    (void)id; (void)row;
    assert(!"Deck MCP actions belong on its Settings screen");
    return 0;
}

static int register_screen(const char *id, const az_screen_v1 *page)
{
    assert(!strcmp(id, MCP_ID) && page->size == sizeof *page);
    token_screen = page;
    return 0;
}

static void draw_text(az_draw *draw, int x, int y, const char *text, int px, uint32_t color)
{
    (void)draw; (void)px; (void)color;
    if (!strncmp(text, "https://", 8)) {
        if (strstr(text, "/mcp"))
            snprintf(drawn_url, sizeof drawn_url, "%s", text);
        else
            snprintf(drawn_pair_url, sizeof drawn_pair_url, "%s", text);
    }
    if (!strncmp(text, "Setup code: ", 12))
        snprintf(drawn_code, sizeof drawn_code, "%.*s", MCP_PAIR_CODE_DIGITS, text + 12);
    if (strstr(text, "Connection file sent"))
        drawn_pair_used = true;
    if (strstr(text, MCP_SETUP_CA))
        drawn_trust_note = true;
    if (!strcmp(text, "Harness URL"))
        drawn_harness_label = true;
    if (strstr(text, "join all token lines"))
        drawn_token_note = true;
    if (strstr(text, "previous token still works"))
        drawn_save_error = true;
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; ++i) {
        if (!strcmp(text, buttons[i].label)) {
            buttons[i].x = x;
            buttons[i].y = y;
            buttons[i].visible = true;
        }
    }
    size_t length = strlen(text);
    if (length >= 16 && strspn(text, "0123456789abcdefABCDEF") == length) {
        char *hex = drawn_pair_url[0] ? drawn_fingerprint : drawn_token;
        size_t used = strlen(hex);
        assert(used + length < sizeof drawn_token);
        memcpy(hex + used, text, length + 1);
        if (!drawn_pair_url[0])
            ++drawn_token_lines;
    }
}

static void fill_rect(az_draw *draw, int x, int y, int w, int h, uint32_t color)
{
    (void)draw; (void)x; (void)y;
    if (color == 0xffffff && w == h && w >= 80)
        drawn_qr = true;
}

static uint32_t color(int role) { return (uint32_t)role; }
static int text_width(const char *text, int px) { return (int)strlen(text) * px / 2; }
static void redraw(void) { ++redraws; }

static const az_observation_v1 *observation(uint32_t abi)
{
    static const az_observation_v1 api = {
        .abi = AZ_OBSERVATION_ABI, .size = sizeof api, .on_deck_status = watch, .deck_status_release = release,
        .command_target = target, .native_cues = native_bank
    };
    return abi == api.abi ? &api : NULL;
}

static const az_core_v1 core = {
    .abi = AZ_MOD_ABI, .size = sizeof core, .thread_start = start, .data_dir = data_dir,
    .log = fake_log, .observation_service = observation, .command_service = commands,
    .register_row = register_row, .official_setting_read = official_read, .official_setting_write = official_write,
    .view = deck_view, .deck_count = visible_decks, .ui_theme = theme, .button_press = button_press, .button_poll = button_poll,
    .library_service = library_api, .analysis_service = analysis_api, .confirm = confirm,
    .register_screen = register_screen, .draw_text = draw_text, .fill_rect = fill_rect,
    .color = color, .text_width = text_width, .request_redraw = redraw
};

struct reply {
    size_t length;
    char body[MCP_RESPONSE_LIMIT + 1];
};

static size_t receive(char *data, size_t bytes, size_t count, void *user)
{
    struct reply *reply = user;
    size_t length = bytes * count;
    if (length > MCP_RESPONSE_LIMIT - reply->length)
        return 0;
    memcpy(reply->body + reply->length, data, length);
    reply->length += length;
    reply->body[reply->length] = '\0';
    return length;
}

static cmcp_json_t *request(const char *path, const char *body, const char *authorization,
                            const char *headers[], long expected)
{
    char url[256], ca[512];
    assert(snprintf(url, sizeof url, "https://%s:%u%s", fixture_address, port, path) > 0);
    assert(snprintf(ca, sizeof ca, "%s/%s", fixture, fixture_certificate) > 0);
    CURL *curl = curl_easy_init();
    assert(curl);
    struct curl_slist *list = curl_slist_append(NULL, "Content-Type: application/json");
    list = curl_slist_append(list, "Accept: application/json, text/event-stream");
    if (authorization)
        list = curl_slist_append(list, authorization);
    for (size_t i = 0; headers && headers[i]; ++i)
        list = curl_slist_append(list, headers[i]);
    struct reply *reply = calloc(1, sizeof *reply);
    assert(reply);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2000L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, reply);
    if (body)
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    CURLcode status = curl_easy_perform(curl);
    if (status != CURLE_OK)
        fprintf(stderr, "HTTPS request failed: %s\n", curl_easy_strerror(status));
    assert(status == CURLE_OK);
    long actual = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &actual);
    if (actual != expected)
        fprintf(stderr, "Expected HTTP %ld, got %ld\n", expected, actual);
    assert(actual == expected);
    cmcp_json_t *json = cmcp_json_parse(reply->body, reply->length);
    free(reply);
    curl_easy_cleanup(curl);
    curl_slist_free_all(list);
    return json;
}

static const char *const auth = "Authorization: Bearer " TEST_TOKEN;
static const cmcp_json_t *output(const cmcp_json_t *response)
{
    return cmcp_json_object_get(cmcp_json_object_get(response, "result"), "structuredContent");
}

static void expect_rpc_error(cmcp_json_t *response, int code)
{
    assert(response);
    const cmcp_json_t *error = cmcp_json_object_get(response, "error");
    assert(cmcp_json_int(cmcp_json_object_get(error, "code")) == code);
    cmcp_json_free(response);
}

static void test_protocol(void)
{
    cmcp_json_free(request("/other", TOOLS, auth, NULL, 404));
    cmcp_json_free(request("/mcp", TOOLS, NULL, NULL, 401));
    cmcp_json_free(request("/mcp", NULL, auth, NULL, 405));
    const char *origin[] = { "Origin: https://untrusted.example", NULL };
    cmcp_json_free(request("/mcp", TOOLS, auth, origin, 403));
    const char *double_auth[] = { auth, NULL };
    cmcp_json_free(request("/mcp", TOOLS, auth, double_auth, 400));
    expect_rpc_error(request("/mcp", "{", auth, NULL, 400), CMCP_RPC_PARSE_ERROR);
    expect_rpc_error(request("/mcp", "{\"garbled\":\"\xff\"}", auth, NULL, 400), CMCP_RPC_PARSE_ERROR);
    expect_rpc_error(request("/mcp", "[" TOOLS "]", auth, NULL, 400), CMCP_RPC_INVALID_REQUEST);
    expect_rpc_error(request("/mcp", "{\"id\":1,\"\\u0069d\":2}", auth, NULL, 400), CMCP_RPC_PARSE_ERROR);
    expect_rpc_error(request("/mcp", DISCOVER, auth, NULL, 400), CMCP_RPC_HEADER_MISMATCH);
    const char *wrong_method[] = { MODERN_HEADERS, "Mcp-Method: tools/list", NULL };
    expect_rpc_error(request("/mcp", DISCOVER, auth, wrong_method, 400), CMCP_RPC_HEADER_MISMATCH);

    const char *discover[] = { MODERN_HEADERS, "Mcp-Method: server/discover", NULL };
    cmcp_json_t *response = request("/mcp", DISCOVER, auth, discover, 200);
    assert(cmcp_json_object_get(response, "result"));
    cmcp_json_free(response);
    const char *legacy[] = { LEGACY_HEADERS, NULL };
    for (int i = 0; i < 2; ++i) {
        response = request("/mcp", INIT, auth, legacy, 200);
        assert(!strcmp(cmcp_json_string(cmcp_json_object_get(cmcp_json_object_get(response, "result"),
                                                            "protocolVersion")), "2025-11-25"));
        cmcp_json_free(response);
    }
    response = request("/mcp", TOOLS, auth, legacy, 200);
    size_t builtins = getenv("MCP_TEST_PROVIDERS") ? 20 : 19;
    assert(cmcp_json_array_len(cmcp_json_object_get(cmcp_json_object_get(response, "result"), "tools")) == builtins);
    cmcp_json_free(response);

    const char *call[] = { MODERN_HEADERS, "Mcp-Method: tools/call", "Mcp-Name: deck_capabilities", NULL };
    response = request("/mcp", REQ("tools/call", "{" META ",\"name\":\"deck_capabilities\",\"arguments\":{}}"),
                       auth, call, 200);
    assert(cmcp_json_int(cmcp_json_object_get(output(response), "deckCount")) == 4);
    cmcp_json_free(response);
    expect_rpc_error(request("/mcp", REQ("tools/call", "{" META ",\"name\":\"deck_read_state\",\"arguments\":{}}"),
                             auth, call, 400), CMCP_RPC_HEADER_MISMATCH);
    cmcp_json_free(request("/mcp", "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
                           auth, legacy, 202));
}

static az_status_field_info_v1 ready(void)
{
    return (az_status_field_info_v1){ .info = { .availability = AZ_AVAIL_READY,
                                                .provenance = AZ_PROV_NATIVE, .revision = 17 } };
}

static az_status_field_info_v1 load_collection(unsigned epoch, unsigned count)
{
    az_status_field_info_v1 receipt = ready();
    receipt.scope = AZ_STATUS_LOAD_SCOPE;
    receipt.scope_id = epoch;
    receipt.provider_id = 7;
    receipt.count = count;
    return receipt;
}

static void publish_analysis(unsigned deck, unsigned epoch, bool grid_link)
{
    const az_beat beats[] = { {.time_s = 1, .bpm = 120}, {.time_s = 1.5, .bpm = 120} };
    const az_phrase rows[] = { { .start_beat = 0, .end_beat = 2, .category = 3 } };
    const az_status_grid_v1 grid = {
        .header = { .size = sizeof grid, .version = AZ_DATA_VERSION, .valid = 3 },
        .current_info = load_collection(epoch, 2), .saved_info = load_collection(epoch, 2),
        .current = beats, .saved = beats
    };
    const az_status_phrases_v1 phrases = {
        .header = { .size = sizeof phrases, .version = AZ_DATA_VERSION, .valid = grid_link ? 15 : 3 },
        .current_info = load_collection(epoch, 1), .saved_info = load_collection(epoch, 1),
        .current = rows, .saved = rows, .current_grid_info = load_collection(epoch, 1),
        .saved_grid_info = load_collection(epoch, 1),
        .current_grid = { .load_epoch = epoch, .provider_id = 7, .revision = 17, .provenance = AZ_PROV_NATIVE },
        .saved_grid = { .load_epoch = epoch, .provider_id = 7, .revision = 17, .provenance = AZ_PROV_NATIVE,
                        .grid_field = AZ_STATUS_GRID_SAVED }
    };
    const az_deck_status_view_v1 view = { .size = sizeof view, .grid = &grid, .phrases = &phrases };
    const az_deck_status_event_v1 event = { .size = sizeof event, .version = AZ_DATA_VERSION,
        .kind = AZ_STATUS_CHANGE, .deck = deck, .sequence = 9, .load_epoch = epoch, .view = &view };
    observe(&event, observation_user);
}

static void publish_deck(unsigned deck, unsigned epoch, const char *title)
{
    az_status_load_v1 load = {
        .header = { .size = sizeof load }, .state = AZ_STATUS_LOAD_LOADED,
        .state_info = ready(), .track = { .size = sizeof load.track, .track_id = 42, .version = AZ_DATA_VERSION,
                   .source_id = 1, .source_generation = 5, .library_id = 1 },
        .track_info = ready()
    };
    load.track_info.scope = AZ_STATUS_LOAD_SCOPE;
    load.track_info.scope_id = epoch;
    az_status_metadata_v1 metadata = {
        .header = { .size = sizeof metadata }, .title = title, .title_info = ready(),
        .duration_s = 210.5, .duration_s_info = ready()
    };
    az_status_position_v1 position = {
        .header = { .size = sizeof position }, .head = { .seconds = 10.25, .samples = 492000, .rate_hz = 48000 },
        .head_info = ready(), .slip_head_info = { .info = { .availability = AZ_AVAIL_EMPTY } }
    };
    const az_cue_entry entries[] = {
        { .valid = AZ_CUE_START | AZ_CUE_END | AZ_CUE_COLOR | AZ_CUE_COLOR_ID | AZ_CUE_COMMENT |
                   AZ_CUE_AUTO_ACTIVE | AZ_CUE_SAVED, .entry_id = 1, .start_s = 10, .end_s = 20, .comment = "loop" },
        { .entry_id = 2 }
    };
    az_status_cues_v1 cues = { .header = { .size = sizeof cues }, .live_hot = entries, .live_hot_info = ready() };
    cues.live_hot_info.count = 2;
    cues.live_hot_info.provider_id = 7;
    const az_deck_status_view_v1 view = {
        .size = sizeof view, .load = &load, .metadata = &metadata, .position = &position, .cues = &cues
    };
    const az_deck_status_event_v1 event = {
        .size = sizeof event, .version = AZ_DATA_VERSION, .kind = AZ_STATUS_RESYNC,
        .deck = deck, .sequence = 6 + deck, .observed_ns = 123456, .load_epoch = epoch, .view = &view
    };
    epochs[deck] = epoch;
    observe(&event, observation_user);
}

static void test_decks(const az_mod_v1 *mod)
{
    cmcp_json_t *response = request("/mcp", CALL("deck_read_state", "{\"deck\":4}"), auth, NULL, 200);
    const cmcp_json_t *snapshot = cmcp_json_object_get(output(response), "snapshot");
    assert(!cmcp_json_bool(cmcp_json_object_get(snapshot, "observed")));
    cmcp_json_free(response);
    for (unsigned i = 0; i < 4; ++i)
        publish_deck(i, 102 + i, "Audio Test");
    mod->tick();
    response = request("/mcp", CALL("deck_read_state", "{\"deck\":4}"), auth, NULL, 200);
    snapshot = cmcp_json_object_get(output(response), "snapshot");
    assert(cmcp_json_int(cmcp_json_object_get(snapshot, "deck")) == 4);
    assert(!strcmp(cmcp_json_string(cmcp_json_object_get(snapshot, "loadEpoch")), "105"));
    assert(cmcp_json_bool(cmcp_json_object_get(snapshot, "observed")));
    const cmcp_json_t *fields = cmcp_json_object_get(snapshot, "fields");
    assert(!strcmp(cmcp_json_string(cmcp_json_object_get(cmcp_json_object_get(fields, "title"), "value")),
                   "Audio Test"));
    assert(!strcmp(cmcp_json_string(cmcp_json_object_get(cmcp_json_object_get(fields, "slipHead"), "availability")),
                   "empty"));
    assert(cmcp_json_object_get(cmcp_json_object_get(fields, "head"), "value"));
    cmcp_json_free(response);

    response = request("/mcp", CALL("deck_read_cues", "{\"deck\":4,\"bank\":\"liveHot\",\"limit\":1}"), auth, NULL, 200);
    const cmcp_json_t *page = cmcp_json_object_get(output(response), "page");
    assert(cmcp_json_int(cmcp_json_object_get(page, "nextOffset")) == 1);
    const cmcp_json_t *entry = cmcp_json_array_at(cmcp_json_object_get(page, "entries"), 0);
    assert(!strcmp(cmcp_json_string(cmcp_json_object_get(entry, "comment")), "loop"));
    cmcp_json_free(response);

    const char *follow = CALL("deck_read_cues", "{\"deck\":4,\"bank\":\"liveHot\",\"offset\":1,\"loadEpoch\":\"105\",\"revision\":\"17\",\"providerId\":\"7\"}");
    response = request("/mcp", follow, auth, NULL, 200);
    page = cmcp_json_object_get(output(response), "page");
    entry = cmcp_json_array_at(cmcp_json_object_get(page, "entries"), 0);
    assert(cmcp_json_int(cmcp_json_object_get(entry, "entryId")) == 2);
    cmcp_json_free(response);

    publish_deck(3, 106, "New load");
    response = request("/mcp", follow, auth, NULL, 200);
    assert(cmcp_json_bool(cmcp_json_object_get(cmcp_json_object_get(response, "result"), "isError")));
    cmcp_json_free(response);
    response = request("/mcp", CALL("deck_read_state", "{\"deck\":5}"), auth, NULL, 200);
    assert(!output(response));
    cmcp_json_free(response);
}

static void expect_state(cmcp_json_t *response, const char *state)
{
    const char *actual = cmcp_json_string(cmcp_json_object_get(output(response), "state"));
    assert(actual && !strcmp(actual, state));
    cmcp_json_free(response);
}

static void expect_tool_error(cmcp_json_t *response)
{
    const cmcp_json_t *result = cmcp_json_object_get(response, "result");
    assert(cmcp_json_bool(cmcp_json_object_get(result, "isError")));
    cmcp_json_free(response);
}

static char deck_session[65];

static cmcp_json_t *action(const char *tool, const char *name, const char *key,
                           unsigned grant, unsigned epoch, const char *extra)
{
    char json[512];
    int n = snprintf(json, sizeof json, "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\","
                     "\"params\":{\"name\":\"%s\",\"arguments\":{\"key\":\"%s\",\"sessionId\":\"%s\","
                     "\"armGeneration\":%u,\"loadEpoch\":\"%u\",\"action\":\"%s\",\"deck\":4%s}}}",
                     tool, key, deck_session, grant, epoch, name, extra);
    assert(n > 0 && (size_t)n < sizeof json);
    return request("/mcp", json, auth, NULL, 200);
}

static cmcp_json_t *receipt(const char *key)
{
    char json[512];
    int n = snprintf(json, sizeof json, "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\","
                     "\"params\":{\"name\":\"deck_command_status\",\"arguments\":{\"key\":\"%s\","
                     "\"sessionId\":\"%s\"}}}", key, deck_session);
    assert(n > 0 && (size_t)n < sizeof json);
    return request("/mcp", json, auth, NULL, 200);
}

static void complete(unsigned outcome, int reason, bool settled)
{
    az_command_completion event = {
        .size = sizeof event, .version = AZ_DATA_VERSION, .kind = AZ_COMMAND_COMPLETION_TERMINAL,
        .settled = settled, .result = { .size = sizeof event.result, .version = AZ_DATA_VERSION,
                                       .outcome = outcome, .reason = reason, .revision = 29,
                                       .observed_ns = 1234, .request_id = last_request }
    };
    ++event.result.request_id;
    complete_command(&event, completion_user);
    --event.result.request_id;
    complete_command(&event, completion_user);
    event.kind = AZ_COMMAND_COMPLETION_RELEASED;
    event.settled = 1;
    ++event.result.revision;
    complete_command(&event, completion_user);
    --event.result.revision;
    complete_command(&event, completion_user);
}

static void test_commands(const az_mod_v1 *mod)
{
    cmcp_json_t *response = request("/mcp", CALL("deck_capabilities", "{}"), auth, NULL, 200);
    const char *session = cmcp_json_string(cmcp_json_object_get(output(response), "sessionId"));
    assert(session && strlen(session) == 64);
    memcpy(deck_session, session, sizeof deck_session);
    cmcp_json_free(response);

    expect_tool_error(action("deck_loop", "exit", "loop1", 1, 106, ""));
    set_arming("Arm live controls", "Disarm live controls", true);
    expect_state(action("deck_loop", "exit", "loop1", 1, 106, ""), "queued");
    expect_state(action("deck_loop", "exit", "loop1", 1, 106, ""), "queued");
    expect_tool_error(action("deck_loop", "enter", "loop1", 1, 106, ""));
    mod->tick();
    mod->tick();
    expect_state(receipt("loop1"), "accepted");
    assert(submissions == 1);
    complete(AZ_COMMAND_APPLIED, AZ_OK, true);
    mod->tick();
    assert(acknowledgements == 1);
    expect_state(receipt("loop1"), "applied");
    expect_state(action("deck_loop", "exit", "loop1", 1, 106, ""), "applied");
    expect_tool_error(action("deck_cue", "triggerHot", "cue1", 1, 106, ""));

    expect_state(action("deck_cue", "triggerHot", "cue1", 1, 106, ",\"slot\":8"), "queued");
    expect_state(action("deck_loop", "exit", "loop2", 1, 106, ""), "queued");
    mod->tick();
    mod->tick();
    cancel_busy = true;
    set_arming("Arm live controls", "Disarm live controls", false);
    mod->tick();
    assert(cancellations == 2 && submissions == 2);
    expect_state(receipt("loop2"), "cancelled");
    complete(AZ_COMMAND_UNCERTAIN, AZ_CANCELLED, false);
    mod->tick();
    expect_state(receipt("cue1"), "uncertain");
    assert(acknowledgements == 2);

    set_arming("Arm live controls", "Disarm live controls", true);
    expect_state(action("deck_loop", "exit", "stale-load", 2, 105, ""), "queued");
    mod->tick();
    mod->tick();
    expect_state(receipt("stale-load"), "rejected");
    expect_state(action("deck_loop", "double", "unsupported-loop", 2, 106, ""), "queued");
    mod->tick();
    mod->tick();
    expect_state(receipt("unsupported-loop"), "rejected");
    assert(submissions == 2);
    expect_tool_error(action("deck_loop", "length", "length", 2, 106, ",\"amount\":-1.5"));
    expect_state(action("deck_loop", "length", "length", 2, 106, ",\"amount\":1.5"), "queued");
    expect_tool_error(action("deck_loop", "length", "length", 2, 106, ",\"amount\":1.25"));
    mod->tick();
    complete(AZ_COMMAND_REJECTED, AZ_FAILED, true);
    mod->tick();
    expect_state(receipt("length"), "rejected");
    expect_state(action("deck_loop", "adjustOut", "move-out", 2, 106, ",\"amount\":-0.125"), "queued");
    mod->tick();
    complete(AZ_COMMAND_CANCELLED, AZ_CANCELLED, true);
    mod->tick();
    expect_state(receipt("move-out"), "cancelled");
}

static cmcp_json_t *with_session(const char *tool, const char *fields)
{
    char json[2048];
    int length = snprintf(json, sizeof json, "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":{\"sessionId\":\"%s\",%s}}}",
        tool, deck_session, fields);
    assert(length > 0 && (size_t)length < sizeof json);
    return request("/mcp", json, auth, NULL, 200);
}

static void expect_status(cmcp_json_t *response, int expected)
{
    const cmcp_json_t *status = cmcp_json_object_get(output(response), "status");
    assert(status && cmcp_json_int(status) == expected);
    cmcp_json_free(response);
}

static void test_live_analysis(void)
{
    publish_analysis(0, 106, true);
    publish_analysis(0, 106, true); /* unchanged receipts */
    cmcp_json_t *response = request("/mcp", CALL("deck_read_live_analysis",
        "{\"deck\":1,\"loadEpoch\":\"106\",\"component\":\"beatGrid\",\"limit\":1}"), auth, NULL, 200);
    const cmcp_json_t *data = output(response);
    assert(cmcp_json_array_len(cmcp_json_object_get(data, "rows")) == 1);
    assert(cmcp_json_int(cmcp_json_object_get(data, "nextOffset")) == 1);
    cmcp_json_free(response);
    response = request("/mcp", CALL("deck_read_live_analysis",
        "{\"deck\":1,\"loadEpoch\":\"106\",\"component\":\"beatGrid\",\"offset\":1,\"providerId\":\"7\",\"revision\":\"17\"}"), auth, NULL, 200);
    assert(cmcp_json_array_len(cmcp_json_object_get(output(response), "rows")) == 1);
    cmcp_json_free(response);
    expect_status(request("/mcp", CALL("deck_read_live_analysis",
        "{\"deck\":1,\"loadEpoch\":\"106\",\"component\":\"beatGrid\",\"offset\":1,\"providerId\":\"8\",\"revision\":\"17\"}"), auth, NULL, 200), AZ_STALE);
    response = request("/mcp", CALL("deck_read_live_analysis",
        "{\"deck\":1,\"loadEpoch\":\"106\",\"component\":\"phrases\",\"variant\":\"saved\"}"), auth, NULL, 200);
    const cmcp_json_t *grid = cmcp_json_object_get(output(response), "grid");
    assert(!strcmp(cmcp_json_string(cmcp_json_object_get(grid, "variant")), "saved"));
    assert(cmcp_json_array_len(cmcp_json_object_get(output(response), "rows")) == 1);
    cmcp_json_free(response);
    publish_analysis(0, 106, false);
    response = request("/mcp", CALL("deck_read_live_analysis",
        "{\"deck\":1,\"loadEpoch\":\"106\",\"component\":\"phrases\"}"), auth, NULL, 200);
    assert(!cmcp_json_array_len(cmcp_json_object_get(output(response), "rows")));
    cmcp_json_free(response);
    publish_deck(0, 408, "Replacement");
    expect_status(request("/mcp", CALL("deck_read_live_analysis",
        "{\"deck\":1,\"loadEpoch\":\"106\",\"component\":\"beatGrid\"}"), auth, NULL, 200), AZ_STALE);
}

static cmcp_json_t *playlist_response(cmcp_json_t *queued)
{
    const cmcp_json_t *data = output(queued);
    assert(cmcp_json_int(cmcp_json_object_get(data, "status")) == AZ_PENDING);
    char query[100];
    snprintf(query, sizeof query, "\"action\":\"status\",\"id\":\"%s\"",
             cmcp_json_string(cmcp_json_object_get(data, "id")));
    cmcp_json_free(queued);
    for (unsigned i = 0; i < 100; ++i) {
        cmcp_json_t *reply = with_session("deck_library_edit", query);
        int status = (int)cmcp_json_int(cmcp_json_object_get(output(reply), "status"));
        if (status == AZ_OK)
            return reply;
        assert(status == AZ_PENDING);
        cmcp_json_free(reply);
        pause_ms(10);
    }
    assert(0);
    return NULL;
}

static void test_playlist_edit(void)
{
    cmcp_json_t *result = playlist_response(with_session("deck_library_edit",
        "\"action\":\"getOrder\",\"usb\":1,\"playlistId\":5,\"offset\":1,\"limit\":1"));
    const cmcp_json_t *data = cmcp_json_object_get(output(result), "result");
    const cmcp_json_t *row = cmcp_json_array_at(cmcp_json_object_get(data, "rows"), 0);
    assert(!strcmp(cmcp_json_string(cmcp_json_object_get(row, "entryId")), "101"));
    assert(cmcp_json_int(cmcp_json_object_get(row, "position")) == 2);
    cmcp_json_free(result);
    result = playlist_response(with_session("deck_library_edit",
        "\"action\":\"getOrder\",\"usb\":1,\"playlistId\":5,\"offset\":1,\"revision\":\"18\""));
    data = cmcp_json_object_get(output(result), "result");
    assert(cmcp_json_int(cmcp_json_object_get(data, "status")) == AZ_STALE);
    cmcp_json_free(result);
    set_arming("Arm saved edits", "Disarm saved edits", false);
    expect_status(with_session("deck_library_edit",
        "\"action\":\"moveTrack\",\"usb\":1,\"playlistId\":5,\"armGeneration\":1"), AZ_CANCELLED);
    set_arming("Arm saved edits", "Disarm saved edits", true);
    result = request("/mcp", CALL("deck_capabilities", "{}"), auth, NULL, 200);
    long long grant = cmcp_json_int(cmcp_json_object_get(output(result), "savedArmGeneration"));
    assert(grant > 0);
    cmcp_json_free(result);
    char fields[512];
    snprintf(fields, sizeof fields,
        "\"action\":\"moveTrack\",\"usb\":1,\"playlistId\":5,\"armGeneration\":%lld,"
        "\"fromPosition\":2,\"toPosition\":3,\"sourceGeneration\":\"17\",\"libraryId\":\"18\","
        "\"revision\":\"19\",\"entryId\":\"101\"", grant);
    result = playlist_response(with_session("deck_library_edit", fields));
    data = cmcp_json_object_get(output(result), "result");
    assert(!strcmp(cmcp_json_string(cmcp_json_object_get(data, "outcome")), "moved"));
    assert(!strcmp(cmcp_json_string(cmcp_json_object_get(data, "deviceLibrary")), "failed"));
    cmcp_json_free(result);
}

static void test_cue_selectors(const az_mod_v1 *mod)
{
    const struct { const char *action, *extra; } commands[] = {
        { "setMain", "" },
        { "captureHot", ",\"slot\":1,\"cueRevision\":\"16\",\"rgb\":12" },
        { "storeMemory", ",\"cueRevision\":\"16\"" },
        { "deleteMemory", ",\"entryId\":1,\"cueRevision\":\"16\"" },
        { "setSavedLoopAuto", ",\"entryId\":1,\"cueRevision\":\"16\",\"autoActive\":true" },
        { "setColor", ",\"entryId\":1,\"cueRevision\":\"16\",\"rgb\":12,\"classification\":\"hot\"" }
    };
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; ++i) {
        expect_state(action("deck_cue", commands[i].action, commands[i].action, i ? 1 : 2, 106, commands[i].extra), "queued");
        mod->tick();
        mod->tick();
        expect_state(receipt(commands[i].action), "rejected");
    }
}

static void test_saved_commands(const az_mod_v1 *mod)
{
    expect_tool_error(action("deck_cue", "createPoint", "save-hot", 1, 106,
        ",\"cueRevision\":\"17\",\"classification\":\"hot\",\"slot\":5,\"startSeconds\":10"));
    set_arming("Arm saved edits", "Disarm saved edits", true);
    expect_state(action("deck_cue", "createPoint", "save-hot", 1, 106,
        ",\"cueRevision\":\"17\",\"classification\":\"hot\",\"slot\":5,\"startSeconds\":10"), "queued");
    mod->tick();
    complete(AZ_COMMAND_APPLIED, AZ_OK, true);
    mod->tick();
    expect_state(receipt("save-hot"), "applied");

    const char *replace = "\"mode\":\"replace\",\"deck\":4,\"armGeneration\":1,\"loadEpoch\":\"106\","
        "\"key\":\"bank\",\"cueRevision\":\"17\",\"entries\":[{\"type\":\"point\",\"classification\":\"hot\","
        "\"slot\":6,\"startSeconds\":25}]";
    expect_state(with_session("deck_apply_cue_bank", replace), "queued");
    mod->tick();
    assert(confirmation && batch_submissions == 0);
    confirmation(1, confirmation_user);
    mod->tick();
    assert(batch_submissions == 1);
    complete(AZ_COMMAND_APPLIED, AZ_OK, true);
    mod->tick();
    expect_state(receipt("bank"), "applied");

    unsigned before = submissions;
    expect_state(action("deck_loop", "exit", "cancel-me", 2, 106, ""), "queued");
    expect_state(with_session("deck_cancel_command", "\"key\":\"cancel-me\""), "queued");
    mod->tick();
    expect_state(receipt("cancel-me"), "cancelled");
    assert(submissions == before);
    test_cue_selectors(mod);
}

#define SDK_REF "\"track\":{\"trackId\":\"42\",\"sourceId\":\"1\",\"sourceGeneration\":\"5\",\"libraryId\":\"1\"}"

static void test_sdk_reads(void)
{
    cmcp_json_t *response = request("/mcp", CALL("deck_ui_state", "{}"), auth, NULL, 200);
    assert(!strcmp(cmcp_json_string(cmcp_json_object_get(output(response), "view")), "deck"));
    cmcp_json_free(response);
    response = request("/mcp", CALL("deck_native_cues", "{\"deck\":4}"), auth, NULL, 200);
    const cmcp_json_t *data = cmcp_json_object_get(output(response), "bank");
    assert(!strcmp(cmcp_json_string(cmcp_json_object_get(data, "cueRevision")), "17"));
    cmcp_json_free(response);
    expect_status(with_session("deck_read_track_analysis", "\"action\":\"request\",\"component\":\"grid\"," SDK_REF), AZ_OK);
    response = with_session("deck_read_track_analysis", "\"action\":\"read\",\"id\":\"44\",\"component\":\"grid\"");
    assert(cmcp_json_array_len(cmcp_json_object_get(output(response), "rows")) == 1);
    cmcp_json_free(response);
    const char *const components[] = { "metadata", "cues", "phrases", "overview" };
    for (size_t i = 0; i < sizeof components / sizeof components[0]; ++i) {
        char fields[128];
        snprintf(fields, sizeof fields, "\"action\":\"request\",\"component\":\"%s\",\"deck\":4,\"loadEpoch\":\"106\"", components[i]);
        expect_status(with_session("deck_read_track_analysis", fields), AZ_OK);
        snprintf(fields, sizeof fields, "\"action\":\"read\",\"component\":\"%s\",\"id\":\"44\"", components[i]);
        expect_status(with_session("deck_read_track_analysis", fields), AZ_OK);
    }
    expect_status(with_session("deck_read_track_analysis", "\"action\":\"release\",\"id\":\"44\""), AZ_OK);
    expect_status(with_session("deck_read_waveform", "\"action\":\"request\",\"columns\":3,\"encoding\":1," SDK_REF), AZ_OK);
    response = with_session("deck_read_waveform", "\"action\":\"read\",\"id\":\"45\"");
    assert(!strcmp(cmcp_json_string(cmcp_json_object_get(output(response), "base64")), "AQID"));
    cmcp_json_free(response);
    expect_status(with_session("deck_read_waveform", "\"action\":\"cancel\",\"id\":\"45\""), AZ_OK);
    response = with_session("deck_read_vocals", "\"deck\":4,\"loadEpoch\":\"106\"");
    data = cmcp_json_object_get(output(response), "page");
    assert(cmcp_json_array_len(cmcp_json_object_get(data, "intervals")) == 1);
    cmcp_json_free(response);
    expect_status(with_session("deck_read_vocals", "\"offset\":1,\"revision\":\"29\"," SDK_REF), AZ_STALE);
}

static void test_library(void)
{
    expect_status(with_session("deck_library", "\"action\":\"searchUsb\",\"usb\":1,\"query\":\"jazz\""), AZ_PENDING);
    cmcp_json_t *response = NULL;
    const cmcp_json_t *result = NULL;
    for (unsigned i = 0; i < 25 && !result; ++i) {
        cmcp_json_free(response);
        response = with_session("deck_library", "\"action\":\"status\",\"id\":\"1\"");
        result = cmcp_json_object_get(output(response), "result");
        pause_ms(5);
    }
    assert(result);
    const cmcp_json_t *row = cmcp_json_array_at(cmcp_json_object_get(result, "rows"), 0);
    assert(!strcmp(cmcp_json_string(cmcp_json_object_get(row, "title")), "Jazz"));
    assert(!cmcp_json_object_get(row, "path"));
    cmcp_json_free(response);
    expect_status(with_session("deck_library", "\"action\":\"resolveTrack\",\"usb\":1,\"trackId\":\"42\""), AZ_PENDING);
}

static void test_settings_analysis_panel(const az_mod_v1 *mod)
{
    cmcp_json_t *response = with_session("deck_azmods_settings", "\"action\":\"read\",\"setting\":\"test-setting\"");
    assert(cmcp_json_array_len(cmcp_json_object_get(output(response), "settings")) == 1);
    cmcp_json_free(response);
    const char *setting = "\"action\":\"write\",\"key\":\"setting1\",\"setting\":\"test-setting\",\"expected\":0,\"configured\":1";
    expect_status(with_session("deck_azmods_settings", setting), AZ_PENDING);
    mod->tick();
    expect_status(with_session("deck_azmods_settings", "\"action\":\"status\",\"key\":\"setting1\""), AZ_OK);
    assert(setting_value == 1);
    expect_status(with_session("deck_azmods_settings", setting), AZ_OK);
    expect_status(with_session("deck_azmods_settings", "\"action\":\"write\",\"key\":\"setting1\","
        "\"setting\":\"test-setting\",\"expected\":1,\"configured\":0"), AZ_INVALID);
    expect_status(with_session("deck_analysis", "\"action\":\"start\",\"components\":\"both\",\"deck\":4,\"loadEpoch\":\"106\",\"armGeneration\":1,\"key\":\"analysis1\""), AZ_OK);
    expect_status(with_session("deck_analysis", "\"action\":\"status\",\"id\":\"91\""), AZ_OK);
    response = with_session("deck_analysis", "\"action\":\"result\",\"id\":\"91\"");
    assert(cmcp_json_array_len(cmcp_json_object_get(output(response), "phrases")) == 1);
    cmcp_json_free(response);
    expect_status(with_session("deck_analysis", "\"action\":\"release\",\"id\":\"91\""), AZ_OK);

    const char *press = "\"action\":\"press\",\"button\":\"browse\",\"key\":\"open-browser\","
        "\"holdMs\":80,\"expectedView\":\"deck\",\"armGeneration\":2";
    expect_status(with_session("deck_input", press), AZ_PENDING);
    expect_status(with_session("deck_input", press), AZ_OK);
    expect_status(with_session("deck_input", "\"action\":\"status\",\"key\":\"open-browser\""), AZ_OK);
    assert(panel_presses == 1);
}

#if !defined(MCP_TEST_SHARED) || defined(MCP_TEST_PROVIDER_CLIENT)
static pthread_t fixture_worker;
static az_mcp_provider *fixture_provider;
static unsigned test_writes;

static int start_fixture(const char *id, const char *name, void *(*fn)(void *), void *user)
{
    assert(!strcmp(id, "fixture") && !strcmp(name, "mcp-provider"));
    return pthread_create(&fixture_worker, NULL, fn, user);
}

static int fixture_call(const cmcp_json_t *args, uint64_t id, uint64_t deadline,
                         cmcp_json_t **out, void *user)
{
    (void)args;
    assert(id && deadline);
    if (user)
        ++test_writes;
    /* Also exercise completion through the deferred API. */
    cmcp_json_t *result = cmcp_json_parse_cstr("{\"hello\":true}");
    assert(az_mcp_provider_complete(fixture_provider, id, AZ_OK, result) == AZ_OK);
    assert(az_mcp_provider_complete(fixture_provider, id, AZ_OK, NULL) == AZ_STALE);
    *out = NULL;
    return AZ_PENDING;
}

static void start_provider(void)
{
    static const az_core_v1 api = { .abi = AZ_MOD_ABI, .size = sizeof api, .thread_start = start_fixture };
    static const az_mcp_tool_v1 tools[] = {
        { .size = sizeof(az_mcp_tool_v1), .name = "read", .description = "Read fixture result",
          .input_schema = "{\"type\":\"object\"}", .output_schema = "{\"type\":\"object\"}",
          .invoke = fixture_call, .grant = AZ_MCP_READ },
        { .size = sizeof(az_mcp_tool_v1), .name = "write", .description = "Trigger fixture write",
          .input_schema = "{\"type\":\"object\"}", .output_schema = "{\"type\":\"object\"}",
          .invoke = fixture_call, .grant = AZ_MCP_SAVED, .user = &test_writes }
    };
    assert(az_mcp_register_tools(&api, "fixture", tools, 2, &fixture_provider) == AZ_OK);
}

static size_t provider_tool_count(void)
{
    cmcp_json_t *response = request("/mcp", TOOLS, auth, NULL, 200);
    size_t count = cmcp_json_array_len(cmcp_json_object_get(cmcp_json_object_get(response, "result"), "tools"));
    cmcp_json_free(response);
    return count;
}

static void poll_provider(cmcp_json_t *queued)
{
    const cmcp_json_t *data = output(queued);
    const char *op = cmcp_json_string(cmcp_json_object_get(data, "operationId"));
    const char *session = cmcp_json_string(cmcp_json_object_get(data, "sessionId"));
    assert(op && session);
    char call[512];
    int n = snprintf(call, sizeof call, "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"deck_provider_status\",\"arguments\":{\"operationId\":\"%s\",\"sessionId\":\"%s\"}}}",
        op, session);
    assert(n > 0 && (size_t)n < sizeof call);
    cmcp_json_free(queued);
    for (unsigned i = 0; i < 40; ++i) {
        az_mcp_provider_tick(fixture_provider);
        cmcp_json_t *reply = request("/mcp", call, auth, NULL, 200);
        const cmcp_json_t *result = cmcp_json_object_get(output(reply), "result");
        bool done = cmcp_json_bool(cmcp_json_object_get(result, "hello"));
        cmcp_json_free(reply);
        if (done)
            return;
        pause_ms(20);
    }
    assert(0);
}

static void test_providers(void)
{
    set_arming("Arm saved edits", "Disarm saved edits", false);
    start_provider();
    for (unsigned i = 0; i < 40 && provider_tool_count() != 22; ++i) {
        az_mcp_provider_tick(fixture_provider);
        pause_ms(20);
    }
    assert(provider_tool_count() == 22);
    poll_provider(request("/mcp", CALL("fixture.read", "{}"), auth, NULL, 200));
    expect_tool_error(request("/mcp", CALL("fixture.write", "{\"key\":\"once\"}"), auth, NULL, 200));
    set_arming("Arm saved edits", "Disarm saved edits", true);
    poll_provider(request("/mcp", CALL("fixture.write", "{\"key\":\"once\"}"), auth, NULL, 200));
    poll_provider(request("/mcp", CALL("fixture.write", "{\"key\":\"once\"}"), auth, NULL, 200));
    assert(test_writes == 1);
    az_mcp_provider_stop(fixture_provider);
    assert(!pthread_join(fixture_worker, NULL));
    for (unsigned i = 0; i < 40 && provider_tool_count() != 20; ++i)
        pause_ms(20);
    assert(provider_tool_count() == 20);
}
#endif

static int connect_tcp(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    const struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) }, .sin_port = htons(port)
    };
    if (connect(fd, (const struct sockaddr *)&address, sizeof address)) {
        close(fd);
        return -1;
    }
    return fd;
}

static void test_connection_limit(void)
{
    int clients[MCP_CONNECTION_LIMIT];
    for (size_t i = 0; i < MCP_CONNECTION_LIMIT; ++i) {
        clients[i] = connect_tcp();
        assert(clients[i] >= 0);
    }
    int extra = connect_tcp();
    assert(extra >= 0);
    struct timeval timeout = { .tv_sec = 2 };
    assert(!setsockopt(extra, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout));
    const char plaintext[] = "GET /mcp HTTP/1.1\r\nHost: localhost\r\n\r\n";
    (void)send(extra, plaintext, sizeof plaintext - 1, 0);
    char c;
    ssize_t length = recv(extra, &c, 1, 0);
    assert(length == 0 || (length < 0 && errno == ECONNRESET));
    close(extra);
    for (size_t i = 0; i < MCP_CONNECTION_LIMIT; ++i)
        close(clients[i]);
}

static void wait_started(int expected)
{
    for (unsigned i = 0; i < 200 && !atomic_load(&serving); ++i)
        pause_ms(5);
    assert(atomic_load(&serving) == expected);
}

static const az_mod_v1 *load_mod(const char *library)
{
#ifdef MCP_TEST_SHARED
    void *module = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!module)
        fprintf(stderr, "%s\n", dlerror());
    assert(module);
    const az_mod_v1 *(*entry)(void) = (const az_mod_v1 *(*)(void))dlsym(module, "az_mod_v1_entry");
    assert(entry);
    return entry();
#else
    (void)library;
    return az_mod_v1_entry();
#endif
}

static void draw_token_at_width(int width)
{
    drawn_token[0] = drawn_url[0] = drawn_pair_url[0] = drawn_code[0] = drawn_fingerprint[0] = '\0';
    drawn_trust_note = drawn_qr = drawn_harness_label = drawn_token_note = drawn_save_error = false;
    drawn_pair_used = false;
    drawn_token_lines = 0;
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; ++i)
        buttons[i].visible = false;
    token_screen->on_draw(NULL, (az_rect){ 0, 0, width, 400 }, token_screen->user);
}

static void draw_token_screen(void)
{
    draw_token_at_width(800);
}

static const struct drawn_button *find_button(const char *label)
{
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; ++i)
        if (buttons[i].visible && !strcmp(label, buttons[i].label))
            return &buttons[i];
    return NULL;
}

static void tap_button_label(const char *label)
{
    draw_token_screen();
    const struct drawn_button *button = find_button(label);
    assert(button);
    const az_event tap = { .kind = AZ_EV_TAP, .x = button->x, .y = button->y };
    assert(token_screen->on_input(&tap, token_screen->user) == 1);
}

static void set_arming(const char *arm, const char *disarm, bool enabled)
{
    draw_token_screen();
    const char *action = enabled ? arm : disarm;
    if (find_button(action))
        tap_button_label(action);
    draw_token_screen();
    assert(find_button(enabled ? disarm : arm));
}

static void choose_rotation(int confirmed)
{
    confirmation = NULL;
    tap_button_label("Reset token");
    assert(confirmation);
    confirmation(confirmed, confirmation_user);
}

static void wait_for_pairing(void)
{
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        draw_token_screen();
        if (drawn_pair_url[0])
            break;
        pause_ms(10);
    }
    assert(drawn_pair_url[0] && strlen(drawn_code) == 6 && !drawn_qr);
    assert(strspn(drawn_code, "0123456789") == MCP_PAIR_CODE_DIGITS);
    assert(strlen(drawn_fingerprint) == MCP_FINGERPRINT_SIZE - 1);
}

static cmcp_json_t *pair_request(const char *method, const char *body, const char *origin,
                                 const char *content_type, long expected)
{
    char url[256], ca[512];
    assert(snprintf(url, sizeof url, "https://%s:%u" MCP_PAIR_PATH, fixture_address, port) > 0);
    assert(snprintf(ca, sizeof ca, "%s/%s", fixture, fixture_certificate) > 0);
    CURL *curl = curl_easy_init();
    struct reply *reply = calloc(1, sizeof *reply);
    assert(curl && reply);
    struct curl_slist *headers = curl_slist_append(NULL, content_type);
    if (origin)
        headers = curl_slist_append(headers, origin);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2000L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, reply);
    curl_easy_setopt(curl, CURLOPT_HEADER, 1L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (body)
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    assert(curl_easy_perform(curl) == CURLE_OK);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    assert(status == expected);
    assert(strstr(reply->body, "Cache-Control: no-store\r\n"));
    char *payload = strstr(reply->body, "\r\n\r\n");
    assert(payload);
    payload += 4;
    cmcp_json_t *bundle = NULL;
    if (expected == 200 && !strcmp(method, "POST")) {
        assert(strstr(reply->body, "Content-Disposition: attachment; filename=\"deck-mcp-connection.json\"\r\n"));
        assert(strstr(reply->body, "Content-Type: application/json\r\n"));
        bundle = cmcp_json_parse(payload, strlen(payload));
        assert(bundle);
    } else {
        assert(strstr(reply->body, "Content-Type: text/html; charset=utf-8\r\n"));
        assert(strstr(reply->body, "Content-Security-Policy:") && strstr(payload, "<form action=/ method=post"));
        assert(strstr(payload, "6-digit setup code") && strstr(payload, "pattern=\"[0-9]{6}\"") &&
               strstr(payload, "maxlength=6"));
        assert(!strstr(payload, "bearerToken") && !strstr(reply->body, "Content-Disposition:"));
    }
    OPENSSL_cleanse(reply->body, reply->length);
    free(reply);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return bundle;
}

static cmcp_json_t *submit_code(const char *code, long expected)
{
    char origin[64], form[32];
    snprintf(origin, sizeof origin, "Origin: https://%s:%u", fixture_address, port);
    snprintf(form, sizeof form, "code=%s", code);
    return pair_request("POST", form, origin, "Content-Type: application/x-www-form-urlencoded", expected);
}

static void check_string(cmcp_json_t *object, const char *field, const char *expected)
{
    const char *actual = cmcp_json_string(cmcp_json_object_get(object, field));
    assert(actual && !strcmp(actual, expected));
}

static cmcp_json_t *redeem_pairing(void)
{
    cmcp_json_t *bundle = submit_code(drawn_code, 200);
    assert(bundle && cmcp_json_object_len(bundle) == 5);
    assert(cmcp_json_int(cmcp_json_object_get(bundle, "version")) == 1);
    check_string(bundle, "transport", "streamable-http");
    char url[MCP_URL_SIZE];
    snprintf(url, sizeof url, "https://%s:%u/mcp", fixture_address, port);
    check_string(bundle, "url", url);
    const char *ca = cmcp_json_string(cmcp_json_object_get(bundle, "caCertificatePem"));
    assert(ca && strstr(ca, "BEGIN CERTIFICATE") && !strstr(ca, "PRIVATE"));
    return bundle;
}

static void test_pairing(void)
{
    const char *form_type = "Content-Type: application/x-www-form-urlencoded";
    pair_request("GET", NULL, NULL, form_type, 200);
    pair_request("DELETE", NULL, NULL, form_type, 405);
    submit_code("000000", 410);
    tap_button_label("Pair with Codex");
    wait_for_pairing();
    char old[MCP_PAIR_CODE_SIZE];
    memcpy(old, drawn_code, sizeof old);
    tap_button_label("New setup code");
    wait_for_pairing();
    submit_code(old, 403);
    memcpy(old, drawn_code, sizeof old);
    tap_button_label("Back");
    submit_code(old, 410);
    tap_button_label("Pair with Codex");
    wait_for_pairing();
    char form[32], origin[64];
    snprintf(form, sizeof form, "code=%s", drawn_code);
    snprintf(origin, sizeof origin, "Origin: https://%s:%u", fixture_address, port);
    pair_request("POST", form, NULL, form_type, 403);
    pair_request("POST", form, "Origin: https://wrong.example", form_type, 403);
    pair_request("POST", form, origin, "Content-Type: application/json", 415);
    snprintf(form, sizeof form, "%s00", drawn_code);
    submit_code(form, 403);
    memcpy(old, drawn_code, sizeof old);
    old[0] = old[0] == '0' ? '1' : '0';
    for (unsigned attempt = 2; attempt < MCP_PAIR_ATTEMPTS; ++attempt)
        submit_code(old, 403);
    submit_code(old, 429);
    submit_code(drawn_code, 410);
    tap_button_label("New setup code");
    wait_for_pairing();
    memcpy(old, drawn_code, sizeof old);
    cmcp_json_t *bundle = redeem_pairing();
    check_string(bundle, "bearerToken", TEST_TOKEN);
    cmcp_json_free(bundle);
    submit_code(old, 410);
    draw_token_screen();
    assert(drawn_pair_used && !drawn_code[0]);
    const az_event back = { .kind = AZ_EV_BACK };
    assert(token_screen->on_input(&back, token_screen->user) == 1);
}

static const char *test_token_rotation(const az_mod_v1 *mod)
{
    token_screen->on_enter(token_screen->user);
    draw_token_screen();
    assert(!drawn_token[0]);
    assert(drawn_harness_label && find_button("Pair with Codex"));
    char url[MCP_URL_SIZE];
    snprintf(url, sizeof url, "https://127.0.0.1:%u/mcp", port);
    assert(!strcmp(drawn_url, url));
    tap_button_label("Show token and manual setup");
    draw_token_screen();
    assert(!strcmp(drawn_token, TEST_TOKEN) && drawn_token_lines == 1 && drawn_qr);
    draw_token_at_width(280);
    assert(!strcmp(drawn_token, TEST_TOKEN) && drawn_token_lines == 2);
    draw_token_at_width(200);
    assert(!strcmp(drawn_token, TEST_TOKEN) && drawn_token_lines == 4);
    assert(!strcmp(drawn_url, url) && drawn_token_note);
    tap_button_label("Back");
    choose_rotation(0);
    draw_token_screen();
    assert(!drawn_token[0] && find_button("Reset token"));

    char config[512], backup[512];
    assert(snprintf(config, sizeof config, "%s/server.json", fixture) > 0);
    assert(snprintf(backup, sizeof backup, "%s/test-original-config", fixture) > 0);
    assert(rename(config, backup) == 0);
    unsigned before = redraws;
    choose_rotation(1);
    for (unsigned attempt = 0; attempt < 200 && !drawn_save_error; ++attempt) {
        pause_ms(10);
        mod->tick();
        draw_token_screen();
    }
    assert(redraws >= before + 2 && rename(backup, config) == 0);
    draw_token_screen();
    assert(drawn_save_error && !drawn_pair_url[0]);
    cmcp_json_free(request("/mcp", TOOLS, auth, NULL, 200));

    tap_button_label("Back");
    choose_rotation(1);
    wait_for_pairing();
    cmcp_json_t *bundle = redeem_pairing();
    const char *token = cmcp_json_string(cmcp_json_object_get(bundle, "bearerToken"));
    assert(token && strlen(token) == 64 && strcmp(token, TEST_TOKEN));
    static char authorization[96];
    assert(snprintf(authorization, sizeof authorization, "Authorization: Bearer %s", token) > 0);
    cmcp_json_free(bundle);
    cmcp_json_free(request("/mcp", TOOLS, auth, NULL, 401));
    cmcp_json_free(request("/mcp", TOOLS, authorization, NULL, 200));
    return authorization;
}

static void stop_fixture(const az_mod_v1 *mod)
{
    mod->stop();
    assert(!pthread_join(library_worker, NULL));
    assert(!pthread_join(worker, NULL));
}

static void first_run(const az_mod_v1 *mod, const char *directory)
{
    fixture = directory;
    fixture_certificate = MCP_SETUP_CA;
    atomic_store(&serving, 0);
    assert(!mod->init(&core));
    wait_started(1);
    token_screen->on_enter(token_screen->user);
    draw_token_screen();
    assert(!drawn_token[0] && drawn_harness_label && find_button("Pair with Codex"));
    assert(sscanf(drawn_url, "https://%15[0-9.]:%u/mcp", fixture_address, &port) == 2);
    assert(port == 8765 && strcmp(fixture_address, "127.0.0.1"));
    tap_button_label("Pair with Codex");
    wait_for_pairing();
    cmcp_json_t *bundle = redeem_pairing();
    tap_button_label("Back");
    tap_button_label("Show token and manual setup");
    draw_token_screen();
    assert(strlen(drawn_token) == 64 && drawn_trust_note && drawn_qr);
    check_string(bundle, "bearerToken", drawn_token);
    cmcp_json_free(bundle);
    char generated_auth[96];
    snprintf(generated_auth, sizeof generated_auth, "Authorization: Bearer %s", drawn_token);
    cmcp_json_free(request("/mcp", TOOLS, NULL, NULL, 401));
    cmcp_json_free(request("/mcp", TOOLS, generated_auth, NULL, 200));
    stop_fixture(mod);

    atomic_store(&serving, 0);
    assert(!mod->init(&core));
    wait_started(1);
    cmcp_json_free(request("/mcp", TOOLS, generated_auth, NULL, 200));
    stop_fixture(mod);
}

#ifndef MCP_TEST_SHARED
static void test_pair_expiry(void)
{
    struct mcp_runtime runtime = { .lock = PTHREAD_MUTEX_INITIALIZER };
    struct mcp_config config = {0};
    struct mcp_pairing pair = { .code = "123456", .attempts = 2, .expires = az_mcp_clock() - 1 };
    runtime.token_page.pair_status = MCP_PAIR_READY;
    memcpy(runtime.token_page.pair_code, pair.code, sizeof pair.code);
    mcp_pair_poll(&pair, &runtime, &config);
    assert(runtime.token_page.pair_status == MCP_PAIR_EXPIRED);
    assert(!pair.code[0] && !pair.attempts && !runtime.token_page.pair_code[0]);
    assert(pthread_mutex_destroy(&runtime.lock) == 0);
}

static X509 *read_public_ca(const char *dir)
{
    BIO *pem = mcp_open_pem(dir, MCP_SETUP_CA);
    assert(pem);
    X509 *ca = PEM_read_bio_X509(pem, NULL, NULL, NULL);
    assert(ca);
    assert(!mcp_read_key(pem));
    BIO_free(pem);
    return ca;
}

static void check_identity(const char *dir, const char *ip, X509 *ca)
{
    SSL_CTX *tls = mcp_setup_tls(dir, ip);
    assert(tls);
    X509 *server = SSL_CTX_get0_certificate(tls);
    assert(X509_check_ip_asc(server, ip, 0) == 1);
    assert(X509_check_ip_asc(server, "127.0.0.1", 0) == 0);
    EVP_PKEY *key = X509_get_pubkey(ca);
    assert(key && X509_verify(server, key) == 1);
    EVP_PKEY_free(key);
    SSL_CTX_free(tls);
}

static void setup_recovery(const char *base)
{
    char dir[512];
    assert(snprintf(dir, sizeof dir, "%s/setup.XXXXXX", base) > 0 && mkdtemp(dir));
    assert(mcp_save_file(dir, "server.key", "manual", 6, false));
    struct mcp_config config;
    assert(!mcp_prepare_config(dir, &config) && mcp_file_missing(dir, "server.json"));
    assert(!mcp_save_file(dir, "server.key", "overwrite", 9, false));
    char path[512];
    assert(snprintf(path, sizeof path, "%s/server.key", dir) > 0);
    assert(unlink(path) == 0);
    assert(mcp_prepare_config(dir, &config) && !strcmp(config.bind, "auto"));
    struct stat file;
    assert(snprintf(path, sizeof path, "%s/server.json", dir) > 0);
    assert(stat(path, &file) == 0 && (file.st_mode & 0777) == 0600);

    SSL_CTX *tls = mcp_setup_tls(dir, "192.168.1.41");
    assert(tls);
    SSL_CTX_free(tls);
    X509 *ca = read_public_ca(dir);
    assert(snprintf(path, sizeof path, "%s/" MCP_SETUP_CA, dir) > 0);
    assert(unlink(path) == 0);
    check_identity(dir, "192.168.1.42", ca);
    X509 *renewed_export = read_public_ca(dir);
    assert(X509_cmp(ca, renewed_export) == 0);
    X509_free(renewed_export);
    X509_free(ca);
}
#endif

int main(int argc, char **argv)
{
#ifdef MCP_TEST_SHARED
    assert(argc == 3 && argv[2][0]);
#else
    assert(argc == 2);
#endif
    assert(argv[1][0]);
    fixture = argv[1];
    const char *configured_port = getenv("MCP_TEST_PORT");
    port = configured_port ? (unsigned)strtoul(configured_port, NULL, 10) : 18765;
    assert(port >= 1024 && port <= 65535);
    signal(SIGPIPE, SIG_IGN);
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    const az_mod_v1 *mod = load_mod(argv[2]);
    assert(AZ_MOD_HAS_STOP(mod) && mod->init(NULL) != 0);
    assert(!mod->init(&core));
    wait_started(1);
    test_protocol();
    test_decks(mod);
    test_commands(mod);
    test_saved_commands(mod);
    test_sdk_reads();
    test_library();
    test_settings_analysis_panel(mod);
#if !defined(MCP_TEST_SHARED) || defined(MCP_TEST_PROVIDER_CLIENT)
    if (getenv("MCP_TEST_PROVIDERS"))
        test_providers();
#endif
    test_playlist_edit();
    test_live_analysis();
    test_pairing();
#ifndef MCP_TEST_SHARED
    test_pair_expiry();
#endif
    test_connection_limit();
    const char *restart_auth = getenv("MCP_TEST_ROTATION") ? test_token_rotation(mod) : auth;

    int slow = connect_tcp();
    assert(slow >= 0);
    mod->stop();
    assert(!pthread_join(library_worker, NULL));
    assert(!pthread_join(worker, NULL) && releases == 1);
    close(slow);
    assert(connect_tcp() < 0);

    atomic_store(&serving, 0);
    assert(!mod->init(&core));
    wait_started(1);
    cmcp_json_free(request("/mcp", TOOLS, restart_auth, NULL, 200));
    mod->stop();
    assert(!pthread_join(library_worker, NULL));
    assert(!pthread_join(worker, NULL) && releases == 2);

    fixture = "/nonexistent/deck-mcp";
    atomic_store(&serving, 0);
    assert(!mod->init(&core));
    wait_started(-1);
    mod->stop();
    assert(!pthread_join(library_worker, NULL));
    assert(!pthread_join(worker, NULL) && releases == 3);
    const char *empty_directory = getenv("MCP_TEST_FIRST_RUN");
    if (empty_directory) {
#ifndef MCP_TEST_SHARED
        setup_recovery(argv[1]);
#endif
        first_run(mod, empty_directory);
    }
    curl_global_cleanup();
    puts("HTTPS MCP integration passed");
    return 0;
}
