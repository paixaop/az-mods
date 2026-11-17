# Beatport Remember testing

## 0.3.0 regression

The 0.2.1 startup crash occurred while `tick` called
`juce::TextEditor::setText` through a raw firmware address. The mod's tick
runs on the AZ-MODS overlay thread, not EP147's JUCE message thread. The
mod also retained raw controller/widget pointers without destruction
tracking and set its saved flag before checking whether a file write
succeeded.

Version 0.3.0 uses the public core Beatport login service. The mod contains no
firmware addresses, hooks, native object reads, password buffers or file I/O.
The core handles native UI operations and persistence; its tests cover those
operations separately. Mod tests alone cannot prove firmware behavior or a
successful Beatport authentication.

## Automated checks

Run from `az-mods/beatport-remember`:

```sh
make test
make test-harness SAN='plain asan'
../../az-mods-sdk/tools/azmod-pack build .
../../az-mods-sdk/tools/azmod-pack package .
```

The focused tests compile the production mod into a test process and invoke
its exported descriptor, page callbacks and tick against a mock SDK service.
They cover:

- Refusing an old core, an unavailable service, an incompatible/truncated
  service table, missing methods and rejected configuration.
- Disabling automatic actions if menu registration fails.
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

Use a core build that contains the service and the matching 0.3.0 mod. Verify
these cases on supported firmware before publishing:

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

Old test notes for 0.1.x used synthetic credentials and did not establish a
complete successful login round trip. Their no-crash observations do not
validate the 0.3.0 service implementation; retain new evidence per build.

On 2026-11-16, the 0.3.0 mod and updated core completed automatic login from the
existing credential store after three native-player restarts on the physical
deck. The core's visibility gate deferred autofill until the login screen was
actually open. The native UI showed Beatport Connected, and authenticated catalog
tracks loaded and played through SDK controls. The original credential file was
preserved; capture of a changed login and Forget were not retested in this run.
No crash trace appeared during acceptance. The preceding reported 1.2.9 outage
had no surviving fault trace, so its cause remains undetermined.

The compatibility fixture also accepts larger append-only ABI-v1 core/service
tables and rejects missing, truncated or unknown services. This validates the mod's
supported-prefix checks; future core and firmware implementations still need their
own qualification. Evidence is retained in the parent workspace under
`temp/physical-auto-20261116/`.
