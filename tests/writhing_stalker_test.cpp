#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "character.h"
#include "damage.h"
#include "debug.h"
#include "effect.h"
#include "game.h"
#include "horde_entity.h"
#include "json.h"
#include "json_loader.h"
#include "level_cache.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "monster.h"
#include "mongroup.h"
#include "mtype.h"
#include "npc.h"
#include "options_helpers.h"
#include "overmap.h"
#include "overmapbuffer.h"
#include "player_helpers.h"
#include "projectile.h"
#include "scout_observation.h"
#include "type_id.h"
#include "weather.h"
#include "weather_type.h"
#include "writhing_stalker_ai.h"

static const damage_type_id damage_cut( "cut" );
static const mongroup_id GROUP_ZOMBIE( "GROUP_ZOMBIE" );
static const mongroup_id GROUP_ZOMBIE_HORDE( "GROUP_ZOMBIE_HORDE" );
static const mtype_id mon_zombie( "mon_zombie" );
static const mtype_id mon_zombie_runner( "mon_zombie_runner" );
static const mtype_id mon_writhing_stalker( "mon_writhing_stalker" );
static const species_id species_HUMAN( "HUMAN" );
static const species_id species_ZOMBIE( "ZOMBIE" );

namespace
{

const char *decision_trace_name( const writhing_stalker::decision value )
{
    using writhing_stalker::decision;
    switch( value ) {
        case decision::ignore:
            return "ignore";
        case decision::interested:
            return "interested";
        case decision::shadow:
            return "shadow";
        case decision::hold:
            return "hold";
        case decision::strike:
            return "strike";
        case decision::withdraw:
            return "withdraw";
        case decision::cooling_off:
            return "cooling_off";
    }
    return "unknown";
}

const char *route_trace_name( const writhing_stalker::approach_class value )
{
    using writhing_stalker::approach_class;
    switch( value ) {
        case approach_class::none:
            return "none";
        case approach_class::cover_shadow:
            return "cover_shadow";
        case approach_class::edge_shadow:
            return "edge_shadow";
        case approach_class::direct_forced:
            return "direct_forced";
        case approach_class::hold_exposed:
            return "hold_exposed";
    }
    return "unknown";
}

struct stalker_pattern_row {
    int turn = 0;
    int distance = 0;
    int hp_percent = 100;
    int cooldown_turns = 0;
    int strike_count = 0;
    writhing_stalker::decision next = writhing_stalker::decision::ignore;
    writhing_stalker::approach_class route = writhing_stalker::approach_class::none;
    std::string reason;
};

std::string trace_rows( const std::vector<stalker_pattern_row> &rows )
{
    std::ostringstream out;
    for( const stalker_pattern_row &row : rows ) {
        out << 't' << row.turn << " dist=" << row.distance << " hp=" << row.hp_percent
            << " cd=" << row.cooldown_turns << " decision=" << decision_trace_name( row.next )
            << " route=" << route_trace_name( row.route ) << " strikes=" << row.strike_count
            << " reason=" << row.reason << '\n';
    }
    return out.str();
}

writhing_stalker::live_context pattern_base_context()
{
    writhing_stalker::live_context ctx;
    ctx.has_believable_local_evidence = true;
    ctx.cover_route_available = true;
    ctx.direct_open_route_available = true;
    ctx.near_cover_or_clutter = true;
    return ctx;
}

std::vector<stalker_pattern_row> run_vulnerable_stalker_pattern( const int turns )
{
    std::vector<stalker_pattern_row> rows;
    writhing_stalker::live_context ctx = pattern_base_context();
    ctx.player_bleeding = true;
    ctx.player_hurt = true;
    ctx.player_low_stamina = true;
    ctx.player_distracted = true;
    ctx.player_noisy = true;
    ctx.zombie_pressure = 2;

    int distance = 5;
    int cooldown_turns = 0;
    int hp_percent = 100;
    int strike_count = 0;

    for( int turn = 0; turn < turns; ++turn ) {
        ctx.distance_to_target = distance;
        ctx.on_cooldown = cooldown_turns > 0;
        ctx.stalker_hurt = hp_percent <= 55;
        ctx.burst_strikes = strike_count;

        const writhing_stalker::live_response response = writhing_stalker::evaluate_live_response( ctx );
        rows.push_back( stalker_pattern_row{ turn, distance, hp_percent, cooldown_turns, strike_count,
                                             response.next, response.route, response.reason } );

        if( response.next == writhing_stalker::decision::strike ) {
            ++strike_count;
            distance = 2;
            if( strike_count == 2 ) {
                hp_percent = 50;
            }
        } else if( response.next == writhing_stalker::decision::cooling_off ) {
            cooldown_turns = std::max( 0, cooldown_turns - 1 );
            distance = std::min( 5, distance + 1 );
        } else if( response.next == writhing_stalker::decision::shadow ) {
            distance = std::max( 2, distance - 2 );
        } else if( response.next == writhing_stalker::decision::withdraw ) {
            distance += 3;
        }
    }

    return rows;
}

int count_decisions( const std::vector<stalker_pattern_row> &rows,
                     const writhing_stalker::decision next )
{
    int count = 0;
    for( const stalker_pattern_row &row : rows ) {
        if( row.next == next ) {
            ++count;
        }
    }
    return count;
}

void refresh_writhing_stalker_arena( map &here )
{
    g->reset_light_level();
    here.invalidate_visibility_cache();
    here.update_visibility_cache( 0 );
    here.invalidate_map_cache( 0 );
    here.set_transparency_cache_dirty( 0 );
    here.set_pathfinding_cache_dirty( 0 );
    here.build_map_cache( 0 );
}

void prepare_writhing_stalker_arena( map &here, const tripoint_bub_ms &center )
{
    clear_avatar();
    const ter_id t_floor( "t_floor" );
    const ter_id t_wall( "t_wall" );
    for( const tripoint_bub_ms &p : here.points_in_radius( center, 12 ) ) {
        here.ter_set( p, t_floor );
        here.furn_clear( p );
        here.clear_fields( p );
    }

    // A little adjacent clutter gives the live context an honest cover route without
    // blocking the east/west sight line used by the seam-consumption proof.
    here.ter_set( center + point::north, t_wall );
    refresh_writhing_stalker_arena( here );
}

monster round_trip_writhing_stalker( const monster &stalker )
{
    std::ostringstream serialized;
    {
        JsonOut json( serialized, true );
        stalker.serialize( json );
    }
    monster loaded;
    loaded.deserialize( json_loader::from_string( serialized.str() ).get_object() );
    return loaded;
}

} // namespace

TEST_CASE( "writhing_stalker_monster_footing", "[writhing_stalker][monster]" )
{
    const mtype &stalker = *mon_writhing_stalker;
    const std::string exact_description =
        "A gaunt, trembling figure keeps to the edges of cover, its long arms clutched around a body "
        "that never stops writhing.  Its frantic eyes follow each struggle, and its wet teeth show "
        "whenever someone falters.";

    CHECK( stalker.nname() == "writhing stalker" );
    CHECK( stalker.get_description() == exact_description );
    CHECK( stalker.in_species( species_ZOMBIE ) );
    CHECK( stalker.in_species( species_HUMAN ) );
    CHECK( stalker.hp >= 85 );
    CHECK( stalker.hp <= 100 );
    CHECK( stalker.speed >= 115 );
    CHECK( stalker.speed <= 125 );
    CHECK( stalker.sk_dodge >= 4 );
    CHECK( stalker.sk_dodge <= 5 );
    CHECK( stalker.melee_damage.type_damage( damage_cut ) >= 4.0f );
    CHECK( stalker.has_special_attack( "scratch" ) );
    CHECK( stalker.has_special_attack( "bite" ) );
    CHECK( stalker.has_flag( mon_flag_KEENNOSE ) );
    CHECK( stalker.has_flag( mon_flag_PATH_AVOID_DANGER ) );
    CHECK( stalker.has_flag( mon_flag_HARDTOSHOOT ) );
    // The stalker now owns its burst/fade cadence in the custom live planner; the generic
    // HIT_AND_RUN flag would force one-hit fleeing before the bounded burst can happen.
    CHECK_FALSE( stalker.has_flag( mon_flag_HIT_AND_RUN ) );
    CHECK_FALSE( stalker.has_flag( mon_flag_UNBREAKABLE_MORALE ) );
    CHECK_FALSE( stalker.has_flag( mon_flag_GRABS ) );
    CHECK_FALSE( stalker.has_flag( mon_flag_GROUP_BASH ) );
    CHECK( stalker.has_anger_trigger( mon_trigger::STALK ) );
    CHECK( stalker.has_anger_trigger( mon_trigger::HOSTILE_WEAK ) );
    CHECK( stalker.has_fear_trigger( mon_trigger::HURT ) );
    CHECK( stalker.has_fear_trigger( mon_trigger::BRIGHT_LIGHT ) );
    CHECK_FALSE( stalker.upgrades );
}

TEST_CASE( "writhing_stalker_spawn_footing_is_early_city_singleton",
           "[writhing_stalker][monster][mongroup]" )
{
    const MonsterGroup &group = GROUP_ZOMBIE.obj();
    const MonsterGroup &horde_group = GROUP_ZOMBIE_HORDE.obj();

    std::optional<MonsterGroupEntry> stalker_entry;
    std::optional<MonsterGroupEntry> runner_entry;
    int direct_entries = 0;
    int direct_runner_entries = 0;
    for( const MonsterGroupEntry &entry : group.monsters ) {
        if( entry.is_group() ) {
            continue;
        }
        if( entry.mtype == mon_writhing_stalker ) {
            stalker_entry = entry;
            direct_entries++;
        }
        if( entry.mtype == mon_zombie_runner ) {
            runner_entry = entry;
            direct_runner_entries++;
        }
    }

    REQUIRE( direct_entries == 1 );
    REQUIRE( stalker_entry.has_value() );
    CHECK( stalker_entry->frequency == 200 );
    CHECK( stalker_entry->cost_multiplier == 25 );
    CHECK( stalker_entry->pack_minimum == 1 );
    CHECK( stalker_entry->pack_maximum == 1 );
    CHECK( stalker_entry->starts == 0_turns );
    // Runner weight is the established nearby early-city calibration used for
    // this starting value; it does not impose a global encounter-rate quota.
    REQUIRE( direct_runner_entries == 1 );
    REQUIRE( runner_entry.has_value() );
    CHECK( stalker_entry->frequency == runner_entry->frequency );

    std::optional<MonsterGroupEntry> horde_stalker_entry;
    std::optional<MonsterGroupEntry> horde_runner_entry;
    int horde_direct_entries = 0;
    int horde_runner_entries = 0;
    for( const MonsterGroupEntry &entry : horde_group.monsters ) {
        if( !entry.is_group() && entry.mtype == mon_writhing_stalker ) {
            horde_stalker_entry = entry;
            horde_direct_entries++;
        }
        if( !entry.is_group() && entry.mtype == mon_zombie_runner ) {
            horde_runner_entry = entry;
            horde_runner_entries++;
        }
    }

    REQUIRE( horde_direct_entries == 1 );
    REQUIRE( horde_stalker_entry.has_value() );
    CHECK( horde_stalker_entry->frequency == 200 );
    CHECK( horde_stalker_entry->cost_multiplier == 1 );
    CHECK( horde_stalker_entry->pack_minimum == 1 );
    CHECK( horde_stalker_entry->pack_maximum == 1 );
    CHECK( horde_stalker_entry->starts == 0_turns );
    REQUIRE( horde_runner_entries == 1 );
    REQUIRE( horde_runner_entry.has_value() );
    CHECK( horde_stalker_entry->frequency == horde_runner_entry->frequency );
}

TEST_CASE( "writhing_stalker_live_plan_consumes_quiet_side_cutoff_seam",
           "[writhing_stalker][monster][map][zombie_shadow]" )
{
    clear_map_without_vision();
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center{ 65, 65, 0 };
    const tripoint_bub_ms stalker_start = center + point::east * 5;
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 0_hours );
    refresh_writhing_stalker_arena( here );

    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), stalker_start );
    monster &pressure_a = spawn_test_monster( mon_zombie.str(), center + point::east * 3 );
    monster &pressure_b = spawn_test_monster( mon_zombie.str(),
                          center + point::east * 4 + point::north );
    monster &pressure_c = spawn_test_monster( mon_zombie.str(),
                          center + point::east * 2 + point::south );
    // Pressure is target-directed evidence, not mere proximity.  Give the
    // fixture the same target commitment that the live resolver observes.
    pressure_a.set_dest( you.pos_abs() );
    pressure_b.set_dest( you.pos_abs() );
    pressure_c.set_dest( you.pos_abs() );
    pressure_a.aggro_character = true;
    pressure_b.aggro_character = true;
    pressure_c.aggro_character = true;
    stalker.anger = 100;
    stalker.aggro_character = true;

    REQUIRE( stalker.sees( here, you ) );
    const int evidence_turn = to_turn<int>( calendar::turn );
    writhing_stalker::record_observed_attack( &stalker, &pressure_a, &you, evidence_turn,
            writhing_stalker::next_resolution_id() );
    writhing_stalker::record_observed_attack( &stalker, &pressure_b, &you, evidence_turn,
            writhing_stalker::next_resolution_id() );
    writhing_stalker::record_observed_attack( &stalker, &pressure_c, &you, evidence_turn,
            writhing_stalker::next_resolution_id() );
    stalker.plan();

    const tripoint_bub_ms dest = here.get_bub( stalker.get_dest() );
    CHECK( dest.x() < you.pos_bub().x() );
    CHECK( rl_dist( dest, you.pos_bub() ) >= 2 );
    CHECK( rl_dist( dest, you.pos_bub() ) <= 4 );
    CHECK_FALSE( dest == you.pos_bub() );
    clear_map_without_vision();
}

TEST_CASE( "writhing_stalker_live_plan_is_independent_of_diagnostic_filter",
           "[writhing_stalker][monster][map][debug]" )
{
    struct plan_state {
        tripoint_abs_ms destination;
        tripoint_abs_ms wander_destination;
        int wander_range;
        int moves;
        int anger;
        bool aggro_character;
        time_point turn;
    };

    const auto run_plan = []( bool diagnostics_enabled ) {
        const int previous_level = getDebugLevel();
        const int previous_class = getDebugClass();
        on_out_of_scope restore_filters( [previous_level, previous_class]() {
            limitDebugLevel( previous_level );
            limitDebugClass( previous_class );
        } );
        clear_map_without_vision();
        map &here = get_map();
        Character &you = get_player_character();
        const tripoint_bub_ms center{ 65, 65, 0 };
        restore_on_out_of_scope restore_calendar_turn( calendar::turn );
        set_time( calendar::turn_zero );
        prepare_writhing_stalker_arena( here, center );
        you.setpos( here, center );

        limitDebugLevel( diagnostics_enabled ? DL_ALL : 0 );
        limitDebugClass( D_GAME );
        monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 5 );
        stalker.anger = 100;
        stalker.aggro_character = true;
        REQUIRE( stalker.sees( here, you ) );
        stalker.plan();

        return plan_state{ stalker.get_dest(), stalker.wander_pos, stalker.wandf, stalker.get_moves(),
                           stalker.anger, stalker.aggro_character, calendar::turn };
    };

    const int caller_level = getDebugLevel();
    const int caller_class = getDebugClass();
    const plan_state enabled = run_plan( true );
    CHECK( getDebugLevel() == caller_level );
    CHECK( getDebugClass() == caller_class );
    const plan_state disabled = run_plan( false );
    CHECK( getDebugLevel() == caller_level );
    CHECK( getDebugClass() == caller_class );
    CHECK( enabled.destination == disabled.destination );
    CHECK( enabled.wander_destination == disabled.wander_destination );
    CHECK( enabled.wander_range == disabled.wander_range );
    CHECK( enabled.moves == disabled.moves );
    CHECK( enabled.anger == disabled.anger );
    CHECK( enabled.aggro_character == disabled.aggro_character );
    CHECK( enabled.turn == disabled.turn );

    clear_map_without_vision();
}

TEST_CASE( "writhing_stalker_interest_sources_are_ranked_without_bandit_economy",
           "[writhing_stalker][ai]" )
{
    using namespace writhing_stalker;

    const interest_report empty = evaluate_interest( interest_context{} );
    interest_context human_ctx;
    human_ctx.recent_human_evidence = true;
    human_ctx.evidence_age_minutes = 4;
    const interest_report human = evaluate_interest( human_ctx );

    CHECK( human.source == interest_source::human );
    CHECK( human.can_latch );
    CHECK( human.confidence >= 4 );
    CHECK( human.score > empty.score );
    CHECK( human.reason == "recent_human_evidence" );

    interest_context smoke_ctx;
    smoke_ctx.smoke = true;
    const interest_report smoke = evaluate_interest( smoke_ctx );
    CHECK( smoke.source == interest_source::smoke );
    CHECK_FALSE( smoke.can_latch );
    CHECK( smoke.score < human.score / 4 );
    CHECK( smoke.reason == "smoke_indirect_human_hint" );

    interest_context light_ctx;
    light_ctx.exposed_night_light = true;
    const interest_report light = evaluate_interest( light_ctx );
    CHECK( light.source == interest_source::light );
    CHECK_FALSE( light.can_latch );
    CHECK( light.score >= 50 );
    CHECK( light.score > smoke.score * 3 );

    interest_context terrain_ctx;
    terrain_ctx.forest_or_building_edge = true;
    const interest_report terrain = evaluate_interest( terrain_ctx );
    CHECK( terrain.source == interest_source::terrain );
    CHECK( terrain.score > empty.score );
    CHECK_FALSE( terrain.can_latch );
}

TEST_CASE( "writhing_stalker_latch_is_bounded_and_refresh_requires_evidence",
           "[writhing_stalker][ai]" )
{
    using namespace writhing_stalker;

    interest_context human_ctx;
    human_ctx.recent_human_evidence = true;
    human_ctx.evidence_age_minutes = 3;
    latch_update latched = advance_latch( latch_context{ latch_state{},
                                          evaluate_interest( human_ctx ), 0, 12, false } );

    REQUIRE( latched.state.active );
    CHECK( latched.next == decision::shadow );
    CHECK( latched.state.leash_tiles_remaining == 60 );
    CHECK( latched.reason == "latch_created_by_believable_evidence" );

    latch_update continuing = advance_latch( latch_context{ latched.state,
                              interest_report{}, 12, 20, false } );
    CHECK( continuing.state.active );
    CHECK( continuing.state.age_minutes == 12 );
    CHECK( continuing.reason == "latch_continues_without_refresh" );

    latch_update timed_out = advance_latch( latch_context{ continuing.state,
                                            interest_report{}, 20, 20, false } );
    CHECK_FALSE( timed_out.state.active );
    CHECK( timed_out.next == decision::cooling_off );
    CHECK( timed_out.state.cooldown_minutes > 0 );
    CHECK( timed_out.reason == "latch_timed_out" );

    human_ctx.evidence_age_minutes = 90;
    latch_update stale = advance_latch( latch_context{ latch_state{},
                                        evaluate_interest( human_ctx ), 0, 10, false } );
    CHECK_FALSE( stale.state.active );
    CHECK( stale.next == decision::interested );
    CHECK( stale.reason == "interest_without_latch" );
}

TEST_CASE( "writhing_stalker_approach_prefers_cover_and_holds_at_exposure",
           "[writhing_stalker][ai]" )
{
    using namespace writhing_stalker;

    latch_state latch;
    latch.active = true;
    latch.confidence = 4;
    latch.leash_tiles_remaining = 60;

    approach_context cover_ctx;
    cover_ctx.latch = latch;
    cover_ctx.cover_route_available = true;
    cover_ctx.direct_open_route_available = true;
    const approach_report cover = choose_approach( cover_ctx );
    CHECK( cover.route == approach_class::cover_shadow );
    CHECK( cover.next == decision::shadow );
    CHECK( cover.avoids_direct_line );
    CHECK( cover.reason == "cover_route_preferred" );

    approach_context exposed_ctx;
    exposed_ctx.latch = latch;
    exposed_ctx.cover_route_available = true;
    exposed_ctx.bright_exposure = true;
    const approach_report exposed = choose_approach( exposed_ctx );
    CHECK( exposed.route == approach_class::hold_exposed );
    CHECK( exposed.next == decision::hold );
    CHECK( exposed.avoids_direct_line );
    CHECK( exposed.reason == "bright_exposure_holds_shadow" );

    approach_context forced_ctx;
    forced_ctx.latch = latch;
    forced_ctx.direct_open_route_available = true;
    forced_ctx.forced_no_cover = true;
    const approach_report forced = choose_approach( forced_ctx );
    CHECK( forced.route == approach_class::direct_forced );
    CHECK_FALSE( forced.avoids_direct_line );
    CHECK( forced.reason == "direct_route_only_when_forced_by_map" );
}

TEST_CASE( "writhing_stalker_opportunity_uses_zombies_without_magic_tracking",
           "[writhing_stalker][ai]" )
{
    using namespace writhing_stalker;

    latch_state no_latch;
    opportunity_context zombies_without_latch;
    zombies_without_latch.latch = no_latch;
    zombies_without_latch.zombie_pressure = 6;
    zombies_without_latch.player_bleeding = true;
    const opportunity_report ignored = evaluate_opportunity( zombies_without_latch );
    CHECK( ignored.next == decision::ignore );
    CHECK( ignored.reason == "no_latch_no_strike" );

    latch_state latch;
    latch.active = true;
    latch.confidence = 4;
    latch.leash_tiles_remaining = 60;

    opportunity_context calm_ctx;
    calm_ctx.latch = latch;
    calm_ctx.near_cover_or_clutter = true;
    const opportunity_report calm = evaluate_opportunity( calm_ctx );
    CHECK( calm.next == decision::hold );

    opportunity_context zombie_ctx = calm_ctx;
    zombie_ctx.zombie_pressure = 3;
    const opportunity_report zombie = evaluate_opportunity( zombie_ctx );
    CHECK( zombie.zombie_distraction > calm.zombie_distraction );
    CHECK( zombie.opportunity > calm.opportunity );
    CHECK( zombie.next != decision::strike );

    opportunity_context vulnerable_ctx = zombie_ctx;
    vulnerable_ctx.player_bleeding = true;
    vulnerable_ctx.player_hurt = true;
    vulnerable_ctx.player_low_stamina = true;
    vulnerable_ctx.player_distracted = true;
    const opportunity_report vulnerable = evaluate_opportunity( vulnerable_ctx );
    CHECK( vulnerable.vulnerability > calm.vulnerability );
    CHECK( vulnerable.next == decision::strike );
    CHECK( vulnerable.reason == "vulnerability_window_strike" );
}

TEST_CASE( "writhing_stalker_heavy_same_target_pressure_can_open_in_bright_exposure",
           "[writhing_stalker][ai][pressure]" )
{
    using namespace writhing_stalker;

    latch_state latch;
    latch.active = true;
    opportunity_context ctx;
    ctx.latch = latch;
    ctx.bright_exposure = true;
    ctx.distance_to_target = 2;
    ctx.zombie_pressure = 4;

    const opportunity_report report = evaluate_opportunity( ctx );
    CHECK( report.next == decision::strike );
    CHECK( report.reason == "heavy_zombie_pressure_strike" );
    CHECK( report.zombie_distraction > 0 );

    live_context approach;
    approach.has_believable_local_evidence = true;
    approach.distance_to_target = 6;
    approach.zombie_pressure = 4;
    approach.stalker_in_bright_exposure = true;
    approach.target_has_focus = true;
    approach.cover_route_available = true;
    approach.has_overmap_interest_footing = true;
    approach.overmap_intent = handoff_intent::committed_ambush;
    const live_response committed = evaluate_live_response( approach );
    CHECK( committed.next == decision::shadow );
    CHECK( committed.writeback_intent == handoff_intent::committed_ambush );
    CHECK( committed.persistent_state_required );
    CHECK( committed.reason == "live_heavy_zombie_pressure_approach" );
}

TEST_CASE( "writhing_stalker_pressure_requires_observable_target_directed_intent",
           "[writhing_stalker][ai][pressure]" )
{
    using namespace writhing_stalker;

    CHECK( is_meaningful_pressure( pressure_observation{ true, true, true, true, false } ) );
    CHECK( is_meaningful_pressure( pressure_observation{ true, true, true, false, true } ) );
    // Direct native target plus destination is already current pursuit; a
    // newly spawned observer must not need a second frame of motion/damage.
    CHECK( is_meaningful_pressure( pressure_observation{ true, true, true, false, false } ) );
    CHECK_FALSE( is_meaningful_pressure( pressure_observation{ true, true, false, true, false } ) );
    CHECK_FALSE( is_meaningful_pressure( pressure_observation{ true, false, true, false, true } ) );
    CHECK_FALSE( is_meaningful_pressure( pressure_observation{ false, true, true, true, false } ) );
}

TEST_CASE( "writhing_stalker_linked_melee_miss_binds_resolution_time_observers",
           "[writhing_stalker][ai][pressure][counterpressure][resolution]" )
{
    clear_map_without_vision();
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center{ 65, 65, 0 };
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 5 );
    monster &attacker = spawn_test_monster( mon_zombie.str(), center + point::east );
    attacker.aggro_character = true;
    attacker.set_dest( you.pos_abs() );
    refresh_writhing_stalker_arena( here );

    REQUIRE( stalker.sees( here, attacker ) );
    REQUIRE( stalker.sees( here, you ) );
    const int turn = to_turn<int>( calendar::turn );
    CHECK( you.deal_melee_attack( &attacker, -1000 ) < 0 );
    CHECK( writhing_stalker::has_recent_observed_attack( &stalker, &attacker, &you, turn, 0 ) );
    clear_map_without_vision();
}

TEST_CASE( "writhing_stalker_linked_projectile_miss_is_idempotent_and_visibility_bound",
           "[writhing_stalker][ai][pressure][counterpressure][resolution]" )
{
    clear_map_without_vision();
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center{ 65, 65, 0 };
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    monster &observer = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 6 );
    monster &target = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 4 );
    monster &attacker = spawn_test_monster( mon_zombie.str(), center + point::east * 2 );
    attacker.aggro_character = true;
    attacker.set_dest( target.pos_abs() );
    refresh_writhing_stalker_arena( here );

    REQUIRE( observer.sees( here, attacker ) );
    REQUIRE( observer.sees( here, target ) );
    const int turn = to_turn<int>( calendar::turn );
    dealt_projectile_attack attack{};
    target.deal_projectile_attack( &here, &attacker, attack, 2.0, false );
    CHECK( writhing_stalker::has_recent_observed_attack( &observer, &attacker, &target, turn, 0 ) );

    const writhing_stalker::resolution_id duplicate = writhing_stalker::next_resolution_id();
    writhing_stalker::observe_attack_resolution( &attacker, &target, turn, duplicate );
    writhing_stalker::observe_attack_resolution( &attacker, &target, turn, duplicate );
    CHECK( writhing_stalker::observed_attack_count( &observer, &attacker, &target, duplicate ) == 1 );

    here.ter_set( center + point::east * 5, ter_id( "t_wall" ) );
    refresh_writhing_stalker_arena( here );
    dealt_projectile_attack hidden_attack{};
    target.deal_projectile_attack( &here, &attacker, hidden_attack, 2.0, false );
    const writhing_stalker::resolution_id hidden = hidden_attack.writhing_stalker_resolution;
    here.ter_set( center + point::east * 5, ter_id( "t_floor" ) );
    refresh_writhing_stalker_arena( here );
    CHECK( writhing_stalker::observed_attack_count( &observer, &attacker, &target, hidden ) == 0 );
    clear_map_without_vision();
}

TEST_CASE( "writhing_stalker_observed_attack_identity_survives_deletion_and_reuse",
           "[writhing_stalker][ai][pressure][resolution]" )
{
    monster observer( mon_writhing_stalker );
    monster target( mon_zombie );
    std::optional<monster> attacker;
    attacker.emplace( mon_zombie );
    const writhing_stalker::resolution_id resolution = writhing_stalker::next_resolution_id();
    writhing_stalker::record_observed_attack( &observer, &*attacker, &target, 100, resolution );
    CHECK( writhing_stalker::observed_attack_count( &observer, &*attacker, &target, resolution ) == 1 );
    attacker.reset();
    attacker.emplace( mon_zombie );
    CHECK_FALSE( writhing_stalker::has_recent_observed_attack( &observer, &*attacker, &target, 100,
                 3 ) );
}

TEST_CASE( "writhing_stalker_creature_copy_gets_fresh_observation_identity",
           "[writhing_stalker][ai][pressure][resolution][identity]" )
{
    monster observer( mon_writhing_stalker );
    monster attacker( mon_zombie );
    monster target( mon_zombie );
    const writhing_stalker::resolution_id resolution = writhing_stalker::next_resolution_id();

    writhing_stalker::record_observed_attack( &observer, &attacker, &target, 100, resolution );
    monster copy = attacker;

    CHECK( writhing_stalker::observed_attack_count( &observer, &attacker, &target, resolution ) == 1 );
    CHECK_FALSE( writhing_stalker::has_recent_observed_attack( &observer, &copy, &target, 100, 0 ) );
}

TEST_CASE( "writhing_stalker_creature_assignment_preserves_receiver_observation_identity",
           "[writhing_stalker][ai][pressure][resolution][identity]" )
{
    monster observer( mon_writhing_stalker );
    monster source( mon_zombie );
    monster receiver( mon_zombie );
    monster target( mon_zombie );
    const writhing_stalker::resolution_id resolution = writhing_stalker::next_resolution_id();

    writhing_stalker::record_observed_attack( &observer, &receiver, &target, 100, resolution );
    receiver = source;

    CHECK( writhing_stalker::has_recent_observed_attack( &observer, &receiver, &target, 100, 0 ) );
    CHECK_FALSE( writhing_stalker::has_recent_observed_attack( &observer, &source, &target, 100, 0 ) );
}

TEST_CASE( "writhing_stalker_handoff_does_not_spend_attack_budget_while_walking",
           "[writhing_stalker][ai][accounting]" )
{
    using namespace writhing_stalker;

    handoff_memory incoming;
    incoming.strike_budget_spent = 0;
    live_response planned;
    planned.next = decision::strike;
    planned.burst_limit = 2;
    planned.reason = "planned_but_not_resolved";

    const handoff_memory out = writeback_handoff_memory( incoming, planned );
    CHECK( out.strike_budget_spent == 0 );
    CHECK( out.reason == planned.reason );
}

TEST_CASE( "writhing_stalker_quiet_side_cutoff_prefers_the_side_zombies_are_not",
           "[writhing_stalker][ai][zombie_shadow]" )
{
    using namespace writhing_stalker;

    const std::vector<relative_point> east_zombies = { { 3, 0, 1 }, { 4, 1, 1 }, { 2, -1, 1 } };
    const std::vector<quiet_candidate> candidates = {
        { -3, 0, 8, true, false, true, true, false, 0 },
        { 3, 0, 3, true, false, true, true, false, 0 }
    };

    const quiet_candidate_report chosen = choose_quiet_side_cutoff( east_zombies, candidates );
    REQUIRE( chosen.has_candidate );
    CHECK( chosen.pressure.has_dominant_pressure );
    CHECK( chosen.pressure.pressure_x > 0 );
    CHECK( chosen.pressure.quiet_x < 0 );
    CHECK( chosen.chosen.rel_x < 0 );
    CHECK( chosen.quiet_alignment > 0 );
    CHECK( chosen.reason == "quiet_side_cutoff_preferred" );
}

TEST_CASE( "writhing_stalker_quiet_side_cutoff_avoids_fake_precision_when_pressure_splits",
           "[writhing_stalker][ai][zombie_shadow]" )
{
    using namespace writhing_stalker;

    const std::vector<relative_point> split_zombies = { { 3, 0, 1 }, { -3, 0, 1 } };
    const std::vector<quiet_candidate> candidates = {
        { -3, 0, 6, true, false, false, false, false, 0 },
        { 0, -3, 6, true, false, true, true, false, 0 }
    };

    const quiet_candidate_report chosen = choose_quiet_side_cutoff( split_zombies, candidates );
    REQUIRE( chosen.has_candidate );
    CHECK_FALSE( chosen.pressure.has_dominant_pressure );
    CHECK( chosen.pressure.ambiguous_pressure );
    CHECK( chosen.quiet_alignment == 0 );
    CHECK( chosen.chosen.rel_y < 0 );
    CHECK( chosen.reason == "ambiguous_pressure_no_precise_quiet_side" );
}

TEST_CASE( "writhing_stalker_quiet_side_cutoff_can_follow_retreat_route_when_it_matches_pressure",
           "[writhing_stalker][ai][zombie_shadow]" )
{
    using namespace writhing_stalker;

    const std::vector<relative_point> north_zombies = { { 0, -3, 1 }, { 1, -2, 1 } };
    const std::vector<quiet_candidate> candidates = {
        { 0, 3, 3, true, false, true, true, false, 0 },
        { 1, 3, 6, true, false, true, true, false, 4 }
    };

    const quiet_candidate_report chosen = choose_quiet_side_cutoff( north_zombies, candidates );
    REQUIRE( chosen.has_candidate );
    CHECK( chosen.pressure.quiet_y > 0 );
    CHECK( chosen.chosen.rel_y > 0 );
    CHECK( chosen.chosen.retreat_alignment > 0 );
    CHECK( chosen.reason == "quiet_side_cutoff_preferred" );
}

TEST_CASE( "writhing_stalker_confidence_gates_zombie_pressure_and_suppresses_cutoff_with_light_focus",
           "[writhing_stalker][ai][zombie_shadow]" )
{
    using namespace writhing_stalker;

    confidence_context no_evidence;
    no_evidence.zombie_pressure = 5;
    no_evidence.quiet_side_cutoff_available = true;
    const confidence_report ignored = evaluate_confidence( no_evidence );
    CHECK_FALSE( ignored.pressure_allowed );
    CHECK_FALSE( ignored.cutoff_allowed );
    CHECK( ignored.zombie_pressure == 0 );
    CHECK( ignored.quiet_side_cutoff == 0 );
    CHECK( ignored.reason == "no_evidence_or_interest_pressure_ignored" );

    confidence_context evidence = no_evidence;
    evidence.has_believable_local_evidence = true;
    const confidence_report pressure = evaluate_confidence( evidence );
    CHECK( pressure.pressure_allowed );
    CHECK( pressure.cutoff_allowed );
    CHECK( pressure.evidence > 0 );
    CHECK( pressure.zombie_pressure > 0 );
    CHECK( pressure.quiet_side_cutoff > 0 );
    CHECK( pressure.total > ignored.total );
    CHECK( pressure.reason == "zombie_shadow_quiet_cutoff_confidence" );

    confidence_context interest = no_evidence;
    interest.has_overmap_interest_footing = true;
    const confidence_report overmap_pressure = evaluate_confidence( interest );
    CHECK( overmap_pressure.pressure_allowed );
    CHECK( overmap_pressure.zombie_pressure > 0 );
    CHECK( overmap_pressure.quiet_side_cutoff > 0 );

    confidence_context countered = evidence;
    countered.target_in_bright_exposure = true;
    countered.stalker_in_bright_exposure = true;
    countered.target_has_focus = true;
    countered.open_exposure = true;
    const confidence_report suppressed = evaluate_confidence( countered );
    CHECK( suppressed.counterpressure > 0 );
    CHECK_FALSE( suppressed.cutoff_allowed );
    CHECK( suppressed.quiet_side_cutoff == 0 );
    CHECK( suppressed.total < pressure.total );
    CHECK( suppressed.reason == "counterpressure_suppresses_cutoff" );
}


TEST_CASE( "writhing_stalker_threat_handoff_overmatched_allies_retreats_to_stalking_distance",
           "[writhing_stalker][ai][handoff]" )
{
    using namespace writhing_stalker;

    const threat_report calm = evaluate_threat( threat_context{ true, true, true, 1, 0, 0 } );
    CHECK_FALSE( calm.overmatched );
    CHECK( calm.reason == "threat_not_overmatched" );

    const threat_report zombies_break_the_firing_squad = evaluate_threat( threat_context{ true, true,
            true, 3, 3, 0 } );
    CHECK_FALSE( zombies_break_the_firing_squad.overmatched );

    const threat_report overmatched = evaluate_threat( threat_context{ true, true, true, 3, 0, 0 } );
    REQUIRE( overmatched.overmatched );
    CHECK( overmatched.next == decision::withdraw );
    CHECK( overmatched.intent == handoff_intent::overmatched_stalk );
    CHECK( overmatched.stalk_distance_omt == 5 );
    CHECK( overmatched.threat_memory >= 3 );
    CHECK( overmatched.avoid_sight_tiles );
    CHECK( overmatched.reason == "high_threat_allied_light_retreat_stalk" );

    live_context ctx = pattern_base_context();
    ctx.distance_to_target = 6;
    ctx.target_in_bright_exposure = true;
    ctx.target_has_focus = true;
    ctx.allied_support_nearby = 3;
    const live_response response = evaluate_live_response( ctx );
    CHECK( response.next == decision::withdraw );
    CHECK( response.writeback_intent == handoff_intent::overmatched_stalk );
    CHECK( response.overmap_stalk_distance_omt == 5 );
    CHECK( response.persistent_state_required );
    CHECK( response.reason == "live_high_threat_allied_light_retreat_stalk" );
}

TEST_CASE( "writhing_stalker_sighted_stalk_uses_cover_or_withdraws_to_far_watch",
           "[writhing_stalker][ai][live][sight]" )
{
    using namespace writhing_stalker;

    live_context covered = pattern_base_context();
    covered.distance_to_target = 6;
    covered.target_has_focus = true;
    covered.cover_route_available = true;
    const live_response shadow = evaluate_live_response( covered );
    CHECK( shadow.next == decision::shadow );
    CHECK( shadow.route == approach_class::cover_shadow );
    CHECK( shadow.writeback_intent == handoff_intent::shadowing );
    CHECK( shadow.overmap_stalk_distance_omt == 5 );
    CHECK( shadow.persistent_state_required );
    CHECK( shadow.reason == "live_sighted_stalker_breaks_los_shadow" );

    live_context exposed = covered;
    exposed.cover_route_available = false;
    exposed.edge_route_available = false;
    exposed.direct_open_route_available = true;
    exposed.forced_no_cover = false;
    const live_response withdrawal = evaluate_live_response( exposed );
    CHECK( withdrawal.next == decision::withdraw );
    CHECK( withdrawal.writeback_intent == handoff_intent::spent_disengage );
    CHECK( withdrawal.overmap_stalk_distance_omt == 5 );
    CHECK( withdrawal.persistent_state_required );
    CHECK( withdrawal.reason == "live_sighted_stalker_no_cover_withdraw" );
}

TEST_CASE( "writhing_stalker_night_reachable_anti_gnome_resolves_loiter",
           "[writhing_stalker][ai][handoff]" )
{
    using namespace writhing_stalker;

    const live_response not_yet = resolve_anti_gnome( anti_gnome_context{ true, true, true, false,
                                  2, 1 } );
    CHECK_FALSE( not_yet.anti_gnome_triggered );

    const live_response strike = resolve_anti_gnome( anti_gnome_context{ true, true, true, false,
                                 2, 2 } );
    CHECK( strike.anti_gnome_triggered );
    CHECK( strike.next == decision::strike );
    CHECK( strike.writeback_intent == handoff_intent::committed_ambush );
    CHECK( strike.reason == "anti_gnome_night_reachable_probe_strike" );

    live_context gnome = pattern_base_context();
    gnome.distance_to_target = 2;
    gnome.night_outside_reachable_target = true;
    gnome.direct_open_route_available = true;
    gnome.bad_position_loiter_turns = 2;
    const live_response response = evaluate_live_response( gnome );
    CHECK( response.next == decision::strike );
    CHECK( response.anti_gnome_triggered );
    CHECK( response.reason == "live_anti_gnome_night_reachable_probe_strike" );
}

TEST_CASE( "writhing_stalker_zombie_distraction_enables_dark_square_strike_without_omniscience",
           "[writhing_stalker][ai][handoff][zombie_shadow]" )
{
    using namespace writhing_stalker;

    live_context no_evidence = pattern_base_context();
    no_evidence.has_believable_local_evidence = false;
    no_evidence.distance_to_target = 2;
    no_evidence.zombie_pressure = 3;
    no_evidence.near_cover_or_clutter = true;
    const live_response ignored = evaluate_live_response( no_evidence );
    CHECK( ignored.next == decision::ignore );
    CHECK( ignored.reason == "live_no_believable_evidence" );

    live_context distracted = no_evidence;
    distracted.has_believable_local_evidence = true;
    const live_response strike = evaluate_live_response( distracted );
    CHECK( strike.next == decision::strike );
    CHECK( strike.writeback_intent == handoff_intent::committed_ambush );
    CHECK( strike.reason == "live_zombie_distraction_dark_square_strike" );

    live_context probe = distracted;
    probe.distance_to_target = 6;
    probe.cover_route_available = true;
    const live_response shadow = evaluate_live_response( probe );
    CHECK( shadow.next == decision::shadow );
    CHECK( shadow.writeback_intent == handoff_intent::opportunity_probe );
    CHECK( shadow.reason == "live_zombie_distraction_dark_square_probe" );
}

TEST_CASE( "writhing_stalker_threat_drop_strikes_on_low_zombie_pressure_then_boots_out",
           "[writhing_stalker][ai][live][threat]" )
{
    using namespace writhing_stalker;

    live_context dropped = pattern_base_context();
    dropped.distance_to_target = 2;
    dropped.zombie_pressure = 2;
    const live_response strike = evaluate_live_response( dropped );
    CHECK( strike.next == decision::strike );
    CHECK( strike.writeback_intent == handoff_intent::committed_ambush );
    CHECK( strike.reason == "live_zombie_distraction_dark_square_strike" );

    live_context spent = dropped;
    spent.burst_strikes = strike.burst_limit;
    const live_response boot_out = evaluate_live_response( spent );
    CHECK( boot_out.next == decision::withdraw );
    CHECK( boot_out.writeback_intent == handoff_intent::spent_disengage );
    CHECK( boot_out.overmap_stalk_distance_omt == 5 );
    CHECK( boot_out.persistent_state_required );
    CHECK( boot_out.reason == "live_burst_limit_reached_withdraw" );
}

TEST_CASE( "writhing_stalker_handoff_writeback_preserves_spent_cooldown_and_threat_memory",
           "[writhing_stalker][ai][handoff]" )
{
    using namespace writhing_stalker;

    handoff_memory incoming;
    incoming.intent = handoff_intent::overmatched_stalk;
    incoming.strike_budget_spent = 1;
    incoming.cooldown_minutes = 2;
    incoming.threat_memory = 3;
    incoming.stalk_distance_omt = 5;

    live_context ctx = pattern_base_context();
    ctx.distance_to_target = 2;
    ctx.player_bleeding = true;
    ctx.player_hurt = true;
    ctx.player_low_stamina = true;
    ctx.player_distracted = true;
    ctx.overmap_intent = incoming.intent;
    ctx.has_overmap_interest_footing = true;
    ctx.overmap_stalk_distance_omt = incoming.stalk_distance_omt;
    ctx.overmap_cooldown_minutes = incoming.cooldown_minutes;
    ctx.overmap_threat_memory = incoming.threat_memory;
    const live_response response = evaluate_live_response( ctx );

    const handoff_memory out = writeback_handoff_memory( incoming, response );
    CHECK( out.reason == response.reason );
    CHECK( out.cooldown_minutes >= incoming.cooldown_minutes );
    CHECK( out.threat_memory >= incoming.threat_memory );
    CHECK( out.stalk_distance_omt == 5 );
    CHECK( out.strike_budget_spent >= incoming.strike_budget_spent );
}

TEST_CASE( "writhing_stalker_withdrawal_and_cooldown_block_repeat_spam",
           "[writhing_stalker][ai]" )
{
    using namespace writhing_stalker;

    latch_state latch;
    latch.active = true;
    latch.confidence = 4;
    latch.leash_tiles_remaining = 60;

    opportunity_context exposed_ctx;
    exposed_ctx.latch = latch;
    exposed_ctx.bright_exposure = true;
    exposed_ctx.player_focused = true;
    const opportunity_report exposed = evaluate_opportunity( exposed_ctx );
    CHECK( exposed.next == decision::withdraw );
    CHECK( exposed.reason == "exposed_and_focused_withdraw" );

    // Reciprocal daylight sight still gives the stalker a reason to withdraw
    // in a quiet scene.  Four observed, target-directed zombies are a real
    // local distraction, however, and preserve the close-range opportunity
    // instead of consuming it before the heavy-pressure rule can evaluate.
    opportunity_context pressured_exposed_ctx = exposed_ctx;
    pressured_exposed_ctx.zombie_pressure = 4;
    pressured_exposed_ctx.distance_to_target = 2;
    const opportunity_report pressured_exposed = evaluate_opportunity( pressured_exposed_ctx );
    CHECK( pressured_exposed.next == decision::strike );
    CHECK( pressured_exposed.reason == "heavy_zombie_pressure_strike" );

    opportunity_context hurt_ctx;
    hurt_ctx.latch = latch;
    hurt_ctx.player_bleeding = true;
    hurt_ctx.player_hurt = true;
    hurt_ctx.player_low_stamina = true;
    hurt_ctx.player_distracted = true;
    hurt_ctx.stalker_hurt = true;
    const opportunity_report hurt = evaluate_opportunity( hurt_ctx );
    CHECK( hurt.next == decision::withdraw );
    CHECK( hurt.reason == "stalker_hurt_withdraw" );

    latch.cooldown_minutes = 8;
    opportunity_context cooldown_ctx;
    cooldown_ctx.latch = latch;
    cooldown_ctx.player_bleeding = true;
    cooldown_ctx.player_hurt = true;
    cooldown_ctx.player_low_stamina = true;
    cooldown_ctx.player_distracted = true;
    const opportunity_report cooldown = evaluate_opportunity( cooldown_ctx );
    CHECK( cooldown.next == decision::cooling_off );
    CHECK( cooldown.reason == "cooldown_blocks_repeat_strike" );
}


TEST_CASE( "writhing_stalker_threat_distraction_state_handles_overmatch_and_anti_gnome",
           "[writhing_stalker][ai][threat]" )
{
    using namespace writhing_stalker;

    threat_context overmatched;
    overmatched.close_overmap_pressure = true;
    overmatched.allied_support_nearby = 3;
    overmatched.daylight_or_bright = true;
    overmatched.strong_visibility = true;
    overmatched.valid_evidence_or_path = true;
    const threat_report retreat = evaluate_threat_state( overmatched );
    CHECK( retreat.state == threat_state::overmatched_retreat );
    CHECK( retreat.next == decision::withdraw );
    CHECK( retreat.stalking_distance_omt == 5 );
    CHECK( retreat.avoids_sight_tiles );
    CHECK( retreat.reason == "high_threat_allied_light_retreat_stalk" );

    threat_context zombie_distraction = overmatched;
    zombie_distraction.has_distraction = true;
    zombie_distraction.night_or_dark = true;
    zombie_distraction.daylight_or_bright = false;
    const threat_report opportunity = evaluate_threat_state( zombie_distraction );
    CHECK( opportunity.next != decision::withdraw );
    CHECK( opportunity.opportunity_score > retreat.opportunity_score );

    threat_context window_gnome;
    window_gnome.night_or_dark = true;
    window_gnome.outside_reachable_player = true;
    window_gnome.valid_evidence_or_path = true;
    window_gnome.bad_position_loiter_turns = 3;
    const threat_report anti_gnome = evaluate_threat_state( window_gnome );
    CHECK( anti_gnome.state == threat_state::opportunity_probe );
    CHECK( anti_gnome.next == decision::shadow );
    CHECK( anti_gnome.anti_gnome_fired );
    CHECK( anti_gnome.reason == "anti_gnome_dark_reposition_probe" );
}

TEST_CASE( "writhing_stalker_live_response_retreats_from_daylight_three_allies_and_breaks_dark_loiter",
           "[writhing_stalker][ai][live][threat]" )
{
    using namespace writhing_stalker;

    live_context overmatched = pattern_base_context();
    overmatched.distance_to_target = 4;
    overmatched.target_in_bright_exposure = true;
    overmatched.target_has_focus = true;
    overmatched.allied_support_nearby = 3;
    overmatched.direct_open_route_available = true;
    const live_response retreat = evaluate_live_response( overmatched );
    CHECK( retreat.next == decision::withdraw );
    CHECK( retreat.retreat_distance >= 14 );
    CHECK( retreat.persistent_state_required );
    CHECK( retreat.writeback_intent == handoff_intent::overmatched_stalk );
    CHECK( retreat.overmap_stalk_distance_omt == 5 );
    CHECK( retreat.reason == "live_high_threat_allied_light_retreat_stalk" );

    live_context dark_window = pattern_base_context();
    dark_window.distance_to_target = 2;
    dark_window.cover_route_available = false;
    dark_window.edge_route_available = true;
    dark_window.direct_open_route_available = true;
    dark_window.forced_no_cover = false;
    dark_window.near_cover_or_clutter = false;
    dark_window.night_outside_reachable_target = true;
    dark_window.bad_position_loiter_turns = 3;
    const live_response anti_gnome = evaluate_live_response( dark_window );
    CHECK( anti_gnome.next == decision::strike );
    CHECK( anti_gnome.persistent_state_required );
    CHECK( anti_gnome.anti_gnome_triggered );
    CHECK( anti_gnome.reason == "live_anti_gnome_night_reachable_probe_strike" );
}

TEST_CASE( "writhing_stalker_live_response_requires_evidence_and_uses_cooldown",
           "[writhing_stalker][ai][live]" )
{
    using namespace writhing_stalker;

    live_context no_evidence;
    no_evidence.distance_to_target = 8;
    no_evidence.player_bleeding = true;
    no_evidence.player_hurt = true;
    no_evidence.player_low_stamina = true;
    no_evidence.player_distracted = true;
    const live_response ignored = evaluate_live_response( no_evidence );
    CHECK( ignored.next == decision::ignore );
    CHECK( ignored.reason == "live_no_believable_evidence" );

    live_context shadow_ctx;
    shadow_ctx.has_believable_local_evidence = true;
    shadow_ctx.distance_to_target = 8;
    shadow_ctx.cover_route_available = true;
    shadow_ctx.near_cover_or_clutter = true;
    const live_response shadow = evaluate_live_response( shadow_ctx );
    CHECK( shadow.next == decision::shadow );
    CHECK( shadow.route == approach_class::cover_shadow );
    CHECK( shadow.reason == "live_shadowing_believable_evidence" );

    live_context far_strike_window_ctx = shadow_ctx;
    far_strike_window_ctx.player_bleeding = true;
    far_strike_window_ctx.player_hurt = true;
    far_strike_window_ctx.player_low_stamina = true;
    far_strike_window_ctx.player_distracted = true;
    far_strike_window_ctx.zombie_pressure = 2;
    const live_response far_strike_window = evaluate_live_response( far_strike_window_ctx );
    CHECK( far_strike_window.next == decision::shadow );
    CHECK( far_strike_window.reason == "live_shadowing_before_strike_window" );

    live_context strike_ctx = far_strike_window_ctx;
    strike_ctx.distance_to_target = 2;
    const live_response strike = evaluate_live_response( strike_ctx );
    CHECK( strike.next == decision::strike );
    CHECK( strike.reason == "live_vulnerability_window_strike" );

    live_context cooldown_ctx = strike_ctx;
    cooldown_ctx.on_cooldown = true;
    const live_response cooldown = evaluate_live_response( cooldown_ctx );
    CHECK( cooldown.next == decision::cooling_off );
    CHECK( cooldown.reason == "live_latch_cooldown_blocks_relatched_pressure" );
}

TEST_CASE( "writhing_stalker_burst_limit_is_shaped_by_stress_and_counterpressure",
           "[writhing_stalker][ai][pattern]" )
{
    using namespace writhing_stalker;

    live_context base = pattern_base_context();
    base.distance_to_target = 2;
    base.near_cover_or_clutter = true;

    live_context cautious = base;
    cautious.player_bleeding = true;
    cautious.player_hurt = true;
    cautious.player_low_stamina = true;
    cautious.target_in_bright_exposure = true;
    cautious.target_has_focus = true;
    cautious.allied_support_nearby = 2;
    const live_response cautious_first = evaluate_live_response( cautious );
    CHECK( cautious_first.next == decision::withdraw );
    CHECK( cautious_first.burst_limit == 1 );
    CHECK( cautious_first.retreat_distance >= 11 );

    live_context pressure = base;
    pressure.player_bleeding = true;
    pressure.player_hurt = true;
    pressure.player_low_stamina = true;
    pressure.player_distracted = true;
    pressure.player_noisy = true;
    pressure.zombie_pressure = 4;
    const live_response pressure_first = evaluate_live_response( pressure );
    CHECK( pressure_first.next == decision::strike );
    CHECK( pressure_first.burst_limit == 4 );

    live_context post_burst = pressure;
    post_burst.burst_strikes = pressure_first.burst_limit;
    const live_response retreat = evaluate_live_response( post_burst );
    CHECK( retreat.next == decision::withdraw );
    CHECK( retreat.reason == "live_burst_limit_reached_withdraw" );
    CHECK( retreat.retreat_distance >= 8 );
}

TEST_CASE( "writhing_stalker_live_plan_retreats_about_eight_tiles_after_burst",
           "[writhing_stalker][monster][map][pattern]" )
{
    clear_map_without_vision();
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center{ 65, 65, 0 };
    const tripoint_bub_ms stalker_start = center + point::east * 2;
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 0_hours );
    refresh_writhing_stalker_arena( here );

    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), stalker_start );
    stalker.anger = 100;
    stalker.aggro_character = true;
    stalker.writhing_stalker_state().attempts_spent = 2;

    REQUIRE( stalker.sees( here, you ) );
    stalker.plan();

    const auto &withdrawal_state = stalker.writhing_stalker_state();
    REQUIRE( withdrawal_state.phase == writhing_stalker::lifecycle_phase::retreating );
    const int withdrawal_entered_turn = withdrawal_state.phase_entered_turn;
    const int withdrawal_cooldown_until = withdrawal_state.cooldown_until_turn;
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::cooldown );
    const int cooling_entered_turn = stalker.writhing_stalker_state().phase_entered_turn;
    const int cooling_until_turn = stalker.writhing_stalker_state().cooldown_until_turn;
    CHECK( cooling_entered_turn >= withdrawal_entered_turn );
    CHECK( cooling_until_turn >= withdrawal_cooldown_until );
    stalker.plan();
    CHECK( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::cooldown );
    CHECK( stalker.writhing_stalker_state().phase_entered_turn == cooling_entered_turn );
    CHECK( stalker.writhing_stalker_state().cooldown_until_turn == cooling_until_turn );

    const tripoint_bub_ms dest = here.get_bub( stalker.get_dest() );
    INFO( "retreat destination=" << dest.to_string() << " player=" << you.pos_bub().to_string() );
    CHECK( rl_dist( dest, you.pos_bub() ) >= 8 );
    CHECK( dest.x() > you.pos_bub().x() );
    CHECK_FALSE( dest == you.pos_bub() );
    clear_map_without_vision();
}

TEST_CASE( "writhing_stalker_pattern_helper_covers_fair_dread_baselines",
           "[writhing_stalker][ai][pattern]" )
{
    using namespace writhing_stalker;

    live_context no_evidence = pattern_base_context();
    no_evidence.has_believable_local_evidence = false;
    no_evidence.distance_to_target = 2;
    no_evidence.player_bleeding = true;
    no_evidence.player_hurt = true;
    no_evidence.player_low_stamina = true;
    no_evidence.player_distracted = true;
    no_evidence.zombie_pressure = 4;
    const live_response no_magic = evaluate_live_response( no_evidence );
    CHECK( no_magic.next == decision::ignore );
    CHECK( no_magic.reason == "live_no_believable_evidence" );

    interest_context weak_evidence;
    weak_evidence.recent_human_evidence = true;
    weak_evidence.evidence_age_minutes = 25;
    latch_update weak = advance_latch( latch_context{ latch_state{}, evaluate_interest( weak_evidence ),
                                       0, 8, false } );
    REQUIRE( weak.state.active );
    weak = advance_latch( latch_context{ weak.state, interest_report{}, 31, 8, false } );
    CHECK_FALSE( weak.state.active );
    CHECK( weak.next == decision::cooling_off );
    CHECK( weak.reason == "latch_timed_out" );

    live_context cover = pattern_base_context();
    cover.distance_to_target = 7;
    const live_response covered_route = evaluate_live_response( cover );
    CHECK( covered_route.next == decision::shadow );
    CHECK( covered_route.route == approach_class::cover_shadow );
    CHECK( covered_route.reason == "live_shadowing_believable_evidence" );

    live_context exposed = cover;
    exposed.stalker_in_bright_exposure = true;
    exposed.target_has_focus = true;
    const live_response counterplay = evaluate_live_response( exposed );
    CHECK( counterplay.next == decision::withdraw );
    CHECK( counterplay.reason == "live_exposed_and_focused_withdraw" );

    live_context zombie_only = pattern_base_context();
    zombie_only.distance_to_target = 2;
    zombie_only.zombie_pressure = 4;
    const live_response distraction_without_vulnerability = evaluate_live_response( zombie_only );
    CHECK( distraction_without_vulnerability.next == decision::strike );
    CHECK( distraction_without_vulnerability.reason == "live_zombie_distraction_dark_square_strike" );

    live_context vulnerable = zombie_only;
    vulnerable.player_bleeding = true;
    vulnerable.player_hurt = true;
    vulnerable.player_low_stamina = true;
    vulnerable.player_distracted = true;
    const live_response strike_window = evaluate_live_response( vulnerable );
    CHECK( strike_window.next == decision::strike );
    CHECK( strike_window.reason == "live_vulnerability_window_strike" );
}

TEST_CASE( "writhing_stalker_persistent_state_round_trips_and_advances_once",
           "[writhing_stalker][persistence]" )
{
    using namespace writhing_stalker;

    persistent_state original;
    original.phase = lifecycle_phase::cooldown;
    original.evidence_target = 42;
    original.evidence_turn = 17;
    original.last_observed_position = tripoint_abs_ms( 10, 11, 0 );
    original.has_last_observed_position = true;
    original.committed_waypoint = tripoint_abs_ms( 12, 13, 0 );
    original.has_committed_waypoint = true;
    original.retreat_waypoint = tripoint_abs_ms( 8, 9, 0 );
    original.has_retreat_waypoint = true;
    original.attempts_spent = 3;
    original.attempt_sequence = 9;
    original.phase_entered_turn = 20;
    original.cooldown_until_turn = 30;
    original.last_progress_turn = 25;
    original.last_advanced_turn = 25;
    original.remaining_route_progress = 6;
    original.search_until_turn = -1;

    std::ostringstream serialized;
    {
        JsonOut json( serialized );
        original.serialize( json );
    }
    persistent_state restored;
    restored.deserialize( json_loader::from_string( serialized.str() ) );
    CHECK( restored.phase == lifecycle_phase::cooldown );
    CHECK( restored.evidence_target == 42 );
    CHECK( restored.evidence_turn == 17 );
    CHECK( restored.last_observed_position == tripoint_abs_ms( 10, 11, 0 ) );
    CHECK( restored.committed_waypoint == tripoint_abs_ms( 12, 13, 0 ) );
    CHECK( restored.retreat_waypoint == tripoint_abs_ms( 8, 9, 0 ) );
    CHECK( restored.attempts_spent == 3 );
    CHECK( restored.attempt_sequence == 9 );
    CHECK( restored.cooldown_active( 29 ) );

    restored.advance_to( 30 );
    CHECK( restored.phase == lifecycle_phase::searching );
    CHECK( restored.search_until_turn == 50 );
    const int attempts_after_expiry = restored.attempts_spent;
    restored.advance_to( 30 );
    CHECK( restored.attempts_spent == attempts_after_expiry );
    restored.advance_to( 50 );
    CHECK( restored.phase == lifecycle_phase::idle );
    CHECK( restored.attempts_spent == 0 );
    CHECK_FALSE( restored.has_committed_waypoint );
    CHECK_FALSE( restored.has_retreat_waypoint );
}

TEST_CASE( "writhing_stalker_no_target_plan_retains_committed_waypoint",
           "[writhing_stalker][monster][routing]" )
{
    clear_map_without_vision();
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center{ 65, 65, 0 };
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    here.ter_set( center + point::east, ter_id( "t_wall" ) );
    refresh_writhing_stalker_arena( here );

    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 2 );
    auto &state = stalker.writhing_stalker_state();
    stalker.anger = 0;
    stalker.aggro_character = false;
    const bool native_seen = stalker.sees( here, you );
    CAPTURE( stalker.pos_abs(), you.pos_abs(), here.ter( center + point::east ).id().str(),
             here.get_cache_ref( 0 ).seen_cache[stalker.pos_bub().x()][stalker.pos_bub().y()],
             here.get_cache_ref( 0 ).vision_transparency_cache[center.x() + 1][center.y()],
             stalker.sees_without_clairvoyance_physical( here, you ) );
    CAPTURE( trigdist, here.supports_zlevels(), you.clairvoyance(),
             get_avatar().get_active_moncams().size(),
             here.get_cache_ref( 0 ).no_floor_gaps,
             here.get_cache_ref( 1 ).floor_cache[center.x()][center.y()],
             here.get_cache_ref( -1 ).floor_cache[center.x()][center.y()] );
    REQUIRE_FALSE( native_seen );
    state.phase = writhing_stalker::lifecycle_phase::shadowing;
    state.committed_waypoint = stalker.pos_abs() + point::east * 3;
    state.has_committed_waypoint = true;
    stalker.plan();

    CHECK( here.get_bub( stalker.get_dest() ) == here.get_bub( state.committed_waypoint ) );

    // Reaching the remembered area starts bounded search instead of holding the
    // same destination forever, and repeated calls at one turn do not refresh it.
    state.phase = writhing_stalker::lifecycle_phase::shadowing;
    state.committed_waypoint = stalker.pos_abs();
    state.has_committed_waypoint = true;
    stalker.plan();
    REQUIRE( state.phase == writhing_stalker::lifecycle_phase::searching );
    const int search_deadline = state.search_until_turn;
    CHECK_FALSE( stalker.has_dest() );
    stalker.plan();
    CHECK( state.search_until_turn == search_deadline );

    // A route that makes no progress for the stall window must not be written
    // back as the same stale destination after entering bounded search.
    state.phase = writhing_stalker::lifecycle_phase::shadowing;
    state.committed_waypoint = stalker.pos_abs() + point::east * 3;
    state.has_committed_waypoint = true;
    state.last_observed_position = state.committed_waypoint;
    state.has_last_observed_position = true;
    state.remaining_route_progress = 3;
    state.last_progress_turn = 0;
    stalker.unset_dest();
    set_time( calendar::turn_zero + 51_turns );
    stalker.plan();
    CHECK( state.phase == writhing_stalker::lifecycle_phase::searching );
    CHECK_FALSE( stalker.has_dest() );

    // If a distinct remembered observation remains reachable, the stalled
    // route may replan to it, but must reset progress to the new waypoint.
    state.phase = writhing_stalker::lifecycle_phase::shadowing;
    state.committed_waypoint = stalker.pos_abs() + point::east * 3;
    state.has_committed_waypoint = true;
    state.last_observed_position = stalker.pos_abs() + point::north * 3;
    state.has_last_observed_position = true;
    state.remaining_route_progress = 3;
    state.last_progress_turn = 0;
    set_time( calendar::turn_zero + 102_turns );
    stalker.plan();
    CHECK( here.get_bub( stalker.get_dest() ) == here.get_bub( state.last_observed_position ) );
    CHECK( state.remaining_route_progress == 3 );
    CHECK( state.last_progress_turn == 102 );

    set_time( calendar::turn_zero + 122_turns );
    stalker.plan();
    CHECK( state.phase == writhing_stalker::lifecycle_phase::idle );
    clear_map_without_vision();
}

TEST_CASE( "writhing_stalker_hidden_target_cannot_refresh_observation_or_route",
           "[writhing_stalker][monster][routing][evidence]" )
{
    clear_map_without_vision();
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center{ 65, 65, 0 };
    const tripoint_bub_ms stalker_start = center + point::east * 2;
    const tripoint_bub_ms hidden_position = center + point::south * 3;
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    refresh_writhing_stalker_arena( here );

    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), stalker_start );
    stalker.anger = 100;
    stalker.aggro_character = true;
    REQUIRE( stalker.sees( here, you ) );
    stalker.plan();

    auto &state = stalker.writhing_stalker_state();
    REQUIRE( state.has_last_observed_position );
    const tripoint_abs_ms last_seen = state.last_observed_position;
    const int observation_turn = state.evidence_turn;
    REQUIRE( last_seen == you.pos_abs() );

    // Move the player behind a new wall. The stalker's native plan no longer
    // has a visible target, but this simulates the stale target pointer path
    // that must not overwrite the recognized observation.
    const ter_id t_wall( "t_wall" );
    for( int x = center.x() - 12; x <= center.x() + 12; ++x ) {
        here.ter_set( tripoint_bub_ms( x, center.y() + 1, center.z() ), t_wall );
    }
    you.setpos( here, hidden_position );
    refresh_writhing_stalker_arena( here );
    REQUIRE_FALSE( stalker.sees( here, you ) );
    stalker.plan();

    CHECK( state.last_observed_position == last_seen );
    CHECK( state.evidence_turn == observation_turn );
    const bool routed_to_hidden_position = stalker.has_dest() && stalker.get_dest() == you.pos_abs();
    CHECK_FALSE( routed_to_hidden_position );
    if( stalker.has_dest() ) {
        const bool routed_to_recognized_route = stalker.get_dest() == state.committed_waypoint ||
                                                stalker.get_dest() == last_seen;
        CHECK( routed_to_recognized_route );
    } else {
        const bool bounded_search_or_idle = state.phase == writhing_stalker::lifecycle_phase::searching ||
                                            state.phase == writhing_stalker::lifecycle_phase::idle;
        CHECK( bounded_search_or_idle );
    }
    clear_map_without_vision();
}

TEST_CASE( "writhing_stalker_pressure_history_is_visible_absolute_and_lifecycle_bounded",
           "[writhing_stalker][monster][pressure][lifecycle]" )
{
    clear_map_without_vision();
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center{ 65, 65, 0 };
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    refresh_writhing_stalker_arena( here );

    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 6 );
    monster &pressure = spawn_test_monster( mon_zombie.str(), center + point::east * 3 );
    pressure.set_dest( you.pos_abs() );
    pressure.aggro_character = true;
    REQUIRE( stalker.sees( here, pressure ) );
    REQUIRE( pressure.sees( here, you ) );

    writhing_stalker::reset_transient_pressure_history();
    CHECK( writhing_stalker::observed_zombie_pressure( stalker, here, you ) == 1 );
    CHECK( writhing_stalker::transient_pressure_sample_count() == 1 );
    // A repeated same-turn facade read must not duplicate a position sample.
    CHECK( writhing_stalker::observed_zombie_pressure( stalker, here, you ) == 1 );
    CHECK( writhing_stalker::transient_pressure_sample_count() == 1 );

    // Occluded motion supplies no sample. After reacquisition, current native
    // target intent qualifies independently of any unobserved closing motion.
    writhing_stalker::reset_transient_pressure_history();
    here.ter_set( center + point::east * 4, ter_id( "t_wall" ) );
    refresh_writhing_stalker_arena( here );
    REQUIRE_FALSE( stalker.sees( here, pressure ) );
    CHECK( writhing_stalker::observed_zombie_pressure( stalker, here, you ) == 0 );
    CHECK( writhing_stalker::transient_pressure_sample_count() == 0 );
    here.ter_set( center + point::east * 4, ter_id( "t_floor" ) );
    pressure.setpos( here, center + point::east * 2 );
    pressure.set_dest( you.pos_abs() );
    set_time( calendar::turn_zero + 1_turns );
    refresh_writhing_stalker_arena( here );
    REQUIRE( stalker.sees( here, pressure ) );
    CHECK( writhing_stalker::observed_zombie_pressure( stalker, here, you ) == 1 );
    CHECK( writhing_stalker::transient_pressure_sample_count() == 1 );

    // Replacing a live actor must discard its old identity sample rather than
    // accumulating a closing-history trail under the surviving stalker.
    g->remove_zombie( pressure );
    for( int replacement = 0; replacement < 5; ++replacement ) {
        monster &new_pressure = spawn_test_monster( mon_zombie.str(), center + point::east * 3 );
        new_pressure.set_dest( you.pos_abs() );
        new_pressure.aggro_character = true;
        set_time( calendar::turn_zero + time_duration::from_turns( 2 + replacement ) );
        refresh_writhing_stalker_arena( here );
        REQUIRE( stalker.sees( here, new_pressure ) );
        CHECK( writhing_stalker::observed_zombie_pressure( stalker, here, you ) == 1 );
        CHECK( writhing_stalker::transient_pressure_sample_count() == 1 );
        g->remove_zombie( new_pressure );
    }

    // clear_map_without_vision clears the creature tracker through game::clear_zombies,
    // the real reset owner for this transient cache in same-process test worlds.
    clear_map_without_vision();
    CHECK( writhing_stalker::transient_pressure_sample_count() == 0 );
}

TEST_CASE( "writhing_stalker_pressure_history_survives_a_real_bubble_shift",
           "[writhing_stalker][monster][pressure][lifecycle]" )
{
    clear_map_without_vision();
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center{ 65, 65, 0 };
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero );
    prepare_writhing_stalker_arena( here, center );
    // The player drives the bubble rebase; the NPC remains the observed target.
    you.setpos( here, center + point::south * 30 );
    npc &target = spawn_npc( point_bub_ms( center.xy() ), "test_talker" );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 6 );
    monster &pressure = spawn_test_monster( mon_zombie.str(), center + point::east * 3 );
    pressure.set_dest( target.pos_abs() );
    pressure.aggro_character = true;
    refresh_writhing_stalker_arena( here );
    REQUIRE( stalker.sees( here, pressure ) );
    REQUIRE( pressure.sees( here, target ) );
    REQUIRE( pressure.attack_target() == &target );

    writhing_stalker::reset_transient_pressure_history();
    CHECK( writhing_stalker::observed_zombie_pressure( stalker, here, target ) == 1 );

    // A real visible approach is still recognized as meaningful pressure.
    pressure.setpos( here, center + point::east * 2 );
    pressure.set_dest( target.pos_abs() );
    set_time( calendar::turn_zero + 1_turns );
    refresh_writhing_stalker_arena( here );
    REQUIRE( pressure.attack_target() == &target );
    CHECK( writhing_stalker::observed_zombie_pressure( stalker, here, target ) == 1 );

    const tripoint_abs_ms target_before_shift = target.pos_abs();
    const tripoint_abs_ms pressure_before_shift = pressure.pos_abs();
    const tripoint_abs_ms stalker_before_shift = stalker.pos_abs();
    // This crosses the production bubble boundary.  game::update_map shifts
    // local coordinates for all active creatures while retaining their world
    // positions, which is precisely the stale-bubble-coordinate failure mode.
    g->place_player( tripoint_bub_ms( 120, you.pos_bub().y(), you.pos_bub().z() ) );
    refresh_writhing_stalker_arena( here );
    CHECK( target.pos_abs() == target_before_shift );
    CHECK( pressure.pos_abs() == pressure_before_shift );
    CHECK( stalker.pos_abs() == stalker_before_shift );
    set_time( calendar::turn_zero + 2_turns );
    REQUIRE( stalker.sees( here, pressure ) );
    REQUIRE( pressure.sees( here, target ) );
    REQUIRE( pressure.attack_target() == &target );
    // No actor closed distance after the rebase. Current native target intent
    // still qualifies once; absolute positions and sample uniqueness survive.
    CHECK( writhing_stalker::observed_zombie_pressure( stalker, here, target ) == 1 );
    CHECK( writhing_stalker::transient_pressure_sample_count() == 1 );
    clear_map_without_vision();
}

TEST_CASE( "writhing_stalker_monster_save_load_owns_typed_state_without_legacy_migration",
           "[writhing_stalker][monster][persistence]" )
{
    monster original( mon_writhing_stalker );
    auto &state = original.writhing_stalker_state();
    original.predator_state().actor_id = "same-process-save-reload-actor";
    original.predator_state().ammo_initialization_version = 2;
    original.predator_state().band_reference = "opaque-band-cache";
    original.predator_state().band_revision_cache = 4;
    state.phase = writhing_stalker::lifecycle_phase::retreating;
    state.evidence_target = 77;
    state.evidence_turn = 123;
    state.light_observed_position = tripoint_abs_ms( 12, 13, 0 );
    state.has_light_observed_position = true;
    state.light_observed_turn = 119;
    state.light_expires_turn = 209;
    state.light_sample_id = "window-glow#119";
    state.retreat_waypoint = tripoint_abs_ms( 20, 21, 0 );
    state.has_retreat_waypoint = true;
    state.attempts_spent = 2;
    state.attempt_sequence = 8;
    state.phase_entered_turn = 120;
    state.cooldown_until_turn = 180;
    state.last_progress_turn = 125;
    state.remaining_route_progress = 4;

    // These are intentionally contradictory legacy diagnostics.  The nested
    // typed object must remain authoritative and no old value is migrated.
    original.set_value( "caol_writhing_stalker_handoff_intent", "shadowing" );
    original.set_value( "caol_writhing_stalker_handoff_cooldown", 1 );
    original.set_value( "caol_writhing_stalker_stalk_omt", 99 );

    const monster loaded = round_trip_writhing_stalker( original );
    const auto &restored = loaded.writhing_stalker_state();
    CHECK( loaded.predator_state().actor_id == original.predator_state().actor_id );
    CHECK( loaded.predator_state().ammo_initialization_version == 2 );
    CHECK( loaded.predator_state().band_reference == "opaque-band-cache" );
    CHECK( loaded.predator_state().band_revision_cache == 4 );
    CHECK( restored.phase == writhing_stalker::lifecycle_phase::retreating );
    CHECK( restored.evidence_target == 77 );
    CHECK( restored.evidence_turn == 123 );
    CHECK( restored.has_light_observed_position );
    CHECK( restored.light_observed_position == tripoint_abs_ms( 12, 13, 0 ) );
    CHECK( restored.light_observed_turn == 119 );
    CHECK( restored.light_expires_turn == 209 );
    CHECK( restored.light_sample_id == "window-glow#119" );
    CHECK( restored.retreat_waypoint == tripoint_abs_ms( 20, 21, 0 ) );
    CHECK( restored.attempts_spent == 2 );
    CHECK( restored.attempt_sequence == 8 );
    CHECK( restored.cooldown_until_turn == 180 );
    CHECK( restored.last_progress_turn == 125 );
    CHECK( restored.remaining_route_progress == 4 );
}

TEST_CASE( "writhing_stalker_light_interest_is_uncertain_finite_and_never_replaces_direct_prey",
           "[writhing_stalker][ai][light][memory]" )
{
    using namespace writhing_stalker;
    persistent_state state;
    const tripoint_abs_ms first_glow( 40, 50, 0 );
    const tripoint_abs_ms later_glow( 60, 70, 0 );

    CHECK( state.observe_light_interest( first_glow, "lamp#100", 100, 90 ) );
    CHECK( state.light_interest_active( 101 ) );
    CHECK( state.has_light_observed_position );
    CHECK( state.light_observed_position == first_glow );
    CHECK( state.evidence_target == 0 );
    CHECK( state.phase == lifecycle_phase::shadowing );
    CHECK( state.committed_waypoint == first_glow );
    CHECK_FALSE( state.observe_light_interest( later_glow, "lamp#100", 100, 90 ) );
    CHECK_FALSE( state.observe_light_interest( later_glow, "lamp#99", 99, 90 ) );
    CHECK( state.light_observed_position == first_glow );

    // A later physical sample can replace the uncertain area, but its expiry
    // remains anchored to that detection rather than the old blink.
    CHECK( state.observe_light_interest( later_glow, "lamp#120", 120, 90 ) );
    CHECK( state.light_observed_position == later_glow );
    CHECK( state.light_expires_turn == 210 );
    state.advance_to( 210 );
    CHECK_FALSE( state.light_interest_active( 210 ) );
    CHECK_FALSE( state.has_light_observed_position );

    state.evidence_target = 17;
    state.evidence_turn = 220;
    state.last_observed_position = tripoint_abs_ms( 21, 22, 0 );
    state.has_last_observed_position = true;
    CHECK( state.has_direct_evidence( 221 ) );
    CHECK_FALSE( state.observe_light_interest( first_glow, "lamp#221", 221, 90 ) );
    CHECK_FALSE( state.has_light_observed_position );

    state.evidence_target = 0;
    state.phase = lifecycle_phase::attacking;
    CHECK_FALSE( state.observe_light_interest( first_glow, "lamp#500", 500, 90 ) );
    CHECK_FALSE( state.has_light_observed_position );
}

TEST_CASE( "writhing_stalker_pattern_helper_traces_repeated_strikes_then_injured_retreat",
           "[writhing_stalker][ai][pattern]" )
{
    using namespace writhing_stalker;

    const std::vector<stalker_pattern_row> rows = run_vulnerable_stalker_pattern( 8 );
    INFO( "writhing stalker pattern trace:\n" << trace_rows( rows ) );

    REQUIRE( rows.size() == 8 );
    CHECK( rows[0].next == decision::shadow );
    CHECK( rows[0].route == approach_class::cover_shadow );
    CHECK( rows[1].next == decision::strike );
    CHECK( rows[2].next == decision::strike );
    CHECK( rows[3].hp_percent == 50 );
    CHECK( rows[3].next == decision::withdraw );
    CHECK( rows[3].reason == "live_stalker_hurt_withdraw" );

    CHECK( count_decisions( rows, decision::strike ) == 2 );
    CHECK( count_decisions( rows, decision::withdraw ) >= 1 );
    CHECK_FALSE( rows[3].next == decision::strike );
    CHECK_FALSE( rows[4].next == decision::strike );

    int jitter_count = 0;
    for( size_t i = 1; i < rows.size(); ++i ) {
        if( rows[i - 1].next == decision::withdraw && rows[i].next == decision::strike ) {
            ++jitter_count;
        }
    }
    CHECK( jitter_count == 0 );
}

TEST_CASE( "stalker_pursuit_observed_heading_is_bounded_and_world_facts_survive_pause",
           "[writhing_stalker][pursuit]" )
{
    using namespace writhing_stalker;
    persistent_state state;
    state.advance_to( 100 );
    REQUIRE( state.observe_target( 77, 1, tripoint_abs_ms( 100, 100, 0 ), 100 ) );
    CHECK( state.heading_interval == 0 );
    state.advance_to( 110 );
    REQUIRE( state.observe_target( 77, 1, tripoint_abs_ms( 100, 90, 0 ), 110 ) );
    CHECK( state.heading == tripoint_rel_ms( 0, -10, 0 ) );
    CHECK( state.heading_interval == 10 );
    state.advance_to( 140 );
    const auto identity_route = []( const tripoint_abs_ms & p, bool ) {
        return std::optional<tripoint_abs_ms>( p );
    };
    CHECK( state.pursuit_waypoint( tripoint_abs_ms( 100, 120, 0 ), identity_route ) ==
           tripoint_abs_ms( 100, 66, 0 ) );
    CHECK( state.evidence_turn == 110 );
    CHECK( state.last_observed_position == tripoint_abs_ms( 100, 90, 0 ) );

    // A visible turn replaces the northbound heading. Reloading the observer
    // or target does not confuse a stable Character ID with a new runtime ID.
    state.advance_to( 150 );
    REQUIRE( state.observe_target( 88, 1, tripoint_abs_ms( 110, 90, 0 ), 150 ) );
    CHECK( state.heading == tripoint_rel_ms( 10, 0, 0 ) );
    CHECK( state.heading_interval == 40 );
    state.advance_to( 151 );
    REQUIRE( state.observe_target( 88, 1, tripoint_abs_ms( 110, 90, 0 ), 151 ) );
    CHECK( state.heading_interval == 0 );
    CHECK_FALSE( state.observe_target( 88, 1, tripoint_abs_ms( 999, 999, 0 ), 151 ) );

    std::ostringstream saved;
    { JsonOut json( saved ); state.serialize( json ); }
    persistent_state loaded;
    loaded.deserialize( json_loader::from_string( saved.str() ) );
    const int active = loaded.active_turns;
    loaded.advance_to( 100000, 0 );
    CHECK( loaded.active_turns == active );
    CHECK( loaded.evidence_turn == 151 );
    CHECK_FALSE( loaded.has_direct_evidence( 100000 ) );
    loaded.advance_to( 100001, 1 );
    CHECK( loaded.active_turns == active + 1 );
    loaded.advance_to( 100001, 200 );
    CHECK( loaded.active_turns == active + 1 );

    persistent_state old_save;
    old_save.deserialize( json_loader::from_string( "{\"last_advanced_turn\":10}" ) );
    old_save.advance_to( 100000 );
    CHECK( old_save.active_turns == 0 );
    CHECK_FALSE( old_save.has_pursuit_memory );

    // Cumulative active deadlines do not wrap at the old 32-bit turn bound.
    loaded.active_turns = std::numeric_limits<int>::max();
    loaded.advance_to( 100002, 10 );
    CHECK( loaded.active_turns == static_cast<std::int64_t>( std::numeric_limits<int>::max() ) + 10 );
    REQUIRE( loaded.observe_target( 88, 1, tripoint_abs_ms( 110, 90, 0 ), 100002 ) );
    loaded.finish_search();
    CHECK( loaded.familiar[0].revisit_at_active > loaded.active_turns );
}

TEST_CASE( "stalker_pursuit_search_revisits_are_finite_and_do_not_invent_sightings",
           "[writhing_stalker][pursuit]" )
{
    using namespace writhing_stalker;
    persistent_state state;
    state.advance_to( 100 );
    REQUIRE( state.observe_target( 77, 1, tripoint_abs_ms( 100, 100, 0 ), 100 ) );
    state.advance_to( 161 );
    const auto open_route = []( const tripoint_abs_ms & p, bool ) {
        return std::optional<tripoint_abs_ms>( p );
    };
    tripoint_abs_ms position( 80, 100, 0 );
    for( int step = 0; step < 9; ++step ) {
        const auto waypoint = state.pursuit_waypoint( position, open_route );
        REQUIRE( waypoint );
        CHECK( rl_dist( project_to<coords::omt>( *waypoint ),
                        project_to<coords::omt>( state.last_observed_position ) ) <= 1 );
        position = *waypoint;
        state.advance_to( 162 + step );
    }
    CHECK_FALSE( state.pursuit_waypoint( position, open_route ) );
    CHECK( state.phase == lifecycle_phase::idle );
    CHECK( state.searched_positions == 511 );
    CHECK( state.evidence_turn == 100 );
    CHECK( state.familiar[0].last_seen_world == 100 );
    const int due = state.familiar[0].revisit_at_active;
    CHECK( due >= state.active_turns + to_turns<int>( 6_hours ) );
    CHECK( due <= state.active_turns + to_turns<int>( 12_hours ) );
    CHECK_FALSE( state.pursuit_waypoint( position, open_route ) );
    CHECK( state.familiar[0].revisit_at_active == due );

    std::ostringstream serialized;
    { JsonOut json( serialized ); state.serialize( json ); }
    persistent_state reloaded;
    reloaded.deserialize( json_loader::from_string( serialized.str() ) );
    CHECK( reloaded.familiar[0].revisit_at_active == due );
    reloaded.advance_to( 100000, due - reloaded.active_turns );
    const auto revisit = reloaded.pursuit_waypoint( position + point::east, open_route );
    REQUIRE( revisit );
    CHECK( reloaded.revisiting_slot == 0 );
    const int revisit_start = reloaded.active_turns;
    reloaded.advance_to( 100600, to_turns<int>( 10_minutes ) );
    CHECK_FALSE( reloaded.pursuit_waypoint( position + point::east, open_route ) );
    CHECK( reloaded.phase == lifecycle_phase::idle );
    CHECK( reloaded.familiar[0].revisit_at_active >= revisit_start +
           to_turns<int>( 10_minutes + 6_hours ) );
    CHECK( reloaded.familiar[0].last_seen_world == 100 );
    CHECK_FALSE( reloaded.pursuit_waypoint( position, open_route ) );

    // An independently blocked observer may end a search without claiming it
    // searched the nine rejected positions or refreshing the learned place.
    persistent_state blocked;
    blocked.advance_to( 10 );
    blocked.observe_target( 99, 2, tripoint_abs_ms( 10, 10, 0 ), 10 );
    const auto no_route = []( const tripoint_abs_ms &, bool ) {
        return std::optional<tripoint_abs_ms>();
    };
    CHECK_FALSE( blocked.pursuit_waypoint( position, no_route ) );
    CHECK( blocked.searched_positions == 0 );
    CHECK( blocked.familiar[0].last_seen_world == 10 );
    CHECK( state.familiar[0].last_seen_world == 100 );
}

TEST_CASE( "stalker_pursuit_stalled_search_has_active_deadline_and_revisit_cannot_chain",
           "[writhing_stalker][pursuit]" )
{
    using namespace writhing_stalker;
    persistent_state state;
    state.advance_to( 100 );
    state.observe_target( 77, 1, tripoint_abs_ms( 100, 100, 0 ), 100 );
    state.advance_to( 161 );
    const tripoint_abs_ms waiting( 80, 100, 0 );
    const auto route = []( const tripoint_abs_ms & p, bool ) {
        return std::optional<tripoint_abs_ms>( p );
    };
    REQUIRE( state.pursuit_waypoint( waiting, route ) );
    const int started = state.search_started_active;
    state.advance_to( 100000, 0 );
    REQUIRE( state.pursuit_waypoint( waiting, route ) );
    CHECK( state.search_started_active == started );
    CHECK( state.searched_positions == 0 );
    state.advance_to( 100001, to_turns<int>( 30_minutes ) );
    CHECK_FALSE( state.pursuit_waypoint( waiting, route ) );
    CHECK( state.phase == lifecycle_phase::idle );
    CHECK( state.evidence_turn == 100 );

    state.familiar[1] = state.familiar[0];
    state.familiar[1].position += point::east;
    state.familiar[0].revisit_at_active = state.active_turns;
    state.familiar[1].revisit_at_active = state.active_turns;
    const auto blocked = []( const tripoint_abs_ms &, bool ) {
        return std::optional<tripoint_abs_ms>();
    };
    CHECK_FALSE( state.pursuit_waypoint( waiting, blocked ) );
    CHECK( state.phase == lifecycle_phase::idle );
    for( const auto &place : state.familiar ) {
        CHECK( place.revisit_at_active >= state.active_turns + to_turns<int>( 6_hours ) );
        CHECK( place.last_seen_world == 100 );
    }
    CHECK_FALSE( state.pursuit_waypoint( waiting, route ) );

    persistent_state delayed;
    delayed.advance_to( 10 );
    delayed.observe_target( 9, 1, tripoint_abs_ms( 100, 100, 0 ), 10 );
    delayed.advance_to( 100000, to_turns<int>( 32_minutes ) );
    CHECK_FALSE( delayed.pursuit_waypoint( waiting, route ) );
    CHECK( delayed.phase == lifecycle_phase::idle );
    CHECK( delayed.searched_positions == 0 );
}

TEST_CASE( "stalker_pursuit_familiar_places_use_two_slots_and_active_expiry",
           "[writhing_stalker][pursuit]" )
{
    using namespace writhing_stalker;
    persistent_state state;
    state.advance_to( 1 );
    state.observe_target( 1, 1, tripoint_abs_ms( 100, 100, 0 ), 1 );
    state.advance_to( 2 );
    state.observe_target( 1, 1, tripoint_abs_ms( 200, 100, 0 ), 2 );
    state.advance_to( 3 );
    state.observe_target( 1, 1, tripoint_abs_ms( 100, 100, 0 ), 3 );
    state.advance_to( 4 );
    state.observe_target( 1, 1, tripoint_abs_ms( 300, 100, 0 ), 4 );
    CHECK( state.familiar[0].last_seen_world == 3 );
    CHECK( state.familiar[1].last_seen_world == 4 );
    state.advance_to( 10000000, 0 );
    CHECK( state.familiar[0].valid );
    CHECK( state.familiar[1].valid );
    state.advance_to( 10000001, to_turns<int>( 7_days ) );
    CHECK_FALSE( state.familiar[0].valid );
    CHECK_FALSE( state.familiar[1].valid );
    CHECK( state.evidence_turn == 4 );
}

TEST_CASE( "stalker_closing_a_real_attack_opening_replaces_the_attack_destination",
           "[writhing_stalker][pursuit][monster][opening_transition]" )
{
    clear_map_without_vision();
    clear_npcs();
    map &here = get_map();
    Character &you = get_player_character();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1_days );
    scoped_weather_override weather( weather_type_id( "clear" ) );
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    refresh_writhing_stalker_arena( here );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 3 );
    stalker.anger = 100;
    stalker.aggro_character = true;
    // Actual visible pressure supplies the evaluator's commitment footing;
    // dark solitude, rather than an invented attack, supplies the opening.
    std::vector<monster *> pressure;
    for( const point &offset : { point::west, point::south, point::east, point( -2, 2 ) } ) {
        monster &zombie = spawn_test_monster( mon_zombie.str(), center + offset );
        zombie.aggro_character = true;
        zombie.set_dest( you.pos_abs() );
        pressure.push_back( &zombie );
    }
    stalker.plan();
    auto &state = stalker.writhing_stalker_state();
    REQUIRE( state.phase == writhing_stalker::lifecycle_phase::attacking );
    REQUIRE( state.committed_waypoint == you.pos_abs() );
    REQUIRE( stalker.get_dest() == you.pos_abs() );
    const auto target_position = you.pos_abs();

    // The target stays still, so a new sighting cannot incidentally invalidate
    // the old attack waypoint. Cover both the explicit opening veto and a
    // natural evaluator shadow exit, while dark solitude itself remains true.
    const bool evaluator_shadow = GENERATE( false, true );
    if( evaluator_shadow ) {
        // Departing pressure is a fixture change, not a combat-victory claim.
        for( monster *zombie : pressure ) {
            g->remove_zombie( *zombie );
        }
    } else {
        npc &ally = spawn_npc( point_bub_ms( ( center + point::south_west ).xy() ), "test_talker" );
        ally.set_fac( faction_id( "your_followers" ) );
        ally.set_attitude( NPCATT_FOLLOW );
        refresh_writhing_stalker_arena( here );
        REQUIRE( ally.attitude_to( you ) == Creature::Attitude::FRIENDLY );
        REQUIRE( ally.sees( here, you ) );
        REQUIRE( writhing_stalker::can_observe_actor( stalker, ally ) );
    }
    set_time( calendar::turn + 1_turns );
    if( evaluator_shadow ) {
        REQUIRE( writhing_stalker::observed_zombie_pressure( stalker, here, you ) == 0 );
    }
    stalker.plan();
    REQUIRE( you.pos_abs() == target_position );
    REQUIRE( state.phase == writhing_stalker::lifecycle_phase::shadowing );
    CHECK_FALSE( ( state.has_committed_waypoint && state.committed_waypoint == target_position ) );
    REQUIRE( stalker.has_dest() );
    CHECK( rl_dist( stalker.get_dest(), target_position ) >= 2 );
    const int target_hp = you.get_hp();
    stalker.set_moves( 100 );
    stalker.move();
    CHECK( you.get_hp() == target_hp );
    CHECK( state.attempts_spent == 0 );
    CHECK( rl_dist( stalker.pos_abs(), target_position ) >= 2 );
    clear_npcs();
    clear_map_without_vision();
}

TEST_CASE( "stalker_finished_pursuit_then_light_arrival_expires_after_reload_and_repeat",
           "[writhing_stalker][pursuit][monster][light_transition]" )
{
    clear_map_without_vision();
    map &here = get_map();
    Character &you = get_player_character();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1_days );
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    refresh_writhing_stalker_arena( here );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 3 );
    stalker.anger = 100;
    stalker.aggro_character = true;
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().has_pursuit_memory );
    const int observed = stalker.writhing_stalker_state().evidence_turn;
    you.setpos( here, center + point::west * 20 );
    for( int y = 0; y < 132; ++y ) {
        here.ter_set( tripoint_bub_ms( 55, y, 0 ), ter_id( "t_wall" ) );
    }
    refresh_writhing_stalker_arena( here );
    REQUIRE_FALSE( writhing_stalker::can_observe_actor( stalker, you ) );
    set_time( calendar::turn + 32_minutes );
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::idle );
    REQUIRE( stalker.writhing_stalker_state().sighting_active_turn == -1 );
    const auto glow = stalker.pos_abs() + point::east * 3;
    // A fresh physical-light adapter input, not a target sighting. The caller
    // route and actual movement/arrival below own the search deadline proof.
    REQUIRE( stalker.writhing_stalker_state().observe_light_interest(
                 glow, "post-pursuit-lamp", to_turn<int>( calendar::turn ), 90 ) );
    for( int steps = 0; steps < 12 && stalker.pos_abs() != glow; ++steps ) {
        stalker.plan();
        stalker.set_moves( 100 );
        stalker.move();
        set_time( calendar::turn + 1_turns );
    }
    REQUIRE( stalker.pos_abs() == glow );
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::searching );
    const int deadline = stalker.writhing_stalker_state().search_until_turn;
    REQUIRE( deadline == to_turn<int>( calendar::turn ) + 20 );
    const bool reload = GENERATE( false, true );
    if( reload ) {
        stalker = round_trip_writhing_stalker( stalker );
        REQUIRE( stalker.writhing_stalker_state().search_until_turn == deadline );
    }
    set_time( calendar::turn_zero + time_duration::from_turns( deadline ) );
    for( int repeat = 0; repeat < 3; ++repeat ) {
        stalker.plan();
        CHECK( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::idle );
        CHECK_FALSE( stalker.writhing_stalker_state().has_committed_waypoint );
        CHECK_FALSE( ( stalker.has_dest() && stalker.get_dest() == glow ) );
        CHECK( stalker.writhing_stalker_state().evidence_turn == observed );
        CHECK( stalker.writhing_stalker_state().has_pursuit_memory );
    }
    clear_map_without_vision();
}

TEST_CASE( "stalker_actor_weather_uses_the_physical_ray_without_indoor_range_penalty",
           "[writhing_stalker][pursuit][monster][weather]" )
{
    clear_map_without_vision();
    clear_avatar();
    map &here = get_map();
    Character &you = get_player_character();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 12_hours );
    const bool indoors = GENERATE( false, true );
    for( int x = 0; x < 132; ++x ) {
        for( int y = 0; y < 132; ++y ) {
            here.ter_set( tripoint_bub_ms( x, y, 0 ), ter_id( indoors ? "t_floor" : "t_grass" ) );
            here.ter_set( tripoint_bub_ms( x, y, 1 ), ter_id( indoors ? "t_flat_roof" : "t_open_air" ) );
            here.furn_clear( tripoint_bub_ms( x, y, 0 ) );
        }
    }
    const tripoint_bub_ms origin( 10, 65, 0 );
    you.setpos( here, origin + point::east * ( indoors ? 20 : 71 ) );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), origin );
    for( const char *weather_id : { "clear", "fog" } ) {
        scoped_weather_override weather{ weather_type_id( weather_id ) };
        here.invalidate_map_cache( 1 );
        refresh_writhing_stalker_arena( here );
        const int range = std::min( 72, static_cast<int>( std::floor(
                                       stalker.sight_range( here.ambient_light_at( you.pos_bub() ) ) * 72.0f / 35 ) ) );
        std::vector<tripoint_abs_omt> footprint;
        for( const auto &cell : scout_observation::site_sightline( stalker.pos_abs(), you.pos_abs() ) ) {
            footprint.push_back( project_to<coords::omt>( cell ) );
        }
        const bool physical = stalker.sees_site_with_range( here, you, range, footprint );
        scout_observation::site_reader reader;
        const auto read = reader.read_actor( stalker, you, { 72, 72.0f / 35 } );
        CAPTURE( indoors, weather_id, range, read.reason, physical );
        REQUIRE( here.is_outside( you.pos_bub() ) == !indoors );
        REQUIRE( read.known );
        CHECK( read.visible == physical );
        CHECK( read.visible == ( indoors || std::string( weather_id ) == "clear" ) );
    }
    clear_map_without_vision();
}

TEST_CASE( "stalker_native_retreat_authority_survives_the_abstract_intent_caller",
           "[writhing_stalker][predator][pursuit][monster]" )
{
    clear_overmaps();
    clear_map_without_vision();
    map &here = get_map();
    Character &you = get_player_character();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 100_turns );
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    refresh_writhing_stalker_arena( here );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 2 );
    stalker.anger = 100;
    stalker.aggro_character = true;
    auto &state = stalker.writhing_stalker_state();
    state.advance_to( 90 );
    REQUIRE( state.observe_target( you.get_identity(), you.getID().get_value(), you.pos_abs(), 90 ) );
    state.committed_waypoint = you.pos_abs();
    state.has_committed_waypoint = true;
    state.attempts_spent = 2;
    stalker.plan();
    REQUIRE( state.phase == writhing_stalker::lifecycle_phase::retreating );
    const bool cooling = GENERATE( false, true );
    if( cooling ) {
        stalker.plan();
        REQUIRE( state.phase == writhing_stalker::lifecycle_phase::cooldown );
    }
    REQUIRE( state.has_retreat_waypoint );
    const auto retreat = state.retreat_waypoint;
    REQUIRE( stalker.get_dest() == retreat );
    REQUIRE( retreat != state.committed_waypoint );
    const auto position = stalker.pos_abs();
    const auto actor = stalker.predator_state().actor_id;
    const auto epoch = stalker.predator_state().handoff_epoch;
    g->despawn_monster( stalker );
    horde_entity *owner = overmap_buffer.entity_at( position );
    REQUIRE( owner != nullptr );
    REQUIRE( owner->monster_data != nullptr );
    REQUIRE( owner->destination == retreat );
    set_time( calendar::turn + 1_turns );
    REQUIRE( owner->advance_predator_intent( position, to_turn<int>( calendar::turn ) ) );
    CHECK( owner->destination == retreat );
    CHECK( owner->monster_data->wander_pos == retreat );
    CHECK( owner->monster_data->predator_state().actor_id == actor );
    CHECK( owner->monster_data->predator_state().handoff_epoch == epoch + 1 );
    const auto phase = owner->monster_data->writhing_stalker_state().phase;
    CHECK( phase == ( cooling ? writhing_stalker::lifecycle_phase::cooldown :
                     writhing_stalker::lifecycle_phase::retreating ) );
    REQUIRE( owner->advance_predator_intent( position, to_turn<int>( calendar::turn ) ) );
    CHECK( owner->destination == retreat );
    clear_overmaps();
    clear_map_without_vision();
}

TEST_CASE( "stalker_pursuit_actual_planner_acquires_at_three_OMT_and_rejects_hidden_turn",
           "[writhing_stalker][pursuit][monster]" )
{
    clear_map_without_vision();
    clear_avatar();
    map &here = get_map();
    Character &you = get_player_character();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 12_hours );
    const tripoint_bub_ms origin( 10, 65, 0 );
    scoped_weather_override weather( weather_type_id( "clear" ) );
    for( int x = 5; x < 100; ++x ) {
        for( int y = 40; y < 80; ++y ) {
            here.ter_set( tripoint_bub_ms( x, y, 0 ), ter_id( "t_grass" ) );
            here.ter_set( tripoint_bub_ms( x, y, 1 ), ter_id( "t_open_air" ) );
            here.furn_clear( tripoint_bub_ms( x, y, 0 ) );
        }
    }
    you.setpos( here, tripoint_bub_ms( 82, 65, 0 ) );
    refresh_writhing_stalker_arena( here );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), origin );
    monster &second = spawn_test_monster( mon_writhing_stalker.str(), origin + point::north * 2 );
    stalker.anger = 100;
    stalker.aggro_character = true;
    second.anger = 100;
    second.aggro_character = true;
    REQUIRE_FALSE( stalker.sees( here, you ) );
    REQUIRE( rl_dist( project_to<coords::omt>( stalker.pos_abs() ),
                      project_to<coords::omt>( you.pos_abs() ) ) == 3 );
    scout_observation::site_reader sight;
    const auto direct = sight.read_actor( stalker, you, { 72, 72.0f / 35 } );
    CAPTURE( direct.known, direct.visible, direct.reason, you.visibility(),
             here.ambient_light_at( you.pos_bub() ), stalker.sight_range( 100.0f ),
             static_cast<int>( stalker.attitude_to( you ) ),
             static_cast<int>( stalker.attitude_to( second ) ) );
    // Native avatar visibility is 99%, so exactly72 squares is beyond this
    // target's recognition boundary. One square closer is still three OMT.
    REQUIRE_FALSE( direct.visible );
    you.setpos( here, tripoint_bub_ms( 81, 65, 0 ) );
    refresh_writhing_stalker_arena( here );
    REQUIRE( writhing_stalker::can_observe_actor( stalker, you ) );
    stalker.plan();
    second.plan();
    auto &state = stalker.writhing_stalker_state();
    REQUIRE( state.has_pursuit_memory );
    REQUIRE( second.writhing_stalker_state().has_pursuit_memory );
    const int second_first_sighting = second.writhing_stalker_state().evidence_turn;
    CHECK( state.last_observed_position == you.pos_abs() );
    CHECK( state.sighting_active_turn == state.active_turns );
    REQUIRE( stalker.has_dest() );
    CHECK( rl_dist( stalker.pos_abs(), stalker.get_dest() ) <= 10 );
    if( state.phase == writhing_stalker::lifecycle_phase::retreating ||
        state.phase == writhing_stalker::lifecycle_phase::cooldown ) {
        CHECK( rl_dist( stalker.get_dest(), you.pos_abs() ) >=
               rl_dist( stalker.pos_abs(), you.pos_abs() ) );
    }

    set_time( calendar::turn + 10_turns );
    you.setpos( here, tripoint_bub_ms( 81, 55, 0 ) );
    refresh_writhing_stalker_arena( here );
    stalker.plan();
    CHECK( state.heading == tripoint_rel_ms( 0, -10, 0 ) );
    CHECK( state.sighting_active_turn == state.active_turns );
    CHECK( second.writhing_stalker_state().evidence_turn == second_first_sighting );
    set_time( calendar::turn + 10_turns );
    you.setpos( here, tripoint_bub_ms( 71, 55, 0 ) );
    refresh_writhing_stalker_arena( here );
    stalker.plan();
    CHECK( state.heading == tripoint_rel_ms( -10, 0, 0 ) );
    CHECK( state.sighting_active_turn == state.active_turns );
    const int last_seen = state.evidence_turn;
    const auto last_position = state.last_observed_position;
    for( int y = 0; y < 132; ++y ) {
        here.ter_set( tripoint_bub_ms( 40, y, 0 ), ter_id( "t_wall" ) );
    }
    set_time( calendar::turn + 10_turns );
    you.setpos( here, tripoint_bub_ms( 71, 45, 0 ) );
    refresh_writhing_stalker_arena( here );
    stalker.plan();
    CHECK( state.evidence_turn == last_seen );
    CHECK( state.last_observed_position == last_position );
    CHECK( state.heading == tripoint_rel_ms( -10, 0, 0 ) );
    const bool magically_tracked = stalker.has_dest() && stalker.get_dest() == you.pos_abs();
    CHECK_FALSE( magically_tracked );
    clear_map_without_vision();
}

TEST_CASE( "stalker_pursuit_native_route_uses_short_steps_without_invented_search_progress",
           "[writhing_stalker][pursuit][monster]" )
{
    clear_map_without_vision();
    clear_avatar();
    map &here = get_map();
    Character &you = get_player_character();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time_to_day();
    scoped_weather_override weather( weather_type_id( "clear" ) );
    for( int x = 25; x < 90; ++x ) {
        for( int y = 50; y < 85; ++y ) {
            here.ter_set( tripoint_bub_ms( x, y, 0 ), ter_id( "t_grass" ) );
            here.ter_set( tripoint_bub_ms( x, y, 1 ), ter_id( "t_open_air" ) );
            here.furn_clear( tripoint_bub_ms( x, y, 0 ) );
        }
    }
    you.setpos( here, tripoint_bub_ms( 80, 66, 0 ) );
    refresh_writhing_stalker_arena( here );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(),
                       tripoint_bub_ms( 30, 66, 0 ) );
    stalker.anger = 100;
    stalker.aggro_character = true;
    REQUIRE( writhing_stalker::observe_current_target( stalker ) == &you );
    auto &state = stalker.writhing_stalker_state();
    const int observation = state.evidence_turn;
    for( int y = 0; y < 132; ++y ) {
        here.ter_set( tripoint_bub_ms( 65, y, 0 ), ter_id( "t_wall" ) );
    }
    set_time( calendar::turn + 1_turns );
    refresh_writhing_stalker_arena( here );
    REQUIRE_FALSE( writhing_stalker::can_observe_actor( stalker, you ) );
    stalker.plan();
    REQUIRE( stalker.has_dest() );
    CHECK( rl_dist( stalker.pos_abs(), stalker.get_dest() ) <= 10 );
    CHECK( stalker.get_dest().x() > stalker.pos_abs().x() );
    const auto before = stalker.pos_abs();
    stalker.set_moves( 100 );
    stalker.move();
    CHECK( stalker.pos_abs() != before );
    CHECK( state.evidence_turn == observation );
    CHECK( state.searched_positions == 0 );

    // Search can travel through intermediate native points without claiming
    // an unreached OMT, and its absolute active deadline is not renewed.
    set_time( calendar::turn + 61_turns );
    stalker.plan();
    const int search_start = state.search_started_active;
    REQUIRE( search_start >= 0 );
    CHECK( state.searched_positions == 0 );
    set_time( calendar::turn + 30_minutes );
    stalker.plan();
    CHECK( state.phase == writhing_stalker::lifecycle_phase::idle );
    CHECK( state.search_started_active == -1 );
    CHECK( state.evidence_turn == observation );
    CHECK_FALSE( state.has_committed_waypoint );
    clear_map_without_vision();
}

TEST_CASE( "stalker_pursuit_native_flight_preempts_visible_and_remembered_approach",
           "[writhing_stalker][pursuit][monster]" )
{
    clear_map_without_vision();
    clear_avatar();
    map &here = get_map();
    Character &you = get_player_character();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time_to_day();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 5 );
    stalker.anger = 100;
    stalker.aggro_character = true;
    refresh_writhing_stalker_arena( here );
    REQUIRE( writhing_stalker::observe_current_target( stalker ) == &you );
    auto &state = stalker.writhing_stalker_state();
    state.begin_search();

    SECTION( "current visible threat retains the native away destination" ) {
        stalker.add_effect( efftype_id( "run" ), 5_minutes );
        REQUIRE( stalker.is_fleeing( you ) );
        stalker.plan();
        REQUIRE( stalker.has_dest() );
        CHECK( stalker.get_dest() == stalker.pos_abs() - you.pos_abs() + stalker.pos_abs() );
        CHECK( state.searched_positions == 0 );
        CHECK( state.attempts_spent == 0 );
    }
    SECTION( "hidden threat does not resume a remembered search during flight" ) {
        here.ter_set( center + point::east * 2, ter_id( "t_wall" ) );
        refresh_writhing_stalker_arena( here );
        REQUIRE_FALSE( writhing_stalker::can_observe_actor( stalker, you ) );
        set_time( calendar::turn + 1_turns );
        stalker.plan();
        REQUIRE( stalker.has_dest() );
        REQUIRE( state.has_committed_waypoint );
        const auto approach = state.committed_waypoint;
        stalker.add_effect( efftype_id( "run" ), 5_minutes );
        REQUIRE( stalker.is_fleeing( you ) );
        const auto cursor = state.search_cursor;
        set_time( calendar::turn + 1_turns );
        stalker.plan();
        CHECK_FALSE( stalker.has_dest() );
        CHECK( state.committed_waypoint == approach );
        CHECK( state.search_cursor == cursor );
        CHECK( state.searched_positions == 0 );
    }
    clear_map_without_vision();
}

TEST_CASE( "stalker_pursuit_native_opening_requires_dark_solitude_or_witnessed_attack",
           "[writhing_stalker][pursuit][monster][counterpressure]" )
{
    clear_map_without_vision();
    clear_avatar();
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center( 65, 65, 0 );
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    set_time_to_day();
    scoped_weather_override weather( weather_type_id( "clear" ) );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 3 );
    stalker.anger = 100;
    stalker.aggro_character = true;
    std::vector<monster *> zombies;
    for( const point &offset : { point::west, point::south, point::east, point::south_west } ) {
        auto &zombie = spawn_test_monster( mon_zombie.str(), center + offset );
        zombie.aggro_character = true;
        zombie.set_dest( you.pos_abs() );
        REQUIRE( zombie.attack_target() == &you );
        zombies.push_back( &zombie );
    }
    refresh_writhing_stalker_arena( here );
    REQUIRE( stalker.sees( here, you ) );
    REQUIRE( writhing_stalker::observed_zombie_pressure( stalker, here, you ) == 4 );
    stalker.plan();
    auto &state = stalker.writhing_stalker_state();
    REQUIRE( state.has_pursuit_memory );
    // Four merely closing zombies are not an actual bright-light opening.
    CHECK( state.phase != writhing_stalker::lifecycle_phase::attacking );
    CHECK( state.attempts_spent == 0 );

    SECTION( "a real visible attack resolution supplies the missing predicate" ) {
        CHECK( you.deal_melee_attack( zombies.front(), -1000 ) < 0 );
        REQUIRE( writhing_stalker::has_recent_observed_attack( &stalker, zombies.front(), &you,
                 to_turn<int>( calendar::turn ), 0 ) );
        set_time( calendar::turn + 1_turns );
        stalker.plan();
        CHECK( state.phase == writhing_stalker::lifecycle_phase::attacking );
        CHECK( stalker.get_dest() == you.pos_abs() );
        CHECK( state.attempts_spent == 0 ); // Planning does not spend an attack.
    }
    SECTION( "a target-directed attack hidden behind a wall is not witnessed" ) {
        const int observation = state.evidence_turn;
        here.ter_set( center + point::east * 2, ter_id( "t_wall" ) );
        refresh_writhing_stalker_arena( here );
        CHECK( you.deal_melee_attack( zombies.front(), -1000 ) < 0 );
        CHECK_FALSE( writhing_stalker::has_recent_observed_attack( &stalker, zombies.front(), &you,
                     to_turn<int>( calendar::turn ), 0 ) );
        set_time( calendar::turn + 1_turns );
        stalker.plan();
        CHECK( state.evidence_turn == observation );
        CHECK( state.phase != writhing_stalker::lifecycle_phase::attacking );
    }
    SECTION( "dark solitude supplies an opening without inventing an attack" ) {
        set_time( calendar::turn - time_past_midnight( calendar::turn ) + 1_days );
        for( const auto *zombie : zombies ) {
            CHECK_FALSE( writhing_stalker::has_recent_observed_attack( &stalker, zombie, &you,
                         to_turn<int>( calendar::turn ), 2 ) );
        }
        stalker.plan();
        CHECK( state.phase == writhing_stalker::lifecycle_phase::attacking );
    }
    SECTION( "visible actual allies remove solitude even in darkness" ) {
        for( const point &offset : { point( 2, 2 ), point( -2, 2 ), point( 0, 3 ) } ) {
            npc &ally = spawn_npc( point_bub_ms( ( center + offset ).xy() ), "test_talker" );
            ally.set_fac( faction_id( "your_followers" ) );
            ally.set_attitude( NPCATT_FOLLOW );
        }
        set_time( calendar::turn - time_past_midnight( calendar::turn ) + 1_days );
        refresh_writhing_stalker_arena( here );
        stalker.plan();
        CHECK( state.phase != writhing_stalker::lifecycle_phase::attacking );
    }
    clear_map_without_vision();
}

TEST_CASE( "stalker_real_horde_scheduler_retains_arrival_search_and_revisit",
           "[writhing_stalker][predator][pursuit][scheduler]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 161_turns );
    map &here = get_map();
    const auto learned = project_to<coords::omt>( here.get_abs( tripoint_bub_ms( -36, 65, 0 ) ) );
    const auto center = project_to<coords::ms>( learned ) + point( 12, 12 );
    const auto source = center + point::east;
    REQUIRE_FALSE( here.inbounds( source ) );
    overmap_buffer.get( project_to<coords::om>( source ).xy() );
    for( int x = -60; x <= 60; ++x ) {
        for( int y = -60; y <= 60; ++y ) {
            overmap_buffer.set_passable( center + point( x, y ), true );
        }
    }
    monster original( mon_writhing_stalker );
    original.set_pos_abs_only( source );
    original.set_moves( 100 );
    auto &memory = original.writhing_stalker_state();
    // An authenticated sighting fixture feeds the real scheduler, not a
    // direct advance_predator_intent call that bypasses bucket admission.
    memory.advance_to( 100 );
    REQUIRE( memory.observe_target( 77, 1, center, 100 ) );
    memory.advance_to( 161 );
    memory.begin_search();
    original.set_dest( center );
    const auto actor = original.predator_state().actor_id;
    overmap &om = overmap_buffer.get( project_to<coords::om>( source ).xy() );
    om.hordes.clear();
    REQUIRE( om.hordes.spawn_entity( source, original ) );
    overmap_buffer.move_hordes();
    horde_entity *owner = overmap_buffer.entity_at( center );
    REQUIRE( owner != nullptr );
    REQUIRE( owner->monster_data != nullptr );
    REQUIRE( owner->tracking_intensity == 0 );
    REQUIRE( owner->monster_data->writhing_stalker_state().search_cursor == 0 );
    const int arrival_turn = owner->monster_data->writhing_stalker_state().last_advanced_turn;
    set_time( calendar::turn + 1_turns );
    overmap_buffer.move_hordes();
    // Arrival must reach the planner again even without a sound/light alert.
    horde_entity *continued = nullptr;
    for( const auto &entry : om.horde_entities() ) {
        if( entry.second->monster_data &&
            entry.second->monster_data->predator_state().actor_id == actor ) {
            continued = entry.second;
        }
    }
    REQUIRE( continued != nullptr );
    const auto &after = continued->monster_data->writhing_stalker_state();
    CHECK( after.last_advanced_turn > arrival_turn );
    CHECK( after.search_cursor >= 1 );
    CHECK( ( after.searched_positions & 1 ) != 0 );
    set_time( calendar::turn + 31_minutes );
    overmap_buffer.move_hordes();
    continued = nullptr;
    tripoint_abs_ms position;
    for( const auto &entry : om.horde_entities() ) {
        if( entry.second->monster_data &&
            entry.second->monster_data->predator_state().actor_id == actor ) {
            continued = entry.second;
            position = entry.first;
        }
    }
    REQUIRE( continued != nullptr );
    auto &idle = continued->monster_data->writhing_stalker_state();
    REQUIRE( idle.phase == writhing_stalker::lifecycle_phase::idle );
    REQUIRE( idle.familiar[0].revisit_at_active > idle.active_turns );
    const auto due = idle.familiar[0].revisit_at_active;
    const auto until_due = due - idle.active_turns;
    const auto epoch = continued->monster_data->predator_state().handoff_epoch;
    const int seen = idle.evidence_turn;
    const bool reload = GENERATE( false, true );
    if( reload ) {
        *continued->monster_data = round_trip_writhing_stalker( *continued->monster_data );
        om.hordes.refresh_entity_bucket( position );
    }
    set_time( calendar::turn + time_duration::from_turns( until_due ) );
    overmap_buffer.move_hordes();
    continued = overmap_buffer.entity_at( position );
    // A revisit may take a real transport step. Find its sole owner again.
    int owners = 0;
    for( const auto &entry : om.horde_entities() ) {
        if( entry.second->monster_data &&
            entry.second->monster_data->predator_state().actor_id == actor ) {
            continued = entry.second;
            ++owners;
        }
    }
    REQUIRE( continued != nullptr );
    CHECK( owners == 1 );
    const auto &revisit = continued->monster_data->writhing_stalker_state();
    CHECK( revisit.revisiting_slot == 0 );
    CHECK( revisit.evidence_turn == seen );
    CHECK( continued->monster_data->predator_state().handoff_epoch == epoch );
    const auto active = revisit.active_turns;
    overmap_buffer.move_hordes();
    CHECK( continued->monster_data->writhing_stalker_state().active_turns == active );
    om.hordes.clear();
    clear_map_without_vision();
}

TEST_CASE( "stalker_transferred_route_fallback_ends_with_pursuit_but_real_sound_survives",
           "[writhing_stalker][predator][pursuit][fallback]" )
{
    clear_map_without_vision();
    clear_avatar();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 12_hours );
    map &here = get_map();
    Character &you = get_player_character();
    scoped_weather_override weather( weather_type_id( "clear" ) );
    for( int x = 25; x < 90; ++x ) {
        for( int y = 45; y < 90; ++y ) {
            here.ter_set( tripoint_bub_ms( x, y, 0 ), ter_id( "t_grass" ) );
            here.ter_set( tripoint_bub_ms( x, y, 1 ), ter_id( "t_open_air" ) );
            here.furn_clear( tripoint_bub_ms( x, y, 0 ) );
        }
    }
    you.setpos( here, tripoint_bub_ms( 80, 66, 0 ) );
    refresh_writhing_stalker_arena( here );
    const bool retreat = GENERATE( false, true );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(),
                       tripoint_bub_ms( retreat ? 77 : 30, 66, 0 ) );
    stalker.anger = 100;
    stalker.aggro_character = true;
    REQUIRE( writhing_stalker::observe_current_target( stalker ) == &you );
    const int observed = stalker.writhing_stalker_state().evidence_turn;
    const auto actor = stalker.predator_state().actor_id;
    const auto epoch = stalker.predator_state().handoff_epoch;
    if( retreat ) {
        // A consumed native attack budget selects a real escape destination.
        stalker.writhing_stalker_state().attempts_spent = 2;
        stalker.plan();
        REQUIRE( stalker.writhing_stalker_state().has_retreat_waypoint );
    }
    for( int y = 0; y < 132; ++y ) {
        here.ter_set( tripoint_bub_ms( retreat ? 79 : 65, y, 0 ), ter_id( "t_wall" ) );
    }
    set_time( calendar::turn + 1_turns );
    refresh_writhing_stalker_arena( here );
    REQUIRE_FALSE( writhing_stalker::can_observe_actor( stalker, you ) );
    stalker.plan();
    REQUIRE( stalker.has_dest() );
    const auto source = stalker.pos_abs();
    overmap_buffer.get( project_to<coords::om>( source ).xy() ).hordes.clear();
    g->despawn_monster( stalker );
    REQUIRE( overmap_buffer.entity_at( source ) != nullptr );
    set_time( calendar::turn + 1_turns );
    overmap_buffer.move_hordes();
    monster *local = nullptr;
    for( monster &candidate : g->all_monsters() ) {
        if( candidate.predator_state().actor_id == actor ) {
            local = &candidate;
        }
    }
    REQUIRE( local != nullptr );
    REQUIRE( local->predator_state().handoff_epoch == epoch + 2 );
    REQUIRE( local->wandf > 1 );
    const auto &transported = local->writhing_stalker_state();
    REQUIRE( local->wander_pos == ( retreat ? transported.retreat_waypoint :
                                   transported.committed_waypoint ) );
    const int sound = GENERATE( 0, 1, 2 );
    const bool reload = GENERATE( false, true );
    tripoint_abs_ms sound_position;
    if( sound ) {
        local->morale = 20;
        // A real sound at the same coordinate is independent evidence too.
        const auto sound_source = sound == 2 ? here.get_bub( local->wander_pos ) :
                                  local->pos_bub() + point::south * 4;
        local->hear_sound( sound_source, 2000, 4, true );
        sound_position = here.get_abs( sound_source );
        REQUIRE( local->wander_pos == sound_position );
        REQUIRE( local->provocative_sound );
    }
    if( reload ) {
        *local = round_trip_writhing_stalker( *local );
    }
    set_time( calendar::turn + 32_minutes );
    local->plan();
    // Delayed cooldown recovery can enter search here. Let that real search
    // finish without inventing an arrival or changing its elapsed budget.
    set_time( calendar::turn + 31_minutes );
    local->plan();
    REQUIRE( local->writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::idle );
    CHECK( local->writhing_stalker_state().evidence_turn == observed );
    if( sound ) {
        CHECK( local->wandf > 0 );
        CHECK( local->wander_pos == sound_position );
    } else {
        CHECK( local->wandf == 0 );
        CHECK( horde_entity( *local ).tracking_intensity == 0 );
    }
    const auto before = local->pos_abs();
    local->set_moves( 100 );
    local->move();
    if( sound ) {
        CHECK( rl_dist( local->pos_abs(), sound_position ) < rl_dist( before, sound_position ) );
    } else {
        CHECK( local->wandf == 0 );
    }
    clear_map_without_vision();
}

TEST_CASE( "stalker_idle_memory_offload_schedules_without_fabricated_movement_and_eventually_forgets",
           "[writhing_stalker][predator][pursuit][scheduler]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 100_turns );
    const auto source = get_map().get_abs( tripoint_bub_ms( -36, 65, 0 ) );
    overmap &om = overmap_buffer.get( project_to<coords::om>( source ).xy() );
    om.hordes.clear();
    monster original( mon_writhing_stalker );
    original.set_pos_abs_only( source );
    CHECK_FALSE( horde_entity( original ).is_active() );
    auto &memory = original.writhing_stalker_state();
    memory.advance_to( 100 );
    REQUIRE( memory.observe_target( 77, 1, source + point::east * 3, 100 ) );
    memory.finish_search();
    const auto due = memory.familiar[0].revisit_at_active;
    const auto actor = original.predator_state().actor_id;
    REQUIRE_FALSE( original.has_dest() );
    REQUIRE( original.wandf == 0 );
    REQUIRE( om.hordes.spawn_entity( source, original ) );
    auto *owner = overmap_buffer.entity_at( source );
    REQUIRE( owner != nullptr );
    CHECK( owner->is_active() );
    CHECK( owner->tracking_intensity == 0 );
    set_time( calendar::turn + 1_turns );
    overmap_buffer.move_hordes();
    owner = overmap_buffer.entity_at( source );
    REQUIRE( owner != nullptr );
    CHECK( owner->monster_data->writhing_stalker_state().active_turns == 1 );
    CHECK( owner->tracking_intensity == 0 );
    CHECK( owner->monster_data->wandf == 0 );
    CHECK( owner->monster_data->predator_state().actor_id == actor );
    CHECK( owner->monster_data->writhing_stalker_state().familiar[0].revisit_at_active == due );
    // The lifecycle's finite familiar memory, not permanent has_memory,
    // controls scheduling. Forgotten places do not leave a forever-active actor.
    set_time( calendar::turn + 7_days );
    overmap_buffer.move_hordes();
    set_time( calendar::turn + 1_turns );
    overmap_buffer.move_hordes();
    owner = overmap_buffer.entity_at( source );
    REQUIRE( owner != nullptr );
    CHECK_FALSE( owner->is_active() );
    CHECK_FALSE( owner->monster_data->writhing_stalker_state().familiar[0].valid );
    CHECK( owner->tracking_intensity == 0 );
    CHECK( owner->monster_data->writhing_stalker_state().evidence_turn == 100 );
    om.hordes.clear();
    clear_map_without_vision();
}

TEST_CASE( "stalker_idle_abstract_sound_survives_pending_familiar_revisit",
           "[writhing_stalker][predator][pursuit][scheduler][signal]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    const auto source = get_map().get_abs( tripoint_bub_ms( -36, 65, 0 ) );
    const auto sound = source + point::east * 5;
    overmap &om = overmap_buffer.get( project_to<coords::om>( source ).xy() );
    om.hordes.clear();
    for( int x = 0; x <= 5; ++x ) {
        overmap_buffer.set_passable( source + point::east * x, true );
    }
    monster original( mon_writhing_stalker );
    original.set_pos_abs_only( source );
    original.set_moves( 100 );
    const bool learned = GENERATE( false, true );
    if( learned ) {
        auto &state = original.writhing_stalker_state();
        state.advance_to( 100 );
        REQUIRE( state.observe_target( 77, 1, sound, 100 ) );
        state.finish_search();
        state.advance_to( 1000 );
        REQUIRE( state.familiar[0].revisit_at_active > state.active_turns );
    }
    const auto actor = original.predator_state().actor_id;
    REQUIRE( om.hordes.spawn_entity( source, original ) );
    // The actual horde sound owner assigns this independent order. No
    // tracking field is patched after intent dispatch.
    om.hordes.signal_entities( sound, 20 );
    auto *owner = overmap_buffer.entity_at( source );
    REQUIRE( owner != nullptr );
    REQUIRE( owner->destination == sound );
    REQUIRE( owner->tracking_intensity > 0 );
    const bool reload = GENERATE( false, true );
    if( reload ) {
        *owner->monster_data = round_trip_writhing_stalker( *owner->monster_data );
        om.hordes.refresh_entity_bucket( source );
    }
    overmap_buffer.move_hordes();
    owner = overmap_buffer.entity_at( source + point::east );
    CHECK( owner != nullptr );
    if( owner ) {
        CHECK( owner->destination == sound );
        CHECK( owner->tracking_intensity > 0 );
        CHECK( owner->monster_data->predator_state().actor_id == actor );
        if( learned ) {
            CHECK( owner->monster_data->writhing_stalker_state().evidence_turn == 100 );
            CHECK( owner->monster_data->writhing_stalker_state().phase ==
                   writhing_stalker::lifecycle_phase::idle );
        }
        const auto active = owner->monster_data->writhing_stalker_state().active_turns;
        overmap_buffer.move_hordes();
        CHECK( overmap_buffer.entity_at( source + point::east ) == owner );
        CHECK( owner->monster_data->writhing_stalker_state().active_turns == active );
    }
    om.hordes.clear();
    clear_map_without_vision();
}

TEST_CASE( "stalker_abstract_retreat_arrival_and_recovery_are_finite",
           "[writhing_stalker][predator][pursuit][scheduler][recovery]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    const auto source = get_map().get_abs( tripoint_bub_ms( -36, 65, 0 ) );
    const auto retreat = source + point::east;
    overmap &om = overmap_buffer.get( project_to<coords::om>( source ).xy() );
    om.hordes.clear();
    for( int x = -40; x <= 40; ++x ) {
        for( int y = -40; y <= 40; ++y ) {
            overmap_buffer.set_passable( source + point( x, y ), true );
        }
    }
    monster original( mon_writhing_stalker );
    original.set_pos_abs_only( source );
    original.set_moves( 100 );
    auto &state = original.writhing_stalker_state();
    state.advance_to( 100 );
    REQUIRE( state.observe_target( 77, 1, source + point::west * 3, 100 ) );
    state.advance_to( 1000 );
    // Native withdraw/cooling-off payloads are already covered by the local
    // caller controls. This fixture follows their real abstract arrival.
    const bool cooling = GENERATE( false, true );
    state.phase = cooling ? writhing_stalker::lifecycle_phase::cooldown :
                  writhing_stalker::lifecycle_phase::retreating;
    state.retreat_waypoint = retreat;
    state.has_retreat_waypoint = true;
    state.committed_waypoint = source + point::west * 5;
    state.has_committed_waypoint = true;
    state.cooldown_until_turn = 1060;
    state.attempts_spent = 2;
    original.set_dest( retreat );
    const auto actor = original.predator_state().actor_id;
    REQUIRE( om.hordes.spawn_entity( source, original ) );
    overmap_buffer.move_hordes();
    auto *owner = overmap_buffer.entity_at( retreat );
    REQUIRE( owner != nullptr );
    REQUIRE( owner->tracking_intensity == 0 );
    const bool reload = GENERATE( false, true );
    if( reload ) {
        *owner->monster_data = round_trip_writhing_stalker( *owner->monster_data );
        om.hordes.refresh_entity_bucket( retreat );
    }
    set_time( calendar::turn + 1_turns );
    overmap_buffer.move_hordes();
    owner = overmap_buffer.entity_at( retreat );
    REQUIRE( owner != nullptr );
    CHECK( owner->monster_data->writhing_stalker_state().phase ==
           writhing_stalker::lifecycle_phase::cooldown );
    CHECK( owner->tracking_intensity == 0 );
    CHECK( owner->monster_data->writhing_stalker_state().cooldown_until_turn == 1060 );
    const bool delayed = GENERATE( false, true );
    set_time( calendar::turn_zero + ( delayed ? 3000_turns : 1061_turns ) );
    overmap_buffer.move_hordes();
    owner = nullptr;
    for( const auto &entry : om.horde_entities() ) {
        if( entry.second->monster_data &&
            entry.second->monster_data->predator_state().actor_id == actor ) {
            owner = entry.second;
        }
    }
    REQUIRE( owner != nullptr );
    auto &recovered = owner->monster_data->writhing_stalker_state();
    CHECK( recovered.phase != writhing_stalker::lifecycle_phase::retreating );
    CHECK( recovered.phase != writhing_stalker::lifecycle_phase::cooldown );
    CHECK( recovered.cooldown_until_turn == -1 );
    if( delayed ) {
        CHECK( recovered.phase == writhing_stalker::lifecycle_phase::idle );
    }
    const auto active = recovered.active_turns;
    const auto search_start = recovered.search_started_active;
    overmap_buffer.move_hordes();
    CHECK( owner->monster_data->writhing_stalker_state().active_turns == active );
    CHECK( owner->monster_data->writhing_stalker_state().search_started_active == search_start );
    set_time( calendar::turn + 31_minutes );
    overmap_buffer.move_hordes();
    owner = nullptr;
    for( const auto &entry : om.horde_entities() ) {
        if( entry.second->monster_data &&
            entry.second->monster_data->predator_state().actor_id == actor ) {
            owner = entry.second;
        }
    }
    REQUIRE( owner != nullptr );
    CHECK( owner->monster_data->writhing_stalker_state().phase ==
           writhing_stalker::lifecycle_phase::idle );
    CHECK( owner->tracking_intensity == 0 );
    CHECK( owner->monster_data->writhing_stalker_state().evidence_turn == 100 );
    CHECK( owner->monster_data->writhing_stalker_state().familiar[0].revisit_at_active >
           owner->monster_data->writhing_stalker_state().active_turns );
    om.hordes.clear();
    clear_map_without_vision();
}

TEST_CASE( "stalker_abstract_light_arrival_expires_without_replacing_new_sound",
           "[writhing_stalker][predator][pursuit][scheduler][signal][light_transition]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    const auto source = get_map().get_abs( tripoint_bub_ms( -36, 65, 0 ) );
    const auto light = source + point::east;
    overmap &om = overmap_buffer.get( project_to<coords::om>( source ).xy() );
    om.hordes.clear();
    for( int x = -5; x <= 5; ++x ) {
        for( int y = -5; y <= 5; ++y ) {
            overmap_buffer.set_passable( source + point( x, y ), true );
        }
    }
    monster original( mon_writhing_stalker );
    original.set_pos_abs_only( source );
    original.set_moves( 100 );
    auto &state = original.writhing_stalker_state();
    state.advance_to( 100 );
    const bool learned = GENERATE( false, true );
    if( learned ) {
        REQUIRE( state.observe_target( 77, 1, source + point::west * 3, 100 ) );
        state.finish_search();
    }
    state.advance_to( 1000 );
    const auto actor = original.predator_state().actor_id;
    REQUIRE( om.hordes.spawn_entity( source, original ) );
    const bool blocked = GENERATE( false, true );
    if( blocked ) {
        for( const auto &candidate : squares_closer_to( source, light ) ) {
            overmap_buffer.set_passable( candidate, false );
        }
    }
    // A physical-light adapter packet fixture feeds the production owner;
    // this does not claim a native fire or newly observed target.
    REQUIRE( om.hordes.attract_entities_to_light( light, 6, 20, "abstract-arrival-light",
             calendar::turn, []( const tripoint_abs_ms & ) { return true; }, nullptr ) == 1 );
    overmap_buffer.move_hordes();
    const auto arrival = blocked ? source : light;
    auto *owner = overmap_buffer.entity_at( arrival );
    REQUIRE( owner != nullptr );
    const bool reload = GENERATE( false, true );
    if( reload ) {
        *owner->monster_data = round_trip_writhing_stalker( *owner->monster_data );
        om.hordes.refresh_entity_bucket( arrival );
    }
    set_time( calendar::turn + 1_turns );
    overmap_buffer.move_hordes();
    owner = overmap_buffer.entity_at( arrival );
    REQUIRE( owner != nullptr );
    const auto &arrived = owner->monster_data->writhing_stalker_state();
    if( !blocked ) {
        CHECK( arrived.phase == writhing_stalker::lifecycle_phase::searching );
        CHECK( arrived.search_until_turn == 1021 );
        CHECK( owner->tracking_intensity == 0 );
    }
    set_time( calendar::turn_zero + 1022_turns );
    overmap_buffer.move_hordes();
    owner = overmap_buffer.entity_at( arrival );
    REQUIRE( owner != nullptr );
    CHECK( owner->monster_data->writhing_stalker_state().phase ==
           writhing_stalker::lifecycle_phase::idle );
    CHECK_FALSE( owner->monster_data->writhing_stalker_state().has_committed_waypoint );
    CHECK( owner->tracking_intensity == 0 );
    CHECK( owner->is_active() == learned );
    const int finished_turn = owner->monster_data->writhing_stalker_state().last_advanced_turn;
    set_time( calendar::turn_zero + 1200_turns );
    overmap_buffer.move_hordes();
    owner = overmap_buffer.entity_at( arrival );
    REQUIRE( owner != nullptr );
    if( !learned ) {
        // The actual scheduler must stop admitting this finite legacy job.
        CHECK( owner->monster_data->writhing_stalker_state().last_advanced_turn == finished_turn );
        CHECK_FALSE( owner->is_active() );
    }
    for( int x = -5; x <= 5; ++x ) {
        for( int y = -5; y <= 5; ++y ) {
            overmap_buffer.set_passable( source + point( x, y ), true );
        }
    }
    const auto sound = arrival + point::south * 3;
    om.hordes.signal_entities( sound, 20 );
    set_time( calendar::turn + 1_turns );
    overmap_buffer.move_hordes();
    owner = overmap_buffer.entity_at( arrival + point::south );
    CHECK( owner != nullptr );
    if( owner ) {
        CHECK( owner->destination == sound );
        CHECK( owner->monster_data->predator_state().actor_id == actor );
        CHECK( owner->monster_data->writhing_stalker_state().has_pursuit_memory == learned );
        if( learned ) {
            CHECK( owner->monster_data->writhing_stalker_state().evidence_turn == 100 );
        }
    }
    om.hordes.clear();
    clear_map_without_vision();
}

TEST_CASE( "stalker_recovery_budget_survives_visible_reacquisition_before_local_combat",
           "[writhing_stalker][predator][pursuit][recovery]" )
{
    clear_map_without_vision();
    clear_avatar();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero );
    set_time( calendar::turn - time_past_midnight( calendar::turn ) + 1_days );
    scoped_weather_override weather( weather_type_id( "clear" ) );
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    refresh_writhing_stalker_arena( here );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 2 );
    stalker.anger = 100;
    stalker.aggro_character = true;
    stalker.writhing_stalker_state().attempts_spent = 2;
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().phase ==
             writhing_stalker::lifecycle_phase::retreating );
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().phase ==
             writhing_stalker::lifecycle_phase::cooldown );
    const int recovery = stalker.writhing_stalker_state().cooldown_until_turn;
    // Native withdrawal starts a short run effect. Let ordinary body turns
    // expire it before testing recognition after the longer recovery.
    for( int turn = 0; turn < 8; ++turn ) {
        set_time( calendar::turn + 1_turns );
        stalker.process_turn();
    }
    const auto actor = stalker.predator_state().actor_id;
    const bool abstract_owner = GENERATE( false, true );
    set_time( calendar::turn_zero + time_duration::from_turns( recovery + 1 ) );
    INFO( "reacquire anger=" << stalker.anger << " morale=" << stalker.morale <<
          " attitude=" << static_cast<int>( stalker.attitude_to( you ) ) <<
          " run=" << stalker.has_effect( efftype_id( "run" ) ) <<
          " fleeing=" << stalker.is_fleeing( you ) <<
          " physical=" << writhing_stalker::can_observe_actor( stalker, you ) );
    if( abstract_owner ) {
        // The actual abstract caller advances recovery then immediately
        // reacquires the physically visible character before local combat.
        horde_entity transferred( stalker );
        REQUIRE( transferred.advance_predator_intent( stalker.pos_abs(), recovery + 1 ) );
        stalker = *transferred.monster_data;
    } else {
        REQUIRE( writhing_stalker::observe_current_target( stalker ) == &you );
    }
    REQUIRE( stalker.writhing_stalker_state().evidence_turn == recovery + 1 );
    REQUIRE( stalker.writhing_stalker_state().search_started_active == -1 );
    REQUIRE( stalker.writhing_stalker_state().search_until_turn == -1 );
    CHECK( stalker.writhing_stalker_state().attempts_spent == 0 );
    const bool reload = GENERATE( false, true );
    if( reload ) {
        stalker = round_trip_writhing_stalker( stalker );
    }
    set_time( calendar::turn_zero + time_duration::from_turns( recovery + 21 ) );
    // Supply the existing actual same-target pressure/witnessed-attack
    // opening. A healthy avatar alone may truthfully receive only shadow.
    std::vector<monster *> zombies;
    for( const point &offset : { point::west, point::south, point::east, point::south_west } ) {
        monster &zombie = spawn_test_monster( mon_zombie.str(), center + offset );
        zombie.aggro_character = true;
        zombie.set_dest( you.pos_abs() );
        REQUIRE( zombie.attack_target() == &you );
        zombies.push_back( &zombie );
    }
    refresh_writhing_stalker_arena( here );
    REQUIRE( writhing_stalker::observed_zombie_pressure( stalker, here, you ) == 4 );
    CHECK( you.deal_melee_attack( zombies.front(), -1000 ) < 0 );
    REQUIRE( writhing_stalker::has_recent_observed_attack( &stalker, zombies.front(), &you,
             to_turn<int>( calendar::turn ), 0 ) );
    stalker.plan();
    CHECK( stalker.writhing_stalker_state().attempts_spent == 0 );
    CHECK( stalker.writhing_stalker_state().search_until_turn == -1 );
    CHECK( stalker.writhing_stalker_state().phase ==
           writhing_stalker::lifecycle_phase::attacking );
    CHECK( stalker.has_dest() );
    CHECK( stalker.get_dest() == you.pos_abs() );
    CHECK( stalker.predator_state().actor_id == actor );
    stalker.plan();
    CHECK( stalker.writhing_stalker_state().attempts_spent == 0 );
    // Execute real native combat after recovery, then prove the already
    // consumed deadline cannot erase the new burst's actual attempts.
    const auto sequence = stalker.writhing_stalker_state().attempt_sequence;
    for( int step = 0; step < 6 && stalker.writhing_stalker_state().attempts_spent == 0; ++step ) {
        set_time( calendar::turn + 1_turns );
        stalker.plan();
        stalker.set_moves( 100 );
        stalker.move();
    }
    const auto spent = stalker.writhing_stalker_state().attempts_spent;
    REQUIRE( spent > 0 );
    CHECK( stalker.writhing_stalker_state().attempt_sequence == sequence + spent );
    const bool reload_after_attack = GENERATE( false, true );
    if( reload_after_attack ) {
        stalker = round_trip_writhing_stalker( stalker );
    }
    set_time( calendar::turn + 1_turns );
    stalker.plan();
    CHECK( stalker.writhing_stalker_state().attempts_spent == spent );
    CHECK( stalker.writhing_stalker_state().search_until_turn == -1 );
    clear_map_without_vision();
}

TEST_CASE( "stalker_local_unlearned_light_expiry_retires_native_route_preserving_sound",
           "[writhing_stalker][predator][pursuit][light_transition][fallback]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    map &here = get_map();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    // Arena setup resets the avatar; keep this light-only control physically
    // separated from character recognition after that setup.
    get_player_character().setpos( here, tripoint_bub_ms( 65, 65, -2 ) );
    refresh_writhing_stalker_arena( here );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center );
    stalker.anger = 100;
    stalker.morale = 20;
    const auto light = stalker.pos_abs() + point::east * 6;
    auto &state = stalker.writhing_stalker_state();
    state.advance_to( 1000 );
    // Accepted physical-light adapter fixture; actual local planning and
    // native movement below judge executable route ownership, not a fire.
    REQUIRE( state.observe_light_interest( light, "local-unlearned-light", 1000, 5 ) );
    stalker.plan();
    REQUIRE_FALSE( state.has_pursuit_memory );
    REQUIRE( stalker.has_dest() );
    REQUIRE( stalker.get_dest() == light );
    const int sound = GENERATE( 0, 1, 2 );
    tripoint_abs_ms heard;
    if( sound ) {
        heard = sound == 2 ? light : stalker.pos_abs() + point::south * 6;
        stalker.hear_sound( here.get_bub( heard ), 2000, 6, true );
        REQUIRE( stalker.provocative_sound );
        REQUIRE( stalker.wander_pos == heard );
    }
    const bool reload = GENERATE( false, true );
    if( reload ) {
        stalker = round_trip_writhing_stalker( stalker );
    }
    set_time( calendar::turn_zero + 1006_turns );
    stalker.plan();
    CHECK( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::idle );
    CHECK_FALSE( stalker.writhing_stalker_state().has_committed_waypoint );
    CHECK_FALSE( ( stalker.has_dest() && stalker.get_dest() == light ) );
    const auto before = stalker.pos_abs();
    stalker.set_moves( 100 );
    stalker.move();
    if( sound ) {
        CHECK( stalker.provocative_sound );
        CHECK( stalker.wander_pos == heard );
        CHECK( rl_dist( stalker.pos_abs(), heard ) < rl_dist( before, heard ) );
    } else {
        CHECK( stalker.pos_abs() == before );
        CHECK( stalker.wandf == 0 );
    }
    CHECK_FALSE( stalker.writhing_stalker_state().has_pursuit_memory );
    clear_map_without_vision();
}

TEST_CASE( "stalker_legacy_light_search_acquisition_cannot_reset_a_new_native_burst",
           "[writhing_stalker][predator][pursuit][light_transition][recovery]" )
{
    clear_map_without_vision();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    scoped_weather_override weather( weather_type_id( "clear" ) );
    map &here = get_map();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    Character &you = get_player_character();
    you.setpos( here, tripoint_bub_ms( 65, 65, -2 ) );
    refresh_writhing_stalker_arena( here );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center );
    stalker.anger = 100;
    stalker.aggro_character = true;
    auto &state = stalker.writhing_stalker_state();
    state.advance_to( 1000 );
    // Actual local arrival starts a legacy light search without prey memory.
    REQUIRE( state.observe_light_interest( stalker.pos_abs(), "light-before-first-prey", 1000, 90 ) );
    stalker.plan();
    REQUIRE_FALSE( state.has_pursuit_memory );
    REQUIRE( state.phase == writhing_stalker::lifecycle_phase::searching );
    const int old_light_deadline = state.search_until_turn;
    REQUIRE( old_light_deadline == 1020 );
    you.setpos( here, center + point::west * 2 );
    refresh_writhing_stalker_arena( here );
    set_time( calendar::turn + 1_turns );
    REQUIRE( writhing_stalker::observe_current_target( stalker ) == &you );
    CHECK( state.search_until_turn == -1 );
    std::vector<monster *> zombies;
    for( const point &offset : { point::west, point::south, point::east, point::south_west } ) {
        monster &zombie = spawn_test_monster( mon_zombie.str(), you.pos_bub() + offset );
        zombie.aggro_character = true;
        zombie.set_dest( you.pos_abs() );
        REQUIRE( zombie.attack_target() == &you );
        zombies.push_back( &zombie );
    }
    refresh_writhing_stalker_arena( here );
    REQUIRE( writhing_stalker::observed_zombie_pressure( stalker, here, you ) == 4 );
    CHECK( you.deal_melee_attack( zombies.front(), -1000 ) < 0 );
    REQUIRE( writhing_stalker::has_recent_observed_attack( &stalker, zombies.front(), &you,
             to_turn<int>( calendar::turn ), 0 ) );
    stalker.plan();
    REQUIRE( state.phase == writhing_stalker::lifecycle_phase::attacking );
    for( int step = 0; step < 6 && state.attempts_spent == 0; ++step ) {
        set_time( calendar::turn + 1_turns );
        stalker.plan();
        stalker.set_moves( 100 );
        stalker.move();
    }
    const auto spent = state.attempts_spent;
    const auto sequence = state.attempt_sequence;
    REQUIRE( spent > 0 );
    REQUIRE( state.cooldown_until_turn == -1 );
    const bool reload = GENERATE( false, true );
    if( reload ) {
        stalker = round_trip_writhing_stalker( stalker );
    }
    set_time( calendar::turn_zero + time_duration::from_turns( old_light_deadline + 1 ) );
    stalker.plan();
    CHECK( state.attempts_spent == spent );
    CHECK( state.attempt_sequence == sequence );
    CHECK( state.search_until_turn == -1 );
    clear_map_without_vision();
}

TEST_CASE( "stalker_old_recovery_cannot_erase_an_early_native_burst",
           "[writhing_stalker][predator][pursuit][recovery]" )
{
    clear_map_without_vision();
    clear_avatar();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero );
    const int old_spent = GENERATE( 1, 2 );
    if( old_spent == 1 ) {
        set_time_to_day();
    } else {
        set_time( calendar::turn - time_past_midnight( calendar::turn ) + 1_days );
    }
    scoped_weather_override weather( weather_type_id( "clear" ) );
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    refresh_writhing_stalker_arena( here );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::north_east );
    stalker.anger = 100;
    stalker.aggro_character = true;
    stalker.writhing_stalker_state().attempts_spent = old_spent;
    stalker.plan();
    REQUIRE( ( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::retreating ||
               stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::cooldown ) );
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::cooldown );
    const int recovered_at = stalker.writhing_stalker_state().cooldown_until_turn;
    for( int turn = 0; turn < 8; ++turn ) {
        set_time( calendar::turn + 1_turns );
        stalker.process_turn();
    }
    set_time( calendar::turn_zero + time_duration::from_turns( recovered_at + 1 ) );
    // The opening changes after the original, smaller burst was spent.
    // Actual low stamina/bleeding adds vulnerability to the new opening.
    you.set_stamina( 0 );
    you.add_effect( efftype_id( "bleed" ), 1_hours, bodypart_id( "arm_l" ) );
    refresh_writhing_stalker_arena( here );
    const bool abstract_owner = GENERATE( false, true );
    if( abstract_owner ) {
        horde_entity owner( stalker );
        REQUIRE( owner.advance_predator_intent( stalker.pos_abs(), recovered_at + 1 ) );
        stalker = *owner.monster_data;
    } else {
        REQUIRE( writhing_stalker::observe_current_target( stalker ) == &you );
    }
    std::vector<monster *> zombies;
    for( const point &offset : { point::west, point::south, point::east, point::south_west } ) {
        monster &zombie = spawn_test_monster( mon_zombie.str(), center + offset );
        zombie.aggro_character = true;
        zombie.set_dest( you.pos_abs() );
        REQUIRE( zombie.attack_target() == &you );
        zombies.push_back( &zombie );
    }
    refresh_writhing_stalker_arena( here );
    REQUIRE( writhing_stalker::observed_zombie_pressure( stalker, here, you ) == 4 );
    CHECK( you.deal_melee_attack( zombies.front(), -1000 ) < 0 );
    REQUIRE( writhing_stalker::has_recent_observed_attack( &stalker, zombies.front(), &you,
             to_turn<int>( calendar::turn ), 0 ) );
    const auto sequence = stalker.writhing_stalker_state().attempt_sequence;
    for( int step = 0; step < 6 && stalker.writhing_stalker_state().attempt_sequence == sequence; ++step ) {
        // A real adjacent attack uses native combat, independent of path occupancy.
        stalker.plan();
        INFO( "old_spent=" << old_spent << " phase=" << static_cast<int>( stalker.writhing_stalker_state().phase ) );
        CHECK( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::attacking );
        stalker.set_moves( 100 );
        stalker.attack_at( you.pos_bub() );
        set_time( calendar::turn + 1_turns );
    }
    const auto fresh_attempts = stalker.writhing_stalker_state().attempt_sequence - sequence;
    REQUIRE( fresh_attempts > 0 );
    REQUIRE( to_turn<int>( calendar::turn ) < recovered_at + 20 );
    CHECK( static_cast<std::uint64_t>( stalker.writhing_stalker_state().attempts_spent ) == fresh_attempts );
    const bool reload = GENERATE( false, true );
    if( reload ) {
        stalker = round_trip_writhing_stalker( stalker );
    }
    set_time( calendar::turn_zero + time_duration::from_turns( recovered_at + 21 ) );
    stalker.plan();
    CHECK( static_cast<std::uint64_t>( stalker.writhing_stalker_state().attempts_spent ) == fresh_attempts );
    CHECK( stalker.writhing_stalker_state().attempt_sequence == sequence + fresh_attempts );
    clear_map_without_vision();
}

TEST_CASE( "stalker_hidden_recovery_cannot_own_a_later_withdrawal_toward_moved_prey",
           "[writhing_stalker][predator][pursuit][recovery][flight]" )
{
    clear_map_without_vision();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero );
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    refresh_writhing_stalker_arena( here );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 2 );
    stalker.anger = 100;
    stalker.aggro_character = true;
    stalker.writhing_stalker_state().attempts_spent = 2;
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().has_retreat_waypoint );
    const auto old_retreat = stalker.writhing_stalker_state().retreat_waypoint;
    you.setpos( here, tripoint_bub_ms( 65, 65, -2 ) );
    refresh_writhing_stalker_arena( here );
    for( int step = 0; step < 30 && stalker.pos_abs() != old_retreat; ++step ) {
        set_time( calendar::turn + 1_turns );
        stalker.process_turn();
        stalker.plan();
        stalker.set_moves( 100 );
        stalker.move();
    }
    REQUIRE( stalker.pos_abs() == old_retreat );
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::cooldown );
    const int recovered_at = stalker.writhing_stalker_state().cooldown_until_turn;
    for( int turn = 0; turn < 8; ++turn ) {
        set_time( calendar::turn + 1_turns );
        stalker.process_turn();
    }
    set_time( calendar::turn_zero + time_duration::from_turns( recovered_at + 1 ) );
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().search_started_active >= 0 );
    CHECK_FALSE( stalker.writhing_stalker_state().has_retreat_waypoint );
    const bool reload = GENERATE( false, true );
    if( reload ) {
        stalker = round_trip_writhing_stalker( stalker );
    }
    you.setpos( here, here.get_bub( old_retreat ) + point::west );
    refresh_writhing_stalker_arena( here );
    set_time( calendar::turn + 1_turns );
    REQUIRE( writhing_stalker::observe_current_target( stalker ) == &you );
    // Genuine native damage supplies a later withdrawal reason, independent
    // of either the old spent budget or the completed retreat episode.
    stalker.apply_damage( &you, bodypart_id( "torso" ), stalker.get_hp() / 2 );
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::retreating );
    REQUIRE( stalker.has_dest() );
    CHECK( stalker.get_dest() != old_retreat );
    CHECK( rl_dist( stalker.get_dest(), you.pos_abs() ) >= 8 );
    const int separation = rl_dist( stalker.pos_abs(), you.pos_abs() );
    const auto sequence = stalker.writhing_stalker_state().attempt_sequence;
    // Native stumbling/movement cost need not produce a tile in one call.
    for( int step = 0; step < 8 && rl_dist( stalker.pos_abs(), you.pos_abs() ) <= separation; ++step ) {
        set_time( calendar::turn + 1_turns );
        stalker.process_turn();
        stalker.plan();
        stalker.set_moves( 100 );
        stalker.move();
    }
    CHECK( rl_dist( stalker.pos_abs(), you.pos_abs() ) > separation );
    CHECK( stalker.writhing_stalker_state().attempt_sequence == sequence );
    clear_map_without_vision();
}

TEST_CASE( "stalker_native_combat_consumes_old_recovery_without_erasing_sound_or_attempts",
           "[writhing_stalker][predator][pursuit][recovery][fallback]" )
{
    clear_map_without_vision();
    clear_avatar();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero );
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    refresh_writhing_stalker_arena( here );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::north_east );
    stalker.anger = 100;
    stalker.aggro_character = true;
    stalker.writhing_stalker_state().attempts_spent = 2;
    stalker.plan();
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::cooldown );
    const int old_deadline = stalker.writhing_stalker_state().cooldown_until_turn;
    for( int turn = 0; turn < 8; ++turn ) {
        set_time( calendar::turn + 1_turns );
        stalker.process_turn();
    }
    set_time( calendar::turn_zero + time_duration::from_turns( old_deadline - 1 ) );
    const auto old_retreat = stalker.writhing_stalker_state().retreat_waypoint;
    horde_entity owner( stalker );
    owner.synchronize_payload( stalker.pos_abs() );
    stalker = *owner.monster_data;
    REQUIRE( stalker.wander_pos == old_retreat );
    REQUIRE( stalker.wandf > 0 );
    const bool sound = GENERATE( false, true );
    if( sound ) {
        stalker.morale = 20;
        stalker.hear_sound( here.get_bub( old_retreat ), 2000, 6, true );
        REQUIRE( stalker.provocative_sound );
        REQUIRE( stalker.wander_pos == old_retreat );
    }
    const auto sequence = stalker.writhing_stalker_state().attempt_sequence;
    stalker.attack_at( you.pos_bub() );
    REQUIRE( stalker.writhing_stalker_state().attempt_sequence > sequence );
    const auto spent = stalker.writhing_stalker_state().attempts_spent;
    CHECK( static_cast<std::uint64_t>( spent ) == 2 + stalker.writhing_stalker_state().attempt_sequence - sequence );
    CHECK( stalker.writhing_stalker_state().cooldown_until_turn == -1 );
    CHECK_FALSE( stalker.writhing_stalker_state().has_retreat_waypoint );
    CHECK_FALSE( stalker.has_dest() );
    CHECK( ( stalker.wandf > 0 ) == sound );
    if( sound ) {
        CHECK( stalker.wander_pos == old_retreat );
    }
    const bool reload = GENERATE( false, true );
    if( reload ) {
        stalker = round_trip_writhing_stalker( stalker );
    }
    set_time( calendar::turn_zero + time_duration::from_turns( old_deadline + 21 ) );
    REQUIRE( writhing_stalker::observe_current_target( stalker ) == &you );
    CHECK( stalker.writhing_stalker_state().attempts_spent == spent );
    // Still-spent combat now chooses a fresh finite native recovery, rather
    // than refunding attempts or reviving the consumed old deadline.
    stalker.plan();
    CHECK( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::retreating );
    CHECK( stalker.writhing_stalker_state().cooldown_until_turn > to_turn<int>( calendar::turn ) );
    CHECK( stalker.writhing_stalker_state().attempts_spent == spent );
    clear_map_without_vision();
}

TEST_CASE( "stalker_bounded_search_completion_cannot_refund_native_combat",
           "[writhing_stalker][predator][pursuit][recovery]" )
{
    clear_map_without_vision();
    clear_avatar();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero );
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    refresh_writhing_stalker_arena( here );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::north_east );
    stalker.anger = 100;
    stalker.aggro_character = true;
    REQUIRE( writhing_stalker::observe_current_target( stalker ) == &you );
    stalker.writhing_stalker_state().begin_search();
    const auto search_start = stalker.writhing_stalker_state().search_started_active;
    stalker.attack_at( you.pos_bub() );
    const auto sequence = stalker.writhing_stalker_state().attempt_sequence;
    const auto spent = stalker.writhing_stalker_state().attempts_spent;
    REQUIRE( sequence > 0 );
    CHECK( stalker.writhing_stalker_state().search_started_active == search_start );
    you.setpos( here, tripoint_bub_ms( 65, 65, -2 ) );
    refresh_writhing_stalker_arena( here );
    const bool reload = GENERATE( false, true );
    if( reload ) {
        stalker = round_trip_writhing_stalker( stalker );
    }
    set_time( calendar::turn + 31_minutes );
    stalker.plan();
    CHECK( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::idle );
    CHECK( stalker.writhing_stalker_state().attempts_spent == spent );
    CHECK( stalker.writhing_stalker_state().attempt_sequence == sequence );
    CHECK_FALSE( stalker.has_dest() );
    stalker.plan();
    CHECK( stalker.writhing_stalker_state().attempts_spent == spent );
    clear_map_without_vision();
}

TEST_CASE( "stalker_distant_light_uses_reachable_native_steps_before_finite_arrival_search",
           "[writhing_stalker][predator][pursuit][light_transition][routing]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    map &here = get_map();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    get_player_character().setpos( here, tripoint_bub_ms( 65, 65, -2 ) );
    const bool slow = GENERATE( false, true );
    here.ter_set( center + point::east * 4, ter_id( slow ? "t_dirtmound" : "t_wall" ) );
    refresh_writhing_stalker_arena( here );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center );
    stalker.anger = 100;
    const auto light = here.get_abs( center + point::east * 24 );
    REQUIRE( stalker.get_pathfinding_settings().max_dist == 10 );
    INFO( "native max_length=" << stalker.get_pathfinding_settings().max_length << " slow=" << slow );
    REQUIRE( stalker.can_move_to( here.get_bub( light ) ) );
    // The full route refuses solely because regular pathfinding is bounded;
    // a real short route can still detour around this wall/slow tile.
    REQUIRE( here.route( stalker, pathfinding_target::point( here.get_bub( light ) ) ).empty() );
    REQUIRE_FALSE( here.route( stalker,
                              pathfinding_target::point( center + point( 8, 4 ) ) ).empty() );
    auto &state = stalker.writhing_stalker_state();
    const bool learned = GENERATE( false, true );
    if( learned ) {
        state.advance_to( 100 );
        REQUIRE( state.observe_target( 77, 1, light + point::north * 24, 100 ) );
        state.finish_search();
    }
    state.advance_to( 1000 );
    // Accepted physical-light adapter input, not native fire/sight proof.
    REQUIRE( state.observe_light_interest( light, "distant-native-light", 1000, 300 ) );
    stalker.plan();
    REQUIRE( stalker.has_dest() );
    CHECK( state.phase != writhing_stalker::lifecycle_phase::searching );
    const bool reload = GENERATE( false, true );
    if( reload ) {
        stalker = round_trip_writhing_stalker( stalker );
    }
    for( int step = 0; step < 100 && stalker.pos_abs() != light; ++step ) {
        stalker.plan();
        REQUIRE( stalker.writhing_stalker_state().phase != writhing_stalker::lifecycle_phase::searching );
        stalker.set_moves( 100 );
        stalker.move();
        set_time( calendar::turn + 1_turns );
    }
    REQUIRE( stalker.pos_abs() == light );
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::searching );
    const int deadline = stalker.writhing_stalker_state().search_until_turn;
    REQUIRE( deadline == to_turn<int>( calendar::turn ) + 20 );
    stalker = round_trip_writhing_stalker( stalker );
    set_time( calendar::turn_zero + time_duration::from_turns( deadline ) );
    stalker.plan();
    CHECK( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::idle );
    CHECK_FALSE( stalker.has_dest() );
    CHECK( stalker.writhing_stalker_state().evidence_turn == ( learned ? 100 : -1 ) );
    clear_map_without_vision();
}

TEST_CASE( "stalker_due_or_started_familiar_visit_preserves_accepted_sound_then_finishes",
           "[writhing_stalker][predator][pursuit][scheduler][signal]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    const auto source = get_map().get_abs( tripoint_bub_ms( -36, 65, 0 ) );
    const auto sound = source + point::east * 5;
    overmap &om = overmap_buffer.get( project_to<coords::om>( source ).xy() );
    om.hordes.clear();
    for( int x = -100; x <= 10; ++x ) {
        for( int y = -24; y <= 24; ++y ) {
            overmap_buffer.set_passable( source + point( x, y ), true );
        }
    }
    monster original( mon_writhing_stalker );
    original.set_pos_abs_only( source );
    original.set_moves( 100 );
    auto &memory = original.writhing_stalker_state();
    memory.advance_to( 100 );
    REQUIRE( memory.observe_target( 77, 1, source + point::west * 48, 100 ) );
    memory.finish_search();
    const auto due = memory.familiar[0].revisit_at_active;
    memory.advance_to( 1000 );
    const auto actor = original.predator_state().actor_id;
    REQUIRE( om.hordes.spawn_entity( source, original ) );
    const auto find_owner = [&]() -> std::pair<tripoint_abs_ms, horde_entity *> {
        for( const auto &entry : om.horde_entities() ) {
            if( entry.second->monster_data &&
                entry.second->monster_data->predator_state().actor_id == actor ) {
                return { entry.first, entry.second };
            }
        }
        return { source, nullptr };
    };
    set_time( calendar::turn_zero + time_duration::from_turns( 100 + due + 1 ) );
    const bool started = GENERATE( false, true );
    if( started ) {
        overmap_buffer.move_hordes();
        REQUIRE( find_owner().second != nullptr );
        REQUIRE( find_owner().second->monster_data->writhing_stalker_state().revisiting_slot == 0 );
        set_time( calendar::turn + 1_turns );
    }
    const auto before = find_owner().first;
    om.hordes.signal_entities( sound, 20 );
    auto *owner = find_owner().second;
    REQUIRE( owner != nullptr );
    REQUIRE( owner->destination == sound );
    REQUIRE( owner->monster_data->provocative_sound );
    const auto search_start = owner->monster_data->writhing_stalker_state().search_started_active;
    const bool reload = GENERATE( false, true );
    if( reload ) {
        *owner->monster_data = round_trip_writhing_stalker( *owner->monster_data );
        om.hordes.refresh_entity_bucket( before );
    }
    overmap_buffer.move_hordes();
    owner = find_owner().second;
    REQUIRE( owner != nullptr );
    CHECK( owner->destination == sound );
    CHECK( owner->monster_data->wander_pos == sound );
    CHECK( owner->monster_data->provocative_sound );
    CHECK( rl_dist( find_owner().first, sound ) < rl_dist( before, sound ) );
    const auto &pending = owner->monster_data->writhing_stalker_state();
    CHECK( pending.search_started_active == search_start );
    CHECK( pending.familiar[0].revisit_at_active == ( started ? -1 : due ) );
    for( int turn = 0; turn < 12 && find_owner().first != sound; ++turn ) {
        set_time( calendar::turn + 1_turns );
        overmap_buffer.move_hordes();
        REQUIRE( find_owner().second != nullptr );
        REQUIRE( find_owner().second->destination == sound );
    }
    REQUIRE( find_owner().first == sound );
    REQUIRE( find_owner().second->tracking_intensity == 0 );
    set_time( calendar::turn + 1_turns );
    overmap_buffer.move_hordes();
    owner = find_owner().second;
    REQUIRE( owner != nullptr );
    CHECK( owner->monster_data->writhing_stalker_state().revisiting_slot == 0 );
    CHECK( owner->destination != sound );
    set_time( calendar::turn + 11_minutes );
    overmap_buffer.move_hordes();
    owner = find_owner().second;
    REQUIRE( owner != nullptr );
    CHECK( owner->monster_data->writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::idle );
    CHECK( owner->monster_data->writhing_stalker_state().revisiting_slot == -1 );
    CHECK( owner->monster_data->writhing_stalker_state().familiar[0].revisit_at_active >
           owner->monster_data->writhing_stalker_state().active_turns );
    CHECK( owner->monster_data->writhing_stalker_state().evidence_turn == 100 );
    CHECK( owner->tracking_intensity == 0 );
    om.hordes.clear();
    clear_map_without_vision();
}

TEST_CASE( "stalker_distant_light_short_step_survives_transfer_then_expires_without_motion",
           "[writhing_stalker][predator][pursuit][light_transition][fallback]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    map &here = get_map();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    get_player_character().setpos( here, tripoint_bub_ms( 65, 65, -2 ) );
    here.ter_set( center + point::east * 4, ter_id( "t_wall" ) );
    refresh_writhing_stalker_arena( here );
    monster *local = &spawn_test_monster( mon_writhing_stalker.str(), center );
    local->anger = 100;
    const auto light = here.get_abs( center + point::east * 24 );
    REQUIRE( local->writhing_stalker_state().observe_light_interest(
                 light, "transferred-light-step", 1000, 30 ) );
    local->plan();
    REQUIRE( local->has_dest() );
    REQUIRE( local->get_dest() != light );
    REQUIRE( local->writhing_stalker_state().owns_waypoint( local->get_dest() ) );
    const auto actor = local->predator_state().actor_id;
    const auto epoch = local->predator_state().handoff_epoch;
    const bool transfer = GENERATE( false, true );
    if( transfer ) {
        const auto source = local->pos_abs();
        overmap &om = overmap_buffer.get( project_to<coords::om>( source ).xy() );
        om.hordes.clear();
        g->despawn_monster( *local );
        local = nullptr;
        set_time( calendar::turn + 1_turns );
        overmap_buffer.move_hordes();
        for( monster &candidate : g->all_monsters() ) {
            if( candidate.predator_state().actor_id == actor ) {
                local = &candidate;
            }
        }
        REQUIRE( local != nullptr );
        CHECK( local->predator_state().handoff_epoch == epoch + 2 );
        CHECK( local->writhing_stalker_state().light_observed_position == light );
        CHECK( local->writhing_stalker_state().phase != writhing_stalker::lifecycle_phase::searching );
        local->plan();
    }
    REQUIRE( local->has_dest() );
    REQUIRE( local->get_dest() != light );
    REQUIRE( local->writhing_stalker_state().owns_waypoint( local->get_dest() ) );
    const bool reload = GENERATE( false, true );
    if( reload ) {
        *local = round_trip_writhing_stalker( *local );
    }
    const auto position = local->pos_abs();
    const auto expiry = local->writhing_stalker_state().light_expires_turn;
    set_time( calendar::turn_zero + time_duration::from_turns( expiry ) );
    local->plan();
    CHECK( local->writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::idle );
    CHECK_FALSE( local->writhing_stalker_state().has_light_observed_position );
    CHECK_FALSE( local->has_dest() );
    CHECK( local->wandf == 0 );
    local->set_moves( 100 );
    local->move();
    CHECK( local->pos_abs() == position );
    CHECK( horde_entity( *local ).tracking_intensity == 0 );
    CHECK( local->writhing_stalker_state().evidence_turn == -1 );
    clear_map_without_vision();
}

TEST_CASE( "stalker_abstract_sound_keeps_its_carrier_when_light_is_active_or_expires",
           "[writhing_stalker][predator][pursuit][light_transition][signal]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    map &here = get_map();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    get_player_character().setpos( here, tripoint_bub_ms( 65, 65, -2 ) );
    here.ter_set( center + point::east * 4, ter_id( "t_wall" ) );
    refresh_writhing_stalker_arena( here );
    monster &original = spawn_test_monster( mon_writhing_stalker.str(), center );
    original.anger = 100;
    const auto source = original.pos_abs();
    const auto sound = source + point::south * 6;
    REQUIRE( original.writhing_stalker_state().observe_light_interest(
                 source + point::east * 24, "sound-versus-light-step", 1000, 90 ) );
    original.plan();
    REQUIRE( original.has_dest() );
    const auto old_step = original.get_dest();
    const auto actor = original.predator_state().actor_id;
    overmap &om = overmap_buffer.get( project_to<coords::om>( source ).xy() );
    om.hordes.clear();
    g->despawn_monster( original );
    om.hordes.signal_entities( sound, 200 );
    REQUIRE( overmap_buffer.entity_at( source ) != nullptr );
    REQUIRE( overmap_buffer.entity_at( source )->destination == sound );
    const bool expired = GENERATE( false, true );
    set_time( calendar::turn + ( expired ? 91_turns : 1_turns ) );
    overmap_buffer.move_hordes();
    monster *local = nullptr;
    for( monster &candidate : g->all_monsters() ) {
        if( candidate.predator_state().actor_id == actor ) {
            local = &candidate;
        }
    }
    REQUIRE( local != nullptr );
    CHECK( local->provocative_sound );
    CHECK( local->wander_pos == sound );
    CHECK( rl_dist( local->pos_abs(), sound ) < rl_dist( source, sound ) );
    CHECK_FALSE( ( local->has_dest() && local->get_dest() == old_step ) );
    if( expired ) {
        CHECK_FALSE( local->writhing_stalker_state().has_light_observed_position );
    }
    {
        const int separation = rl_dist( local->pos_abs(), sound );
        for( int turn = 0; turn < 6 && rl_dist( local->pos_abs(), sound ) >= separation; ++turn ) {
            set_time( calendar::turn + 1_turns );
            local->plan();
            local->set_moves( 100 );
            local->move();
        }
        CHECK( rl_dist( local->pos_abs(), sound ) < separation );
    }
    CHECK( local->writhing_stalker_state().evidence_turn == -1 );
    clear_map_without_vision();
}

TEST_CASE( "stalker_distant_remembered_goal_routes_local_steps_after_real_sound",
           "[writhing_stalker][predator][pursuit][routing][signal]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    map &here = get_map();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    get_player_character().setpos( here, tripoint_bub_ms( 65, 65, -2 ) );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center );
    stalker.anger = 100;
    const auto remembered = stalker.pos_abs() + point::west * 180;
    REQUIRE_FALSE( here.inbounds( remembered ) );
    REQUIRE( scout_observation::site_sightline( stalker.pos_abs(), remembered ).empty() );
    auto &memory = stalker.writhing_stalker_state();
    memory.advance_to( 1000 );
    // Accepted observation fixture; navigation does not claim remote sensing.
    REQUIRE( memory.observe_target( 77, 1, remembered, 1000 ) );
    memory.finish_search();
    const auto due = memory.familiar[0].revisit_at_active;
    const bool followed_sound = GENERATE( false, true );
    if( followed_sound ) {
        const auto before = stalker.pos_abs();
        const auto sound = center + point::east * 8;
        stalker.hear_sound( sound, 2000, 8, true );
        REQUIRE( stalker.provocative_sound );
        for( int turn = 0; turn < 40 && stalker.pos_bub() != sound; ++turn ) {
            stalker.plan();
            stalker.set_moves( 100 );
            stalker.move();
            set_time( calendar::turn + 1_turns );
        }
        REQUIRE( stalker.pos_bub() == sound );
        REQUIRE( stalker.pos_abs() != before );
        REQUIRE( memory.familiar[0].revisit_at_active == due );
    }
    const bool light = GENERATE( false, true );
    if( light ) {
        // Recent direct evidence deliberately outranks a light observation.
        set_time( calendar::turn + 201_turns );
        memory.advance_to( to_turn<int>( calendar::turn ) );
        REQUIRE( memory.observe_light_interest( remembered, "remembered-distant-source",
                 to_turn<int>( calendar::turn ), 300 ) );
    } else {
        set_time( calendar::turn_zero + time_duration::from_turns( 1000 + due + 1 ) );
    }
    const bool reload = GENERATE( false, true );
    if( reload ) {
        stalker = round_trip_writhing_stalker( stalker );
    }
    const auto before = stalker.pos_abs();
    const auto evidence = stalker.writhing_stalker_state().evidence_turn;
    stalker.plan();
    REQUIRE( stalker.has_dest() );
    const auto step = stalker.get_dest();
    CHECK( here.inbounds( step ) );
    CHECK( step != before );
    CHECK( step != remembered );
    CHECK( rl_dist( step, remembered ) < rl_dist( before, remembered ) );
    REQUIRE_FALSE( here.route( stalker, pathfinding_target::point( here.get_bub( step ) ) ).empty() );
    if( !light ) {
        CHECK( stalker.writhing_stalker_state().revisiting_slot == 0 );
        CHECK( stalker.writhing_stalker_state().searched_positions == 0 );
    }
    for( int turn = 0; turn < 6 && stalker.pos_abs() == before; ++turn ) {
        stalker.set_moves( 100 );
        stalker.move();
        set_time( calendar::turn + 1_turns );
        stalker.plan();
    }
    CHECK( rl_dist( stalker.pos_abs(), remembered ) < rl_dist( before, remembered ) );
    CHECK( stalker.writhing_stalker_state().evidence_turn == evidence );
    clear_map_without_vision();
}

TEST_CASE( "stalker_distant_remembered_navigation_keeps_loaded_and_blocked_limits",
           "[writhing_stalker][predator][pursuit][routing]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    map &here = get_map();
    const tripoint_bub_ms center( 2, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    get_player_character().setpos( here, tripoint_bub_ms( 65, 65, -2 ) );
    const bool blocked = GENERATE( false, true );
    if( blocked ) {
        for( const auto &p : here.points_in_radius( center, 1 ) ) {
            if( p != center ) {
                here.ter_set( p, ter_id( "t_wall" ) );
            }
        }
        refresh_writhing_stalker_arena( here );
    }
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center );
    auto &memory = stalker.writhing_stalker_state();
    memory.advance_to( 1000 );
    const auto remembered = stalker.pos_abs() + point::west * 180;
    REQUIRE( memory.observe_target( 77, 1, remembered, 1000 ) );
    memory.finish_search();
    set_time( calendar::turn_zero + time_duration::from_turns(
                  1000 + memory.familiar[0].revisit_at_active + 1 ) );
    stalker.plan();
    if( blocked ) {
        CHECK_FALSE( stalker.has_dest() );
        CHECK_FALSE( memory.has_committed_waypoint );
        CHECK( memory.searched_positions == 0 );
    } else {
        REQUIRE( stalker.has_dest() );
        CHECK( stalker.get_dest() != stalker.pos_abs() );
        CHECK( here.inbounds( stalker.get_dest() ) );
        CHECK( stalker.get_dest() != remembered );
        CHECK( rl_dist( stalker.get_dest(), remembered ) < rl_dist( stalker.pos_abs(), remembered ) );
        CHECK_FALSE( here.route( stalker,
                                pathfinding_target::point( here.get_bub( stalker.get_dest() ) ) ).empty() );
        CHECK( memory.revisiting_slot == 0 );
    }
    CHECK( memory.evidence_turn == 1000 );
    clear_map_without_vision();
}

TEST_CASE( "stalker_visible_stationary_shadow_continues_steps_then_holds_standoff",
           "[writhing_stalker][predator][pursuit][routing][shadow_continuation]" )
{
    clear_map_without_vision();
    clear_npcs();
    clear_avatar();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1_days + 12_hours );
    scoped_weather_override weather( weather_type_id( "clear" ) );
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    for( const auto &p : here.points_in_radius( center, 35 ) ) {
        here.ter_set( p, ter_id( "t_floor" ) );
        here.furn_clear( p );
        here.clear_fields( p );
    }
    // Native route permits long straight cheap paths before its bounded
    // detour search. A transparent slow tile exercises the short-step path
    // without obstructing the actual continuous sight under test.
    for( int dy = -1; dy <= 1; ++dy ) {
        here.ter_set( center + point( 28, dy ), ter_id( "t_dirtmound" ) );
    }
    you.setpos( here, center );
    refresh_writhing_stalker_arena( here );
    monster &stalker = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 32 );
    stalker.anger = 100;
    stalker.aggro_character = true;
    // Actual same-target pressure keeps this native bright/focused approach
    // in Shadow; a lone exposed observer legitimately enters cooldown.
    for( const point &offset : { point::west, point::south, point::east, point::north } ) {
        monster &zombie = spawn_test_monster( mon_zombie.str(), center + offset );
        zombie.aggro_character = true;
        zombie.set_dest( you.pos_abs() );
    }
    const bool occupied_standoffs = GENERATE( false, true );
    if( occupied_standoffs ) {
        for( const auto &p : here.points_in_radius( center, 4 ) ) {
            const int distance = rl_dist( p, center );
            if( distance >= 2 && distance <= 4 ) {
                monster &zombie = spawn_test_monster( mon_zombie.str(), p );
                zombie.aggro_character = true;
                zombie.set_dest( you.pos_abs() );
            }
        }
    }
    REQUIRE( writhing_stalker::observed_zombie_pressure( stalker, here, you ) >= 4 );
    const auto target_position = you.pos_abs();
    const int target_hp = you.get_hp();
    REQUIRE( writhing_stalker::can_observe_actor( stalker, you ) );
    REQUIRE( stalker.get_pathfinding_settings().max_dist == 10 );
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::shadowing );
    REQUIRE( stalker.has_dest() );
    const auto first = stalker.get_dest();
    REQUIRE( first != stalker.pos_abs() );
    REQUIRE( rl_dist( first, target_position ) > 10 );
    // A native route may become unreachable after a partial move. Follow
    // its actual accepted replacement step, not an assumed fixed first goal.
    tripoint_abs_ms completed = first;
    bool reached = false;
    for( int turn = 0; turn < 40 && !reached; ++turn ) {
        REQUIRE( writhing_stalker::can_observe_actor( stalker, you ) );
        stalker.plan();
        REQUIRE( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::shadowing );
        REQUIRE( stalker.has_dest() );
        completed = stalker.get_dest();
        stalker.set_moves( 100 );
        stalker.move();
        reached = stalker.pos_abs() == completed;
        set_time( calendar::turn + 1_turns );
    }
    REQUIRE( reached );
    REQUIRE( rl_dist( completed, target_position ) > 10 );
    const bool reload = GENERATE( false, true );
    if( reload ) {
        stalker = round_trip_writhing_stalker( stalker );
    }
    stalker.plan();
    REQUIRE( stalker.writhing_stalker_state().last_observed_position == target_position );
    REQUIRE( stalker.writhing_stalker_state().evidence_turn == to_turn<int>( calendar::turn ) );
    REQUIRE( stalker.has_dest() );
    REQUIRE( stalker.get_dest() != completed );
    for( int turn = 0; turn < 80 && rl_dist( stalker.pos_abs(), target_position ) > 8; ++turn ) {
        REQUIRE( writhing_stalker::can_observe_actor( stalker, you ) );
        stalker.plan();
        REQUIRE( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::shadowing );
        stalker.set_moves( 100 );
        stalker.move();
        set_time( calendar::turn + 1_turns );
    }
    REQUIRE( rl_dist( stalker.pos_abs(), target_position ) <= 8 );
    // A legitimate final shadow destination is allowed to remain occupied;
    // renewal of intermediate steps must not create perpetual movement.
    for( int turn = 0; turn < 20; ++turn ) {
        stalker.plan();
        stalker.set_moves( 100 );
        stalker.move();
        set_time( calendar::turn + 1_turns );
    }
    const auto settled = stalker.pos_abs();
    REQUIRE( rl_dist( settled, target_position ) >= 2 );
    REQUIRE( rl_dist( settled, target_position ) <= 8 );
    REQUIRE( stalker.writhing_stalker_state().has_committed_waypoint );
    const auto final_goal = stalker.writhing_stalker_state().committed_waypoint;
    if( occupied_standoffs ) {
        CHECK( rl_dist( final_goal, target_position ) >= 7 );
    } else {
        CHECK( rl_dist( final_goal, target_position ) <= 4 );
    }
    stalker = round_trip_writhing_stalker( stalker );
    for( int turn = 0; turn < 20; ++turn ) {
        stalker.plan();
        CHECK( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::shadowing );
        CHECK( stalker.writhing_stalker_state().committed_waypoint == final_goal );
        stalker.set_moves( 100 );
        stalker.move();
        set_time( calendar::turn + 1_turns );
        // Ordinary idle movement may stumble one square; it must not renew
        // the approach or walk a chain of new goals beside stationary prey.
        CHECK( rl_dist( stalker.pos_abs(), final_goal ) <= 1 );
    }
    CHECK( you.pos_abs() == target_position );
    CHECK( you.get_hp() == target_hp );
    CHECK( stalker.writhing_stalker_state().attempts_spent == 0 );
    const auto observed = stalker.writhing_stalker_state().last_observed_position;
    const int last_seen = stalker.writhing_stalker_state().evidence_turn;
    const auto wall_x = ( stalker.pos_bub().x() + you.pos_bub().x() ) / 2;
    for( int y = 30; y <= 100; ++y ) {
        here.ter_set( tripoint_bub_ms( wall_x, y, 0 ), ter_id( "t_wall" ) );
    }
    you.setpos( here, center + point::south );
    refresh_writhing_stalker_arena( here );
    REQUIRE_FALSE( writhing_stalker::can_observe_actor( stalker, you ) );
    set_time( calendar::turn + 1_turns );
    stalker.plan();
    CHECK( stalker.writhing_stalker_state().evidence_turn == last_seen );
    CHECK( stalker.writhing_stalker_state().last_observed_position == observed );
    CHECK( stalker.writhing_stalker_state().heading == tripoint_rel_ms::zero );
    set_time( calendar::turn + 31_minutes );
    stalker.plan();
    CHECK( stalker.writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::idle );
    CHECK_FALSE( stalker.has_dest() );
    clear_map_without_vision();
}

TEST_CASE( "stalker_expired_sound_flag_cannot_own_delayed_transferred_pursuit",
           "[writhing_stalker][predator][pursuit][scheduler][signal][fallback]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    map &here = get_map();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    get_player_character().setpos( here, tripoint_bub_ms( 65, 65, -2 ) );
    monster &original = spawn_test_monster( mon_writhing_stalker.str(), center );
    original.anger = 100;
    original.hear_sound( center + point::south * 3, 24, 3, true );
    REQUIRE( original.provocative_sound );
    REQUIRE( original.wandf > 0 );
    const int heard_budget = original.wandf;
    for( int turn = 0; turn <= heard_budget && original.wandf > 0; ++turn ) {
        original.set_moves( 100 );
        original.move();
        set_time( calendar::turn + 1_turns );
    }
    REQUIRE( original.wandf == 0 );
    REQUIRE( original.provocative_sound );
    original = round_trip_writhing_stalker( original );
    REQUIRE( original.wandf == 0 );
    REQUIRE( original.provocative_sound );
    auto &memory = original.writhing_stalker_state();
    const int seen = to_turn<int>( calendar::turn );
    memory.advance_to( seen );
    const bool learned = GENERATE( true, false );
    // Accepted observation fixtures feed the actual local planner; neither
    // this remembered sight nor the light packet claims native acquisition.
    if( learned ) {
        REQUIRE( memory.observe_target( 77, 1, original.pos_abs() + point::east * 24, seen ) );
    } else {
        REQUIRE( memory.observe_light_interest( original.pos_abs() + point::east * 24,
                 "expired-flag-native-light", seen, 120 ) );
    }
    set_time( calendar::turn + 61_turns );
    original.plan();
    REQUIRE( memory.phase == ( learned ? writhing_stalker::lifecycle_phase::searching :
                              writhing_stalker::lifecycle_phase::shadowing ) );
    REQUIRE( original.has_dest() );
    REQUIRE( memory.owns_waypoint( original.get_dest() ) );
    const auto old_route = original.get_dest();
    const auto source = original.pos_abs();
    const auto actor = original.predator_state().actor_id;
    const auto epoch = original.predator_state().handoff_epoch;
    // A later genuinely accepted sound remains independent even when it is
    // at exactly the carried route coordinate.
    const int sound = GENERATE( 0, 1, 2 );
    tripoint_abs_ms heard;
    if( sound ) {
        heard = sound == 2 ? old_route : source + point::south * 6;
        original.hear_sound( here.get_bub( heard ), 2000, 6, true );
        REQUIRE( original.wander_pos == heard );
        REQUIRE( original.wandf > 0 );
    }
    overmap &om = overmap_buffer.get( project_to<coords::om>( source ).xy() );
    om.hordes.clear();
    for( int x = -30; x <= 30; ++x ) {
        for( int y = -30; y <= 30; ++y ) {
            overmap_buffer.set_passable( source + point( x, y ), true );
        }
    }
    g->despawn_monster( original );
    auto *owner = overmap_buffer.entity_at( source );
    REQUIRE( owner != nullptr );
    REQUIRE( owner->tracking_intensity == 1000 );
    REQUIRE( owner->monster_data->provocative_sound );
    REQUIRE( owner->monster_data->predator_state().handoff_epoch == epoch + 1 );
    const bool early_reentry = GENERATE( false, true );
    if( early_reentry ) {
        // The real load caller synchronizes a disposable handoff payload.
        // Exercise its provenance before a later unload and expiry too.
        overmap_buffer.spawn_monster( project_to<coords::sm>( source ) );
        monster *local = nullptr;
        for( monster &candidate : g->all_monsters() ) {
            if( candidate.is_caol_predator() && candidate.predator_state().actor_id == actor ) {
                REQUIRE( local == nullptr );
                local = &candidate;
            }
        }
        REQUIRE( local != nullptr );
        REQUIRE( local->predator_state().handoff_epoch == epoch + 2 );
        g->despawn_monster( *local );
        owner = overmap_buffer.entity_at( source );
        REQUIRE( owner != nullptr );
        REQUIRE( owner->monster_data->predator_state().handoff_epoch == epoch + 3 );
    }
    const bool reload = GENERATE( false, true );
    if( reload ) {
        *owner->monster_data = round_trip_writhing_stalker( *owner->monster_data );
        om.hordes.refresh_entity_bucket( source );
    }
    // Native placement is blocked only at the old location. An expired route
    // must not move elsewhere or turn itself into a fresh sound carrier.
    spawn_test_monster( mon_zombie.str(), here.get_bub( source ) );
    set_time( calendar::turn + 31_minutes );
    overmap_buffer.move_hordes();
    monster *payload = nullptr;
    tripoint_abs_ms position;
    int owners = 0;
    horde_entity *abstract = nullptr;
    for( const auto &entry : om.horde_entities() ) {
        if( entry.second->monster_data && entry.second->monster_data->is_caol_predator() &&
            entry.second->monster_data->predator_state().actor_id == actor ) {
            abstract = entry.second;
            payload = abstract->monster_data.get();
            position = entry.first;
            ++owners;
        }
    }
    for( monster &candidate : g->all_monsters() ) {
        if( candidate.is_caol_predator() && candidate.predator_state().actor_id == actor ) {
            payload = &candidate;
            position = candidate.pos_abs();
            ++owners;
        }
    }
    REQUIRE( payload != nullptr );
    REQUIRE( owners == 1 );
    CHECK( payload->writhing_stalker_state().phase == writhing_stalker::lifecycle_phase::idle );
    CHECK_FALSE( payload->writhing_stalker_state().has_committed_waypoint );
    CHECK( payload->writhing_stalker_state().evidence_turn == ( learned ? seen : -1 ) );
    if( sound ) {
        CHECK( payload->provocative_sound );
        CHECK( payload->wandf > 0 );
        CHECK( payload->wander_pos == heard );
        CHECK( rl_dist( position, heard ) < rl_dist( source, heard ) );
    } else {
        CHECK( position == source );
        REQUIRE( abstract != nullptr );
        CHECK( abstract->tracking_intensity == 0 );
        CHECK( abstract->is_active() == learned );
        CHECK( payload->wandf == 0 );
        CHECK_FALSE( payload->has_dest() );
        CHECK_FALSE( payload->provocative_sound );
        const auto active = payload->writhing_stalker_state().active_turns;
        overmap_buffer.move_hordes();
        CHECK( payload->writhing_stalker_state().active_turns == active );
        CHECK( abstract->tracking_intensity == 0 );
    }
    om.hordes.clear();
    clear_map_without_vision();
}

TEST_CASE( "stalker_sound_stays_pending_beneath_current_sight_or_retreat_handoff",
           "[writhing_stalker][predator][pursuit][signal][fallback]" )
{
    clear_map_without_vision();
    clear_avatar();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1_days );
    map &here = get_map();
    Character &you = get_player_character();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    you.setpos( here, center );
    refresh_writhing_stalker_arena( here );
    monster &original = spawn_test_monster( mon_writhing_stalker.str(), center + point::east * 3 );
    original.anger = 100;
    original.aggro_character = true;
    const bool retreat = GENERATE( false, true );
    if( retreat ) {
        original.writhing_stalker_state().attempts_spent = 2;
    }
    original.plan();
    REQUIRE( original.writhing_stalker_state().phase == ( retreat ?
             writhing_stalker::lifecycle_phase::retreating : writhing_stalker::lifecycle_phase::shadowing ) );
    REQUIRE( original.has_dest() );
    const auto goal = original.get_dest();
    tripoint_abs_ms position = original.pos_abs();
    const bool same_coordinate = GENERATE( false, true );
    const auto sound = same_coordinate ? goal : position + point::south * 6;
    original.hear_sound( here.get_bub( sound ), 2000, 6, true );
    REQUIRE( original.wander_pos == sound );
    REQUIRE( original.wandf > 0 );
    const auto actor = original.predator_state().actor_id;
    const auto epoch = original.predator_state().handoff_epoch;
    overmap &om = overmap_buffer.get( project_to<coords::om>( position ).xy() );
    om.hordes.clear();
    for( int x = -12; x <= 12; ++x ) {
        for( int y = -12; y <= 12; ++y ) {
            overmap_buffer.set_passable( position + point( x, y ), true );
        }
    }
    g->despawn_monster( original );
    set_time( calendar::turn + 1_turns );
    overmap_buffer.move_hordes();
    monster *local = nullptr;
    for( monster &candidate : g->all_monsters() ) {
        if( candidate.is_caol_predator() && candidate.predator_state().actor_id == actor ) {
            REQUIRE( local == nullptr );
            local = &candidate;
        }
    }
    REQUIRE( local != nullptr );
    REQUIRE( local->predator_state().handoff_epoch == epoch + 2 );
    CHECK( local->provocative_sound );
    CHECK( local->wandf > 0 );
    CHECK( local->wander_pos == sound );
    position = local->pos_abs();
    horde_entity owner( *local );
    REQUIRE( owner.advance_predator_intent( position, to_turn<int>( calendar::turn ) ) );
    // Native fleeing does not acquire a fresh hostile watch observation.
    CHECK( owner.monster_data->writhing_stalker_state().evidence_turn ==
           to_turn<int>( calendar::turn ) - ( retreat ? 1 : 0 ) );
    if( retreat ) {
        CHECK( owner.destination == goal );
    } else {
        CHECK( owner.monster_data->writhing_stalker_state().owns_waypoint( owner.destination ) );
    }
    owner.synchronize_payload( position );
    CHECK( owner.monster_data->provocative_sound );
    CHECK( owner.monster_data->wandf > 0 );
    CHECK( owner.monster_data->wander_pos == sound );
    if( !same_coordinate ) {
        CHECK( owner.destination != sound );
        CHECK( owner.monster_data->get_dest() == owner.destination );
    }
    // Remove actual sight and let the existing recovery deadline settle.
    // The still-live sound can then own movement above remembered searching.
    you.setpos( here, tripoint_bub_ms( 65, 65, -2 ) );
    refresh_writhing_stalker_arena( here );
    set_time( calendar::turn + 31_minutes );
    REQUIRE( owner.advance_predator_intent( position, to_turn<int>( calendar::turn ) ) );
    CHECK( owner.destination == sound );
    CHECK( owner.tracking_intensity > 0 );
    CHECK( owner.monster_data->wander_pos == sound );
    CHECK_FALSE( owner.monster_data->has_dest() );
    clear_map_without_vision();
}

TEST_CASE( "stalker_selected_sound_budget_is_not_renewed_before_native_transport",
           "[writhing_stalker][predator][pursuit][scheduler][signal]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    map &here = get_map();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    get_player_character().setpos( here, tripoint_bub_ms( 65, 65, -2 ) );
    monster &original = spawn_test_monster( mon_writhing_stalker.str(), center );
    original.anger = 100;
    original.hear_sound( center + point::south * 6, 30, 6, true );
    REQUIRE( original.provocative_sound );
    REQUIRE( original.wandf > 3 );
    const auto sound = original.wander_pos;
    const int budget = original.wandf;
    const auto source = original.pos_abs();
    original.set_moves( -300 );
    overmap &om = overmap_buffer.get( project_to<coords::om>( source ).xy() );
    om.hordes.clear();
    g->despawn_monster( original );
    for( int step = 1; step <= 2; ++step ) {
        set_time( calendar::turn + 1_turns );
        overmap_buffer.move_hordes();
        auto *owner = overmap_buffer.entity_at( source );
        REQUIRE( owner != nullptr );
        CHECK( owner->destination == sound );
        CHECK( owner->tracking_intensity == budget - step );
        CHECK( owner->moves <= 0 );
        // No transport has synchronized the payload yet. Repeated intent
        // service must not refill the entity from this older native budget.
        CHECK( owner->monster_data->wandf == budget );
    }
    om.hordes.clear();
    clear_map_without_vision();
}

TEST_CASE( "stalker_native_unlearned_goal_precedes_pending_sound_during_handoff",
           "[writhing_stalker][predator][pursuit][scheduler][signal][fallback]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    map &here = get_map();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    get_player_character().setpos( here, tripoint_bub_ms( 65, 65, -2 ) );
    monster &original = spawn_test_monster( mon_writhing_stalker.str(), center );
    original.anger = 100;
    const auto goal = here.get_abs( center + point::east * 6 );
    const auto sound = here.get_abs( center + point::south * 6 );
    original.set_dest( goal );
    original.hear_sound( here.get_bub( sound ), 2000, 6, true );
    REQUIRE( original.wander_pos == sound );
    original.set_moves( 100 );
    original.move();
    const auto source = original.pos_abs();
    REQUIRE( rl_dist( source, goal ) < rl_dist( here.get_abs( center ), goal ) );
    REQUIRE( original.has_dest() );
    REQUIRE_FALSE( original.writhing_stalker_state().has_pursuit_memory );
    const auto actor = original.predator_state().actor_id;
    original.set_moves( 100 );
    overmap &om = overmap_buffer.get( project_to<coords::om>( source ).xy() );
    om.hordes.clear();
    for( int x = -12; x <= 12; ++x ) {
        for( int y = -12; y <= 12; ++y ) {
            overmap_buffer.set_passable( source + point( x, y ), true );
        }
    }
    g->despawn_monster( original );
    set_time( calendar::turn + 1_turns );
    overmap_buffer.move_hordes();
    monster *local = nullptr;
    for( monster &candidate : g->all_monsters() ) {
        if( candidate.is_caol_predator() && candidate.predator_state().actor_id == actor ) {
            REQUIRE( local == nullptr );
            local = &candidate;
        }
    }
    REQUIRE( local != nullptr );
    CHECK( rl_dist( local->pos_abs(), goal ) < rl_dist( source, goal ) );
    CHECK( local->has_dest() );
    CHECK( local->get_dest() == goal );
    CHECK( local->provocative_sound );
    CHECK( local->wander_pos == sound );
    CHECK_FALSE( local->writhing_stalker_state().has_pursuit_memory );
    om.hordes.clear();
    clear_map_without_vision();
}

TEST_CASE( "stalker_pending_sound_spends_native_budget_on_committed_route_steps",
           "[writhing_stalker][predator][pursuit][scheduler][signal][fallback]" )
{
    clear_map_and_put_player_underground();
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    set_time( calendar::turn_zero + 1000_turns );
    map &here = get_map();
    const tripoint_bub_ms center( 65, 65, 0 );
    prepare_writhing_stalker_arena( here, center );
    get_player_character().setpos( here, tripoint_bub_ms( 65, 65, -2 ) );
    monster &heard_locally = spawn_test_monster( mon_writhing_stalker.str(), center );
    heard_locally.anger = 100;
    heard_locally.hear_sound( center + point::south * 6, 30, 6, true );
    REQUIRE( heard_locally.provocative_sound );
    const int budget = heard_locally.wandf;
    REQUIRE( budget > 2 );
    monster original( heard_locally );
    g->remove_zombie( heard_locally );
    // Translate the accepted callback into an unloaded route fixture. This
    // proves transport lifetime, not physical hearing or remote perception.
    const auto source = here.get_abs( tripoint_bub_ms( -budget - 48, 65, 0 ) );
    const auto sound = source + point::south * 6;
    const auto goal = source + point::east * ( budget + 12 );
    original.set_pos_abs_only( source );
    original.wander_to( sound, budget );
    original.set_dest( goal );
    original.set_moves( 100 );
    const auto actor = original.predator_state().actor_id;
    overmap &om = overmap_buffer.get( project_to<coords::om>( source ).xy() );
    om.hordes.clear();
    for( int x = 0; x <= budget + 12; ++x ) {
        const auto cell = source + point::east * x;
        overmap_buffer.get( project_to<coords::om>( cell ).xy() );
        overmap_buffer.set_passable( cell, true );
    }
    REQUIRE( om.hordes.spawn_entity( source, original ) );
    for( int step = 1; step <= budget; ++step ) {
        set_time( calendar::turn + 1_turns );
        overmap_buffer.move_hordes();
        const auto position = source + point::east * step;
        REQUIRE_FALSE( here.inbounds( position ) );
        auto *owner = overmap_buffer.entity_at( position );
        REQUIRE( owner != nullptr );
        REQUIRE( owner->monster_data->predator_state().actor_id == actor );
        CHECK( owner->destination == goal );
        if( step < budget ) {
            CHECK( owner->monster_data->provocative_sound );
            CHECK( owner->monster_data->wander_pos == sound );
            CHECK( owner->monster_data->wandf == budget - step );
        } else {
            // The remaining route carrier is explicitly not new sound.
            CHECK_FALSE( owner->monster_data->provocative_sound );
            CHECK( owner->monster_data->wander_pos == goal );
        }
        const int remaining = owner->monster_data->wandf;
        overmap_buffer.move_hordes();
        CHECK( owner->monster_data->wandf == remaining );
    }
    om.hordes.clear();
    clear_map_without_vision();
}
