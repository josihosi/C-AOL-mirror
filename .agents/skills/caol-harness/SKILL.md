---
name: caol-harness
description: Query, explain, launch, operate, and audit C-AOL playtests through the authoritative registry and cockpit.
---

# C-AOL harness

## The world we are testing

This is a grim, dangerous zombie survival game. Cannibals, thugs and survivors pursue purposes
with the knowledge, abilities and fears they actually have. Absurd situations, dark humor and
desperate choices belong here; accidental silliness caused by disconnected AI systems does not.
A robbery crew should remain purposeful through scouting, confrontation, taking goods and leaving.
Preserve believable fear, exhaustion, uncertainty and failure rather than forcing every encounter
to succeed. Judge the scene as a player experiences it. Passing code tests cannot excuse a scene
that contradicts this vision; a mismatch calls for diagnosis, not an unauthorized redesign.

## Own a scenario through its outcome

The coordinator supplies the whole outcome,
current bindings and real constraints; the player chooses practical strategy and recovery.
Protocols are reusable recipes, not scripts whose first unexpected result ends the test.

Start fresh full-story runs from their coherent initial save. Use compatible checkpoints for
focused questions and continue the same observed journey through ordinary save/reload. Credit
what actually happened; a checkpoint cannot prove skipped stages. For mixed-load scaling, reuse
the qualified current continuation when source/build compatibility, actor population, signal/setup
state and ownership match the intended comparison. Recheck changed premises only; do not restart
a proved setup or repeat Luna→chaperone→coordinator summaries without a new decision. Increasing
load is distinct from replaying a whole story, and inactive actors do not prove active mixed work. See [selection and launch](references/selection-and-launch.md).

Reach the condition being tested and follow it to a judged endpoint. Missing an event or failing
one tactic means adapt the setup or strategy. A stopped session, first injury, save or partial
report is not completion. An actual product defect or external blocker can prevent progress;
retain the concrete cause and unfinished repair/retest obligation. Death, defeat or retreat can
be a valid endpoint when that is the behavior being tested. Continue independent useful scenarios
in parallel at measured capacity, without taking another game's input or changing its live build.

Reason practically about shelter, routes, fuel, sight, danger and waiting positions. Josef permits
liberal avatar setup mutations and teleportation to remove irrelevant friction. Establish the
experiment efficiently, then let the tested NPC/game mechanism operate normally. An induced
condition earns no proof of the bypassed natural stage, but subsequent native behavior can fully
prove the downstream question. Use [setup and interactions](references/setup-and-interactions.md)
for mutation effects and [scenario composition](GAME-MANUAL.md#compose-a-useful-scenario) for examples.

For representative camp/tower signal and response stories, require at least 10 OMTs of
player/activity-source separation from participating camp/tower footprints. Use substantially
greater separation for 30–40 OMT reach scenarios. Record actual distances and measured source
reach. Closer fixtures are labelled focused controls and do not prove representative remote
behavior; close-home setup visits end before the remote leg while the emitter stays actively
simulated. Follow
[representative source placement](references/smoke-light-visibility.md#representative-source-placement).

## Starting a game

Work from the game checkout on the machine that will run it. Reuse a ready source-bound product
and compatible save; a new test does not itself require a build. Give the direct Luna player the
test outcome, selected save/build, relevant recipe and exact registry-returned launch command.

- Query the registry, then use its selected `next_action`. A query does not launch a game.
  The token is single-use; `registry-detached-launch` creates its own new session directory.
- Choose the right bootstrap. An ordinary fixture starts through the fresh HUD/World route.
  A saved continuation uses its real snapshot and retained state; being based on a save does not
  by itself justify `allow_saved_native_input_owner`. See the
  [fresh versus saved launch settings](references/selection-and-launch.md#fresh-hud-or-saved-input-owner).
- Use a profile name under `.userdata`, not an absolute path. From the returned session, use
  `play_cli.py --session SESSION look` and `controls` to obtain current native input authority.
  Resolve advertised startup menus if needed; require the intended same-run World before play.
  A live process or successful launcher is not an attached game.
- On Windows/WSL, use the existing native host owner and its checkout, paths and build settings.
  Keep the owning launch/session alive through the game and closeout; do not finish a one-shot
  wrapper while its game still depends on it. Reuse the existing route rather than adding owners.

For a changed declaration, lint it before selection. The [launch reference](references/selection-and-launch.md)
contains the commands, snapshot/reentry shape, helper prerequisites and conditional build checks.

## Live playtesting belongs to Luna

GPT-6.1 Terra normally chaperones assigned stories and spawns one direct GPT-6 Luna game owner per game. Only Luna
operates native input; no additional player/chaperone layer. Give Luna the intended outcome,
current save/build/input state and relevant recipe, not the whole protocol collection. Luna owns
movement, waits, survival care, permitted setup, supported recovery, useful saves and native exit
through the assigned outcome. Ordinary steps do not need parent approval.

The chaperone owns practical diagnosis, supported recovery and accountable story closeout. Sol handles shared resources, cross-task priorities and difficult coupled engineering. The coordinator
retains scope, priorities, lifecycle, current owner context and capacity. Honor explicit stops and
original ownership. An unavailable worker or timed-out command does not prove its game ended.
A real unresolved authority, input or save boundary needs the exact failed operation and a useful
recovery recommendation; it does not stop unrelated work.

Luna reports meaningful changes and final findings concisely: actual outcome, first divergence,
source/run and evidence handles, remaining question, current save/input and owned processes.
Sol verifies material claims without repeating every query. Current assignments and repair batches
belong in the owner context/ledger; protocols do not create another repair or approval gate.

## Operate from current controls

Use returned action/menu observations directly. Current advertised targets supply input authority;
historical menu numbers, actor IDs and screenshots do not. Collect a pending request rather than
submit it again. Refresh after a stale frame or missing successor. Batch quiet travel and waits
until the next useful decision, preserving interruption policy and actual elapsed game time.

Keep in-game Safe mode off for these playtests: check on entry/reload or a blocked move and toggle
only if on. This is distinct from the CLI's explicit `safe`, `stop` and `ignore` wait policies.
`safe` returns on danger; `ignore` permits intended combat, including death. Tab is autoattack,
not a neutral wait. Read-only observations and wall-clock waiting do not advance simulation.

When progress is unclear, compare actual positions, time, actions and conditions. Resolve doors,
fuel, stamina, prompts and service cadence before assuming a game defect. A repeatable contradiction
needs targeted inspection rather than endless unchanged waiting. Use existing selected-actor
traces and evidence handles; missing compact detail is not absence. Screenshots/OCR help locate
controls; native actions, events and saves support gameplay conclusions.

## Keep guidance useful

Routine results and hypotheses stay in the existing task notes; a run does not automatically
require a protocol edit. Promote only verified knowledge that changes a future setup, action,
interpretation or recovery. Replace obsolete advice in its owning reference and remove conflicting
duplicates. Never turn an incident into another permanent prohibition or approval step. The
[manual's maintenance rule](GAME-MANUAL.md#add-prune-and-correct-notes) governs these updates.

Reuse dev/dev_w and their current compatible builds/caches; workspace rules forbid temporary
worktrees or source clones. After a run ends, its owner closes the game and helpers, verifies OS
exit and removes generated residue. A worker return, hidden window or report is not process
closure. Protect active games and unsaved owner state; use [witness and finish](references/witness-and-finish.md)
for native closeout, evidence and specifically authorized disposal.

## Choose the relevant reference

| Decision | Reference |
| --- | --- |
| Compose a scenario; fuel, survival, visibility or NPC behavior | [Game manual](GAME-MANUAL.md) |
| Choose an existing family or variant | [Playtest suite](references/playtest-suite.md) |
| Bind a save/build, launch or resume | [Selection and launch](references/selection-and-launch.md) |
| Act, wait, collect, trade or save/reload | [Live operation](references/live-operation.md) |
| Mutations, setup, speech or item ownership | [Setup and interactions](references/setup-and-interactions.md) |
| Item activation/placement or Mac input fallback | [Item controls](references/setup-interaction.md) |
| Movement, separation or apparent nonprogress | [Behavior and movement](references/behavior-and-movement.md) |
| Fire source range, weather, terrain or height | [Smoke and light](references/smoke-light-visibility.md) |
| Native debug warning | [Debug error catalogue](references/debug-errors.md) |
| NPC events or missing evidence | [Evidence and diagnostics](references/evidence-and-diagnostics.md), [search map](references/searching.md) |
| Finish the result and close owned processes | [Witness and finish](references/witness-and-finish.md) |

Read only what the current decision needs. The suite links detailed faction, away/return and
companion stories; those pages supply scenario-specific outcomes and controls.
