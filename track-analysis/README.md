# Track Analysis

Track Analysis ports DJTrackAnalysis 6.0.0 into an AZ-MODS community mod.
It automatically checks a loaded USB OneLibrary track, analyzes missing phrases
in a separate low-priority ARM64 process, and asks the public SDK to save PSSI
in the track's USB `ANLZ0000.EXT`. When PQTZ is missing, it also generates the
beat grid in `ANLZ0000.DAT`. Existing populated grids and phrases are preserved.
Cues, waveforms, unknown tags and existing PMAI header extensions survive unchanged.

This is a development implementation. **Streaming is not implemented.** The current
public source provider cannot qualify a streamed track's downloaded audio or its USB
destination. Folder-only tracks are also unsupported. For library rows without an analysis
location, the core creates a stable USB ANLZ folder and queues a conditional native
library update. That update waits for playback to permit the existing spool service. The mod requires a core build containing the new analysis service;
checking a version number alone is insufficient. It does not require private mod hooks.

The worker runs on the deck. It supports MPEG-1 Layer III MP3 at 44.1/48 kHz, FLAC and WAV, up to twenty minutes.
MP3 decoding preserves the encoded frame clock and the original conditional header
silence; six generated decoder witnesses cover exact length, startup and seek behavior.
It preserves the native beat/refinement and phrase classifier stages. Portable C FFT,
matrix operations and miniaudio decoding replace Apple frameworks. These decoder
changes mean the original package's exact audio-corpus results do not transfer to this
port without further comparison. AAC, AIFF and encrypted streaming media are unsupported.

Enable **Track Analysis** in Community Mods. Its page reports each deck's result.
Disabling it cancels pending work. Loading another track cancels the old job; an obsolete
completion cannot announce success for the new load. One worker processes jobs serially.
Already analyzed tracks are skipped. Busy resources and source/file changes retry while
the same load remains, so a cue edit during analysis does not permanently abandon it.
Other failures are retried after a new load or an off/on toggle. The worker's bounded
log is `data/analysis.log` on the installation USB.

Saved files become visible to subsequent SDK reads. This version does **not** replace
the firmware's active phrase/grid objects or refresh its native phrase display during
the current load. That integration and signed-in streaming playback still require work.
No physical deck validation or Beatport account validation has been performed.

## Build and test

Install the Swift 6.4.2 Static Linux SDK using Swift.org's instructions. Set
`SWIFT_SDKS_PATH` when it is installed somewhere other than macOS's default SDK directory.
The worker builder uses the pinned official ARM64 Swift container and the existing
`azmod-build:18.04` image for stripping. Docker must be running.

```sh
make worker
make test
../../az-mods-sdk/tools/azmod-pack build .
../../az-mods-sdk/tools/azmod-pack package .
```

The worker has no dependency on a Swift installation or a newer glibc on the deck.
Its compressed executable and model resources fit the mod installer's 16 MiB limit.
The mod validates the gzip CRC and size before publishing its extracted executable
inside its own data directory. The core validates output lengths and track/file
fingerprints before saving. ANLZ writes use same-directory temporary files and synced
rename publication. Grid publication precedes phrase publication; a failure between
files can leave a valid new grid without phrases, which the next analysis can complete.

Code is GPL-3.0-only. Miniaudio and miniz retain their upstream notices.
The bundled madmom model weights are **CC BY-NC-SA 4.0**, including a noncommercial
restriction; see `assets/NativeBeatModels/LICENSE.txt` and its README for attribution
and source. That restriction remains relevant to distribution and commercial DJ use.
