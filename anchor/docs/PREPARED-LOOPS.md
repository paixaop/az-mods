# Prepared-loop reader, EP147 2.00

Anchor 0.6.1 reads the current working loop on the same firmware already supported by
its loaded-analysis reader (`ep147-9e9454e357389ece`). Native commands remain unused.

The deployed experimental SDK observer copied notification byte 0 and reported false
through manual loop/EXIT/RELOOP. The corrected source predicate is documented in the
core's native investigation ledger, but the SDK still supplies no IN/OUT points. A
prepared paused loop therefore cannot be recovered by Anchor's old wrap detector.

The read-only worker follows a checked DjPlayerUnit (`0x2bf2348`) at `+0x1a0` to its
UnitStateServer (`0x2bf49b8`, secondary PlayerState base `0x2bf4a08` at +8). The logical
deck is at publisher `+0x1f18`. Its working CueProperty (`0x2be12b0`) is at publisher
`+0xb20`, independently of retained cue slots. Every pointer, deck and property is
rechecked after two identical bounded copies. This is best-effort concurrent observation,
not an atomic native transaction or a claim of general track-data SDK completeness.

REA 6.7.0 / Kuna 1.833 evidence, against EP147 SHA-256
`6577f42532375936c7e3fa830efa35b7daa9cde8e0a265c1fc7143f824321925`:

- `ev_1ea412f9f23ceb8ccb046b0b14a4311f5e2e06d88f69d979832b4a2c9ebf260c`:
  `0x14a7668` validates working-property offsets 0x30, 0x34, 0x36, 0x38 and 0x40;
  zero position remains valid. This is the same predicate as the corrected observer.
- `ev_bcd070cee2fc552ff9811f8ec4f787dba11ffece7147f4ead9cc312d21e8425f`:
  `0x154e058` forwards PlayerState +0xb18 to that predicate.
- `ev_ea8f6fb660e55f8f95e8b029fb2b2d3b126f081eeae2ec19141fecf874093b78`:
  `CueProperty::setOutPoint`, `0x14a17e8`, checks sampling rate at +0x60 and updates
  the point at +0x70. IN is the corresponding point at +0x18.
- `ev_8993e3d8f7cdce3c32d234193a4e89bbee9dba1f7a4bb32913700876251c1746`:
  `0x14aa460` copies the position, writes Hertz at point +0x48 and caches
  `floor(samples * 1,000,000 / Hertz)` at point +0x50. Anchor verifies both
  representations, both point vtables and the shared coordinate owner before conversion.

The source Evidence records are saved under `out/rea-*.json`. Kuna pseudocode is a static
reconstruction; its unresolved control flow and single-function profile limits remain.
A separate Ghidra attempt timed out and is not supporting evidence.

The read-only hardware probe identified all four native units and distinguished B's
prepared loop from the inactive decks. While B was paused with a 32-beat loop at source
126 BPM, native points were 0.209–15.448 seconds, a 15.239-second interval. See
`out/prepared-loop-verified.txt` and `out/prepared-loop-native.txt`. This establishes that
the reader can see prepared points before playback; it does not establish human timing
or audio alignment accuracy.

The dedicated mod worker reads at 125 ms intervals and ties publication to the mod's
load sequence plus stable before/after deck identity. The UI accepts results for 750 ms;
stale or unavailable results show READING LOOP instead of a speculative no-loop plan.
The worker copies scalars only, never calls native code and never retains native references.
On other firmware or older cores without a worker service, existing observation remains
the fallback and requires playback. Loop moves/resizes and EXIT are read afresh; track
loads discard old observations.
