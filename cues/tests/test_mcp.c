/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include "../mcp.c"

static cues_config active;
static cues_revisions revisions;
static int commit_blocked;
static cmcp_json_t *reply;
static int reply_status;
static unsigned provider_stops, worker_attempts;
static int register_status, worker_status;
static max_align_t fake_provider;

static az_mcp_provider *registered_provider(void)
{
    return (az_mcp_provider *)&fake_provider;
}

const cues_config *cues_settings_config(void)
{
    return &active;
}

void cues_settings_revisions(cues_revisions *out)
{
    *out = revisions;
}

int cues_settings_remote_apply(const cues_config *config, unsigned expected)
{
    if (commit_blocked)
        return AZ_BUSY;
    if (expected != revisions.active)
        return AZ_STALE;
    active = *config;
    ++revisions.active;
    return AZ_OK;
}

int cues_settings_remote_save(unsigned expected)
{
    if (expected != revisions.active)
        return AZ_STALE;
    revisions.saved = expected;
    return AZ_OK;
}

int cues_mcp_apply_track(int deck, uint64_t load, unsigned revision)
{
    return deck == 0 && load == 5 && revision == revisions.active ? AZ_OK : AZ_STALE;
}

int cues_mcp_request_preview(int deck, uint64_t load, unsigned revision, uint64_t call)
{
    return cues_mcp_apply_track(deck, load, revision) == AZ_OK && call == 9 ? AZ_PENDING : AZ_STALE;
}

int az_mcp_register_tools(const az_core_v1 *core, const char *id,
                          const az_mcp_tool_v1 *entries, size_t count, az_mcp_provider **out)
{
    (void)core;
    assert(!strcmp(id, CUES_ID) && count == 19 && entries[0].grant == AZ_MCP_READ);
    *out = register_status == AZ_OK ? registered_provider() : NULL;
    return register_status;
}

void az_mcp_provider_stop(az_mcp_provider *p)
{
    assert(p == registered_provider());
    ++provider_stops;
}

int az_mcp_provider_complete(az_mcp_provider *p, uint64_t id, int status, cmcp_json_t *result)
{
    (void)p;
    assert(id == 9);
    cmcp_json_free(reply);
    reply = result;
    reply_status = status;
    return AZ_OK;
}

void az_mcp_provider_tick(az_mcp_provider *p)
{
    (void)p;
}

static int run(enum operation task, const char *json)
{
    const cmcp_json_t *arguments = cmcp_json_parse_cstr(json);
    assert(arguments);
    cmcp_json_t *immediate = NULL;
    int status = invoke(arguments, 9, az_mcp_clock() + AZ_MCP_NS, &immediate, &task);
    if (status == AZ_PENDING && atomic_load(&job.phase) == WORK) {
        worker_step(&job);
        cues_mcp_tick();
        worker_step(&job);
        status = reply_status;
    }
    cmcp_json_free(immediate);
    cmcp_json_free((cmcp_json_t *)arguments);
    return status;
}

static void prepare(void)
{
    cues_config_default(&active);
    active.group_count = 0;
    revisions = (cues_revisions){ .active = 1, .saved = 1, .available = 1 };
    generation = 100;
}

static void test_groups_rules(void)
{
    prepare();
    assert(run(RULES_LIST, "{}") == AZ_OK);
    assert(cmcp_json_array_len(cmcp_json_object_get(reply, "rules")) == 0);
    assert(run(GROUPS_CREATE, "{\"configVersion\":\"64-1\",\"group\":{\"name\":\"first\"}}") == AZ_OK);
    assert(run(GROUPS_LIST, "{}") == AZ_OK);
    assert(cmcp_json_array_len(cmcp_json_object_get(reply, "groups")) == 1);
    assert(run(GROUPS_GET, "{\"groupRef\":\"64-1:g1\"}") == AZ_STALE);
    assert(run(GROUPS_GET, "{\"groupRef\":\"64-2:g1\"}") == AZ_OK);
    assert(run(GROUPS_UPDATE, "{\"configVersion\":\"64-2\",\"groupRef\":\"64-2:g1\",\"patch\":{\"name\":\"renamed\"}}") == AZ_OK);
    assert(!strcmp(active.groups[0].name, "renamed"));

    const char *add = "{\"configVersion\":\"64-3\",\"newGroup\":{\"name\":\"second\"},"
        "\"rule\":{\"type\":\"hot\",\"pad\":\"A\",\"anchor\":{\"type\":\"absolute\",\"absolute\":{\"value\":10,\"unit\":\"seconds\"}}}}";
    assert(run(RULES_CREATE, add) == AZ_OK);
    assert(active.group_count == 2 && active.groups[1].rule_count == 1);
    assert(!strcmp(az_mcp_string(reply, "ruleRef"), "64-4:g2:r1"));
    assert(run(RULES_LIST, "{\"groupRef\":\"64-4:g2\"}") == AZ_OK);
    assert(cmcp_json_array_len(cmcp_json_object_get(reply, "rules")) == 1);
    assert(run(RULES_LIST, "{\"offset\":8}") == AZ_STALE);
    assert(run(RULES_GET, "{\"groupRef\":\"64-4:g2\",\"ruleRef\":\"64-4:g2:r1\"}") == AZ_OK);
    assert(run(RULES_UPDATE, "{\"configVersion\":\"64-4\",\"groupRef\":\"64-4:g2\",\"ruleRef\":\"64-4:g2:r1\",\"patch\":{\"name\":\"hot\"}}") == AZ_OK);
    assert(!strcmp(active.groups[1].rules[0].name, "hot"));
    assert(run(GROUPS_DELETE, "{\"configVersion\":\"64-5\",\"groupRef\":\"64-5:g2\"}") == AZ_INVALID);
    assert(run(GROUPS_MOVE, "{\"configVersion\":\"64-5\",\"groupRef\":\"64-5:g2\",\"position\":1}") == AZ_OK);
    assert(!strcmp(active.groups[0].name, "second"));
    assert(run(RULES_DELETE, "{\"configVersion\":\"64-6\",\"groupRef\":\"64-6:g1\",\"ruleRef\":\"64-6:g1:r1\"}") == AZ_OK);
    assert(run(GROUPS_DELETE, "{\"configVersion\":\"64-7\",\"groupRef\":\"64-7:g1\"}") == AZ_OK);
    assert(active.group_count == 1 && !strcmp(active.groups[0].name, "renamed"));
}

static void test_settings(void)
{
    prepare();
    assert(run(CONFIG_STATUS, "{}") == AZ_OK);
    assert(!strcmp(az_mcp_string(reply, "saveState"), "saved"));
    assert(run(SETTINGS_GET, "{}") == AZ_OK);
    const char *patch = "{\"configVersion\":\"64-1\",\"patch\":{\"enabled\":true}}";
    commit_blocked = 1;
    assert(run(SETTINGS_UPDATE, patch) == AZ_BUSY);
    commit_blocked = 0;
    assert(run(SETTINGS_UPDATE, patch) == AZ_OK);
    assert(active.enabled && revisions.active == 2);
    assert(!strcmp(az_mcp_string(reply, "saveState"), "savePending"));
    assert(run(SETTINGS_UPDATE, patch) == AZ_STALE);
    assert(run(CONFIG_SAVE, "{\"configVersion\":\"64-2\"}") == AZ_OK);
    assert(!strcmp(az_mcp_string(reply, "saveState"), "saved"));
    assert(run(CONFIG_VALIDATE, "{\"document\":{\"version\":1,\"config\":{},\"groups\":[]}}") == AZ_OK);
    assert(revisions.active == 2);
    assert(run(CONFIG_APPLY, "{\"configVersion\":\"64-2\",\"document\":{\"version\":1,\"config\":{},\"groups\":[]}}") == AZ_OK);
    assert(revisions.active == 3);
    assert(run(CONFIG_VALIDATE, "{\"document\":{\"version\":99}}") == AZ_INVALID);
    assert(run(PREVIEW, "{\"deck\":1,\"loadEpoch\":\"5\",\"configVersion\":\"64-3\"}") == AZ_PENDING);
    cues_plan plan = { .count = 1, .points = { { .slot = 1 } } };
    cues_mcp_preview_complete(9, AZ_OK, &plan);
    assert(cmcp_json_array_len(cmcp_json_object_get(reply, "points")) == 1);
    assert(run(APPLY_TRACK, "{\"deck\":1,\"loadEpoch\":\"5\",\"configVersion\":\"64-3\"}") == AZ_OK);
    assert(run(APPLY_TRACK, "{\"deck\":1,\"loadEpoch\":\"4\",\"configVersion\":\"64-3\"}") == AZ_STALE);
}

static int fake_thread(const char *id, const char *name, void *(*fn)(void *), void *user)
{
    assert(!strcmp(id, CUES_ID) && !strcmp(name, "cues-mcp-edit"));
    assert(fn && user);
    ++worker_attempts;
    return worker_status;
}

static void test_startup(void)
{
    const az_core_v1 core = { .size = sizeof core, .thread_start = fake_thread };
    register_status = AZ_BUSY;
    assert(cues_mcp_init(&core) == AZ_BUSY);
    assert(!worker_attempts && !provider_stops && !provider);
    register_status = AZ_OK;
    worker_status = -1;
    assert(cues_mcp_init(&core) == AZ_FAILED);
    assert(worker_attempts == 1 && provider_stops == 1 && !provider);
    worker_status = 0;
    assert(cues_mcp_init(&core) == AZ_OK);
    assert(worker_attempts == 2 && provider_stops == 1 && provider == registered_provider());
}

int main(void)
{
    test_startup();
    test_groups_rules();
    test_settings();
    cmcp_json_free(reply);
    puts("Cues MCP tool tests passed");
    return 0;
}
