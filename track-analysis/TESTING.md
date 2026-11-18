# Track Analysis validation

Version 0.5.2 replaces the deployed helper process with C code linked into the
mod. The checks below apply to this implementation. Earlier emulator and physical
deck acceptance of the helper-process versions does not establish playback safety
for this in-process version.

## Automated checks

```sh
make CC=clang OUT=out/host SANITIZE=address,undefined test
```

The fixture generator emits C test inputs from the retained JSON values. Python
is used only to generate test source; tests and production analysis run in C.
The suite checks 2,337 reference cases covering spectral peaks, opening detection,
complete boundary routes, route and mood selection, role classification, recurrence, fills, decorators and
all exported phrase fields. Numeric tolerances and reduction order are retained.
The independent native-execution provenance remains with each fixture.

Complete 64-second synthetic stereo tracks at 44100 and 48000 Hz produce the same
four phrase entries, mood and terminal as the retained previous implementation.
The vocal test compares every typed activity frame with the frozen PVDI witness.
Cancellation is exercised during decoding and inference, alongside cleanup and
allocation-limit rejection. The runtime test has no process-launch callbacks and
checks typed publication, grid sizing/copy, stale leases, disable/reload cancellation,
commit retries, timeout and lease cleanup.

Twelve generated AAC/MP3 fixtures check source frame counts, AAC priming and edit
metadata, full decoding and exact backwards seek replay. HE-AAC and changing MP3
formats are rejected. These checks establish the decoder clock; they do not prove
equivalence to every encoder or native firmware resampler.

The core's `tests/sdk-analysis.sh` covers the production file merger, ownership,
stale source/load refusal, populated-map preservation, typed grid capacity and
copying, typed phrase/vocal validation and serialization, and lease consumption.
Run it under the SDK ARM64 test image with `SAN=asan`.

## Initial C validation on 2026-11-17

The C suite passed with AddressSanitizer and UndefinedBehaviorSanitizer on macOS
and ARM64 Ubuntu 18.04. The additional sixteen complete-boundary cases passed on
the host and the production GCC build, bringing both to 2,337 cases. Core analysis/lease tests passed under ARM64 sanitizers. The SDK and
core headers are byte-identical; all seven header-tool tests and 39 documentation
code snippets passed. The updated core and C mod built with GCC 7.5 on ARM64.

The validated 0.5.0 package is approximately 7.3 MiB. Its only executable code is
`track-analysis.so`, which exports only `az_mod_v1_entry`. Dependencies are libm,
libc and the ARM64 loader, with GLIBC requirements no newer than 2.27. Inspection
found no Swift/C++ runtime or process-launch imports and no ARMv8.1 LSE instructions.
The model and licenses are data assets. No device deployment was performed.

## Parallel C validation (0.5.1)

The C suite now also covers:

- Actual simultaneous execution with one, two and three workers, primary-stage
  execution on the calling thread, nested 1+2 workers and unique result slots.
- Joining all started children on creation failure, callback failure,
  cancellation and aggregate allocation-limit failure.
- Bitwise equality of every decimator sample, CQT bin, FFT/recurrence value,
  vocal feature and probability across thread counts and stage modes.
- All twelve combinations: each sequential stage independently uses 1–3 threads;
  concurrent splits are 1/1, 1/2 and 2/1.
  The repeated-voice input has more than three four-window groups and a short
  tail. Complete typed phrase/vocal results must match in every configuration.
- Packed matrix kernels for every model shape and batch width 1–4 against an
  independent scalar FMA oracle, including subnormal inputs and untouched slots.
- 12,288 activation outputs from 4,096 inputs compared bitwise with the retained
  ARM64 musl runtime. The fixture provenance and independent C generator are in
  `tests/fixtures/musl-activation-bits.json` and
  `tests/generate_activation_reference.c`.

The C suite passed macOS AddressSanitizer/UndefinedBehaviorSanitizer, production
ARM64 GCC 7.5, and ARM64 Clang 10 address/undefined sanitizers. ThreadSanitizer
passed the executor, complete pipeline and every concurrent split. The activation fixture also passes on the production toolchain.
The original 2,337 numerical witnesses, decoder tests and mod lifecycle tests
remain required.

A direct comparison against the retained 0.4.4 worker passed the representative six configurations
on macOS and all twelve combinations on ARM64 Linux: every published field, vocal
activity frame, feature float and probability float matched exactly. Linux uses
private musl activation functions with explicit FMA operations matching the
retained worker. This avoids changes from glibc activation rounding without
changing the process-wide math library. Timing and result artifacts are retained
outside the repository; these comparisons do not establish physical-deck speed
or playback interference.

`tests/analysis_probe.c` is an optional standalone C test driver, built with
`make OUT=out/probe out/probe/analysis_probe`. It supplies a typed constant-120-BPM
grid and emits typed result fields as JSON for reference comparisons. It is
never packaged or launched by the mod. To reproduce the direct comparison:

```sh
python3 tests/compare_reference.py /path/to/retained-worker \
  out/probe/analysis_probe /path/outside/checkout/comparison
```

Both test executables must run on the same host/target. The script checks all
twelve combinations and retains the typed JSON, original reference output,
feature/probability bits and logs. The mod package contains one shared
library plus model/license assets, with no Swift/C++ code, helper executable,
process-launch imports, exported allocator symbols or ARMv8.1 LSE instructions.

## C kernel optimization coverage (0.5.2)

`test_vocal_frontend` compares all 1,025 power bins against the frozen scalar
FFT for 408 frames: silence, impulses, tones, random inputs, subnormals, short
sources, reflected edges and partial final frames at both supported source rates.
Comparison is bitwise. The planned NEON FFT preserves each multiply, explicit
FMA and addition; it changes traversal and storage without changing reductions.

The packed-matrix oracle covers batches 1–8, every model shape, input tails,
subnormals and untouched output slots. The complete parallel fixture repeats the
voice input eighteen times, providing more than three eight-window groups and a
short tail. All twelve execution configurations must preserve complete typed
results and every feature/probability bit. Larger per-worker scratch allocations
remain charged to the existing aggregate limit; no allocation cap is relaxed.

The complete suite passed host AddressSanitizer/UndefinedBehaviorSanitizer and
production ARM64 GCC 7.5. ThreadSanitizer passed the executor and all twelve
pipeline configurations. Both kernel oracles also passed on the physical A53.

On 2026-11-17, the idle physical deck analyzed the same 496.039-second, 44100 Hz
stereo MP3 with its native 124 BPM grid. Concurrent phrases/vocals used a 1/2
worker split, CPUs 1–3 and nice 19. Three alternating runs per C build produced:

| Build | Median combined time | Observed range |
| --- | ---: | ---: |
| Original C 0.5.1 | 36.428 s | 36.033–37.171 s |
| Optimized C 0.5.2 | 31.289 s | 31.233–32.032 s |
| Verified 0.4.4, earlier comparison | 35.086 s | 34.807–35.176 s |

The optimized median is 14.1% lower than original C and 10.8% lower than the
verified 0.4.4 baseline. The 0.4.4 binary was matched to its packaged payload;
an initial stale 0.4.3 binary was excluded. CPU time fell 10.7%; peak RSS was
effectively unchanged. Private inference scratch grows from 2.84 to 5.67 MiB
per worker, within the unchanged aggregate limit.

Every published field matched across builds. The optimized diagnostic also
matched all 309,778 original C feature floats and 10,682 probability floats
bitwise. Small preexisting intermediate differences between C and the 0.4.4
worker remain on this MP3. The workload contains very little detected vocal
activity, so it does not establish quality across a broad vocal corpus.

Measurements used standalone drivers with warm filesystem caches and included
startup, model loading, decoding, analysis and output. Diagnostic dumps were
disabled for timed runs. The packaged vocal object is byte-identical to the
measured object; the SDK build and package validation passed. No mod was installed.
These measurements establish idle engine speed; the integration and playback
checks below remain outstanding.

## Target and playback scope

Build the mod with the SDK Ubuntu 18.04 ARM64 toolchain and validate/package it
with `azmod-pack`. Inspect its dependencies and exported symbols: the artifact
must export only `az_mod_v1_entry`, contain no helper executable, and have no Swift,
C++ runtime or process-launch imports. Use ARMv8.0-compatible instructions.

The in-process implementation still needs emulator and physical playback validation
before release: native phrase refresh, USB withdrawal, replacement loads, Beatport
publication and CPU/memory behavior during four-deck playback. Host sanitizers and
container tests cannot establish those outcomes. No deployment is part of this change.
