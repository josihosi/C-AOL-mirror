#include "cata_catch.h"
#include "bandit_live_world.h"
#include "cata_scope_helpers.h"
#include "coordinates.h"
#include "json.h"
#include "json_loader.h"
#include "npc.h"
#include "overmapbuffer.h"

#include <fstream>
#include <iterator>
#include <string>

bandit_live_world::local_projection_reconciliation_result
reconcile_loaded_bandit_live_world_projections_for_test();

TEST_CASE( "remote saved local scouts are loaded before lease reconciliation",
           "[.][bandit][projection_lease][remote_overmap]" )
{
    const int mode = GENERATE( 0, 1, 2 ); // complete, missing member, mismatched epoch
    CAPTURE( mode );
    std::ifstream input( "tests/data/r054_local_ownership_clock.json" );
    REQUIRE( input.good() );
    const std::string bytes( std::istreambuf_iterator<char>( input ), {} );
    JsonObject fixture = json_loader::from_string( bytes ).get_object().get_array( "cases" ).get_object( 0 );
    fixture.allow_omitted_members();
    bandit_live_world::world_state saved;
    saved.deserialize( fixture.get_object( "world" ) );
    const std::string site_id = "overmap_special:bandit_camp@129,149,0";
    const auto *original = saved.find_site( site_id );
    REQUIRE( original );
    REQUIRE( original->active_outing.owner == bandit_live_world::simulation_owner::local );
    REQUIRE( original->active_outing.local_handoff.members.size() == 2 );
    const point_abs_om home_om = project_to<coords::om>(
                                     original->active_outing.local_handoff.members.front().entry_position.xy() );
    const auto prior_world = overmap_buffer.global_state.bandit_live_world;
    on_out_of_scope cleanup( [&]() {
        overmap_buffer.clear();
        overmap_buffer.global_state.bandit_live_world = prior_world;
    } );
    overmap_buffer.clear();
    // Each generated case owns its saved NPCs and removes the previous case's
    // records before exercising the real disk-backed overmap loader.
    overmap_buffer.get_existing( home_om );
    for( const character_id id : { character_id( 4 ), character_id( 5 ) } ) {
        if( overmap_buffer.find_npc( id ) ) {
            overmap_buffer.remove_npc( id );
        }
    }
    for( JsonObject record : fixture.get_array( "npcs" ) ) {
        auto actor = make_shared_fast<npc>();
        actor->deserialize( record );
        if( mode == 1 && actor->getID() == character_id( 5 ) ) {
            continue;
        }
        if( mode == 2 && actor->getID() == character_id( 5 ) ) {
            auto lease = actor->get_bandit_live_world_projection_lease();
            ++lease.handoff_epoch;
            actor->set_bandit_live_world_projection_lease( lease );
        }
        overmap_buffer.insert_npc( actor );
    }
    overmap_buffer.save();
    overmap_buffer.clear();
    overmap_buffer.global_state.bandit_live_world = saved;
    CHECK_FALSE( overmap_buffer.find_npc( character_id( 4 ) ) );
    CHECK_FALSE( overmap_buffer.find_npc( character_id( 5 ) ) );
    const auto result = reconcile_loaded_bandit_live_world_projections_for_test();
    const auto *site = overmap_buffer.global_state.bandit_live_world.find_site( site_id );
    REQUIRE( site );
    if( mode == 0 ) {
        CHECK( result == bandit_live_world::local_projection_reconciliation_result::repaired );
        CHECK_FALSE( site->active_outing.local_projection_reconciliation_rejected );
        CHECK( overmap_buffer.find_npc( character_id( 4 ) ) );
        CHECK( overmap_buffer.find_npc( character_id( 5 ) ) );
        CHECK( site->active_outing.handoff_epoch == 1 );
        CHECK( site->active_outing.last_advanced_minutes == 9246 );
        for( const character_id id : { character_id( 4 ), character_id( 5 ) } ) {
            const auto actor = overmap_buffer.find_npc( id );
            REQUIRE( actor );
            CHECK( actor->get_bandit_live_world_projection_lease().last_advanced_minutes == 9246 );
            CHECK( actor->hp_percentage() == 100 );
        }
        CHECK( overmap_buffer.find_npc( character_id( 4 ) )->pos_abs() ==
               tripoint_abs_ms( 3084, 3479, 0 ) );
        CHECK( overmap_buffer.find_npc( character_id( 5 ) )->pos_abs() ==
               tripoint_abs_ms( 3084, 3480, 0 ) );
    } else {
        CHECK( result == bandit_live_world::local_projection_reconciliation_result::rejected );
        CHECK( site->active_outing.local_projection_reconciliation_rejected );
        CHECK( site->active_outing.handoff_epoch == 1 );
        CHECK( site->active_outing.last_advanced_minutes == 9246 );
    }
}
