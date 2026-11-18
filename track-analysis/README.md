# Track Analysis

Track Analysis fills missing phrases and vocal intervals for loaded USB OneLibrary
and completed Beatport tracks. Version 0.5.2 contains its audio decoders, phrase
classifier and vocal LSTM entirely in C inside `track-analysis.so`. A thread
created by the public SDK coordinates bounded parallel analysis in the mod's process. No helper
executable is extracted or launched.

The mod obtains a loaded-track lease through `analysis_open_loaded_v2`, reads
`az_beat` records through `analysis_grid`, and publishes typed phrase and vocal
arrays through `analysis_commit_result`. The core owns native file serialization,
freshness checks and firmware publication. Existing populated grids, phrases,
PVDI, cues and waveform data are preserved. The mod uses no private firmware hooks.
A core must expose all three services; the version string alone is insufficient.

Enable **Track Analysis** in Community Mods. Its page reports each deck's result.
A new load or disabling the toggle cancels queued and running analysis. The core
rejects stale publication. Track jobs run one at a time. Each job can use up to three analysis threads,
all at nice 19 on A53 CPUs 1–3, outside the audio and UI cores. Child threads
inherit the coordinator's scheduling policy. If that policy cannot be applied,
analysis does not start. Each job has a ten-minute deadline and a 512 MiB limit on tracked analysis
scratch allocations across both stages and every child thread. Codec state and
the small returned arrays are additional.
There is no process-wide memory limit. Errors go to the mod log. Busy leases and
source changes retry; decoder, memory-limit and model failures wait for another
load or an off/on toggle.

Phrase work requires the loaded native beat grid and preserves it. Vocal-only
work does not read or require a grid. All-zero vocal activity is a completed
instrumental timeline. Missing grid generation for standalone files is outside
this mod's loaded-track workflow.

## Execution and optimizations

The original execution controls and defaults are retained:

| Control | Values | Default |
| --- | --- | --- |
| `TRACK_ANALYSIS_STAGE_MODE` | `sequential`, `concurrent` | `sequential` |
| `TRACK_ANALYSIS_PHRASE_THREADS` | `1`, `2`, `3` | `1` |
| `TRACK_ANALYSIS_PVDI_THREADS` | `1`, `2`, `3` | `1`; `2` for combined concurrent work |

Concurrent combined work accepts phrase/vocal splits 1/1, 1/2 and 2/1.
Sequential stages each accept up to three threads. Invalid counts or an
oversubscribed split fail before decoding. The C engine also accepts these
settings through the typed `ta_execution` record in `ta_request`.

Phrase workers divide independent decimator bands, CQT pitch bins and FFT bars.
Filter state stays with its band across PCM blocks. Vocal workers divide
eight-window groups, sharing read-only weights and features with private scratch.
Output slots are disjoint; overlap scores merge in the original source order.
Concurrent stages use separate decoders and join before returning. Cancellation,
allocation failure or either-stage failure yields no partial published result.

The port retains 8 KiB decimator tiles, 16,384-sample envelope chunks with
in-place section passes and ARMv8 NEON FMA kernels. Vocal matrices pack eight
output columns and share each weight load across up to eight windows, keeping
sixteen vector accumulators in registers. Bounded prefetching reads upcoming
weight rows without leaving the packed block. The vocal FFT reuses an immutable
per-job permutation/twiddle plan, windows directly into bit-reversed order, and
uses four-lane NEON butterflies and power conversion. Its original multiply,
explicit FMA and addition order is preserved, including scalar tails.
The CQT uses the original eight independent double-precision FMA chains and
reduction order. DSP/codec kernels retain `-O3`; ARM64 builds target ARMv8.0 and
tune for Cortex-A53 without fast-math. The original mimalloc 2.2.4 allocator is
linked privately for analysis and codec allocations, with dynamic TLS and no
process-wide allocator override or exported allocator symbols.

`TRACK_ANALYSIS_PHRASE_PROFILE=1` retains phrase stage timings. Test drivers can
inspect typed feature/probability arrays and stage timings through
`ta_diagnostics`; the original `TRACK_ANALYSIS_PVDI_FEATURES` and
`TRACK_ANALYSIS_PVDI_PROBABILITIES` float dumps remain opt-in diagnostics.

## Audio and vocal model

Supported audio is WAV/FLAC at 44.1 or 48 kHz, MPEG-1 Layer III MP3 at those rates,
and mono/stereo AAC-LC in unencrypted, nonfragmented M4A/MP4 or ADTS. Maximum
source duration is twenty minutes, subject to the memory bound. HE-AAC, AIFF,
encrypted media, folder-only tracks and complex MP4 edit lists are unsupported.
AAC files are limited to 128 MiB and MP4 parser allocations to 16 MiB per decoder.
MP3/AAC decoding preserves the encoded source clock and encoder priming. MP4 edit
metadata is reported without silently trimming the audio.

Vocal analysis uses the recovered 29-feature, bidirectional two-layer LSTM with
256 units per layer and two dense layers. Its 1,669,314 weights remain a local
model asset, imported with `tools/import-pvdi-model.py`; they are not committed.
Inference uses C and ARMv8 NEON with deterministic window merging. Linux retains
the original musl activation rounding through private C math functions. The frontend
runs at 22050 Hz with 2048-sample FFT windows and a 1024-sample hop. Activity values
0–4 use strict thresholds 0.2, 0.4, 0.6 and 0.8. At least nine frames are required.
The public SDK records `AZ_VOCAL_LSTM` provenance. Existing vocal maps, including
older heuristic maps, are preserved.

## Build and test

Import the supported locally installed model with `tools/import-pvdi-model.py
--help`, then use the SDK's ARM64 Ubuntu 18.04 build environment:

```sh
../../az-mods-sdk/tools/azmod-pack build .
../../az-mods-sdk/tools/azmod-pack package . --github paixaop/az-mods
```

`make` builds the shared mod directly; it needs a C compiler, the public SDK
headers and libm. There is no Swift or C++ build dependency. The package contains
the shared mod, model data and licenses. Host checks use:

```sh
make CC=clang OUT=out/host SANITIZE=address,undefined test
```

The C tests cover numerical reference fixtures, complete phrase analysis at both
source rates, typed vocal output, codec clocks and seeks, cancellation, memory
limits and mod lifecycle. See [TESTING.md](TESTING.md) for validation scope.
Decoder provenance and licenses are in [analysis/dsp/SOURCE.md](analysis/dsp/SOURCE.md)
and [vendor/faad/SOURCE.md](vendor/faad/SOURCE.md).
