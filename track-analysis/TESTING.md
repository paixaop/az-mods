# Track Analysis validation

`make test` runs 59 retained native beat/phrase and decoder tests, the CPU model
regression, the C lifecycle tests and three bounded gzip extraction tests. The gzip
checks include malformed/truncated input and CRC rejection. Swift tests require
`TRACK_ANALYSIS_MODELS` to point to `worker/Sources/Analysis/Resources/NativeBeatModels`.

The same 59 Swift tests pass on macOS and ARM64 Linux with the original fixture
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
during four-deck playback and signed-in streaming on a physical deck. Streaming and
native active-map publication are currently absent, rather than validated features.
