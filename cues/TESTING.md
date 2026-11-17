# Cues validation

`make unit` covers the pure evaluator, asynchronous SDK consumer, JSON parser, worker persistence and production settings draw/input callbacks. It checks actual sample rates, variable tempo and meter, missing boundaries, offsets, loop ranges, filtering, limits, UTF-8, direct replacement of `data/cue-rules.json` followed by startup loading, invalid JSON, USB loss, paging, touch geometry, knob navigation and staged Apply.

The import/export controls used in the historical firmware validation below have been removed. Configuration now uses `data/cue-rules.json` directly; the Files page retains save status, Apply/save and draft discard. The historical results do not validate the current Files page.

The default rules file is parsed and evaluated against two choruses with three-beat bars and a tempo change. The fixture checks track time zero, both chorus starts, and both negative bar offsets for each chorus, including hot-cue priority when 16/32-bar choruses cause a collision. Automatic hot-pad fixtures cover occupied pads, skipped candidates, configured pad limits, all eight pads and fixed numbered occurrences. Settings fixtures check the `*` display, disabled memory-cue pad field, and switching back to a fixed pad. JSON tests cover `pad: "*"` round trips and rejection outside hot-cue All rules. All five unit suites pass in plain and ASan/UBSan builds.

A 48 kHz creator clock paired with a 44.1 kHz POSITION clock checks that planning uses the SDK native-cue source clock. The consumer fixture checks that automatic placement waits for analysis and skips an entire existing bank. It also checks manual replacement with auto-load disabled, confirmation cancellation, a changed cue revision during confirmation, sequential public SDK deletion/creation, cache lag after completion, request cleanup and a track reload before an old completion arrives. A subscription resync cannot repeat an already completed or skipped automatic application on the same load. A failed or uncertain SDK write stops the sequence.

The runtime stub accepts only saved-point creation, saved-loop creation, hot deletion and memory deletion. A regression injects a same-load RESYNC during a confirmed four-cue replacement and verifies that all deletions and creations finish. The fresh status image still has to satisfy the track, analysis and native-revision fences. Reload fixtures retain the saved point and loop definitions and check that automatic placement makes zero further submission attempts. A memory loop alone and a hot loop alone also protect a reloaded track.

The consumer fixture leaves the legacy `deck_snapshot.track` unsupported. It joins a qualified READY native LOAD status to the fresh native command generation, rejects a non-native load state, and defers a delayed status image after the native target has changed.

`make test` runs the SDK harness in plain, AddressSanitizer/UndefinedBehaviorSanitizer, and ThreadSanitizer passes. The host has no native player store: its `native_cues` service returns `AZ_UNSUPPORTED`, so these passes exercise initialization, UI and lifecycle without claiming firmware writes. On macOS the harness does not measure its Linux-only no-I/O/no-allocation gates. The Linux plain pass also passed both of those gates with `azmod-test:18.04` and `TEST_CC=clang-10 TEST_CXX=clang++-10`. The functional unit fixtures cover native-command policy with controlled SDK responses.

`../../az-mods-sdk/tools/azmod-pack build .` builds the AArch64 shared library in the SDK container and validates its manifest, exports, imports and PNG. `tools/preview_settings.py` renders the actual settings draw calls. `tools/render_deck.c` renders the production CUES button and logical-deck picker on a neutral canvas; it does not establish how the firmware behind them looks.

Earlier firmware 1.30 emulator validation on logical deck 0 saved a memory point at zero, a hot point at 12.5 seconds, a memory loop at 30–32.5 seconds and a hot loop at 20–22.5 seconds. It verified labels, colors, unchanged paused source and preservation of earlier saved records. All four definitions survived a graceful shutdown and cold boot. Occupied-hot, duplicate-memory and stale-revision requests were refused by the SDK without changing the bank. The stale-load probe case was refused before SDK admission and is not a firmware service pass. The actual settings page also passed import/Apply, USB persistence, confirmation cancellation, manual replacement and automatic placement after an empty load.

The firmware 2.00 save-only acceptance on 2026-11-07 used logical deck 0, a source-bound core/probe pair, the packaged Cues mod and a 44.1 kHz source and engine. Configuration was imported and applied through the actual settings page. Its four rules save:

- Hot A point at 12.5 seconds, red, with its configured label.
- Memory point at zero, aqua, with its configured label.
- Memory loop at 30–32.5 seconds, blue, with its configured label.
- Hot B loop spanning the first phrase, 0.447–5.365 seconds, red, with its configured label. These boundaries were independently decoded from the fixture's saved PSSI and PQTZ sections.

Automatic mode preserved the original existing four-record bank with zero SDK submission attempts. Confirmed manual replacement deleted all four original records and saved all four configured definitions. This reproduces the previous same-load resync stall and verifies its fix. The independent source history remained stationary throughout paused replacement; the native working-loop observation remained disengaged. Cancel was tested only after the replacement dialog was visibly present and preserved both the bank and the submission-attempt counter.

The same four-rule replacement also passed while playing. Independent source observations before and after showed MOVING, the captured playhead never moved backwards, and all intervening native cue snapshots retained the same load and disengaged working-loop state. The resulting saved bank matched all four rules. Playback was paused explicitly with the physical PLAY button only after this check, to prepare the next reload.

After manual placement, an explicit native track reload retained all four saved definitions, including classifications, slots, timing, loop bounds, colors and labels. The load generation advanced, while Cues' attempted-submission counter remained at eight (four deletions and four creations). Repeated observations after reload retained that counter and the new load's cue revision, proving that Cues did not even attempt another write. Counter inspection reads only the test mod's own state and does not mutate the player or cue store.

A fresh 2.00 boot with the applied settings and an empty saved bank passed automatic placement on its first track load. Cues saved the exact four definitions with four submissions and marked that load complete, with no loop engaged. The initial source observer ran before native loading completed; the saved result and completed automatic epoch were validated afterwards. Full source-history preservation throughout writes is established by the separate paused and playing manual cases, which use the same save commands.

Reloading the automatically populated track retained all four definitions. Cues received a qualified loaded track, marked the new epoch processed through its existing-bank protection, and kept its submission counter at four. Neither its counter nor the reloaded cue revision changed during repeated observations.

One separate warm, repeated-load observation remains incomplete for automatic placement: the native store was empty, but the SDK's `LOAD.track` stayed STALE and no usable analysis reached Cues. It made zero writes, as required for an unqualified load. The retained library row and analysis path were intact. This observation is not counted as an automatic-placement pass; the fresh-load pass above does not resolve that source-provider case.

The legacy independent deck snapshot does not expose its main cue in this fixture. Main-cue preservation is covered by the core's typed postcondition and host fixtures. The emulator results above are separate from the physical results below.

Physical XDJ-AZ validation on 2026-11-09 used Deck 1 (logical deck 0), the packaged Cues 0.1.0 build, the same accepted core pair and a 44.1 kHz source and engine. The deck reports firmware 2.01; its EP147 executable has SHA-256 `6577f42532375936c7e3fa830efa35b7daa9cde8e0a265c1fc7143f824321925`, matching the retained 2.00 native profile. The test track was Ariana Grande's “no tears left to cry,” with the same independently decoded phrase boundaries described above. The core, configuration, approvals and original track analysis files were backed up before testing.

| Physical case | Result |
| --- | --- |
| Import and Apply through the settings UI | Passed. One group containing the four rules above was persisted to the USB. |
| Cancel visible replacement confirmation | Passed. All 16 original saved records remained unchanged, with zero Cues submissions and 229 stationary source observations. |
| Confirm replacement while paused | Passed. Exactly 20 submissions deleted the 16 original records and saved the four configured definitions. Classifications, slots, times, loop bounds, labels and colors matched. All 94 captured source observations remained stationary; the working loop remained disengaged. |
| Confirm replacement while playing | Passed. The public SDK's `HOT_TRIGGER` started playback from the original Hot A point. After a fresh independent MOVING observation, replacement deleted all 16 original records and saved the four configured definitions in exactly 20 submissions. All 101 captured source observations were nondecreasing, the source remained MOVING afterward, and the working loop remained disengaged. `MAIN_RECALL` then paused playback, confirmed by a fresh STATIONARY observation. |
| Manual bank persistence and automatic existing-bank protection | Passed after restarting the native player process and reloading the track. All four records survived. Automatic mode processed the load with zero submissions; five subsequent snapshots retained the cue revision. |
| Automatic placement on an empty first load | Passed after clearing the saved bank through public SDK commands and restarting the native player. Exactly four submissions saved the four definitions. The 155 captured loaded-source observations remained stationary, and no working loop engaged. |
| Automatic bank and configuration persistence through device reboot | Passed after an orderly operating-system reboot, confirmed by a changed boot ID. The applied configuration and all four saved definitions survived. Automatic mode made zero submissions on reload; six subsequent snapshots retained the cue revision. |

Cue mutations used only public SDK commands. The independent position observer checked source motion during the paused cases. Submission counts came from read-only inspection of Cues' own state. Timing comparisons used `compare_timestamps()` with the verified source/engine rates and a one-sample window. The original core pair, configuration, approvals and all three original analysis files were restored and verified byte-for-byte afterward; the library database was unchanged. The original mods were running with all decks empty, and temporary test mods, flags and rollback timer were removed.

The initial remote button fixture received zero hardware input frames. The playback case was subsequently completed through the existing public `HOT_TRIGGER` and `MAIN_RECALL` commands, without a physical button press or a new firmware hook. The trigger's intentional seek occurred before the replacement baseline. This run does not establish main-cue preservation, preservation of an already engaged working loop, audible continuity, other decks or sample rates, abrupt power-loss durability, or the separate warm repeated-load source-provider case described above. Local transcripts, screenshots, snapshots, source histories, build hashes and restoration checks are retained under `temp/cues-physical-20261109/` and `temp/cues-physical-playback-20261109/` in the parent workspace.

## 0.1.2 load-source regression

The runtime fixture now delivers native loaded events without a qualified
`LOAD.track`, including a stale nonzero track payload whose valid bit is clear.
Automatic planning and revision-fenced SDK submission proceed for that load.
Unit-scoped artist/title fields stay unavailable without a qualified join, while
metadata explicitly scoped to that load remains usable. Repeated events preserve
the plan. Existing replacement, stale-generation and occupied-bank checks remain.
The runtime passed plain and ASan/UBSan runs; the SDK lifecycle harness passed plain
and ASan/UBSan. This does not establish authenticated Beatport firmware acceptance.

All five `make unit` suites pass with the updated storage and settings fixtures.

Filtered groups wait for required artist, key and tempo metadata. A delayed
qualified USB reference and metadata enrichment during an admitted write retain
the selected plan for the same native load, avoiding a partially populated bank.
The regression delivers that join while the first command is pending and verifies
that the second command uses the same load and updated cue revision.

Firmware 2.00 on the Cortex-A72 emulator, with the updated default core and no
SDK feature `.on` files, automatically saved the three configured definitions
on an empty ordinary USB track: one hot point and two memory entries. The public
probe observed metadata/reference enrichment between the second and third writes;
all three completed. This reproduces the partial-bank case and verifies the fix.

## 0.1.3 delayed phrase analysis

The evaluator regression combines an Absolute track-start cue with a Phrase
rule. EMPTY and LOADING phrase maps return `AZ_PENDING` with an empty plan;
disabling the Phrase rule permits the Absolute cue, and a later READY map
produces the complete plan. This prevents a new load's temporary empty map from
consuming automatic placement before Track Analysis publishes its results.
All five unit suites pass in plain and AddressSanitizer/UndefinedBehaviorSanitizer
builds. The AArch64 library and package validate successfully.

Physical acceptance on 2026-11-16 used the qualified 2.00 EP147 image (the deck
reports firmware 2.01), Cues 0.1.3 and the corrected core native-mood reader.
Fresh Beatport loads of Victory Lap and Nowhere to Run started with empty cue
banks. Cues made no writes until Track Analysis published 15 and 13 phrases,
then automatically created 8 hot/10 memory and 8 hot/5 memory cues respectively.
Both completed during the original load. The grid row counts and revisions
remained unchanged. The default preset's chorus selectors matched public PSSI
moods 2 and 1 respectively; checking only the phrase count would have missed the
previous core enum-conversion defect.

Four additional Beatport loads automatically produced complete plans, including
phrase-based cues. Two USB loads preserved existing 7/7 and 8/8 hot/memory banks.
This USB check covers preservation and automatic load handling, not generation on
an empty physical USB bank. Anchor reacted to SDK Play for both sources. All panel
and touch operations used public SDK commands. The parent workspace retains the
logs, screenshots, source and binary hashes in `temp/physical-auto-20261116/`.
