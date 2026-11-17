# Track Analysis validation

`make test` runs 64 retained native beat/phrase and decoder tests, the CPU model
regression, the C lifecycle tests and three bounded gzip extraction tests. The gzip
checks include malformed/truncated input and CRC rejection. Swift tests require
`TRACK_ANALYSIS_MODELS` to point to `worker/Sources/Analysis/Resources/NativeBeatModels`.

The preexisting 59 Swift tests pass on macOS and ARM64 Linux with the original fixture
hashes and numerical assertions intact. The native HMM transition kernel is frozen
and verified against its original full-model probability SHA-256; regenerate it with
`tools/generate-hmm-kernel.py` only on a host that passes that check. Fixture JSON
uses direct floating-point decoding so Linux Foundation decimal boxing cannot move
test inputs across classifier thresholds.

The core's `tests/sdk-analysis.sh` tests the production ANLZ merger and source lease
service against disposable USB/SQLite fixtures. Run it in `azmod-test:18.04` with
`SAN=plain` and `SAN=asan HOST_CC=clang-10`. It covers stale audio, ownership, UI-thread
refusal, missing maps, zero-byte export placeholders, existing-map refusal and
publication consumption. Missing row locators are allocated and linked through the
native spool; a conditional update preserves later DJ edits.

The final static ARM64 worker runs on Ubuntu 18.04 under a 512 MiB memory limit.
A generated three-minute stereo WAV produced 384 beats and 14 phrases; its MP3
encoding produced 385 beats and 14 phrases on the encoded frame clock. Both outputs
were saved through the production core service, including an ASan/UBSan run.
The existing disk-provider regression passes 124 checks, the public header tools
pass seven tests, and the core and packaged mod compile with warnings as errors.

The ARM64 static worker must also run against whole audio on Linux. A successful build
or a fixture callback does not establish firmware integration. Before release, verify
USB removal, track reload, saved-grid reuse, native display refresh, CPU/memory use
during four-deck playback. Authenticated streaming, native active-map publication,
and the scope of physical-deck acceptance are documented below.

## AAC decoder and static-worker checks (2026-11-16)

All 64 Swift tests passed in a macOS release build with `TRACK_ANALYSIS_MODELS`
set. The five AAC tests cover six generated AAC-LC fixtures: FFmpeg 44.1 kHz
stereo and 48 kHz mono, plus Apple AudioToolbox 44.1 kHz stereo, each in M4A and
ADTS. They check packet-derived output length, the explicit MP4 edit metadata,
impulse positions against independently decoded FFmpeg PCM, exact backwards
seeks, simultaneous decoder state, truncation, changed ADTS formats, impossible
sample-table counts, unsupported edits, encrypted sample types and HE-AAC rejection.
Hashes and generation details are retained with the fixtures.

AAC output restores only the first 1024-frame slot suppressed by FAAD's overlap
initialization. The Apple witness retains all 2112 frames of encoder priming;
the FFmpeg witnesses retain 1024. Container edit lists remain descriptive metadata.
These checks establish the decoder clock, not equivalence to every native firmware
decoder or resampler. AddressSanitizer and UndefinedBehaviorSanitizer runs decoded
the supported fixtures, replayed seeks and exercised 1024 deterministic byte mutations
without findings. Repeating with unsigned plain `char` covered the AArch64 Linux
interpretation of FAAD's unspecified-extension sentinel.

The packaged ARMv8.0 static worker also analyzed a generated 128-second, 44.1 kHz
stereo M4A using a saved 257-row PQTZ grid inside ARM64 Linux Docker, with networking
disabled and a 512 MiB memory cap. It returned a valid `TA01` result containing a
224-byte PSSI and no replacement PQTZ. Peak resident memory was 147616 KiB; elapsed
time on that host was 1.01 seconds. The stripped executable was 11906520 bytes and
its gzip asset was 4605356 bytes. These are worker checks, not emulator, native
phrase-refresh, physical-deck or authenticated Beatport acceptance results.

## 0.1.1 firmware emulator check (2026-11-16)

Firmware 2.00 EP147 (SHA-256
`6577f42532375936c7e3fa830efa35b7daa9cde8e0a265c1fc7143f824321925`),
core 1.2.8 and QEMU TCG with a Cortex-A72 CPU exercised a fresh disposable USB
overlay. The approved mod used the normal community loader. Track-data and
native-command development flags were absent; passive status supplied the native
load and qualified USB reference.

A OneLibrary fixture contained a three-minute stereo 44.1 kHz WAV, a PQTZ grid
with 384 entries, and an empty EXT container. Loading it from the native browser
automatically launched the packaged worker and saved 21 PSSI sections to EXT.
DAT remained byte-for-byte unchanged. The settings page opened and reported
“Phrases saved to USB.” Reloading advanced the load epoch, preserved both files,
and did not launch another worker.

The original worker failed before main: its bundled mimalloc executed `ldaddal`,
an ARMv8.1 instruction unavailable on the deck's Cortex-A53/A72 cores. Building
the same allocator version for ARMv8.0 removed that failure. Separately, the mod
passed the program name in its argument array even though the SDK supplies it.
The corrected call passes only the worker's five arguments. Repeat this native
browser exercise after toolchain or launcher changes; a direct worker run on a
newer host CPU cannot verify these requirements.

All 59 Swift tests, the C runtime test under address/undefined-behavior sanitizers,
and three gzip tests passed. The runtime test now exercises extraction, SDK launch
arguments, result publication, and the retained settings-screen descriptor after
`init` returns. Use `-fsanitize-address-use-after-return=always` to detect the
original stack-lifetime error. The worker build, ARM mod build and 0.1.1 package
validation passed. These results do not establish physical-deck or Beatport
acceptance; no authenticated streaming playback was available in this emulator.

The executable extraction descriptor now uses atomic close-on-exec creation on
Linux. A default-service emulator run reported `Text file busy` while firmware
was spawning background shell children; an inherited writable descriptor can
prevent the worker from executing. The runtime fixture checks the extraction
descriptor's close-on-exec flag, and both macOS and Linux sanitizer runs cover
the extraction/launch path.

## 0.2.0 loaded-track and native refresh checks (2026-11-16)

Firmware 2.00, Cortex-A72 QEMU and the normal community loader exercised two real
44.1 kHz AAC-LC M4A USB tracks with phrase and cue tags removed from a disposable
copy. No feature-enabling development flags were present. Loading each track
launched analysis automatically and published 17 phrases into its native map.
The native phrase bars appeared during the current load, and Anchor activated
with one deck playing and the other paused. CUEs applied one hot cue and two
memory entries on each deck. Both original PQTZ tags remained byte-identical.

The source-provider ARM64 tests exercise completed native Beatport resources,
counted resource lifetime, all 12 bytes of native identity, replacement and stale
load rejection. The core tests exercise loaded leases, cache reuse, saved phrase
refresh, malformed PSSI, changed grids and file identities. These controlled
fixtures are separate from authenticated catalog playback.

Authenticated Beatport catalog acceptance also passed on the final core/mod build.
Avarice (Original Mix) and Energy (PEEKABOO Remix) generated 13 and 10 native phrases
without reloading, while native grids retained 448 and 401 rows and their map versions.
CUEs placed one hot cue and two memory entries per deck. Anchor activated automatically
and used the new phrase map for its phrase-end countdown after deck tempos were matched.
Reloading Energy restored its ten cached phrases at the new load epoch without another
worker. The loaded lease returned `AZ_NOT_FOUND` after native refresh, as specified.

The emulator required matching native `cabinet/encryption/csdk.dat` and `bpck.dat`
provisioning before CloudDirectPlay and Beatport login could complete. These private
files are emulator prerequisites, not SDK enable flags, and are not included in source
or packages. Account authentication alone did not supply them. The separate physical
acceptance results are recorded below.

The final 0.2.0 package repeated both USB analyses successfully. A controlled test
paused the actual worker after launch, replaced its deck's track, and observed the
old job return stale and its process exit; the retired track retained zero saved
phrases. Reloading that track then generated 17 phrases normally. A subsequent
reload advanced the accepted load epoch and returned `AZ_NOT_FOUND` without another
worker. The Beatport native resource fixture also passed inside the Cortex-A72
emulator, separately from real catalog playback.

## 0.2.0 physical-deck acceptance (2026-11-16)

The qualified 2.00 EP147 executable ran on a physical deck reporting firmware 2.01.
Six Beatport catalog loads automatically ran analysis and refreshed native phrases
during their original loads. The two fresh final-build witnesses, Victory Lap and
Nowhere to Run, generated 15/13 phrases while preserving native grid counts 388/424
and revision 1. Cues 0.1.3 waited for those maps and created 8/8 hot plus 10/5 memory
cues. The core converted native mood enums to the public PSSI domain. SDK Play
activated Anchor using the incoming chorus target.

Two USB loads recognized existing phrase maps, preserved their cue banks and
activated Anchor on SDK Play. This run did not repeat generation on an empty
physical USB bank or four-deck playback stress. All panel and touch operations
used public SDK commands. The temporary control mod was removed after testing;
the production mods passed the healthy-startup window with an empty crash trace.
Logs, screenshots, source snapshots and hashes remain in the parent workspace's
`temp/physical-auto-20261116/` directory.

## Vocal interval coverage

The worker tests exercise vocal-only analysis without a beat grid, tempo or model
resources; 44.1/48 kHz encoded source clocks; silent instrumental output; short
tracks; and the final partial PVDI frame. Existing native spectral witnesses still
exercise the original activity and vocal-entry calculations after extracting their
shared spectral measurement helper.

Core `tests/sdk-analysis.sh` tests header-clock timestamp conversion, threshold/run
boundaries, exact generated-duration clipping, sizing/capacity handling, all-zero
completed analysis, unknown versions and malformed bytes. Service fixtures cover
existing-PVDI precedence, vocal-only preservation of DAT/EXT, missing-all generation,
saved-phrase refresh before vocal generation, stale load/file refusal, and copied
USB/Beatport interval reads. These checks do not measure singing detection accuracy;
that requires labeled recordings. No physical-deck vocal acceptance has been run.
