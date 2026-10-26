# Brazier smoke and light: strength, time and visibility

Use this when a fire is burning but a camp has not noticed it. Values below describe the current
production adapter inspected 2026-09-28; a source-bound run's measured packet is authoritative.
The standard indoor brazier is [3159,3449,0], roof brazier [3156,3447,1]. Verify the selected
save's actual furniture, fuel, fire, windows and source positions.

## Representative source placement

Owner clarification, 2026-10-26: representative end-to-end camp/tower stories require at least
10 OMTs between the player activity/source area and the camp or tower. Keep the player at least
10 OMTs from the participating home footprints during the representative remote leg; measure from
the nearest footprint edge, not just its anchor. Record actual home, player, source and observer
coordinates/z and OMT distances. Label the footprint-edge layout metric separately from the
native observer-to-source distance and measured reception cap; diagonal layout separation does
not establish that a source is in range. For 30–40 OMT reach scenarios, use substantially greater
separation that exercises the claimed reach, with actual signal strength, terrain and weather
supporting the selected case. Ten OMTs is a floor, not sufficient coverage of long-range reach.
Prepare15 OMT by default for ordinary full stories. Keep the emitter actually loaded/active
when its sampling requires that; the remote party is the camp, not necessarily the fire.

Closer setups remain explicitly labelled component, near-contact, combat or diagnostic controls;
they do not count as representative distant sensing, scouting or return proof. A necessary close-home
setup visit earns no remote proof; return to the separated player/source area before the remote leg.
Stay near the emitter when its native sampling requires a loaded source. If existing source reach
cannot support the minimum, report that coverage
limit and use a suitable scenario rather than silently moving the camp/player closer or relaxing
observation. Preserve current saves, actors and input ownership when adapting a live continuation.
Do not treat historical example coordinates/formulas below as current calibration.

Across the selected signal/journey recipes, include useful separated positive cases and meaningful
near-reach or beyond-reach/occluded negatives. Reuse existing variants; this does not order a full
suite replay or a new run matrix. Keep near-contact, combat, hazard and other deliberately short
controls when their purpose calls for them. Source receipt, scene recognition, selection of a
watching position and physical travel are separate outcomes; a distant signal need not reveal
nearby actors or permit a remote observer to inspect interior details.

Missing remote lighting is a separate diagnosis, not a reason to keep moving the brazier next to
the camp. AI observation must not require the player to see or explore the target. Distinguish
player visibility/history from loaded-map lighting, existing saved geometry and actually missing
physical inputs. At a demonstrated discrepancy, compare matched observer/target/time/terrain with
player near versus far and isolate which geometry the lighting calculation requires. Preserve real
darkness, roofs, occlusion and observer capability; unavailable data is neither proof of darkness
nor permission to declare visibility. Moving the player nearby may be a diagnostic comparison,
not evidence that the original remote behavior is correct. Fix a demonstrated dependency at its
owner rather than bypass it with easier placement. Coordinate any live setup change with the
existing input owner and preserve the first divergence and its reproduction context.

## Do not conclude from the first puff

Ignition, smoke fields, a viable signal, observer admission and dispatch are different events.
One weak sample establishes only that moment's range. Continue through relevant field changes
and the next observer scan using normal `safe` waits; don't repeatedly relight a burning brazier.
Compare samples at changed fire/smoke intensity, weather or time band. If the source is steady
and the rejection unchanged, diagnose that boundary instead of waiting indefinitely.

`field_processor_fd_fire` in `src/map_field.cpp` consumes fuel and probabilistically produces
smoke. It puts smoke on the tile above when that tile has no floor, otherwise on the fire tile.
Fire can strengthen or decay with fuel/field age; elapsed seconds alone do not guarantee growth.
A brazier contains fire spread; that does not mean its smoke/light is invisible.

The adapter's “persistence” is a current-intensity flag, **not time spent burning**. Signal range
does not accumulate every minute. Samples are per source map-square: smoke above the brazier
and fire below can have separate IDs and different z. Don't add their intensities together.

`live_light.cpp` samples once per advancing game turn. The loaded-source index rechecks active
sources each turn; discovery of a new non-player tile uses a 900-turn sweep (up to15 game minutes),
while the player tile is checked directly. Reading `look` does not advance this. Staffed camp
signal recording is on five-minute boundaries; dispatch cadence is30 minutes and structural
maintenance60 minutes (`do_turn.cpp::overmap_npc_move`). These are opportunities, not promises
of dispatch: observer eligibility, existing outings and retained knowledge also matter.

## Smoke: nominal range in OMT

From `bandit_mark_generation.cpp::adapt_local_field_signal_reading` and `adapt_smoke_packet`:
let F be sampled fire intensity, S smoke intensity on **that same tile**, A=clamp(F+S,1,3),
P=1 if S>0 else0, H=1 if F>=2 else0. With F>0 or S>0:

`cap = clamp(4 + 2*A + 2*P + H - weather_penalty, 1, 15)`

The packet is viable only if `A + P + H - weather_penalty > 0`. Therefore a positive
printed cap alone is insufficient. Clear-weather examples:

- F1/S0: cap6; F1/S1: cap10; F1/S2 or S3: cap12.
- F2/S0: cap9; F2 with smoke: cap13. F3/S0: cap11; F3 with smoke: cap13.
- A separate smoke-only tile F0/S1, S2, S3: caps8,10,12.

Subtract0 clear,1 windy,2 rain,3 fog,1 portal storm from the cap and visibility score.
The runtime classifies portal storm first, then precipitation, then sight_penalty>=2 as fog,
then windspeed>=20 as windy. Smoke has **no day/night penalty** in this adapter.
These are emitted range limits, not guaranteed detection radii through terrain.

## Ordinary brazier light: nominal range in OMT

For a brazier without another emitter on the tile, A=clamp(F,1,3), P=1 if F>=2 else0.
The formula is `cap = clamp(1 + 2*A + P + M, 0, 30)`, viable only if `A + P + M > 0`.
M combines exposure, aperture leakage, roof elevation, weather and time; this is ordinary fire,
not the searchlight bonus.

For an exposed flat-roof brazier, M=2 exposure + L leakage +2 roof bonus - T - W.
L is the measured1 or2, not assumed. T=0 night,2 dawn/dusk,6 daylight. W=0 clear,2 rain,3 fog.
Clear roof examples (L1–2), before terrain costs:

- F1: night8–9, twilight6–7, day2–3. Day/L1 is **nonviable** despite cap2.
- F2: night11–12, twilight9–10, day5–6.
- F3: night13–14, twilight11–12, day7–8.

An exposed ground-level fire lacks the roof +2. A direct indoor-to-exterior aperture uses
leakage2 and exposed classification but no roof bonus. `physical_light::evaluate_escape`
checks an actual transparent path to an outside tile within three local tiles, not a README's
“windows open” label. Fully contained light is rejected by `physical_light::detect`; it cannot
shine through a sealed room just because its packet has a positive cap.
Portal storms normally subtract3 for exposed light, reduced to1 for exposed F>=2 at night/twilight;
contained light has additional penalty. Inspect the actual packet rather than assuming clear weather.

## Terrain, height and actual reach

Exposed smoke and light can be seen across z-levels when their physical visibility conditions
hold. Current `do_turn.cpp::live_bandit_overmap_smoke_los_from` uses the observable plume position
or exposed-to-sky source for cross-level smoke and pays intervening terrain costs. A current loaded
source with neither observable plume nor sky exposure is rejected. The generic
`live_bandit_overmap_los_from` still rejects differing z, but it is not the complete smoke path.
Use the bound run's actual smoke support and admission result; neither roof height alone nor
extra waiting establishes visibility. Light has its own exposure/vertical path.

The current coarse overmap sight model uses `line_to(observer, source)` and terrain see costs:
clear/none0, low1, medium2, spaced-high4, high5, full-high10, opaque999.
For distinct points, the ray **excludes its observer origin and includes the source endpoint**;
coincident points produce one origin tile. Earlier reports saying both endpoints always count,
and therefore the observer camp's cost5 always blocks light, were wrong.

Light needs `distance + sum(ray terrain costs) <= cap`, plus exposure/vertical sightline.
Current same-level smoke needs distance<=cap and independently sum(costs)<=cap; it does not
subtract distance from that second budget. Preserve that distinction when explaining results.
A high-cost source or intervening forest/building can reduce reach; don't charge the camp origin
when it is absent from the actual ray. Use the game's `rl_dist` and actual ray, not guessed geometry.

## Judge the experiment

At useful changes record source tile/ID, F/S, sample turn, weather/time band, exposure, cap,
observer position, measured distance, ray costs and admission/rejection. Use existing bound
channel/transition logs and compact native observations; no normal-game analytical spam.
A live_smoke packet can be generated from fire alone, so separately inspect fd_smoke when proving
visible smoke fields. Extinguished fire can leave smoke and already learned camp knowledge.

The earlier roof sample cap6 smoke/cap4 light versus distance7 was out of range **then**;
it does not establish the mature fire's maximum. A changed packet can change reach. A hard cross-z
rejection or an unchanged valid range/occlusion failure is a different diagnosis. If something
“worked five seconds ago,” compare exact source/sample IDs, intensities, height, weather, build and
observer—not just the fire's name. Keep progressing while the inputs are evolving; preserve the
first repeatable contradiction when they are not. Neither one early rejection nor endless waiting
is a complete playtest.
