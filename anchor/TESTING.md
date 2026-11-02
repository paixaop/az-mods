# Testing Anchor 0.6.1

## Automated checks

```sh
make unit ui-test
make unit CC=clang UNIT_SAN_FLAGS='-fsanitize=address,undefined -fno-sanitize-recover=all'
make test SAN=plain,asan,tsan
../../sdk/tools/azmod-pack build .
../../sdk/tools/azmod-pack package .
../../sdk/tools/azmod-pack validate dist/anchor-0.6.1.azmod
python3 tools/preview_displays.py
```

The parser/timing unit suite covers synthetic ANLZ containers, phrase/cue targets,
consolidation, single/loop start plans, waiting, open windows, release and missed timing.
Deadline regressions verify that PLAY NOW is absent before launch, appears at launch,
remains during the 150 ms grace interval and expires afterwards. Monotonic deadlines are
64-bit and tested beyond 2^32 ms.

Load-lifecycle fixtures verify a fresh read on identical-metadata load notifications and
manual refresh, cancellation before/during eject, identity changes during reads, independent
analysis on two decks with identical library IDs/titles, artist-only changes, repeated loads
in Hot cue mode, unread cue errors, delayed phrase availability, a 100-load burst and stale
picker/target cleanup. A manual phrase target overrides a cue-less default Hot cue rule.
Synthetic memory fixtures reproduce a grid/phrase replacement between copies, wrong deck
identity, non-monotonic beats and unusable phrase-only state. Resolver fixtures reject
mismatched/ambiguous library identities and unidentified open ANLZ files. EXIT NOW is tested
25 ms before the final pass and at its exact start; the former must remain a countdown.

Display tests check actual incoming deck numbers, bars/beats, action priority and tempo
warnings. Every state/reason is drawn in all three layouts and several restricted areas;
text must stay in bounds and must not overlap other text.

Loop runtime tests cover 0.5/1/2/8/16/64-second loops with irregular 73–137ms samples at
0.85×, 1× and 1.12× rate, time-zero entry, one-off backward seeks, pause, sample outages,
release and loop resizing. They verify corrected duration and feed an observed loop through
the integration planner to check the exact exit deadline including the loop-out-to-drop tail.
Mock native state tests cover paused engagement, inactive state, invalid fields and wrong
track identity. These mocks do not imply that the installed core publishes native loops.

Display tests assert separate bar/beat colors and persistent purple looping status. Sidebar
integration covers empty-track instructions, 180ms drawer motion, hiding/reopening via the
remaining tab, 4-deck/non-deck suppression and disjoint native Beat FX/Anchor tab bounds.
Tab coordinates were measured from a live two-deck screenshot before deployment. The new
build's touch feel, native FX open/closed interaction and display latency still need checking.

0.5.4 checks the native-style drawer: closed label plus left triangle, open triangle-only
handle on the left, panel right edge at x1280, and the unchanged y292 handle position. The 0.6 handle adds an 10px gap from the drawer and a
premultiplied RGBA charcoal background (80% opacity), with an opaque fallback on older cores.
The open panel intentionally covers Beat FX. Lifecycle regressions delay LoadState readiness
across multiple worker attempts, prove the other deck can finish meanwhile, recover without
manual refresh, and cancel a deferred attempt on eject. Loaded-state regressions discard a
partial discovery and preserve transient snapshot-change errors as retriable. Worker deadlines
use an atomically published UI clock (the harness's simulated clock is not thread-safe).

Live 0.5.3 diagnosis found Deck 1 marked failed with "the loaded state was not found" while
both decks' native grids/phrases were already readable. The production reader probe returned
900 beats/21 phrases and 786 beats/19 phrases for those tracks. This is a readiness failure,
not evidence that those tracks lack rekordbox analysis.

The UI integration renderer checks all three Display choices using the same production
paint/input code. It verifies target paging, target selection, X/BACK, stale hit-rectangle
invalidation, deck role retention after PLAY, role invalidation on track replacement, Card
as the default, live surface movement after a setting change, and saving/reloading Display.

Quick controls tests run production draw/input callbacks at non-zero origins, from
240×180 docked areas through 1280×800. They check drawing and text bounds/overlap,
44×44 minimum touch targets, disjoint hit regions, direct/idempotent placement selection,
conditional phrase/hot-cue fields, touch-only paging to every setting, knob navigation,
saved values, refresh consumption, and invalidation of old hit regions. Smaller unsupported
areas draw nothing and accept no input. Corrupt stored choice values fall back to defaults.

The SDK harness runs plain, AddressSanitizer/UBSan and ThreadSanitizer passes over random
deck states, page sizes, events and reloads. On macOS, no-IO and no-allocation checks are not
measured because its interposer requires glibc. The standard randomized harness does not
supply functional scenarios; the separate UI integration test supplies the assertions above.

## Visual review

`make previews` draws actual C helper calls into `output/implemented-previews/`. The
comparison includes countdown, PLAY NOW, EXIT LOOP NOW and the target picker for Ribbon,
Card and Sidebar. These host previews use a monospace font rather than the deck's font.
`tools/preview_settings.py` also renders Quick controls in full-width and docked areas,
including Day/dark themes and phrase/cue/placement examples. These use production C paint
calls; the preview font differs from the deck's native font.

## Physical-deck checks still required for this release

1. Install the single Anchor package. In Anchor settings, cycle Display through Ribbon,
   Card and Sidebar. Confirm the overlay moves immediately, old pixels disappear, and the
   saved choice survives leaving settings and reloading the mod.
2. In the 2-deck waveform view, check that Ribbon sits between the waves, Card sits below
   them, and Sidebar opens over Beat FX at the screen's right edge. Test Day and
   dark themes. The guide remains hidden in 4-deck/non-deck views.
3. Choose a target by tap in every layout. Check paging, selection and X/BACK. Change
   Display while a picker was open and check that no old hit regions remain active.
4. With tempos matched, compare the PLAY countdown against the deck's beats. Verify the
   final bar changes to amber and reads 1.4 through 1.1, PLAY NOW is visible, and starting B retains the guide.
5. With an automatically detected loop ending at or before B's drop, verify EXIT IN counts down to
   the final pass and EXIT LOOP NOW lasts for that pass. Record audio to verify the deck's
   continue-to-loop-out behaviour and target/drop alignment.
6. Miss a start or release deliberately. Confirm the guide offers a usable later start or
   asks for a later target. A displayed grace interval is not a guarantee of exact alignment.
7. Unmatch tempos: MATCH DECK TEMPOS must replace the launch instruction. Restore matching
   tempos and verify the countdown returns.
8. Load tracks with phrases, cues only and no usable analysis. Check the target/analysis
   messages, refresh, eject/reload and deck-role changes.
9. Check both incoming-deck directions. With both tracks already playing and no clear
   incoming track, select Incoming deck explicitly instead of expecting a guessed role.
10. Load A, then B, reload a previously played track, eject during an analysis read and
    load rapidly several times. The guide must clear old targets immediately and finish
    with only the newest track's analysis. Repeat with Hot cue mode left selected and use
    Refresh analysis after re-analysing the same track.
11. Test Anchor HIDE/SHOW repeatedly, including no loaded tracks and both native Beat FX
    drawer states. Confirm the tabs do not overlap and hidden Anchor leaves a way to reopen.
12. Confirm cyan bars turn amber at the final bar, looping B shows purple status, and
    PLAY/EXIT NOW retains green. Test both incoming directions, loop resize, an isolated
    seek, pause/resume, 0.5s loops and a long loop. Let two wraps occur before expecting
    inferred loop timing; do not expect the current core to know a paused loop.

Load detection uses SDK load/eject notifications plus ID, title and artist changes. The
current SDK exposes no unique load counter: an identical reload with neither a notification
nor a metadata change cannot be distinguished automatically. Refresh analysis always forces
a fresh read. Actual metadata/provider update order still needs verification on the deck.

The previous 0.4 loaded-state reader was checked on a physical EP147 2.00 device. That does
not constitute physical verification of the new 0.5 display layouts or PLAY/EXIT timing.

## Fixed B targets (0.6)

The B destination suite runs the production picker and planner with multiple phrases,
Hot Cues (including time zero), a memory cue to exclude, and a custom millisecond marker.
It verifies that seeking, loop movement/resizing and loop phase errors retain the selected
point, that loop-out-to-target travel changes EXIT timing, and that passing the target never
retargets. It checks reload/eject cleanup, stale picker rejection, and Set here without B
analysis. UI integration taps PHRASE, HOT CUE and SET HERE through all three layouts and
checks every drawn text box for bounds and overlaps. Display tests verify the large sidebar
bar font, smaller beat suffix, final-bar sequence/color, and translucent handle pixel alpha.

Physical checks: mark B, jump back eight bars, set a four-bar loop, and verify the remaining
four-bar tail is accounted for. Repeat with an adjusted loop and Hot Cue target, change A
and Display, then reload B to check cleanup. Compare Anchor/Beat FX open-handle gaps and
transparency over the waveform. No jump control is drawn or exposed in any layout.

## Paused prepared loops (0.6.1)

Native-reader fixtures cover paused engagement without a wrap, inactive working property
with retained points, zero IN, resizing, logical-deck/type/coordinate-owner mismatches,
invalid rate/cache conversions, unknown positions and data changing between copies. Timing
integration verifies that native prepared points yield a loop-start plan with extra passes
before any playhead wrap, that EXIT restores the single-play plan, that B may approach IN
from before the loop, and that expired/prior-load data suppresses launch guidance.

A read-only hardware probe confirmed Deck 2 engaged while paused at IN 209ms, OUT 15448ms
(length 15239ms, matching 32 beats at source 126 BPM), with the other three decks inactive.
This is reader validation plus host-tested planner integration; it does not claim a physical
audio alignment measurement. Native owner discovery includes up to 2048 anonymous mappings,
searching largest arenas first within a 768MiB budget; the live process had 545 such mappings.
Missing or retired owners are retried with a two-second scan limit. Runtime point copies are
bounded and do not scan again while the checked owners remain valid.

UI integration asserts larger control heights and no JUMP text/control in all three layouts.
