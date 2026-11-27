# Cues

Create hot cues, memory cues and saved loops from **Phrase** and **Absolute** rules. Cues uses only the public AZ-MODS SDK; native cue creation belongs to the core.

Open **Mod Menu → Community Mods → Installed → Cues**. Open or add a group, then choose **+ Add rule**. Rules appear as sentences in the group. Tap a highlighted value in the sentence or select it with the browse knob to edit it. Use the toolbar to switch between **Phrase** and **Absolute**, or to show the **Offset** and **Loop** clauses. **Save rule** updates the group's draft and **Cancel** discards the current rule edit. Choose **Apply changes** to activate the draft and queue the USB save. **Files → Discard draft changes** restores the applied configuration.

Choose **Help** in Cues Settings or in the rule editor for a walkthrough and an example. **Back** returns to the rule you were editing.

On the waveform screen, press **CUES**, then choose a logical deck. Manual application works with automatic placement switched off. If the track has hot cues or memory cues, a confirmation asks to replace the whole cue bank. Cancel leaves it untouched. A track change or a cue edit while the confirmation is open invalidates it. If no rules match, existing cues stay intact.

**Config → Apply on track load** enables automatic placement, off by default. It waits for the analysis required by matching rules, the source sample rate, track duration and known cue state. Absolute rules need no phrase analysis. Beat/bar positions, beat/bar loop lengths and quantization wait for a usable grid before any part of the plan is saved. Any existing hot cue, memory cue or saved loop skips the entire track. Automatic placement never deletes or overwrites an existing bank. Saved definitions remain on the track after reload, so reloading a track containing them also skips placement. Changing rules does not repeat a completed automatic application on the same load; use the CUES button to replace an existing bank.

Groups support artist substring, exact original key and BPM range filters. Matching ignores ASCII letter case. Empty artist/key filters match any value; zero group BPM limits inherit the global range. New groups also inherit the global quantization setting by default. Unknown required metadata prevents a match. Up to eight groups can contain sixteen rules each.

Each rule selects:

- Hot cue A–H or memory cue, a native palette color, name and comment.
- **Phrase:** type, All or occurrence 1–10, start/middle/end, and an optional signed offset.
- **Absolute:** a zero-based position in seconds, beats or bars.
- A saved loop with a fixed length, or, for Phrase rules, from the phrase start / to its end.

For hot cues with **Occurrence → all**, the sentence says “on the next free pad.” Each matching occurrence takes the next free pad in A–H order, within the configured pad limit. Numbered occurrences and Absolute rules show a selectable fixed pad. Memory cue sentences omit the pad.

**Config → Consolidate phrases** treats consecutive phrases of the same decoded type that share a beat boundary as a single occurrence. Phrase rules use the combined span for start, middle, end, saved-loop boundaries, and label length. The JSON setting is `consolidatePhrases` and defaults to Off when omitted.

Hot-cue rules can use **Cue color → By pad** to assign colors by the pad that actually receives the cue. A–H use Red, Green, Aqua, Blue, Purple, Pink, Orange, and Yellow, respectively.

Seconds begin at the file start. Beat zero is the first observed grid beat; bar zero is its first observed downbeat. Fractional positions follow measured boundaries. Beat/bar calculations follow grid tempo and downbeats rather than assuming four beats per bar. Missing boundaries are skipped. Quantization is Off, nearest Beat or nearest Bar; ties choose the earlier boundary. Phrase labels use the SDK's raw analysis kinds, mapped consistently with Anchor.

Name and comment templates support two variables.

- `{phrase}` inserts the phrase type. Chorus/Drop phrases display `Chorus`, and Absolute rules display `Absolute`.
- `{length}` inserts the phrase or Absolute loop length in the group's selected units (Bars, Beats, or Seconds). It displays `0` for an Absolute point and `?` when the grid cannot resolve the length.

With **Bars** selected, `{phrase} ({length} bars)` can produce `Chorus (32 bars)`. The native SDK has one cue comment field, so name and comment are combined into that field, bounded to 127 UTF-8 bytes. Global collision preference resolves competing hot/memory candidates; other conflicts use group/rule order. Limits cannot exceed eight hot pads and ten memory cues.

The editor has touch controls, browse-knob navigation and the deck’s native keyboard. Smaller menu areas page the fields. Genre/rating filters and stem controls are omitted because they are not supported by this mod's SDK data model.

Applied configuration is saved by a worker to `MODS/community-mods/cues/data/cue-rules.json` on the installation USB. With the USB mounted at `/media/usb/sda1`, the full path is:

```text
/media/usb/sda1/MODS/community-mods/cues/data/cue-rules.json
```

Each JSON rule contains a `data` object used for placement and a generated `sentence` for display. If the sentences would exceed the 128 KiB file limit, Cues saves the rule data and regenerates the sentences when it loads them. Copy this file to keep a backup or share rules. To restore or edit rules outside the deck, replace the file while Cues is stopped, then restart AZ-MODS or the deck to load it. Cues validates the loaded values atomically and rejects unsupported rule types or fields. An invalid file loads the default configuration and reports the error. USB removal leaves the applied in-memory configuration available. **Files** shows save status and provides **Apply and save changes** and **Discard draft changes**.

The [default rules file](examples/cue-rules.json) enables automatic placement and phrase consolidation for all genres, with no artist/key filters and the full supported BPM range (1–500). Hot cues use distinct pad colors, and phrase-based cues show the phrase type and its total bar count. Save it as `cue-rules.json` at the location above. It contains:

- Hot A at the exact start of the track (zero seconds).
- A hot cue at the start of every Chorus/Drop, allocated to the next available pad.
- A memory cue 16 bars before the end of every Chorus/Drop.
- A memory cue 32 bars before the end of every Chorus/Drop.

Offsets follow the measured bar grid and may fall before the chorus starts. The preset gives hot cues priority: a memory cue at the same position is skipped, such as 32 bars before the end of a 32-bar chorus. Positions before the track starts or outside the available grid are skipped. The eight-hot-pad and ten-memory-cue limits still apply. The file is an optional starting preset; it does not replace existing settings automatically.

On a batch-capable core, Cues submits a complete plan and the waveform shows the saved bank when the batch settles. Older cores apply the plan cue by cue. Manual replacement deletes the confirmed bank before creating the new one. A write error can leave partial changes, and an uncertain result stops further work on that load until the track is reloaded. Cues uses the deck's native cue store and preserves playback, the playhead and the working loop.

Requires AZ-MODS 1.2.8 or newer **with the SDK `AZ_CMD_CUE_POINT_SET` / `AZ_CMD_CUE_LOOP_SET` capabilities**. A core without those commands can display the editor but cannot place cues.
The current core starts cue services by default. On older development builds,
enable the core's track-data, native-load and native-cue-command services before
starting the firmware. `azmod.native-cue-commands.off` disables cue commands in
the current core; this is separate from **Apply on track load** in Cues settings.

Open **Community Mods > Installed > Cues > Settings** to slide the editor across the
full content area. BACK closes an editor subpage first, then returns to the mod list.

Build and validate with the sibling SDK checkout:

```sh
../../az-mods-sdk/tools/azmod-pack build .
../../az-mods-sdk/tools/azmod-pack package . --github paixaop/az-mods
make unit
make test
```

See [TESTING.md](TESTING.md) for validation and firmware acceptance limits. Developer previews require Python with Pillow and run with `python3 tools/preview_settings.py`.

Cues accepts a native loaded track without a USB library reference, including
streaming loads. Writes still require the matching native load generation and cue
revision. Metadata filters only use qualified fields; unavailable metadata cannot
match a filter. Version 0.1.3 waits for a nonempty phrase map when a matching rule
needs phrases, including when Absolute rules could already run. An empty map
published during loading cannot consume automatic placement with a partial cue
bank. Absolute-only rules can still run without phrase analysis.
