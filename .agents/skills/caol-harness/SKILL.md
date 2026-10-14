---
name: caol-harness
description: Query, explain, launch, operate, and audit C-AOL playtests through the authoritative registry and cockpit.
---

# C-AOL harness

## The world we are testing

This is a grim, dangerous zombie survival game. Cannibals, thugs and survivors are people
trying to survive, exploit others and pursue their own purposes. Their behavior should make
sense given what they know, want, fear and can actually do.

Absurd situations, dark humor, mistakes and desperate choices belong here. Accidental silliness
caused by disconnected AI systems does not. A robbery crew should remain purposeful through
scouting, confrontation, taking goods and getting away. Routine needs or minor distractions
should not casually erase that purpose.

Judge behavior as a player would experience it. Working code and passing tests are insufficient
when the resulting scene contradicts this vision. Notice the mismatch, investigate its cause
and recommend the smallest coherent correction. Preserve believable fear, exhaustion,
uncertainty and failure; do not force every encounter to succeed. This vision guides judgment
and proposals; it does not independently authorize gameplay redesign.

## Operating the playtest

Use the registry to select and launch a scenario and the cockpit/player CLI to operate its native
input owners. The harness executes the agent’s chosen playtest; it does not add human
approval or require a canonical file location as authority. Preserve the chosen save/build and
actual input/evidence identity. A setup convention that blocks valid play is a tooling defect to
repair, not a gameplay failure; claim acceptance remains separate from permission to run. The coordinator supplies the gameplay goal, current state and expected next
observable development; the worker owns strategy and evidence. Run CLI commands from the game worktree.

Play toward the intended outcome. An observation is a snapshot, not automatically a verdict.
When conditions are developing, continue ordinary play and compare changes. Preserve the first
meaningful divergence, then decide whether continued play can explain it. Investigate when behavior
contradicts the expected outcome or stops progressing; a crash or known hard rejection needs
recovery, not more waiting. Consult code when gameplay observations leave a consequential question
unresolved. Protocols are adaptable examples, not scripts whose first unexpected result ends the test.

Use the [game manual](GAME-MANUAL.md) for reusable playtest knowledge, behavior
assessment, owner handling corrections and note maintenance. Screenshots and OCR help locate
controls; native logs, receipts and saved state establish gameplay claims.

For the ordered faction/camp replay arms and a compact fire/raid recipe with its verified evidence boundary, use
[the sequential playtest suite](references/playtest-suite.md). It maps behaviors to recipes,
retained proof and the next build-specific replay pass, including bandit drafts. Select only the relevant arm.

## Reuse prepared work; react to the current game

Reuse the existing dev and dev_w checkouts, usually on the same development HEAD. A parallel
worktree is available when it materially helps; merge useful changes and remove the temporary
worktree afterward. Do not create per-attempt clones or redundant source/build copies. Keep the
current necessary build/cache, replacing superseded products.
Playtest saves, logs, captures and proof archives are temporary: remove them after the active
operation ends. Keep the commit, run identity, concise result and reproduction recipe in existing task context, not copies
of the run. Protect user data and active owner operations during cleanup. Cleanup is delivery work.

A prepared handoff supplies the selected save/build/scenario and prior proof. Reuse its exact handles and validation; inspect changed bindings and actual current World/input state, not the whole manifest/history again. Use complete action responses directly when they retain valid authority. Bounded wait/move may cover an unchanged segment, but inspect returned changes and interruptions; historical weather or creature absence is not a promise about this run. Do not blindly chain through a prompt or missing authority. Query only the actor/turn/event facts needed for the next decision, retaining raw retrieval handles. Unreadable evidence is incomplete, never zero events. Once current save, input and owned processes/helpers are settled, reuse that checkpoint/closeout in the handoff rather than rebuilding it at every layer.

## Live playtesting belongs to Luna

For live playtests, the coordinator assigns named stories to Sol oversight; Sol spawns one direct GPT-6 Luna game owner per game. Only Luna takes native input and reports to that same Sol; no additional player/chaperone layer. Luna owns ordinary strategy, supported recovery and evidence through the complete assigned outcome. Sol coordinates shared resources, investigates consequential contradictions and is accountable for proof and closeout, keeping each host/run/build/save/input owner distinct. The coordinator retains scope, priorities, lifecycle and measured capacity.

Give the player the whole outcome, current bindings, relevant successful protocol and real constraints. Luna chooses movement, waits, survival care, targeted evidence queries, meaningful saves, and current native exit controls without a parent approval for each step. A save, observation, first injury or partial report is not a handoff gate. Continue useful play while a nonblocking question is being investigated. Stop only at the assigned endpoint, an explicit owner stop, or a concrete input, save, danger or authority boundary that ordinary recovery cannot resolve.

Luna returns concise findings at meaningful changes and closeout, including source/run, first divergence, evidence handles, owned processes and retained state. Use complete action responses and one targeted extraction when needed. Sol consumes those facts and verifies material claims; do not repeat every query, privately decode every save, or prescribe an interval-by-interval play schedule for each game. Sol coordinates build/cache and runtime import windows, capacity, cross-story causes and unavailable recovery. A supported same-profile reload may be executed by Luna within its assigned authority; if fresh selection is coordinator-owned, Luna requests that specific operation once with the necessary save/run handles, then continues when the returned action arrives. Never replay an issued token or replace another input owner.

Label a previously successful playtest with its exact successful source, initial save, effective setup/protocol and demonstrated outcome. Historical downstream checkpoints are not full-initial proof. Compare the first meaningful change in actual setup, actions, controls or source when a journey diverges. Own ordinary recovery through a usable controller and the assigned gameplay outcome. If successive repairs create new integration failures, reassess and consolidate the ordinary controller path with the coordinator instead of accumulating exception wrappers. Validate its connected entrypoint, action/interruption handling and closeout at their honest offline or native scope; retire superseded runtime paths while retaining original evidence. Use delegated recovery authority and escalate only a material unresolved decision, with evidence and a recommended route. Escalation alone does not stop independent work or end the attempt. The coordinator collects nonblocking verified issues in existing summaries/ledger and judges repair priority and batching against outcome, evidence/save risk and the whole-suite restart cost. Prior success is not a guarantee or permission to force a result.

Preserve mandatory story steps and prioritize the actual assigned encounter; meaningful survival care remains available, but precautionary side work needs a real reason. If contact never arrives, inspect the original operation and receiver situation rather than assuming a pending payment. Briefs carry actual initial facts or uncertainty. Trust the agent: remove contradictory context or broken affordances at their producer. Fresh code does not silently replace a loaded controller. Live evidence uses inspect/evidence/messages/status; journal sealing and reporting require native process exit independently of save proof. Retained games keep their owner and current stop.

The chaperone owns mandatory closeout: completed/abandoned games and their brokers, bridges and
helpers must exit, with OS exit verified, and generated residue must be deleted. A returned worker
or hidden window is not closure. Use the [existing closeout guidance](references/witness-and-finish.md)
for graceful closure and explicitly approved disposal; protect current games and input owners.

## Chaperone-owned playtest field notes

Luna records concise actions, observations and meaningful deviations in the existing run/task notes. The Sol chaperone integrates material findings and verified lessons into the current task context and relevant journey protocol, rather than transcribing each player action or maintaining duplicate reports. Record what happened, native time/phase, source/run and evidence handles, its effect on the assigned outcome, and the current explanation or unresolved question. Distinguish observed facts from hypotheses. Update the same note when further play resolves it: explicitly say normal behavior/non-issue, confirmed defect, or still unresolved, with the resolving evidence. Preserve the original observation rather than leaving a superseded bug suspicion as current advice.

Use concise notes at meaningful boundaries, not a transcript or an entry for every action. Natural creature encounters, alarms and survival responses can be valid deviations; allow actual faction service cadence before calling a timing gap a stall. Resolved examples are in the [bandit journey field notes](references/bandit-journeys.md#field-note-1-party-alarm-delayed-scouting-return--resolved-normal-behavior). A field note does not stop useful play, settle the task or authorize a repair. The owner may request coordinator review of the notes to judge importance and choose what becomes lasting protocol guidance. Carry relevant resolved lessons into subsequent player briefs so they are not rediscovered.

## Choose the guidance for the current decision

Read the relevant theme when its situation arises. This is a reference map, not a sequence to
complete before playing. An existing live session can start with live operation; return to this
map when a new interaction, uncertainty or closeout question needs another theme. Linked pages
contain the detailed commands and evidence boundaries for that situation.

| Situation or question | Read |
| --- | --- |
| Choose a scenario, resolve readiness or binding, build, launch, inspect registry status | [Selection and launch](references/selection-and-launch.md) |
| A native debug warning interrupts loading or play | [Debug error catalogue: record/report, Ignore and continue](references/debug-errors.md) |
| Operate an existing session: look, act, controls, collect, cancel, macros, save/reload | [Live operation](references/live-operation.md) |
| Walk upstairs, reach a brazier or maintain the fire in Josef's base save | [Base-save route and actions](GAME-MANUAL.md#josefs-two-brazier-base-save) |
| Locate both normalized firewood zones/wood piles; maintain or widen the fuel source | [Normalized firewood zones and native refuelling](GAME-MANUAL.md#normalized-firewood-zones-and-stock) |
| Light, extinguish or check the prepared brazier | [Fire in a prepared brazier](GAME-MANUAL.md#fire-in-a-prepared-brazier) |
| Fire is burning but a camp cannot see smoke/light; range, time or height is unclear | [Smoke and light visibility](references/smoke-light-visibility.md) |
| Choose movement, separation or elapsed time; distinguish a weak experiment from a failure | [Behavior and movement](references/behavior-and-movement.md) |
| Apply setup interventions, handle danger, use free-text speech, inspect item ownership or zones | [Setup and interactions](references/setup-and-interactions.md) |
| Understand NPC decisions, retrieve omitted facts, correlate events or compare performance | [Evidence and diagnostics](references/evidence-and-diagnostics.md) |
| Produce journal-cited conclusions, finish a run, separate claim verdicts or check cleanup | [Witness and finish](references/witness-and-finish.md) |

For a specific missing field or implementation location, the [search map](references/searching.md)
points to targeted queries. Item activation and placement examples are in
[setup interaction](references/setup-interaction.md). The [R-029 signal boundary](references/r029-signal-evidence.md)
is relevant when interpreting that feature's signal evidence.

## Keep these distinctions during play

- Choose actions from the current native input owner and its advertised stable targets. Collect
  a pending request rather than submitting it again; refresh after stale authority.
- Keep in-game safe mode off for live playtests. Check its current state after entering World,
  use the advertised `world.toggle_safemode` action only when it is on, and verify the game
  reports it off before movement or waiting. Recheck after save/reload or a blocked move;
  toggling twice would turn it back on. See [live operation](references/live-operation.md).
- When startup or interaction stops making expected progress without explanation, inspect the
  owned window and relevant native/crash logs. Distinguish loading, a modal error, semantic-ready
  and exited; a live PID is not health. Preserve PID/birth, exact error and log reference before
  safely dismissing an understood non-destructive prompt through the verified Peekaboo route;
  verify resulting state/exit. This is conditional recovery, not a polling or screenshot ritual.
- An interruption or failed command leaves the game running. Cancellation stops a request;
  quitting, finishing and declared save/reload continuation have different lifecycle effects.
- Setup and debug interventions have zero natural-gameplay credit. Match conclusions to actual
  native receipts, observed behavior and the run's evidence ceiling. Transport success, startup
  and calculation completion each establish less than feature success.
- Compact views retain handles to complete evidence. Missing displayed detail does not establish
  absence. Choose a targeted local query or Luna extraction by the question and total work, then inspect
  cited evidence rather than duplicating the investigation.
- Keep observations and contradictions through recovery. At closeout, distinguish the feature
  result, remaining uncertainty and actual cleanup; use the witness theme when reaching that point.
- For prepared fire/smoke journeys and NPC observation, retrieve the relevant procedure from
  [the living game manual](GAME-MANUAL.md). It separates verified setup from current state,
  links the ordinary lighter/brazier controls and explains actor-level progress assessment.
