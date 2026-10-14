# Debug error catalogue

For dismissible native game debug warnings during playtesting. Search this file by message
fragment; character IDs, item names and source line numbers may change.

Known warning: retain its occurrence in the run evidence, Ignore, reobserve and continue.
New warning: add its message/signature and evidence here, send a compact report to the
coordinator, then Ignore, reobserve and continue without waiting for a reply. Reporting and
investigating the underlying bug happen alongside the playtest. Do not repeatedly report the
same warning unless its consequence changes. A crash or recovery that cannot restore progress
is a blocker; a warning alone is not. This applies to debug warnings with an Ignore action,
not unrelated game choices or destructive confirmation dialogs.

Use the advertised `play --session SESSION debug-ignore --capture CURRENT_SHA` for a
startup debug dialog; copy the current capture from `play look`, then reobserve. Refresh the
capture for the next dialog and stop sending Ignore once World is ready. If that CLI action
is unavailable, use the existing verified Peekaboo route for the owned PID/birth with literal
`type "i"` (the tested route), then reobserve. Do not mistake input delivery for recovery or send keys blindly to a changed surface.

If a warning bears on the current claim, inspect the relevant state while continuing:
for example, verify actual item ownership/counts for an inventory test. Ignoring a dialog
neither fixes its cause nor proves save integrity.

## item-location-owner

- Match: `Failed to find item_location owner with character_id` (any ID).
- Meaning: a saved item reference could not resolve its character owner during loading.
- Handling: Ignore and continue; check actual item/actor state when relevant to the test.
- Source: `src/item_location.cpp`, observed line388; sorting deserialization appeared in the retained trace.
- Historical occurrence: R013 run `446128da066173112dac7cd7319432d53325ea11dac1d75dfb38b91ffe477069`; raw checkpoint consumed.
- Observed recovery: I/i advanced to another debug warning; this alone did not establish World readiness.

## item-location-target

- Match: `item_location lost its target item during a save/load cycle`.
- Meaning: `ensure_unpacked()` could not restore the saved item reference.
- Handling: Ignore and continue; verify affected item state for inventory/locker/pickup claims.
- Source: `src/item_location.cpp::ensure_unpacked`, observed line174.
- Historical occurrence: same R013 run above, after the owner-ID warning; raw checkpoint consumed.
- Disposition: Ignore-and-continue; subsequent R014 pilot below proves World recovery, not item integrity.

## parent-location-target

- Match: `parent location doesn't exist. Item_location has lost its target over a save/load cycle`.
- Meaning: deserialization could not resolve a parent item location and reports the target lost.
- Handling: Ignore once through the exact current capture, reobserve, and verify affected inventory/actor state if relevant; Ignore is not a save-integrity claim.
- Source: `src/item_location.cpp:1012`, observed in R013 startup.
- Historical occurrence: R013 recovered to a visible map after the owner/target/parent warning chain. Its raw logs and captures were consumed; no historical capture is current Ignore authority.
- Recovery evidence: the subsequent R014 pilot below proves this error family can recover to World; the historical outcome remains documented below, while raw artifacts are consumed.

## Verified CLI recovery

Historical R014 bound three successive owner/target/parent warnings to run `7b3cbe177ede1c365eafa8171a65a34483f49b2f1d74dd12213ad3d0503d972c`.
The CLI typed one literal i per current capture, reached same-run World at turn5216234,
and accepted `world.move.east` at turn5216235. A later stale-capture attempt sent no input
after readiness. The original raw response was consumed. This validates historical recovery and an
operable controller, not clean item deserialization, smoke admission or faction behavior.

## Maintenance

Luna/Sol workers may add observed warning signatures and update outcomes from evidence.
Merge equivalent messages; keep decisive logs only while the active diagnosis needs them, then
retain concise run/outcome facts here and remove consumed raw artifacts. Revise an
entry when observed consequences change; do not accumulate speculative warnings or duplicate
recovery procedures. Keep one short entry per meaningful error family.
