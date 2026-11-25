# Set Planner

Set Planner orders a rekordbox USB playlist using harmonic key, effective BPM, and energy from saved analysis. It ports the FletDJ ordering approach, including tempo and energy clustering, transition scoring, and seeded optimization. The deck SDK exposes key, BPM, and duration for this calculation. Genre is unavailable in the playlist metadata.

Highlight a USB playlist in the deck browser or open its track list. Press the deck’s **MENU** button and tap **Plan Set** in the button row. If the row is full, open **Edit** to find **Plan Set**. Optionally select an opener or closer by playlist position. Leaving either on **Automatic** lets the planner choose that endpoint. Repeated tracks remain distinct playlist entries. **BACK** returns to the browser.

Change defaults under **Mod Menu → Community Mods → Installed → Set Planner → Settings**.

## Energy source

| Mode | How the track is scored |
| --- | --- |
| Full track | Score all available detail waveform columns over the full track. |
| One phrase | Score the first, middle, or last occurrence of a selected phrase type on each track. An even number of matches uses the earlier middle occurrence. |
| All of a type | Score each occurrence of the selected phrase type and take the median. The default phrase type is Chorus. |

Adjacent phrases of the same type merge when they share a beat boundary. Separated phrases keep their own windows. Phrase windows use the saved beat grid. PWV7 scores combine mean band amplitude, the fraction of active columns, and low-band attacks per beat. If PWV7 is unavailable, the planner estimates energy from PWV5 or PWV3 waveform height. If the selected phrase cannot be scored, it tries the full track. Full-track analysis can take longer than a phrase selection.

The preview shows the track's waveform score rounded to a whole number on a fixed 0–100 scale. The same analysis selection on a track has the same displayed energy across playlists. An asterisk marks a full-track or waveform-height fallback, and a dash means no usable measurement. The planner uses a separate playlist-relative scale for fitting the set's energy curve, with a neutral value for tracks that lack a waveform score.

The preview highlights a difficult transition on the incoming track's row. **Jump** shows the BPM difference used by the planner when it exceeds the configured tempo warning limit. With half/double tempo enabled, this uses the smallest gap at original, half, or double tempo. **Key clash** flags keys at least four positions apart on the Camelot wheel.

## USB cache and playlist changes

The mod saves verified energy calculations and their track metadata under
`MODS/community-mods/set-planner/data/track-analysis-v3/` on the USB that installed the mod. Use that USB for the playlist to get persistent caching. Other USB playlists can still be planned and saved. The cache matches the rekordbox track ID, USB-relative path, title, artist, key, BPM, and duration. Full-track, one-phrase, and all-of-a-type scores are cached separately along with how each score was measured. The planner reads current USB metadata to detect changes and recalculates the playlist-relative normalization every time.

Use **Refresh analysis and preview** after reanalyzing tracks in rekordbox if their visible metadata stayed the same. It refreshes the selected mode and clears the other saved scores for each processed track. Tracks missing analysis are retried the next time they are requested.

**Preview set** can update the analysis cache. To write the reviewed order, enable **Playlist Editing** in Mod Menu, select **Save order**, and confirm. The mod verifies the playlist revision, exact entries, and Device Library sync for each move. BACK requests cancellation. A write already accepted by the deck is checked before saving stops. A partial save can be resumed when the current order still matches the verified moves.

## Build and test

Use the ARM64 AZ-MODS SDK toolchain to build `set-planner.so`. The deck needs a core with the public Plan Set menu action, playlist-order, playlist-move, and track-analysis services.

```sh
make AZ_SDK_INCLUDE=/sdk/include OUT=/out
make unit
```
