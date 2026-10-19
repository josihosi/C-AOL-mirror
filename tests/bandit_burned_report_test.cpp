#include "cata_catch.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "bandit_live_world.h"
#include "calendar.h"
#include "cata_scope_helpers.h"
#include "do_turn.h"
#include "game.h"
#include "json.h"
#include "json_loader.h"
#include "map_helpers.h"
#include "npc.h"
#include "omdata.h"
#include "options_helpers.h"
#include "overmapbuffer.h"

namespace
{
using namespace bandit_live_world;

const std::string retained_site_id = "overmap_special:bandit_camp@129,149,0";

template<typename T>
std::string serialized( const T &value )
{
    std::ostringstream stream;
    JsonOut json( stream );
    value.serialize( json );
    return stream.str();
}

std::string serialized_claims( const world_state &world )
{
    std::ostringstream stream;
    JsonOut json( stream );
    json.write( world.hostile_target_opportunities );
    return stream.str();
}

world_state reloaded_world( const world_state &world )
{
    world_state result;
    result.deserialize( json_loader::from_string( serialized( world ) ).get_object() );
    return result;
}

void remove_retained_actors()
{
    clear_npcs();
    for( int id : { 5, 6, 7, 8, 9, 10 } ) {
        g->remove_npc( character_id( id ) );
        if( overmap_buffer.find_npc( character_id( id ) ) ) {
            overmap_buffer.remove_npc( character_id( id ) );
        }
    }
}
}

TEST_CASE( "retained returned intelligence claims only its exact authorized response",
           "[bandit_live_world][burned_report_claim_067]" )
{
    using namespace bandit_live_world;
    const std::string condition = GENERATE( std::string( "burned" ), "normal", "site_signal",
                                          "below_ready", "unlatched", "incapable_party", "expired", "provisional", "foreign_source",
                                          "missing_carrier", "foreign_carrier", "duplicate_carrier", "nonhome_carrier",
                                          "wrong_report_key", "wrong_target", "wrong_revision", "mismatched_lead",
                                          "invalidated_lead", "conflicting_claim", "consumed_claim", "route_missing",
                                          "route_invalid", "apply_failure", "apply_foreign_report", "threshold_none",
                                          "threshold_future" );
    CAPTURE( condition );
    override_option llm( "LLM_INTENT_ENABLE", "false" );
    const auto old_world = overmap_buffer.global_state.bandit_live_world;
    const auto old_turn = calendar::turn;
    const auto old_start = calendar::start_of_cataclysm;
    const auto old_game_start = calendar::start_of_game;
    std::vector<std::pair<tripoint_abs_omt, oter_id>> old_terrain;
    on_out_of_scope restore( [&]() {
        remove_retained_actors();
        for( const auto &tile : old_terrain ) {
            overmap_buffer.ter_set( tile.first, tile.second );
        }
        overmap_buffer.global_state.bandit_live_world = old_world;
        calendar::turn = old_turn;
        calendar::start_of_cataclysm = old_start;
        calendar::start_of_game = old_game_start;
    } );
    remove_retained_actors();
    const char *path = std::getenv( "CAOL_R067_BURNED_REPORT_FIXTURE" );
    std::ifstream input( path ? path : "tests/data/r067_burned_report_8406.json" );
    REQUIRE( input.good() );
    auto fixture = json_loader::from_string(
                       std::string( std::istreambuf_iterator<char>( input ), {} ) ).get_object();
    fixture.allow_omitted_members();
    auto provenance = fixture.get_object( "provenance" );
    provenance.allow_omitted_members();
    calendar::turn = time_point::from_turn( provenance.get_int( "turn" ) );
    calendar::start_of_cataclysm = time_point::from_turn( provenance.get_int( "calendar_start" ) );
    calendar::start_of_game = time_point::from_turn( provenance.get_int( "game_start" ) );
    REQUIRE( to_minutes<int>( calendar::turn - calendar::start_of_cataclysm ) == 8406 );
    auto &world = overmap_buffer.global_state.bandit_live_world;
    world.deserialize( fixture.get_object( "world" ) );
    for( JsonObject tile : fixture.get_array( "terrain" ) ) {
        tripoint_abs_omt position;
        tile.read( "position", position );
        old_terrain.emplace_back( position, overmap_buffer.ter( position ) );
        overmap_buffer.ter_set( position, oter_str_id( tile.get_string( "terrain" ) ).id() );
    }
    for( JsonObject record : fixture.get_array( "actors" ) ) {
        auto actor = make_shared_fast<npc>();
        actor->deserialize( record );
        overmap_buffer.insert_npc( actor );
    }
    auto *site = world.find_site( retained_site_id );
    REQUIRE( site );
    const auto original_report = site->current_scout_report;
    REQUIRE( original_report.carrier_ids ==
             std::vector<character_id>{ character_id( 5 ), character_id( 6 ) } );
    REQUIRE( original_report.target_id == "camp-smoke@(131,143,0)" );
    REQUIRE( original_report.target_lead_revision == 34 );
    REQUIRE( original_report.assessment.threshold_class == scout_assessment_threshold_class::burned );
    REQUIRE( site->applied_report_generation == 1 );
    REQUIRE( site->applied_return_generation == 1 );
    REQUIRE_FALSE( site->active_outing.is_active() );
    REQUIRE_FALSE( site->active_hostile_operation.is_active() );
    const auto *unrelated = world.find_hostile_target_opportunity(
                                "player@131,143,0", original_report.target_omt );
    REQUIRE( unrelated );
    const auto original_unrelated = *unrelated;
    REQUIRE( unrelated->revision == 30 );
    REQUIRE( unrelated->consumed_generation == 0 );
    REQUIRE_FALSE( world.find_hostile_target_opportunity(
                       original_report.target_id, original_report.target_omt ) );

    auto &report = site->current_scout_report;
    const bool positive = condition == "burned" || condition == "normal" ||
                          condition == "site_signal";
    if( condition == "normal" ) {
        report.assessment.threshold_class = scout_assessment_threshold_class::normal;
    }
    if( condition == "site_signal" ) {
        report.assessment.threshold_class = scout_assessment_threshold_class::site_signal;
        const auto signal = std::find_if( report.observations.begin(), report.observations.end(),
        []( const auto &observation ) {
            return observation.sense == sortie_observation_sense::smoke;
        } );
        REQUIRE( signal != report.observations.end() );
        report.assessment.site_signal_source_id = signal->source_id;
        report.assessment.site_signal_observed_minutes = signal->observed_minutes;
    }
    if( condition == "below_ready" ) {
        report.assessment.certainty = 49;
    }
    if( condition == "unlatched" ) {
        report.assessment.readiness_latched = false;
    }
    if( condition == "provisional" ) {
        report.provisional = true;
    }
    if( condition == "foreign_source" ) {
        report.source_activity_id = "foreign-site#structural";
        report.application_key = "foreign-site#structural:report:1";
        site->camp_decision.source_report_activity_id = report.source_activity_id;
        site->camp_decision.source_report_application_key = report.application_key;
    }
    if( condition == "missing_carrier" ) {
        report.carrier_ids.clear();
    }
    if( condition == "foreign_carrier" ) {
        report.carrier_ids = { character_id( 99999 ) };
    }
    if( condition == "duplicate_carrier" ) {
        report.carrier_ids.push_back( report.carrier_ids.front() );
    }
    if( condition == "incapable_party" ) {
        for( auto &member : site->members ) {
            member.wounded_or_unready = true;
        }
    }
    if( condition == "nonhome_carrier" ) {
        site->find_member( report.carrier_ids.front() )->state = member_state::outbound;
    }
    if( condition == "wrong_report_key" ) {
        report.application_key += ":wrong";
        site->camp_decision.source_report_application_key = report.application_key;
    }
    if( condition == "wrong_target" ) {
        report.target_id = "foreign-place";
        site->camp_decision.target_id = report.target_id;
    }
    if( condition == "wrong_revision" ) {
        ++report.target_lead_revision;
        report.assessment.pinned_target_revision = report.target_lead_revision;
        site->camp_decision.target_lead_revision = report.target_lead_revision;
    }
    if( condition == "mismatched_lead" ) {
        site->intelligence_map.find_lead( report.target_lead_id )->target_id = "foreign-place";
    }
    if( condition == "invalidated_lead" ) {
        site->intelligence_map.find_lead( report.target_lead_id )->status = camp_lead_status::invalidated;
    }
    if( condition == "threshold_none" || condition == "threshold_future" ) {
        report.assessment.threshold_class = condition == "threshold_none" ?
                                            scout_assessment_threshold_class::none :
                                            static_cast<scout_assessment_threshold_class>( 99 );
    }
    if( condition == "conflicting_claim" || condition == "consumed_claim" ) {
        // This exact-target negative is declared separately from the real,
        // unrelated player@... revision30 receipt; neither may be overwritten.
        hostile_target_opportunity_record conflict;
        conflict.target_id = original_report.target_id;
        conflict.target_omt = original_report.target_omt;
        conflict.revision = condition == "conflicting_claim" ? 35 : 34;
        if( condition == "consumed_claim" ) {
            conflict.consumed_operation_id = "foreign#hostile:9";
            conflict.consumed_report_key = "foreign:report:9";
            conflict.consumed_generation = 9;
        }
        world.hostile_target_opportunities.push_back( conflict );
    }
    // The retained save has already serviced8400. Use the next real hourly
    // owner call; no scheduler cursor, report timestamp or claim is rewound.
    const int service = condition == "expired" ? report.delivered_minutes + 48 * 60 + 60 : 8460;
    calendar::turn = calendar::start_of_cataclysm + time_duration::from_minutes( service );
    const auto power_reader = []( const site_record &value ) {
        return live_bandit_response_member_power_reads_for_test( value );
    };
    const auto route_reader = [&condition]( const site_record &value ) {
        auto route = live_bandit_hostile_operation_route_read_for_test( value );
        if( condition == "route_missing" ) {
            route.reset();
        } else if( condition == "route_invalid" && route ) {
            route->route = { value.anchor };
        }
        return route;
    };
    const auto apply = [&condition]( site_record &value,
    const authorized_hostile_operation_plan &plan ) {
        const bool applied = apply_hostile_operation_plan_with_authorized_response( value, plan );
        if( applied && condition == "apply_foreign_report" ) {
            value.active_hostile_operation.source_report_application_key = "foreign:report:9";
        }
        return applied && condition != "apply_failure";
    };
    if( positive ) {
        const auto reads = power_reader( *site );
        const auto party = select_capable_response_party( *site, report.action_policy,
                           report.assessment.danger_high, reads );
        INFO( party.rejection_reason );
        REQUIRE( party.eligible );
        const auto auth = evaluate_response_authorization( *site, service, party, reads );
        INFO( auth.rejection_reason );
        REQUIRE( auth.authorized );
        REQUIRE( route_reader( *site ) );
    }
    const std::string before_claims = serialized_claims( world );
    const std::string before_decision = serialized( site->camp_decision );
    const std::string before_operation = serialized( site->active_hostile_operation );
    const std::string before_report = serialized( report );
    const int next_generation = site->next_outing_generation;
    const auto result = advance_structural_bounty_maintenance(
                            world, service, 0, 0, {}, {}, {}, {}, {}, {}, power_reader, {}, route_reader, apply );
    INFO( render_structural_bounty_maintenance_report( result ) );
    CHECK( result.response_operations_applied == ( positive ? 1 : 0 ) );
    CHECK( site->active_hostile_operation.is_active() == positive );
    CHECK( serialized( site->current_scout_report ) == before_report );
    if( positive ) {
        REQUIRE( site->active_hostile_operation.is_active() );
        const auto &operation = site->active_hostile_operation;
        CHECK( operation.reservation.generation == next_generation );
        CHECK( operation.reservation.target_id == report.target_id );
        CHECK( operation.reservation.target_omt == report.target_omt );
        CHECK( operation.reservation.target_lead_revision == report.target_lead_revision );
        CHECK( operation.source_report_application_key == report.application_key );
        CHECK( operation.source_report_generation == report.source_generation );
        CHECK( operation.source_report_revision == report.revision );
        CHECK( operation.reservation.member_ids.size() >= 2 );
        const auto *claim = world.find_hostile_target_opportunity( report.target_id, report.target_omt );
        REQUIRE( claim );
        CHECK( claim->revision == report.target_lead_revision );
        CHECK( claim->goods_value == 0 );
        CHECK( claim->population == 0 );
        CHECK( claim->activity == 0 );
        CHECK( claim->consumed_generation == operation.reservation.generation );
        CHECK( claim->consumed_operation_id == operation.reservation.activity_id );
        CHECK( claim->consumed_report_key == report.application_key );
        CHECK( site->next_outing_generation == next_generation + 1 );
        auto reload = reloaded_world( world );
        REQUIRE( serialized( reload ) == serialized( world ) );
        const std::string saved = serialized( reload );
        const auto repeated = advance_structural_bounty_maintenance(
                                  reload, service, 0, 0, {}, {}, {}, {}, {}, {}, power_reader, {}, route_reader, apply );
        CHECK( repeated.scheduler_replay_suppressed );
        CHECK( serialized( reload ) == saved );
        const auto *restored = reload.find_site( retained_site_id );
        REQUIRE( restored );
        CHECK( restored->active_hostile_operation.reservation.generation == next_generation );
        CHECK( restored->applied_report_generation == 1 );
        CHECK( restored->applied_return_generation == 1 );
    } else {
        CHECK( serialized_claims( world ) == before_claims );
        CHECK( serialized( site->active_hostile_operation ) == before_operation );
        CHECK( site->next_outing_generation == next_generation );
        if( condition == "wrong_target" ) {
            // Existing denial owner abandons an unaccepted report identity;
            // the operation and claim owners still remain unchanged.
            CHECK( result.response_decisions_abandoned == 1 );
            CHECK( site->camp_decision.state == camp_decision_state::abandoned );
            CHECK( site->camp_decision.transition_reason ==
                   "response assessment abandoned after losing its accepted report identity" );
        } else if( condition == "expired" ) {
            CHECK( result.response_rescouts_ready == 1 );
            CHECK( site->camp_decision.state == camp_decision_state::idle );
        } else {
            CHECK( serialized( site->camp_decision ) == before_decision );
        }
    }
    unrelated = world.find_hostile_target_opportunity( "player@131,143,0", original_report.target_omt );
    REQUIRE( unrelated );
    CHECK( unrelated->revision == original_unrelated.revision );
    CHECK( unrelated->goods_value == original_unrelated.goods_value );
    CHECK( unrelated->population == original_unrelated.population );
    CHECK( unrelated->activity == original_unrelated.activity );
    CHECK( unrelated->consumed_generation == 0 );
    CHECK( unrelated->consumed_operation_id.empty() );
    CHECK( unrelated->consumed_report_key.empty() );
    const std::string after = serialized( world );
    advance_structural_bounty_maintenance(
        world, service, 0, 0, {}, {}, {}, {}, {}, {}, power_reader, {}, route_reader, apply );
    CHECK( serialized( world ) == after );
}
