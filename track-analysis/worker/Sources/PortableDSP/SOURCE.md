# Portable audio decoding

`miniaudio.h` is the unmodified miniaudio 0.11.23 single-header distribution from
https://github.com/mackron/miniaudio/blob/0.11.23/miniaudio.h.

SHA-256: `7e4f3f13c8fe66df2080ac3dd12a89193e3c2463cb7f067c798abd7331cd8ee6`.

The upstream MIT-0/public-domain license and attribution are retained in the header.
Only its WAV, FLAC and MP3 decoders and PCM conversion are built. Audio-device access,
runtime library loading, threading and playback engines are disabled.

The adapter disables MP3 gapless trimming and preserves the encoded frame clock.
It scans contiguous MPEG-1 Layer III frames at 44.1/48 kHz, reproduces the original
conditional Info/Xing header silence, and rejects format changes. Six frozen
generated MP3 fixtures check exact frame counts, startup alignment, seek replay
and PCM prefixes within one Int16 unit. Other MPEG profiles are refused.

This replaces AVFoundation/mpg123 decoding in the original package. Codec and sample
conversion output can differ, so the original package's bit-exact corpus results
are not claimed for this port. The frozen downstream algorithm fixtures remain tests.
