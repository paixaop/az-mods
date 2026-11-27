/* SPDX-License-Identifier: GPL-3.0-only */
#include "cues.h"
#include "az-mcp-provider.h"
#include "wire.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONFIG_BYTES (128u * 1024u)
#define VERSION_BYTES 42
#define REF_BYTES 64
#define PAGE_SIZE 8

enum operation {
    RULES_LIST, RULES_GET, RULES_CREATE, RULES_UPDATE, RULES_DELETE,
    GROUPS_LIST, GROUPS_GET, GROUPS_CREATE, GROUPS_UPDATE, GROUPS_DELETE, GROUPS_MOVE,
    SETTINGS_GET, SETTINGS_UPDATE, CONFIG_STATUS, CONFIG_SAVE, CONFIG_VALIDATE, CONFIG_APPLY,
    PREVIEW, APPLY_TRACK
};
enum job_phase { IDLE, WORK, COMMIT, CLEANUP };

struct config_job {
    atomic_int phase;
    enum operation operation;
    uint64_t call, deadline;
    cues_revisions revisions;
    cues_config snapshot, candidate;
    const cmcp_json_t *args;
    cmcp_json_t *result;
    int status;
    bool changes;
};

static az_mcp_provider *provider;
static struct config_job job;
static uint64_t generation;

static void put(cmcp_json_t *object, const char *key, cmcp_json_t *value)
{
    az_mcp_set(object, key, value);
}

static void put_string(cmcp_json_t *object, const char *key, const char *value)
{
    put(object, key, cmcp_json_new_string(value));
}

static void put_int(cmcp_json_t *object, const char *key, int value)
{
    put(object, key, cmcp_json_new_int(value));
}

static void version_text(char version[VERSION_BYTES], unsigned revision)
{
    snprintf(version, VERSION_BYTES, "%llx-%u", (unsigned long long)generation, revision);
}

static bool version_matches(const cmcp_json_t *args, unsigned revision)
{
    const char *expected = az_mcp_string(args, "configVersion");
    char version[VERSION_BYTES];
    version_text(version, revision);
    return expected && !strcmp(expected, version);
}

static void save_receipt(cmcp_json_t *out, const cues_revisions *receipt)
{
    char revision[VERSION_BYTES];
    version_text(revision, receipt->active);
    put_string(out, "configVersion", revision);
    version_text(revision, receipt->saved);
    put_string(out, "savedVersion", revision);
    const char *state = "savePending";
    if (!receipt->available || (receipt->failed == receipt->active && !receipt->saving && !receipt->queued))
        state = "saveFailed";
    else if (receipt->saved == receipt->active)
        state = "saved";
    put_string(out, "saveState", state);
}

static bool found_ref(const char *argument, unsigned version, int group, int rule)
{
    char prefix[VERSION_BYTES], ref[REF_BYTES];
    version_text(prefix, version);
    if (rule >= 0)
        snprintf(ref, sizeof ref, "%s:g%d:r%d", prefix, group + 1, rule + 1);
    else
        snprintf(ref, sizeof ref, "%s:g%d", prefix, group + 1);
    return argument && !strcmp(argument, ref);
}

static void put_ref(cmcp_json_t *out, const char *key, unsigned version, int group, int rule)
{
    char prefix[VERSION_BYTES], ref[REF_BYTES];
    version_text(prefix, version);
    if (rule >= 0)
        snprintf(ref, sizeof ref, "%s:g%d:r%d", prefix, group + 1, rule + 1);
    else
        snprintf(ref, sizeof ref, "%s:g%d", prefix, group + 1);
    put_string(out, key, ref);
}

static int select_group(const struct config_job *work)
{
    const char *ref = az_mcp_string(work->args, "groupRef");
    for (int i = 0; i < work->snapshot.group_count; ++i)
        if (found_ref(ref, work->revisions.active, i, -1))
            return i;
    return -1;
}

static int select_rule(const struct config_job *work, int group)
{
    const char *ref = az_mcp_string(work->args, "ruleRef");
    for (int i = 0; i < work->snapshot.groups[group].rule_count; ++i)
        if (found_ref(ref, work->revisions.active, group, i))
            return i;
    return -1;
}

static const cmcp_json_t *groups(const cmcp_json_t *doc)
{
    return cmcp_json_object_get(doc, "groups");
}

static const cmcp_json_t *rules(const cmcp_json_t *doc, int group)
{
    return cmcp_json_object_get(cmcp_json_array_at(groups(doc), (size_t)group), "rules");
}

static cmcp_json_t *tagged(const cmcp_json_t *data, unsigned version, int group, int rule)
{
    cmcp_json_t *entry = cmcp_json_new_object();
    put_ref(entry, "groupRef", version, group, -1);
    if (rule >= 0)
        put_ref(entry, "ruleRef", version, group, rule);
    put(entry, rule >= 0 ? "rule" : "group", cmcp_json_clone(data));
    return entry;
}

static void append(cmcp_json_t *list, cmcp_json_t *entry)
{
    if (cmcp_json_array_append(list, entry))
        cmcp_json_free(entry);
}

static long long integer(const cmcp_json_t *args, const char *key)
{
    const cmcp_json_t *field = cmcp_json_object_get(args, key);
    return field && field->type == CMCP_JSON_INT ? field->i : 0;
}

static int list_groups(struct config_job *work, const cmcp_json_t *doc)
{
    cmcp_json_t *entries = cmcp_json_new_array();
    for (int i = 0; i < work->snapshot.group_count; ++i) {
        const cues_group *group = &work->snapshot.groups[i];
        cmcp_json_t *entry = cmcp_json_new_object();
        put_ref(entry, "groupRef", work->revisions.active, i, -1);
        put_string(entry, "name", group->name);
        put(entry, "enabled", cmcp_json_new_bool(group->enabled));
        put_int(entry, "ruleCount", group->rule_count);
        append(entries, entry);
    }
    (void)doc;
    put(work->result, "groups", entries);
    return AZ_OK;
}

struct rule_page {
    cmcp_json_t *entries;
    long long offset, count;
    unsigned revision;
};

static void page_group(struct rule_page *page, const cmcp_json_t *doc, int group)
{
    const cmcp_json_t *list = rules(doc, group);
    for (size_t i = 0; i < cmcp_json_array_len(list); ++i) {
        long long index = page->count++;
        if (index >= page->offset && index < page->offset + PAGE_SIZE)
            append(page->entries, tagged(cmcp_json_array_at(list, i), page->revision, group, (int)i));
    }
}

static bool rule_page_valid(const struct config_job *work, int selected, long long offset)
{
    if (cmcp_json_object_get(work->args, "groupRef") && selected < 0)
        return false;
    return !offset || version_matches(work->args, work->revisions.active);
}

static int list_rules(struct config_job *work, const cmcp_json_t *doc)
{
    int selected = select_group(work);
    long long offset = integer(work->args, "offset");
    if (!rule_page_valid(work, selected, offset))
        return AZ_STALE;
    struct rule_page page = { .entries = cmcp_json_new_array(), .offset = offset,
                              .revision = work->revisions.active };
    for (int i = 0; i < work->snapshot.group_count; ++i)
        if (selected < 0 || selected == i)
            page_group(&page, doc, i);
    put(work->result, "rules", page.entries);
    put(work->result, "nextOffset", page.offset + PAGE_SIZE < page.count
        ? cmcp_json_new_int(page.offset + PAGE_SIZE) : cmcp_json_new_null());
    return AZ_OK;
}

static int get_group(struct config_job *work, const cmcp_json_t *doc)
{
    int group = select_group(work);
    if (group < 0)
        return AZ_STALE;
    const cues_group *current = &work->snapshot.groups[group];
    cmcp_json_t *entry = tagged(cmcp_json_array_at(groups(doc), (size_t)group), work->revisions.active,
                               group, -1);
    put(entry, "effectiveMinBpm", cmcp_json_new_double(current->min_bpm ? current->min_bpm : work->snapshot.min_bpm));
    put(entry, "effectiveMaxBpm", cmcp_json_new_double(current->max_bpm ? current->max_bpm : work->snapshot.max_bpm));
    put_int(entry, "effectiveQuantization", current->quantization ? current->quantization : work->snapshot.quantization);
    put(work->result, "entry", entry);
    return AZ_OK;
}

static int get_rule(struct config_job *work, const cmcp_json_t *doc)
{
    int group = select_group(work);
    int rule = group >= 0 ? select_rule(work, group) : -1;
    if (rule < 0)
        return AZ_STALE;
    put(work->result, "entry", tagged(cmcp_json_array_at(rules(doc, group), (size_t)rule),
                                     work->revisions.active, group, rule));
    return AZ_OK;
}

/* cMCP owns the JSON tree. Deep merge only allowed top-level fields and
 * preserve nested settings unless the patch supplies a replacement value. */
static int merge(cmcp_json_t *target, const cmcp_json_t *patch);

static int merge_member(cmcp_json_t *target, const char *key, const cmcp_json_t *value)
{
    cmcp_json_t *old = (cmcp_json_t *)cmcp_json_object_get(target, key);
    if (old && old->type == CMCP_JSON_OBJECT && value->type == CMCP_JSON_OBJECT)
        return merge(old, value);
    return az_mcp_set(target, key, cmcp_json_clone(value));
}

static int merge(cmcp_json_t *target, const cmcp_json_t *patch)
{
    if (!target || target->type != CMCP_JSON_OBJECT || !patch || patch->type != CMCP_JSON_OBJECT)
        return AZ_INVALID;
    for (size_t i = 0; i < patch->obj.len; ++i) {
        int status = merge_member(target, patch->obj.keys[i], patch->obj.values[i]);
        if (status)
            return status;
    }
    return AZ_OK;
}

static cmcp_json_t *without(const cmcp_json_t *array, int index)
{
    cmcp_json_t *out = cmcp_json_new_array();
    for (size_t i = 0; i < cmcp_json_array_len(array); ++i)
        if ((int)i != index)
            append(out, cmcp_json_clone(cmcp_json_array_at(array, i)));
    return out;
}

static cmcp_json_t *moved(const cmcp_json_t *array, int index, int target)
{
    cmcp_json_t *out = cmcp_json_new_array();
    for (size_t i = 0, source = 0; i < cmcp_json_array_len(array); ++i) {
        if ((int)i == target) {
            append(out, cmcp_json_clone(cmcp_json_array_at(array, (size_t)index)));
            continue;
        }
        if ((int)source == index)
            ++source;
        append(out, cmcp_json_clone(cmcp_json_array_at(array, source++)));
    }
    return out;
}

static cmcp_json_t *config_doc(const cues_config *config)
{
    char *buffer = malloc(CONFIG_BYTES);
    if (!buffer)
        return NULL;
    int size = cues_config_json(config, buffer, CONFIG_BYTES);
    cmcp_json_t *doc = size > 0 ? cmcp_json_parse(buffer, (size_t)size) : NULL;
    free(buffer);
    return doc;
}

static int validate_candidate(struct config_job *work, const cmcp_json_t *doc)
{
    char *text = cmcp_json_emit(doc);
    if (!text)
        return AZ_FAILED;
    char error[160] = {0};
    bool valid = cues_config_parse(&work->candidate, text, strlen(text), error, sizeof error);
    free(text);
    if (!valid)
        put_string(work->result, "error", error[0] ? error : "Invalid Cues configuration");
    return valid ? AZ_OK : AZ_INVALID;
}

static bool fields_allowed(const cmcp_json_t *patch, const char *const *allowed)
{
    if (!patch || patch->type != CMCP_JSON_OBJECT)
        return false;
    for (size_t i = 0; i < patch->obj.len; ++i) {
        const char *key = patch->obj.keys[i];
        const char *const *field = allowed;
        while (*field && strcmp(*field, key))
            ++field;
        if (!*field || strlen(key) != patch->obj.key_lens[i])
            return false;
    }
    return true;
}

static int merge_patch(cmcp_json_t *target, const cmcp_json_t *patch, enum operation operation)
{
    static const char *const group[] = {"enabled", "name", "comment", "tags", "quantization", "bpmRange", "measuresIn", NULL};
    static const char *const rule[] = {"enabled", "type", "pad", "anchor", "offset", "loop", "color", "name", "comment", NULL};
    static const char *const settings[] = {"enabled", "maxMemoryCues", "hotCuePads", "minBpm", "maxBpm",
        "minDistance", "cuePreference", "quantization", "measuresIn", "consolidatePhrases", NULL};
    const char *const *allowed = operation == SETTINGS_UPDATE ? settings
        : operation == RULES_UPDATE ? rule : group;
    if (!fields_allowed(patch, allowed))
        return AZ_INVALID;
    return merge(target, patch);
}

static cmcp_json_t *add_group(struct config_job *work, cmcp_json_t *doc, const cmcp_json_t *definition)
{
    cmcp_json_t *list = (cmcp_json_t *)groups(doc);
    if (cmcp_json_array_len(list) >= CUES_MAX_GROUPS)
        return NULL;
    cmcp_json_t *group = cmcp_json_parse_cstr("{\"rules\":[]}");
    if (!group || merge_patch(group, definition, GROUPS_CREATE) != AZ_OK ||
        cmcp_json_array_append(list, group)) {
        cmcp_json_free(group);
        return NULL;
    }
    put_int(work->result, "groupIndex", (int)cmcp_json_array_len(list));
    return group;
}

static cmcp_json_t *rule_group(struct config_job *work, cmcp_json_t *doc, int group)
{
    const cmcp_json_t *new_group = cmcp_json_object_get(work->args, "newGroup");
    if (group >= 0 && !new_group)
        return (cmcp_json_t *)cmcp_json_array_at(groups(doc), (size_t)group);
    if (!cmcp_json_object_get(work->args, "groupRef") && new_group)
        return add_group(work, doc, new_group);
    return NULL;
}

static int add_rule(struct config_job *work, cmcp_json_t *doc, int group)
{
    const cmcp_json_t *definition = cmcp_json_object_get(work->args, "rule");
    cmcp_json_t *target = rule_group(work, doc, group);
    cmcp_json_t *list = (cmcp_json_t *)cmcp_json_object_get(target, "rules");
    if (!list || cmcp_json_array_len(list) >= CUES_MAX_RULES)
        return AZ_INVALID;
    cmcp_json_t *entry = cmcp_json_new_object();
    if (az_mcp_set(entry, "data", cmcp_json_clone(definition)) || cmcp_json_array_append(list, entry)) {
        cmcp_json_free(entry);
        return AZ_FAILED;
    }
    put_int(work->result, "ruleIndex", (int)cmcp_json_array_len(list));
    if (group >= 0)
        put_int(work->result, "groupIndex", group + 1);
    return AZ_OK;
}

static int edit_rule(struct config_job *work, cmcp_json_t *doc)
{
    int group = select_group(work);
    if (work->operation == RULES_CREATE)
        return add_rule(work, doc, group);
    int rule = group >= 0 ? select_rule(work, group) : -1;
    if (rule < 0)
        return AZ_STALE;
    const cmcp_json_t *list = rules(doc, group);
    if (work->operation == RULES_DELETE) {
        cmcp_json_t *target = (cmcp_json_t *)cmcp_json_array_at(groups(doc), (size_t)group);
        return az_mcp_set(target, "rules", without(list, rule)) == 0 ? AZ_OK : AZ_FAILED;
    }
    const cmcp_json_t *entry = cmcp_json_array_at(list, (size_t)rule);
    return merge_patch((cmcp_json_t *)cmcp_json_object_get(entry, "data"),
                       cmcp_json_object_get(work->args, "patch"), RULES_UPDATE);
}

static int remove_group(struct config_job *work, cmcp_json_t *doc, int group)
{
    if (work->snapshot.groups[group].rule_count &&
        !cmcp_json_bool(cmcp_json_object_get(work->args, "deleteRules")))
        return AZ_INVALID;
    return az_mcp_set(doc, "groups", without(groups(doc), group)) == 0 ? AZ_OK : AZ_FAILED;
}

static int move_group(struct config_job *work, cmcp_json_t *doc, int group)
{
    long long target = integer(work->args, "position") - 1;
    if (target < 0 || target >= work->snapshot.group_count)
        return AZ_INVALID;
    return az_mcp_set(doc, "groups", moved(groups(doc), group, (int)target)) == 0 ? AZ_OK : AZ_FAILED;
}

static int edit_group(struct config_job *work, cmcp_json_t *doc)
{
    if (work->operation == GROUPS_CREATE)
        return add_group(work, doc, cmcp_json_object_get(work->args, "group")) ? AZ_OK : AZ_INVALID;
    int group = select_group(work);
    if (group < 0)
        return AZ_STALE;
    if (work->operation == GROUPS_DELETE)
        return remove_group(work, doc, group);
    if (work->operation == GROUPS_MOVE)
        return move_group(work, doc, group);
    cmcp_json_t *target = (cmcp_json_t *)cmcp_json_array_at(groups(doc), (size_t)group);
    return merge_patch(target, cmcp_json_object_get(work->args, "patch"), GROUPS_UPDATE);
}

static int edit(struct config_job *work, cmcp_json_t *doc)
{
    if (work->operation >= RULES_CREATE && work->operation <= RULES_DELETE)
        return edit_rule(work, doc);
    if (work->operation >= GROUPS_CREATE && work->operation <= GROUPS_MOVE)
        return edit_group(work, doc);
    return merge_patch((cmcp_json_t *)cmcp_json_object_get(doc, "config"),
                       cmcp_json_object_get(work->args, "patch"), SETTINGS_UPDATE);
}

static int handle_doc(struct config_job *work, cmcp_json_t *doc)
{
    switch (work->operation) {
    case RULES_LIST: return list_rules(work, doc);
    case RULES_GET: return get_rule(work, doc);
    case GROUPS_LIST: return list_groups(work, doc);
    case GROUPS_GET: return get_group(work, doc);
    case SETTINGS_GET:
        put(work->result, "settings", cmcp_json_clone(cmcp_json_object_get(doc, "config")));
        return AZ_OK;
    default: break;
    }
    int status = edit(work, doc);
    if (status == AZ_OK)
        status = validate_candidate(work, doc);
    work->changes = status == AZ_OK;
    return status;
}

static int prepare_config(struct config_job *work)
{
    if (work->operation == CONFIG_STATUS || work->operation == CONFIG_SAVE)
        return AZ_OK;
    if (work->operation == CONFIG_VALIDATE || work->operation == CONFIG_APPLY) {
        int status = validate_candidate(work, cmcp_json_object_get(work->args, "document"));
        work->changes = status == AZ_OK && work->operation == CONFIG_APPLY;
        return status;
    }
    cmcp_json_t *doc = config_doc(&work->snapshot);
    if (!doc)
        return AZ_FAILED;
    int status = handle_doc(work, doc);
    cmcp_json_free(doc);
    return status;
}

static void worker_step(struct config_job *work)
{
    int phase = atomic_load(&work->phase);
    if (phase == WORK) {
        work->result = cmcp_json_new_object();
        work->changes = false;
        work->status = work->result ? prepare_config(work) : AZ_FAILED;
        atomic_store(&work->phase, COMMIT);
    } else if (phase == CLEANUP) {
        cmcp_json_free(work->result);
        work->result = NULL;
        atomic_store(&work->phase, IDLE);
    }
}

static void *config_worker(void *user)
{
    struct config_job *work = user;
    const struct timespec wait = { .tv_nsec = 15000000 };
    for (;;) {
        worker_step(work);
        nanosleep(&wait, NULL);
    }
    return NULL;
}

static void updated_refs(struct config_job *work)
{
    long long group = integer(work->result, "groupIndex");
    long long rule = integer(work->result, "ruleIndex");
    if (group > 0)
        put_ref(work->result, "groupRef", work->revisions.active, (int)group - 1, -1);
    if (group > 0 && rule > 0)
        put_ref(work->result, "ruleRef", work->revisions.active, (int)group - 1, (int)rule - 1);
}

static void finish_config(struct config_job *work)
{
    if (work->status == AZ_OK && !az_mcp_before(az_mcp_clock(), work->deadline))
        work->status = AZ_STALE;
    if (work->status == AZ_OK && work->changes)
        work->status = cues_settings_remote_apply(&work->candidate, work->revisions.active);
    if (work->status == AZ_OK && work->operation == CONFIG_SAVE)
        work->status = cues_settings_remote_save(work->revisions.active);
    cues_settings_revisions(&work->revisions);
    save_receipt(work->result, &work->revisions);
    if (work->status == AZ_OK)
        updated_refs(work);
    if (az_mcp_provider_complete(provider, work->call, work->status, work->result) == AZ_OK)
        work->result = NULL;
    atomic_store(&work->phase, CLEANUP);
}

static uint64_t deck_epoch(const cmcp_json_t *args)
{
    const char *value = az_mcp_string(args, "loadEpoch");
    if (!value || strspn(value, "0123456789") != strlen(value))
        return 0;
    return strtoull(value, NULL, 10);
}

static int track_action(enum operation operation, const cmcp_json_t *args, uint64_t call,
                         unsigned revision, cmcp_json_t **result)
{
    long long deck = integer(args, "deck");
    uint64_t epoch = deck_epoch(args);
    if (!epoch || deck < 1 || deck > 4)
        return AZ_INVALID;
    if (operation == PREVIEW)
        return cues_mcp_request_preview((int)deck - 1, epoch, revision, call);
    int status = cues_mcp_apply_track((int)deck - 1, epoch, revision);
    if (status == AZ_OK)
        *result = cmcp_json_parse_cstr("{\"placement\":\"requested\"}");
    return status;
}

static int invoke(const cmcp_json_t *args, uint64_t call, uint64_t deadline,
                   cmcp_json_t **result, void *user)
{
    const enum operation operation = *(const enum operation *)user;
    cues_revisions revisions;
    cues_settings_revisions(&revisions);
    const cmcp_json_t *version = cmcp_json_object_get(args, "configVersion");
    if (version && !version_matches(args, revisions.active))
        return AZ_STALE;
    if (operation == PREVIEW || operation == APPLY_TRACK)
        return track_action(operation, args, call, revisions.active, result);
    if (atomic_load(&job.phase) != IDLE)
        return AZ_BUSY;
    job.operation = operation;
    job.call = call;
    job.deadline = deadline;
    job.revisions = revisions;
    job.args = args;
    job.snapshot = *cues_settings_config();
    atomic_store(&job.phase, WORK);
    return AZ_PENDING;
}

static cmcp_json_t *point_json(const cues_point *point)
{
    cmcp_json_t *entry = cmcp_json_new_object();
    put_int(entry, "classification", (int)point->classification);
    put_int(entry, "slot", (int)point->slot);
    put_int(entry, "type", (int)point->type);
    put_int(entry, "rgb", (int)point->rgb);
    put(entry, "startSeconds", cmcp_json_new_double(point->start_s));
    put(entry, "endSeconds", cmcp_json_new_double(point->end_s));
    put_string(entry, "comment", point->comment);
    return entry;
}

void cues_mcp_preview_complete(uint64_t call, int status, const cues_plan *plan)
{
    cmcp_json_t *result = cmcp_json_new_object();
    cmcp_json_t *points = cmcp_json_new_array();
    if (plan) {
        put_int(result, "skipped", plan->skipped);
        for (int i = 0; i < plan->count; ++i)
            append(points, point_json(&plan->points[i]));
    }
    put(result, "points", points);
    if (az_mcp_provider_complete(provider, call, status, result) != AZ_OK)
        cmcp_json_free(result);
}

#define W "\"key\":{\"type\":\"string\",\"maxLength\":64},\"configVersion\":{\"type\":\"string\"}"
#define WR "\"key\",\"configVersion\""
#define V "\"configVersion\":{\"type\":\"string\"}"
#define GROUP "\"groupRef\":{\"type\":\"string\"}"
#define RULE "\"ruleRef\":{\"type\":\"string\"}"
#define PATCH "\"patch\":{\"type\":\"object\"}"
#define DOC "\"document\":{\"type\":\"object\"}"
#define TRACK "\"deck\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4},\"loadEpoch\":{\"type\":\"string\",\"maxLength\":20}"
#define SHAPE(properties, required) "{\"type\":\"object\",\"properties\":{" properties "},\"required\":[" required "],\"additionalProperties\":false}"
#define TOOL(id, op, grant, summary, props, req) \
    { sizeof(az_mcp_tool_v1), id, NULL, summary, SHAPE(props, req), "{\"type\":\"object\"}", grant, invoke, \
      (void *)&(const enum operation){op} }

static const az_mcp_tool_v1 tools[] = {
    TOOL("rules_list", RULES_LIST, AZ_MCP_READ, "List applied rules in planner order.",
         V "," GROUP ",\"offset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":128}", ""),
    TOOL("rules_get", RULES_GET, AZ_MCP_READ, "Read an applied rule.", GROUP "," RULE, "\"groupRef\",\"ruleRef\""),
    TOOL("rules_create", RULES_CREATE, AZ_MCP_SAVED, "Create a rule from Cues JSON rule data.",
         W "," GROUP ",\"rule\":{\"type\":\"object\"},\"newGroup\":{\"type\":\"object\"}", WR ",\"rule\""),
    TOOL("rules_update", RULES_UPDATE, AZ_MCP_SAVED, "Patch fields on an applied rule.",
         W "," GROUP "," RULE "," PATCH, WR ",\"groupRef\",\"ruleRef\",\"patch\""),
    TOOL("rules_delete", RULES_DELETE, AZ_MCP_SAVED, "Delete a rule.", W "," GROUP "," RULE, WR ",\"groupRef\",\"ruleRef\""),
    TOOL("groups_list", GROUPS_LIST, AZ_MCP_READ, "List applied rule groups in planner order.", "", ""),
    TOOL("groups_get", GROUPS_GET, AZ_MCP_READ, "Read a group and its effective settings.", GROUP, "\"groupRef\""),
    TOOL("groups_create", GROUPS_CREATE, AZ_MCP_SAVED, "Create an empty group.",
         W ",\"group\":{\"type\":\"object\"}", WR ",\"group\""),
    TOOL("groups_update", GROUPS_UPDATE, AZ_MCP_SAVED, "Patch an applied group.", W "," GROUP "," PATCH, WR ",\"groupRef\",\"patch\""),
    TOOL("groups_delete", GROUPS_DELETE, AZ_MCP_SAVED, "Delete a group. Nonempty groups require deleteRules.",
         W "," GROUP ",\"deleteRules\":{\"type\":\"boolean\"}", WR ",\"groupRef\""),
    TOOL("groups_move", GROUPS_MOVE, AZ_MCP_SAVED, "Move a group to a one-based position.",
         W "," GROUP ",\"position\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8}", WR ",\"groupRef\",\"position\""),
    TOOL("settings_get", SETTINGS_GET, AZ_MCP_READ, "Read applied global settings.", "", ""),
    TOOL("settings_update", SETTINGS_UPDATE, AZ_MCP_SAVED, "Patch global settings.", W "," PATCH, WR ",\"patch\""),
    TOOL("config_status", CONFIG_STATUS, AZ_MCP_READ, "Read applied and saved config revisions.", "", ""),
    TOOL("config_save", CONFIG_SAVE, AZ_MCP_SAVED, "Retry saving the applied config to USB.", W, WR),
    TOOL("config_validate", CONFIG_VALIDATE, AZ_MCP_READ, "Validate a complete Cues JSON configuration.", DOC, "\"document\""),
    TOOL("config_apply", CONFIG_APPLY, AZ_MCP_SAVED, "Apply a complete Cues JSON configuration.", W "," DOC, WR ",\"document\""),
    TOOL("preview", PREVIEW, AZ_MCP_READ, "Preview cue placements for a qualified deck load.",
         V "," TRACK, "\"configVersion\",\"deck\",\"loadEpoch\""),
    TOOL("apply_to_deck", APPLY_TRACK, AZ_MCP_SAVED, "Request the Cues placement workflow for a qualified deck load.",
         W "," TRACK, WR ",\"deck\",\"loadEpoch\"")
};

int cues_mcp_init(const az_core_v1 *core)
{
    generation = az_mcp_clock();
    if (!generation)
        return AZ_FAILED;
    az_mcp_provider *started = NULL;
    int status = az_mcp_register_tools(core, CUES_ID, tools, sizeof tools / sizeof tools[0], &started);
    if (status != AZ_OK)
        return status;
    if (core->thread_start(CUES_ID, "cues-mcp-edit", config_worker, &job)) {
        az_mcp_provider_stop(started);
        return AZ_FAILED;
    }
    provider = started;
    return AZ_OK;
}

void cues_mcp_tick(void)
{
    if (atomic_load(&job.phase) == COMMIT)
        finish_config(&job);
    az_mcp_provider_tick(provider);
}
