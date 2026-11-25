# Beatport Remember testing

## Automated checks

Run these checks from the Beatport Remember directory.

```sh
make test
make test-harness SAN='plain asan'
../../az-mods-sdk/tools/azmod-pack build .
../../az-mods-sdk/tools/azmod-pack package .
```

The focused tests compile the production mod into a test process and invoke
its exported descriptor, page callbacks and tick against a mock SDK service.
They cover:

- Checking compatibility with a larger append-only table and refusing an old
  core, an incompatible or truncated table, missing methods, and rejected configuration.
- Registering the Community Mods settings page without adding a main Mod Menu row.
- Disabling automatic actions if settings registration fails.
- Preserving default preferences and clamping legacy delay/attempt settings.
- Applying switches to the service before persisting them, and leaving the
  previous preference intact when the service refuses the change.
- Displaying committed, pending, failed and unavailable storage states.
- Treating an accepted Forget request as pending until the service reports
  completion, retaining a retry button after a storage failure.
- Refreshing changed status without continuously repainting unchanged state.
- Handling a nonterminated username defensively, and allowing BACK navigation.

Plain, AddressSanitizer/UBSan and ThreadSanitizer runs are available through
`SAN`. The mod starts no threads; ThreadSanitizer covers these mocked callback
scenarios, not concurrency inside the core service. The generic SDK harness
supplies an in-memory service stand-in and checks callback/rendering rules;
it does not access firmware or a real account.

## Physical-deck acceptance

Use a core build that contains the Beatport login service and a matching mod
build. Verify these cases on supported firmware before publishing.

1. Start with existing valid saved credentials. Confirm startup remains
   responsive beyond the automatic submission delay and the community-mod
   healthy-startup window. Confirm the fields fill on the Beatport screen.
2. Change the login on the deck and press LOG IN. Confirm the mod page changes
   from pending to the committed username. Restart, reopen Beatport and
   confirm the new login is restored. Do not print credential-file contents
   into logs or include them in test reports.
3. Open and close the login screen repeatedly while a submission is pending.
   Confirm there is no stale-screen action or crash. Edit either filled field
   before the delay expires; confirm no automatic submission of that edit.
4. Confirm automatic login never presses a button in the LOG OUT state.
5. Toggle Auto login and Remember independently and verify their behavior
   across a restart.
6. Forget the saved login, wait for **No saved login**, restart and confirm
   neither field is filled. Check storage failure/retry behavior in the core's
   fault-injection tests instead of filling the deck's settings partition.
7. With the user's own account, complete one authentication round trip:
   enter credentials → LOG IN → restart → automatic login → streaming track.
   A clean crash log or a returned dispatch call does not prove authentication.
