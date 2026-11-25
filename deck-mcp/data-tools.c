/* SPDX-License-Identifier: GPL-3.0-only */
#include "data-tools.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

uint64_t mcp_uint64_arg(const cmcp_json_t *args, const char *key)
{
    const char *value = az_mcp_string(args, key);
    if (!value || strspn(value, "0123456789") != strlen(value))
        return 0;
    errno = 0;
    uint64_t number = strtoull(value, NULL, 10);
    return errno ? 0 : number;
}

bool mcp_track_arg(const cmcp_json_t *args, az_track_ref *out)
{
    const cmcp_json_t *ref = cmcp_json_object_get(args, "track");
    uint64_t track = mcp_uint64_arg(ref, "trackId");
    if (!track || track > UINT32_MAX)
        return false;
    *out = (az_track_ref){ .size = sizeof *out, .version = AZ_DATA_VERSION,
        .source_id = mcp_uint64_arg(ref, "sourceId"),
        .source_generation = mcp_uint64_arg(ref, "sourceGeneration"),
        .library_id = mcp_uint64_arg(ref, "libraryId"), .track_id = (uint32_t)track };
    return out->source_id && out->source_generation && out->library_id;
}

bool mcp_session_arg(const struct mcp_runtime *runtime, const cmcp_json_t *args)
{
    const char *session = az_mcp_string(args, "sessionId");
    return session && !strcmp(session, runtime->session);
}

cmcp_json_t *mcp_sdk_result(const struct mcp_runtime *runtime, int status)
{
    cmcp_json_t *result = mcp_session_result(runtime);
    mcp_set(result, "status", cmcp_json_new_int(status));
    return result;
}

bool mcp_sdk_append(cmcp_json_t *array, cmcp_json_t *value)
{
    if (cmcp_json_array_append(array, value) == 0)
        return true;
    cmcp_json_free(value);
    return false;
}

uint32_t mcp_page_limit(const cmcp_json_t *args)
{
    uint64_t limit = az_mcp_number(args, "limit");
    return limit && limit <= 128 ? (uint32_t)limit : 32;
}

cmcp_json_t *mcp_clock_receipt(const az_component_info *info)
{
    cmcp_json_t *out = cmcp_json_new_object();
    mcp_set(out, "availability", cmcp_json_new_int(info->availability));
    mcp_set(out, "provenance", cmcp_json_new_int(info->provenance));
    mcp_set(out, "observedNs", mcp_id(info->observed_ns));
    mcp_set(out, "revision", mcp_id(info->revision));
    return out;
}
