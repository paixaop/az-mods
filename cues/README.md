# Cues

Create hot cues, memory cues and saved loops from **Phrase** and **Absolute** rules. Cues uses only the public AZ-MODS SDK; native cue creation belongs to the core.

Open **Mod Menu → Community Mods → Installed → Cues**. Create a group, add its rules, and choose **Apply changes**. Edits remain a draft until Apply; **Files → Discard draft** restores the applied configuration.

On the waveform screen, press **CUES**, then choose a logical deck. Manual application works with automatic placement switched off. If the track has hot cues or memory cues, a confirmation asks to replace the whole cue bank. Cancel leaves it untouched. A track change or a cue edit while the confirmation is open invalidates it. If no rules match, existing cues stay intact.

**Config → Apply on track load** enables automatic placement, off by default. It waits for phrase analysis, the source sample rate, track duration and known cue state. Any existing hot cue, memory cue or saved loop skips the entire track. Automatic placement never deletes or overwrites an existing bank. Saved definitions remain on the track after reload, so reloading a track containing them also skips placement. Changing rules does not repeat a completed automatic application on the same load; use the CUES button to replace an existing bank.

Groups support artist substring, exact original key and BPM range filters. Matching ignores ASCII letter case. Empty artist/key filters match any value; zero group BPM limits inherit the global range. Unknown required metadata prevents a match. Up to eight groups can contain sixteen rules each.

Each rule selects:

- Hot cue A–H or memory cue, a native palette color, name and comment.
- **Phrase:** type, All or occurrence 1–10, start/middle/end, and an optional signed offset.
- **Absolute:** a zero-based position in seconds, beats or bars.
- A saved loop with a fixed length, or, for Phrase rules, from the phrase start / to its end.

Seconds begin at the file start. Beat zero is the first observed grid beat; bar zero is its first observed downbeat. Fractional positions follow measured boundaries. Beat/bar calculations follow grid tempo and downbeats rather than assuming four beats per bar. Missing boundaries are skipped. Quantization is Off, nearest Beat or nearest Bar; ties choose the earlier boundary. Phrase labels use the SDK's raw analysis kinds, mapped consistently with Anchor.

`{phrase}` and `{length}` are available in cue names/comments. The length uses the group's selected measures. The native SDK has one cue comment field, so name and comment are combined into that field, bounded to 127 UTF-8 bytes. Global collision preference resolves competing hot/memory candidates; other conflicts use group/rule order. Limits cannot exceed eight hot pads and ten memory cues.

The editor has touch controls, browse-knob navigation and an on-screen keyboard. Smaller menu areas page the fields. Genre/rating filters and stem controls are omitted because they are not supported by this mod's SDK data model.

Applied configuration is saved by a worker to `data/cue-rules.json` on the installation USB. **Files** imports `data/cue-rules-import.json` as a draft and exports applied settings to `data/cue-rules-export.json`. Apply the imported draft to activate it. [An example](examples/cue-rules.json) combines an Intro hot cue, a pre-drop loop and a memory cue at file start; copy it to the import filename to edit it on the deck. Import is bounded to 128 KiB, validates all values atomically and rejects unsupported rule types or fields. USB removal leaves the applied in-memory configuration available; the Files page reports save failures.

Manual replacement performs revision-fenced SDK deletions followed by creation. These are separate persistent writes. An error can leave partial changes; they cannot be rolled back atomically. An uncertain result stops further work on that load until the track is reloaded. Cues saves definitions in the deck's native cue store while preserving playback, the playhead and the working loop. It never triggers or recalls a cue, or engages a saved loop.

Requires AZ-MODS 1.2.6 or newer **with the SDK `AZ_CMD_CUE_POINT_SET` / `AZ_CMD_CUE_LOOP_SET` capabilities**. A core without those commands can display the editor but cannot place cues.

Build and validate with the sibling SDK checkout:

```sh
../../az-mods-sdk/tools/azmod-pack build .
../../az-mods-sdk/tools/azmod-pack package . --github paixaop/az-mods
make unit
make test
```

See [TESTING.md](TESTING.md) for validation and firmware acceptance limits. Developer previews require Python with Pillow and run with `python3 tools/preview_settings.py`.
