/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef DECK_MCP_H
#define DECK_MCP_H

#include "azmod-mod.h"
#include "commands.h"
#include "settings-tools.h"
#include "cmcp.h"
#include "cmcp_server.h"
#include "library-tools.h"
#include "panel-tools.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>

#define MCP_ID "deck-mcp"
#define MCP_VERSION "0.5.0"
#define MCP_DECKS 4
#define MCP_BODY_LIMIT (144 * 1024)
#define MCP_RESPONSE_LIMIT (128 * 1024)
#define MCP_TEXT_LIMIT 128
#define MCP_TOKEN_BYTES 32
#define MCP_SECRET_SIZE (2 * MCP_TOKEN_BYTES + 1)
#define MCP_URL_SIZE 48
#define MCP_PAIR_CODE_DIGITS 6
#define MCP_PAIR_CODE_SIZE (MCP_PAIR_CODE_DIGITS + 1)
#define MCP_FINGERPRINT_SIZE 65
#define MCP_CONNECTION_LIMIT 12
#define MCP_RECEIPT_SECONDS 60u

enum mcp_value_kind { MCP_INT, MCP_NUM, MCP_TEXT, MCP_COORD };
enum mcp_field {
    LOAD_STATE, LOAD_SOURCE, TITLE, ARTIST, ALBUM, DURATION,
    ORIGINAL_KEY, EFFECTIVE_KEY, KEY_SHIFT,
    TRANSPORT, DIRECTION, PLAYBACK_RATE, HEAD, SLIP_HEAD, REMAINING,
    BPM, CURRENT_BPM, TEMPO_SLIDER, TEMPO_LOCKED,
    LOOP_DEFINED, LOOP_ENGAGED, LOOP_IN, LOOP_OUT, WORKING_IN, WORKING_OUT, RELOOP, LOOP_MODE,
    SYNC_ENABLED, SYNC_LOCKED, SYNC_FOLLOWING, SYNC_MASTER,
    MASTER_TEMPO, KEY_LOCK, SLIP, QUANTIZE, PAD_MODE, BEAT_PHASE, BAR_PHASE,
    STEMS_AVAILABLE, STEMS_ENABLED, MCP_FIELDS
};
enum { MAIN_CUE, LIVE_HOT, LIVE_MEMORY, SAVED_HOT, SAVED_MEMORY, MCP_CUE_BANKS };

struct mcp_value {
    az_status_field_info_v1 receipt;
    union {
        int32_t integer;
        double number;
        az_status_coord_v1 coord;
        char text[MCP_TEXT_LIMIT];
    } value;
};

struct mcp_cues {
    az_status_field_info_v1 receipt;
    size_t count;
    az_cue_entry entries[AZ_NATIVE_CUE_LIMIT];
};

struct mcp_deck {
    uint64_t sequence, load_epoch, observed_ns, lost_topics;
    az_track_ref track;
    struct mcp_value fields[MCP_FIELDS];
    struct mcp_cues cues[MCP_CUE_BANKS];
};

#define MCP_ANALYSIS_KEYS 16
struct mcp_analysis_record {
    uint64_t id, release_order;
    cmcp_json_t *args;
};

enum mcp_token_status { MCP_TOKEN_UNAVAILABLE, MCP_TOKEN_READY, MCP_TOKEN_SAVING,
                        MCP_TOKEN_SAVED, MCP_TOKEN_FAILED };
enum mcp_pair_action { MCP_PAIR_NONE, MCP_PAIR_START, MCP_PAIR_CANCEL };
enum mcp_pair_status { MCP_PAIR_UNAVAILABLE, MCP_PAIR_IDLE, MCP_PAIR_PREPARING,
                       MCP_PAIR_READY, MCP_PAIR_USED, MCP_PAIR_EXPIRED, MCP_PAIR_LOCKED };
enum mcp_screen_page { MCP_SCREEN_HOME, MCP_SCREEN_PAIR };
enum mcp_screen_button { MCP_BUTTON_PAIR, MCP_BUTTON_LIVE,
                         MCP_BUTTON_SAVED, MCP_BUTTON_BACK, MCP_BUTTON_RESET, MCP_BUTTONS };

struct mcp_token_page {
    char url[MCP_URL_SIZE];
    char pair_url[MCP_URL_SIZE], pair_code[MCP_PAIR_CODE_SIZE], fingerprint[MCP_FINGERPRINT_SIZE];
    const char *connection_note;
    enum mcp_token_status status;
    enum mcp_pair_status pair_status;
    enum mcp_pair_action pair_action;
    enum mcp_screen_page page;
    bool dirty;
    bool shown_live, shown_saved;
    az_rect buttons[MCP_BUTTONS];
};

struct mcp_runtime {
    const az_core_v1 *core;
    const az_observation_v1 *observation;
    const az_analysis_v1 *analysis;
    struct mcp_analysis_record analysis_keys[MCP_ANALYSIS_KEYS];
    uint64_t analysis_release_order;
    const az_library_v1 *library;
    az_deck_subscription subscription;
    pthread_mutex_t lock;
    atomic_bool stopping, working;
    atomic_uint_fast64_t saved_edits;
    atomic_uint saved_generation;
    unsigned pending;
    struct mcp_commands commands;
    struct mcp_settings settings;
    struct mcp_library_io library_io;
    struct mcp_panel panel;
    char firmware[40];
    char session[MCP_SECRET_SIZE];
    struct mcp_token_page token_page;
    struct mcp_deck ui[MCP_DECKS], published[MCP_DECKS];
};

void mcp_token_screen_init(struct mcp_runtime *runtime);
void mcp_token_screen_tick(struct mcp_runtime *runtime);
void mcp_token_publish(struct mcp_runtime *runtime, enum mcp_token_status status);
void mcp_connection_publish(struct mcp_runtime *runtime, const char *address,
                            unsigned port, const char *note);
bool mcp_token_requested(struct mcp_runtime *runtime);
void mcp_pair_offer(struct mcp_runtime *runtime, enum mcp_pair_status status,
                    const char *url, const char *code, const char *fingerprint);
enum mcp_pair_action mcp_pair_take_action(struct mcp_runtime *runtime);
extern const char *const mcp_cue_names[MCP_CUE_BANKS];

uint64_t mcp_watch_topics(void);
void mcp_capture(const az_deck_status_event_v1 *event, void *user);
void mcp_live_reset(void);
void mcp_live_capture(const az_deck_status_event_v1 *event);
void mcp_live_publish(void); /* caller holds runtime lock */
cmcp_json_t *mcp_receipt_json(const az_status_field_info_v1 *receipt);
void mcp_publish(struct mcp_runtime *runtime);
cmcp_json_t *mcp_deck_json(const struct mcp_deck *deck, int number);
cmcp_json_t *mcp_cue_entry_json(const az_cue_entry *cue);
cmcp_json_t *mcp_cues_json(const struct mcp_deck *deck, int bank, size_t offset, size_t limit);
size_t mcp_utf8_prefix(const char *text, size_t length);
void mcp_set(cmcp_json_t *object, const char *key, cmcp_json_t *value);
cmcp_json_t *mcp_id(uint64_t value);
int mcp_tool_result(cmcp_handler_ctx_t *ctx, cmcp_json_t *data, cmcp_json_t **content);
int mcp_tool_error(const char *message, cmcp_json_t **content, int *is_error);
cmcp_json_t *mcp_session_result(const struct mcp_runtime *runtime);
unsigned mcp_input_deck(const cmcp_json_t *args);
int mcp_register_native_cues(cmcp_server_t *server, struct mcp_runtime *runtime);
bool mcp_base_intent(const cmcp_json_t *args, struct mcp_intent *intent);
int mcp_submit_tool(const cmcp_json_t *args, struct mcp_runtime *runtime, const struct mcp_intent *intent,
                    cmcp_handler_ctx_t *ctx, cmcp_json_t **content, int *is_error);
int mcp_register_cue_batch(cmcp_server_t *server, struct mcp_runtime *runtime);
int mcp_cue_intent(const cmcp_json_t *args, struct mcp_intent *intent);
int mcp_register_command_tools(cmcp_server_t *server, struct mcp_runtime *runtime);
unsigned mcp_live_grant_generation(const struct mcp_runtime *runtime, uint64_t now);
unsigned mcp_saved_grant_generation(const struct mcp_runtime *runtime, uint64_t now);
uint64_t mcp_receipt_deadline(uint64_t now);
bool mcp_receipt_expired(uint64_t now, uint64_t deadline);
/* The caller holds the runtime lock. Reaping leaves active jobs in order. */
void mcp_commands_reap(struct mcp_commands *commands, uint64_t now);
void mcp_settings_reap(struct mcp_settings *settings, uint64_t now);
void mcp_panel_reap(struct mcp_panel *panel, uint64_t now);
void mcp_settings_tick(struct mcp_runtime *runtime);
int mcp_register_settings_tools(cmcp_server_t *server, struct mcp_runtime *runtime);
bool mcp_start_library_io(struct mcp_runtime *runtime);
int mcp_register_library_tools(cmcp_server_t *server, struct mcp_runtime *runtime);
int mcp_register_panel_tools(cmcp_server_t *server, struct mcp_runtime *runtime);
int mcp_register_tools(cmcp_server_t *server, struct mcp_runtime *runtime);
int mcp_register_live_tools(cmcp_server_t *server, struct mcp_runtime *runtime);
void mcp_remove_tool(cmcp_server_t *server, const char *name);
int mcp_exchange(cmcp_server_t *server, const cmcp_rpc_message_t *request, char **wire);
void *mcp_serve(void *arg);

#endif
