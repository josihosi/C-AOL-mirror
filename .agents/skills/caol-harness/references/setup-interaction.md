# Item activation and placement

`look` exposes `current_input.prompt`, `selection` and `controls` from the current native owner.
The highlighted item is separate from the available target list and from a filter string. An
unavailable selection fact is unknown; inspect the current UI rather than assuming the first action
is selected. `actions_selector` retrieves the complete matching actions with enabled state.

For a named item, open the advertised activation owner, filter by the item name, inspect the
matching selection, then confirm it. Inspect the actual placement prompt, choose the intended
destination using its available actions, and verify the resulting World facts. Bindings come from
the current interface; these examples are ingredients, not a fixed-key replay. In a native filter or text action, `act ACTION --param text=VALUE` supplies
the text parameter; the GUI filter prompt is also available. Inspect the resulting selection
before confirming: an accepted filter request alone is not proof that the visible list changed.

## GUI setup on this Mac

Use the verified Mac bridge socket on every Peekaboo call. On an actual failure, query bridge
and permissions status through that same socket; do not repeat already granted permission requests. Read `SESSION/game-process.json` for the bound PID
and confirm that process is still the session's game. Replace PID below with that value; never
copy a historical process ID. Prefer native semantic actions where they expose the operation.

```sh
/opt/homebrew/bin/peekaboo --bridge-socket "/Users/josefhorvath/Library/Application Support/Peekaboo/bridge.sock" --json see --pid PID --path .openclaw/tmp/setup.png
/opt/homebrew/bin/peekaboo --bridge-socket "/Users/josefhorvath/Library/Application Support/Peekaboo/bridge.sock" --json type "brazier" --pid PID
/opt/homebrew/bin/peekaboo --bridge-socket "/Users/josefhorvath/Library/Application Support/Peekaboo/bridge.sock" --json press return --pid PID
```

`type` sends text: `{down}{return}` is literal text, not two key presses. Use separate `press`
calls for special keys. For clicking, use the current element/snapshot or a freshly observed
window ID with window-relative `--coords X,Y`; always target the verified PID. Derive coordinates
from the current capture/window geometry, not arbitrary Retina scaling. A successful tool
transport does not prove selection, deployment or ignition; observe the native result.

## Verify the destination

After World returns, copy `current_input.source_selector` from `look` and substitute that selector
for SOURCE, the session for SESSION, and the observed request for REQUEST. Replace 1,1 below with
the actual destination relative to the avatar in that observation (movement changes these offsets).

```sh
python3 tools/openclaw_harness/play_cli.py --session SESSION inspect SOURCE.surface.facts.visible_local --request-id REQUEST | jq 'if .ok then [.slice[] | select(.dx == 1 and .dy == 1) | {dx,dy,terrain,furniture,fields}] else . end'
python3 tools/openclaw_harness/play_cli.py --session SESSION messages --contains "fire"
```

`f_brazier` alone establishes deployment. `f_brazier` plus `fd_fire` at the destination, correlated
with the recent ignition result message, establishes the observed burning setup. Overlapping sprites,
fixture history and input transport success cannot substitute for these facts. Retain the exact
request and message time. Manufactured setup has zero ecology credit.

## Ordinary fire for smoke and camp playtests

When the owner has prepared a brazier, fuel and a firewood source zone, use them through ordinary
gameplay. From an adjacent safe tile, open inventory, select the current charged lighter, choose
its **Activate** item action, and at **Light where?** choose the brazier tile from the current
direction controls. A historical native check returned World with `fd_fire` and `fd_hot_air1`
on the chosen tile; its matrix/transcript was consumed and is not a current dependency. Use fresh
item and direction IDs in each run. A brazier alone, an accepted command, hot air elsewhere, or a
preseeded fire does not prove this run ignited the brazier. After confirming actual fire, step
about two tiles away without stepping onto the burning tile. Check that fuel and fire persist.

Prepared saves often supply fuel and a lighter; use what is actually present. If fuel, access or
source-zone geometry cannot support the intended interval, arrange practical setup through current
controls rather than treating an old recipe as a prohibition on maintenance. Follow the
[shared setup policy](setup-and-interactions.md#mutation-setup-for-suite-runs). Avoid unnecessary
bulk pickup: moving a whole wood pile is different from selecting enough fuel for this fire.

For closed-room smoke and open-room light comparisons, retain the selected window/curtain geometry
and a real fire; a smokebomb or injected signal tests a different source. If the fire was already lit,
verify it and continue without relighting, preserving who supplied the ignition. Fuel interventions
establish setup; subsequent native signal and NPC behavior retain their own proof.

The reusable isolated scenario `r029.setup_information_diagnostic_mcw` and its matching brief/charter
under `tools/openclaw_harness/charters/r029-setup-information-diagnostic*.json` exercise this route
without ecology credit. It uses a separate profile and semantic startup, avoiding profile startup
keys that can open an unrelated menu before cockpit attachment.
