# Cues testing

Run these checks from the Cues directory.

```sh
make unit
make test
../../az-mods-sdk/tools/azmod-pack build .
```

`make unit` exercises the production planner, public SDK consumer, configuration
parser, storage worker and settings callbacks. It covers time-zero positions,
variable meter and tempo, the native cue clock, missing grid and phrase data,
phrase consolidation, pad allocation and colors, collision handling, and the
default preset. Runtime tests check empty-bank admission, confirmation across
a load change, revision-fenced replacement, stale completions, and automatic
placement after delayed analysis. Save tests cover USB loss, failure and retry,
and applying a new draft while a save is pending.

`make test` runs the SDK lifecycle harness in plain, ASan/UBSan and TSan modes.
The host harness supplies controlled SDK responses. Use the Linux harness for
the no-I/O and no-allocation checks. The packager builds and validates the ARM64
library. The local settings and deck render tools exercise production drawing
on a test canvas.

## Firmware acceptance

1. Load a disposable export with an empty cue bank. Verify an absolute seconds
   rule can place a cue while phrase analysis is pending. On a fresh load,
   verify rules requiring phrases or grid data wait for the required data.
2. Apply a preset with hot and memory points and loops. Compare positions,
   labels and pad colors with the preset. Reload the source and verify the
   resulting cue bank is preserved.
3. Load a track that already contains saved cues or loops. Verify automatic
   placement leaves the bank intact. Start manual replacement, change the load
   before answering the confirmation, then verify the new bank survives.
4. Edit a setting, select Apply and save changes, and reload Cues. Verify the
   settings are restored. Exercise Discard draft changes and USB removal with
   the Files page visible.
