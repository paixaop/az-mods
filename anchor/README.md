# Anchor

Anchor times a transition so the incoming track's selected point reaches a chosen phrase boundary or
cue in the outgoing track. It shows a large instruction for the physical PLAY or loop EXIT
button. Anchor is a read-only guide and requires AZ-MODS 1.2.6 or newer.

## Choose your display

Open **Community Mods > Installed > Anchor**, or **Anchor settings** in the Mod Menu.
Change **Display** to one of:

| Display | Placement | What you see |
|---|---|---|
| **Ribbon** | Between the two waveforms | One large instruction, e.g. `PLAY DECK 2 IN 4 BARS` |
| **Card** (default) | Below the scrolling waveforms | Large action, deck number, and a quiet target line |
| **Sidebar** | Over Beat FX, flush with the right edge | A large countdown, loop status and vertical target picker |

Anchor settings uses **Quick controls**: visual placement buttons and a labeled sample
preview above separate **Outgoing target** and **Incoming deck** groups. Tap a placement to
select it directly; tap a value or its arrows to change it. Phrase options appear only in
Phrase mode; the hot-cue slot appears only when **Use any hot cue** is off. Settings save
automatically and use the Mod Menu's active theme.

The page fits the area the core supplies inside the 1280×800 deck screen. At 900×420 or
larger it shows Quick controls together. Smaller/docked areas use **Guide / Targets /
Incoming** tabs and bottom page arrows, keeping every touch target at least 44×44 pixels.
Turn the browse knob to move focus; press it to change the focused setting. BACK returns
to the menu. The sample preview illustrates placement and is not live playback timing.

Display changes apply immediately and are saved with the other Anchor settings.

The guide appears in the **two-deck waveform view**. The card covers the blank/pad
area above the lower track panels; the sidebar covers the native Beat FX panel when open.
Positions on the 1280×800 screen are:

- Ribbon at x188, y230, 872×54.
- Card at x188, y430, 884×128.
- Sidebar at x1090, y89, 190×333 (right edge x1280).
- Closed Anchor tab at x1195, y292, 85×48, below Beat FX.
- Open close handle at x1054, y292, 26×48, just left of the sidebar.

Tap the left triangle and **ANCHOR** label at the right screen edge to open the sidebar.
The right-facing triangle to the left of the open sidebar closes it. Both taps apply
immediately and update the saved **Show guide** setting. With empty decks, the open
sidebar explains how to load both decks, start A and cue B.

## Use it

1. Load two analysed tracks, with the outgoing track playing.
2. Tap the guide or its **A TARGET** control. Pick the outgoing phrase
   boundary, hot cue or grid boundary where the incoming drop should land.
3. Tap **B TARGET** to choose a phrase start, Hot Cue or **SET HERE**, then prepare B's start/loop.
4. Follow the large instruction for the indicated deck. `PLAY NOW` means press its physical
   PLAY button. `EXIT LOOP NOW` means press its loop EXIT button during the displayed pass.

Anchor uses deck numbers in the main display. In settings, A means outgoing and B means
incoming. Auto establishes these roles while one track is paused, or from the looping track.
Once established, roles are retained when both tracks play, until the track identities or
playback selection change. Two unrelated tracks already playing require an explicit
**Incoming deck** selection if neither is uniquely looping.

## What the instructions mean

| Instruction | Action |
|---|---|
| `PLAY IN …` / `PLAY DECK N IN …` | Wait for the incoming deck's launch moment |
| `PLAY NOW` / `PLAY DECK N NOW` | Press that deck's PLAY button |
| `EXIT IN …` | Keep looping; this counts down to the final loop pass |
| `READING LOOP` | Wait for the SDK to publish the working loop |
| `LOOP STATE UNAVAILABLE` | The SDK has no usable working loop state for timing |
| `EXIT LOOP NOW` | Press EXIT anywhere during this pass |
| `TARGET IN …` | The selected B point is approaching |
| `B TARGET PASSED` | Reposition B or choose another B target |
| `LOOP TO ALIGN` | The incoming deck is playing but its drop is off target |
| `RESTART THIS DECK` | Restart the incoming deck using the guide; its loop phase is off |
| `PICK A LATER TARGET` | There is no longer a usable start/release timing for this target |
| `MATCH DECK TEMPOS` | The decks differ by more than 0.15 BPM; match them first |
| `READING TRACKS` | Wait for the SDK's analysis events |
| `READING DECKS` | Wait for current playback timing |
| `HOT CUES UNAVAILABLE` | Choose a phrase or grid target |
| `ANALYSE TRACK` | Analyse/reload the indicated track in rekordbox |

Countdowns use bars and beats: `4.4`, `4.3`, `4.2`, `4.1`, then `3.4`.
The final bar reads `1.4` through `1.1`, followed by NOW. Beat ticks round up so the cue never
arrives early. `PLAY NOW` becomes eligible at the calculated launch moment and remains for
150 ms. A missed paused-loop launch can offer another valid start. When all start windows
have passed, the guide asks for a later target.

Cyan numbers indicate **bars.beats**; the entire counter turns amber in the **final bar**.
The bar numeral retains each layout's size; the `.beat` suffix uses a smaller font.
Purple status text and the **LOOPING** label identify incoming playback in a confirmed loop.
Green indicates act now or aligned, amber messages indicate a warning, and red indicates a
missed target. Text accompanies the colour. Details such as loop
counts and phase errors are secondary to the next action: the card's target picker shows
the detailed status, and the sidebar picker shows its loop count. The compact ribbon picker
keeps its available space for target chips.

The EXIT window is one whole final loop pass: exiting lets playback continue to loop-out
and then into the drop. Exiting before that pass brings the drop forward.

## Targets and settings

Tap a target to latch it for the transition; `<` and `>` change picker pages and `X`/BACK
closes it. Labels such as `CHORUS END` describe the actual phrase boundary. `HC1`–`HC8`
identify hot cues. `16-BAR` and `32-BAR` identify grid boundaries. A chip's `m:ss` value is
its absolute position within the picker's track, rather than a countdown. Sidebar targets
are stacked vertically, four per page.

| Setting | Choices / purpose |
|---|---|
| Show guide | On/off |
| Display | Ribbon, Card (default), Sidebar |
| Target type | Phrase, Hot cue, 16-bar grid, 32-bar grid |
| Default phrase | Any, Intro, Build, Break, Chorus, Outro, Verse, Bridge |
| Phrase point | Ending or Start |
| Join matching phrases | Join contiguous phrases of the same category into one section |
| Use any hot cue | Use the next cue of any slot |
| Default hot cue | HC1–HC8 when Any hot cue is off |
| Incoming deck | Auto, Deck 1 or Deck 2 |

The default A target is the end of the current phrase, consolidating contiguous sections
of the same category. Hot cues are used when no phrase map is available.
B initially selects the next chorus start, with a phrase/cue fallback, and holds that point
for the current track load. Each layout has separate A/B target buttons. Open **B TARGET**
to select **PHRASE** starts, **HOT CUE**, or **SET HERE**. B's picker spans the whole track,
independent of A's target settings. Set here captures the current playhead to the nearest
millisecond, including time zero, without snapping or changing saved cues. Move the head to
the desired point, mark it, then beat-jump back and prepare a loop of any suitable length.

B's target stays fixed across seeks, pause/resume, loop moves/resizes, A target changes,
analysis updates and layout changes. Loading/ejecting B clears it. Passing it produces
**B TARGET PASSED**; Anchor never silently substitutes a later chorus. Loop-out must be at
or before B's target, and its remaining travel to the target is included in EXIT timing.
An arbitrary playing-loop phase can still need a restart or a different A target.

The A/B target buttons are 44px high in Ribbon and 36px high in Card and Sidebar.

## Deck events and timing

Anchor subscribes to load, transport, playhead, tempo, loop and analysis updates for
Decks 1 and 2. A new load epoch clears that deck's old targets, including when the same
track is reloaded. Grid, phrase and cue edits update the picker as the SDK publishes them.
Phrase boundaries use the exact grid revision named by the SDK. An available live cue bank
takes precedence over its saved edition, including after the last live cue is deleted.

The planner uses the SDK's working IN and OUT, including when B is paused. The card and
sidebar show **LOOP READY** for a paused, engaged loop. **READING LOOP** covers pending or
stale loop state. The SDK's explicit disengagement lets the guide switch to a single-start
plan or follow the run to B's target after EXIT.

Analysis times are source milliseconds at 100% pitch. The planner aligns the two audible
playheads to a shared monotonic time and uses the live playback rate where available. For
a paused launch it uses current tempo divided by the analysed tempo. The PLAY calculation
subtracts B's travel time from A's target deadline. For an engaged loop it also considers
whole passes and the travel from loop-out to B's target.

## Build and verify

With the SDK at `../../az-mods-sdk`:

```sh
make unit ui-test
make unit CC=clang UNIT_SAN_FLAGS='-fsanitize=address,undefined -fno-sanitize-recover=all'
make test SAN=plain,asan,tsan
../../az-mods-sdk/tools/azmod-pack build .
../../az-mods-sdk/tools/azmod-pack package .
make previews
```

The single installable package is `dist/anchor-0.7.0.azmod`. UI integration checks exercise
all three settings, live surface movement, saved selection, target paging/taps/BACK, deck
roles and text bounds. The SDK harness adds randomized callbacks and sanitizers.
See [TESTING.md](TESTING.md) for the deck checks.

`make previews` requires Pillow and a local monospace font. It renders the real C draw calls
into PNG/SVG and JSON under `output/implemented-previews/`; the host font differs from the
deck's native font. `out/render_ui file.json play|now|exit|selector ribbon|card|sidebar`
produces an individual draw list.

| Source | Purpose |
|---|---|
| anchor.c | Subscription, guide updates, layouts and target picker |
| display.c/.h | Action wording and all three layouts |
| settings.c/.h | Persisted settings and their page |
| mod.h | Single mod identity |
| guide.c/.h | Pure target selection and timing arithmetic |
| deck.c/.h | Status receipts and mod-owned copies of analysis |
| tests/, tools/ | Host checks, renderer and development helpers |

GPL-3.0-only. Not affiliated with AlphaTheta, Pioneer DJ or rekordbox.
