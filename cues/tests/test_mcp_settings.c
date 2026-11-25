/* SPDX-License-Identifier: GPL-3.0-only */
#define main local_settings_regressions
#include "test_settings.c"
#undef main

int main(void)
{
    assert(local_settings_regressions() == 0);
    cues_revisions receipt;
    cues_settings_revisions(&receipt);
    cues_config candidate = *cues_settings_config();
    candidate.enabled = !candidate.enabled;
    assert(cues_settings_remote_apply(&candidate, receipt.active + 1) == AZ_STALE);
    draft_pending = 1;
    assert(cues_settings_remote_apply(&candidate, receipt.active) == AZ_BUSY);
    draft_pending = 0;
    view = RULE;
    assert(cues_settings_remote_apply(&candidate, receipt.active) == AZ_BUSY);
    view = GROUPS;
    assert(cues_settings_remote_apply(&candidate, receipt.active) == AZ_OK);
    assert(cues_settings_config()->enabled == candidate.enabled);
    cues_settings_revisions(&receipt);
    assert(receipt.queued);
    pending_save = 0;
    assert(cues_settings_remote_save(receipt.active - 1) == AZ_STALE);
    assert(cues_settings_remote_save(receipt.active) == AZ_OK);
    cues_settings_revisions(&receipt);
    assert(receipt.saving == receipt.active);
    puts("Cues MCP settings handoff passed");
    return 0;
}
