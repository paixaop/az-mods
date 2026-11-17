# Track Analysis

Track Analysis ports DJTrackAnalysis 6.0.0 into an AZ-MODS community mod.
It automatically checks loaded USB OneLibrary and Beatport tracks and analyzes
missing phrases and vocal detection in a separate low-priority ARM64 process. The public SDK supplies
a lease tied to the accepted load and its current native beat grid. After analysis,
the core saves the phrases and refreshes the firmware's active phrase map immediately.
CUEs and Anchor can then react during the same load, without reloading the track.
Existing populated grids, phrases, PVDI, cues and waveform data are preserved.

The loaded-track service is qualified on firmware **2.00**. It requires a ready
native beat grid; it does not replace that grid or analyze folder-only tracks.
USB phrases are saved to the track's ANLZ files. For rows without an analysis
location, the core creates a stable USB folder and queues a conditional native
library update. Beatport analysis uses the firmware's completed, decrypted audio
resource and keeps only four per-deck analysis caches in internal settings.
The mod requires the appended public `analysis_open_loaded_v2` service; a version
number alone does not establish that capability. It uses no private mod hooks.

The worker runs on the deck. It supports MPEG-1 Layer III MP3 at 44.1/48 kHz,
AAC-LC in unencrypted, nonfragmented M4A/MP4 or ADTS at 44.1/48 kHz (mono/stereo),
FLAC and WAV, up to twenty minutes.
MP3 decoding preserves the encoded frame clock and the original conditional header
silence; six generated decoder witnesses cover exact length, startup and seek behavior.
AAC also preserves the encoded clock and encoder priming. MP4 edit-list metadata is
reported explicitly; it is not silently applied as gapless trimming. Generated FFmpeg
and Apple AudioToolbox witnesses verify 1024- and 2112-frame encoder priming,
independent decoding, impulse positions and exact backwards seeks.
It preserves the native beat/refinement and phrase classifier stages. Portable C FFT,
matrix operations, miniaudio, FAAD2 and minimp4 replace Apple frameworks. These decoder
changes mean the original package's exact audio-corpus results do not transfer to this
port without further comparison. HE-AAC, AIFF, encrypted media and complex MP4 edit
lists are unsupported. AAC files are limited to 128 MiB; MP4 parser allocations are
limited to 16 MiB per decoder.

Enable **Track Analysis** in Community Mods. Its page reports each deck's result.
Disabling it cancels pending work. Loading another track cancels the old job; an obsolete
completion cannot announce success for the new load. One worker processes jobs serially.
Tracks with both phrases and populated PVDI are skipped. Valid all-zero PVDI counts as completed vocal analysis. Busy resources and source/file changes retry while
the same load remains, so a cue edit during analysis does not permanently abandon it.
Other failures are retried after a new load or an off/on toggle. The worker's bounded
log is `data/analysis.log` on the installation USB.

Native phrase publication and automatic CUEs/Anchor activation passed USB and
authenticated Beatport emulator tests on firmware 2.00. Physical tests also passed
for Beatport generation and USB reuse; see [TESTING.md](TESTING.md) for scope.

## Vocal intervals

Existing PVDI in `.2EX` takes precedence. If only vocals are missing, the worker
runs vocal detection without a beat grid or phrase classification; existing phrases
are untouched. If phrases and vocals are both missing, it generates both. Missing
phrase analysis still uses the loaded native beat grid. File-only SDK jobs can also
generate a missing grid when no ANLZ exists.

The public SDK's `vocal_intervals` and `vocal_intervals_loaded` return `[start_s,
stop_s)` in original-track seconds. Consecutive PVDI values of 3 or 4 form a segment;
there is no implicit beat snapping, gap merging or minimum-duration filter. Missing
PVDI is distinct from a valid instrumental timeline with zero vocal intervals.
Unknown or malformed PVDI is refused and preserved, not silently overwritten.

The fallback reuses the phrase detector's spectral peak heuristic on a uniform
1024/22050-second timeline. It explicitly resamples the filtered source stream to
the detector's 5512.5 Hz spectral clock, uses centered 2048-sample windows, and saves
binary 0/4 scores. These are estimates, not calibrated probabilities; 46 ms spacing
is not 46 ms acoustic accuracy. The roughly 372 ms analysis windows and spectral
similarities between voices and instruments limit boundary precision and accuracy.
No vocal audio is extracted. PVDI is saved with an `AZVD` marker so SDK consumers can
distinguish this heuristic from pre-existing PVDI and clip the final interval to the
actual decoded duration. Existing MP3/AAC encoded-clock rules also apply to vocals.

The format, source-race and worker tests cover publication and timing, including
44.1/48 kHz input and silent tracks. Labeled singing recordings and physical-deck
acceptance remain necessary before claiming detection accuracy or firmware rendering
of generated PVDI. This feature exposes SDK intervals; it adds no waveform overlay.

## Build and test

Install the Swift 6.4.2 Static Linux SDK using Swift.org's instructions. Set
`SWIFT_SDKS_PATH` when it is installed somewhere other than macOS's default SDK directory.
The worker builder uses the pinned official ARM64 Swift container and the existing
`azmod-build:18.04` image for stripping. Docker must be running. It downloads and
verifies mimalloc 2.2.4, then rebuilds it for ARMv8.0: the static SDK's bundled
allocator uses LSE instructions unavailable on the deck's Cortex-A53/A72 cores.
The replacement is local to the worker build; the installed Swift SDK is unchanged.

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
rename publication. A missing disk grid is filled from the unchanged native snapshot. Successful
publication also updates the active phrase map. A load or grid change rejects an
obsolete job; valid files saved just before a race can be reused on a later load.

See [TESTING.md](TESTING.md) for emulator and physical-deck results and the
remaining validation limits.

Code is GPL-3.0-only. Miniaudio and miniz retain their upstream notices. Vendored
FAAD2 2.11.4 is GPL-2.0-or-later; minimp4 is CC0. Their pinned sources and licenses
are documented in `worker/Sources/FAAD/SOURCE.md` and
`worker/Sources/PortableDSP/SOURCE.md`, and their licenses accompany the package.
The bundled madmom model weights are **CC BY-NC-SA 4.0**, including a noncommercial
restriction; see `assets/NativeBeatModels/LICENSE.txt` and its README for attribution
and source. That restriction remains relevant to distribution and commercial DJ use.
