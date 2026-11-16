# Testing Anchor 0.7.0

## Host and package checks

Run from the Anchor folder with the AZ-MODS 1.2.8 SDK at `../../az-mods-sdk`.

```sh
make unit ui-test
make unit CC=clang UNIT_SAN_FLAGS='-fsanitize=address,undefined -fno-sanitize-recover=all'
make test SAN=plain,asan,tsan
../../az-mods-sdk/tools/azmod-pack build .
../../az-mods-sdk/tools/azmod-pack package .
../../az-mods-sdk/tools/azmod-pack validate dist/anchor-0.7.0.azmod
```

The unit suite drives Anchor's registered deck-status callback through INITIAL, CHANGE
and RESYNC. It verifies same-track reloads, eject, stale receipts, coherent phrase and
grid revisions, borrowed-array lifetime, live cue edits and zero-time cues. Prepared
loops, paused resizing and EXIT update the plan from the working loop fields. The
planner suite checks PLAY grace and the exact start of the final EXIT pass.

The UI suite uses production drawing and input for Ribbon, Card, Sidebar and settings.
It checks text bounds, picker paging and selection, separate A/B targets, **Set here**,
layout changes and sidebar toggling with empty decks. The core's SDK drawing test
checks the two-deck surface mask independently of deck events.

The SDK harness soaks the mod with randomized deck states, settings pages and
reloads. Its plain Linux pass also checks callbacks for heap allocation and I/O.
`make previews` renders the production drawing calls with a host monospace font.

## Physical deck

Use a core build with deck-status events and the two-deck surface mask.

1. Open Anchor with empty decks. Switch among Ribbon, Card and Sidebar and confirm
   the saved layout survives a reload. Open and close Sidebar while both decks are
   paused. Switch to four-deck, browse and source views, then back to two-deck.
2. Load two analysed tracks. Start A and select phrase, Hot Cue and grid targets.
   Edit a live Hot Cue while the picker is open and confirm the list updates.
   Confirm an empty live cue bank clears its chips.
3. Pick B's phrase start, Hot Cue at zero and **Set here**. Beat-jump and resize
   its loop. Verify B's selected time stays fixed. Reload the same track and
   confirm the old target and picker clear.
4. Prepare a loop with B paused. Verify **LOOP READY**, resize it and watch the
   PLAY countdown update before B starts. Check both incoming-deck directions.
5. Confirm the final bar turns amber and PLAY NOW appears at the launch moment.
   Check that the final playing loop pass shows EXIT LOOP NOW and that the
   released run reaches the marked B target on A's boundary. Record audio to
   measure alignment and check a deliberately missed launch.
6. Unmatch the tempos, then match them. Confirm the guide suppresses the action
   while the warning is shown. Try cue audition and reverse playback and confirm
   the forward countdown waits for usable playback.
