# Anchor

Anchor times a transition so the incoming track's selected point reaches a chosen phrase boundary or
cue in the outgoing track. It shows a large instruction for the physical PLAY or loop EXIT
button. It never starts, stops, loops, cues or seeks a deck.

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

This is one mod and one installable package. Display changes take effect immediately on the
next UI tick and are saved with the other Anchor settings. The old crowded display is removed;
there is no compatibility mode or settings migration.

The guide currently appears in the **2-deck waveform view**. The card covers the blank/pad
area above the lower track panels; the sidebar covers the native Beat FX panel when open.
Positions on the 1280×800 screen are:

- Ribbon: x188, y230, 872×54.
- Card: x188, y442, 884×110.
- Sidebar: x1090, y89, 190×333 (right edge x1280).
- Closed Anchor tab: x1196, y292, 84×48, below Beat FX (observed y238–284).
- Open close handle: x1062, y292, 28×48, just left of the sidebar.

Tap the left triangle and **ANCHOR** label at the right screen edge to open the sidebar.
The labeled tab disappears while open, and a separate right-facing triangle on the left
of the sidebar closes it. The handle follows the drawer during its slide. This follows
the saved Show guide setting. The tab and drawer are
limited to the 2-deck waveform view. With no tracks loaded the open sidebar explains how
to load both decks, start the outgoing track and cue the incoming track before its drop.

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
| `DETECTING LOOP` | One wrap was observed; let it repeat before trusting a loop countdown |
| `EXIT LOOP NOW` | Press EXIT anywhere during this pass |
| `TARGET IN …` | The selected B point is approaching |
| `B TARGET PASSED` | Reposition B or choose another B target |
| `LOOP TO ALIGN` | The incoming deck is playing but its drop is off target |
| `RESTART THIS DECK` | Restart the incoming deck using the guide; its loop phase is off |
| `PICK A LATER TARGET` | There is no longer a usable start/release timing for this target |
| `MATCH DECK TEMPOS` | The decks differ by more than 0.15 BPM; match them first |
| `READING TRACKS` | Wait for the analysis reader |
| `ANALYSE TRACK` | Analyse/reload the indicated track in rekordbox |

Countdowns use bars and beats: `4.4`, `4.3`, `4.2`, `4.1`, then `3.4`.
The final bar reads `1.4` through `1.1`, followed by NOW. Beat ticks round up so the cue never
arrives early. `PLAY NOW` becomes eligible at the calculated launch moment and remains for
150 ms, allowing a UI tick to paint it. A missed paused-loop launch can offer another valid
start; otherwise the guide asks for a later target. Actual display latency and human timing
still require verification on a physical deck.

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
| Refresh analysis | Read loaded analysis again |

There is no loop-length setting. Older saved `b-loop` values are ignored and never used to
assume a loop ending at the drop. The default target is the end of the current phrase, consolidating
contiguous sections of the same category. Hot cues are used when no phrase map is available.
B initially selects the next chorus start, with a phrase/cue fallback, and holds that point
for the current track load. Each layout has separate A/B target buttons. Open **B TARGET**
to select **PHRASE** starts, **HOT CUE**, or **SET HERE**. B's picker spans the whole track,
independent of A's target settings. Set here captures the current playhead to the nearest
millisecond, including time zero, without snapping or changing saved cues. Move the head to
the desired point, mark it, then beat-jump back and prepare a loop of any suitable length.

B's target stays fixed across seeks, pause/resume, loop moves/resizes, A target changes,
analysis refreshes and layout changes. Loading/ejecting B clears it. Passing it produces
**B TARGET PASSED**; Anchor never silently substitutes a later chorus. Loop-out must be at
or before B's target, and its remaining travel to the target is included in EXIT timing.
An arbitrary playing-loop phase can still need a restart or a different A target.

Each layout has larger A/B target controls: 44px-high buttons in the ribbon and
36px-high buttons in the card and sidebar. The card now occupies x188/y430, 884×128px,
ending above the native deck panels. Unsupported jump controls are omitted.

### Automatic loop detection

Before 0.5.3, **Auto** inferred a loop from backward playhead movement; the fixed choices
assumed a loop ending at the next drop. A single backward seek could be accepted as a loop,
and subtracting the sampled positions underestimated its length by one sample's travel.

On the verified EP147 2.00 build, a dedicated read-only worker reads the current working
loop's actual IN/OUT points, including a loop prepared while B is paused. It checks native
owner types, logical deck, position validity, source frequency and stable repeated copies.
Retained or saved loop slots are separate. The planner uses the real loop length and the
remaining travel from loop-out to B's selected point, so extra passes can contribute to the
paused PLAY plan. The card/sidebar identify a paused engaged loop as **LOOP READY**.

The loop worker runs independently of USB analysis at 125ms intervals. Publications belong
to the current load sequence and expire after 750ms. Unknown/unstable native state produces
**READING LOOP**, with no launch instruction, rather than a speculative no-loop countdown.
EXIT, moves and resizing are read afresh; track replacement clears old points. See
[prepared-loop evidence](docs/PREPARED-LOOPS.md) for firmware scope and validation limits.

Other firmware keeps the snapshot/observation fallback: two consistent wraps, compatible
pass duration and boundary brackets. Sampling travel corrects inferred length, and a unique
grid beat inside the entry bracket refines the boundary. A lone seek is unconfirmed; pause,
sampling gaps and track changes discard stale inference. On that fallback, a newly prepared
paused loop cannot be timed until its boundaries are observed. Native boundaries avoid that
restriction on the verified build. Human PLAY/EXIT timing still needs audio checks.

## Analysis and timing

The mod reads the deck's loaded TrackPhrase/BeatGrid from EP147 memory through checked
`pread` calls on its worker thread. The loaded-state layout was verified on EP147 2.00.
ANLZ files provide hot cues and the file resolver when loaded-state reading is unavailable.
Tracks without a usable marker produce an analysis message rather than a guessed target.

Each observed load/eject invalidates that deck's old analysis, targets, picker and loop
state. The worker re-reads analysis for every load and for **Refresh analysis**; it never
reuses another track's cached result. Pending work is coalesced per deck, and stale reads
are discarded using load sequence and track ID/title/artist checks. Grid/phrase memory is
validated as one stable snapshot, with a brief retry when the grid arrives before phrases.
If the native UI has not created a deck's loaded state yet, its analysis stays pending and
retries on the worker (0.5–2 seconds between attempts). Deferred readiness retries do not
block the other deck, and a new load/eject cancels old work. A partial early state discovery
is rescanned; scan windows stay inside mapped memory. This fixes healthy loaded tracks
getting stuck at ANALYSE TRACK after an early "loaded state was not found" failure.
Hot cue mode requests cue files again for every new track. A failed cue read is reported
as a read failure rather than claiming the track has no cues.

Analysis times are at 100% pitch. Playback rate is current BPM divided by analysed BPM,
with measured playhead rate as a fallback. With no loop, launch time is the outgoing target
wall time minus the incoming head-to-drop travel time. With a loop, whole loop passes are
added so the final loop-out and the remaining run to the drop reach the target together.

No hooks, deck control, frame/touch filters or network. Surfaces receive taps inside their
own rectangles. The mod opens analysis files read-only and logs through the core.

## Build and verify

With the SDK at `../../sdk`:

```sh
make unit ui-test
make unit CC=clang UNIT_SAN_FLAGS='-fsanitize=address,undefined -fno-sanitize-recover=all'
make test SAN=plain,asan,tsan
../../sdk/tools/azmod-pack build .
../../sdk/tools/azmod-pack package .
make previews
```

The single installable package is `dist/anchor-0.6.1.azmod`. UI integration checks exercise
all three settings, live surface movement, saved selection, target paging/taps/BACK, deck
roles and text bounds. The SDK harness adds randomized callbacks and sanitizers. On macOS,
the harness cannot measure no-IO/no-allocation interposition; see TESTING.md.

`make previews` requires Pillow and a local monospace font. It renders the real C draw calls
into PNG/SVG and JSON under `output/implemented-previews/`; the host font differs from the
deck's native font. `out/render_ui file.json play|now|exit|selector ribbon|card|sidebar`
produces an individual draw list.

| Source | Purpose |
|---|---|
| anchor.c | Integration, analysis worker, loop detection, picker and runtime layout changes |
| display.c/.h | Action wording and all three layouts |
| settings.c/.h | Persisted settings and their page |
| mod.h | Single mod identity |
| guide.c/.h | Pure target selection and timing arithmetic |
| loaded.c/.h | Loaded analysis reader |
| anlz.c/.h, resolve.c/.h | ANLZ parsing and file resolution |
| tests/, tools/ | Host checks, renderer and development helpers |

GPL-3.0-only. Not affiliated with AlphaTheta, Pioneer DJ or rekordbox.
