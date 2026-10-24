#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "avatar.h"
#include "bandit_live_world.h"
#include "bandit_live_world_probe.h"
#include "calendar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "city.h"
#include "common_types.h"
#include "coordinates.h"
#include "debug.h"
#include "do_turn.h"
#include "effect.h"
#include "enum_conversions.h"
#include "enums.h"
#include "game.h"
#include "global_vars.h"
#include "item.h"
#include "item_factory.h"
#include "json.h"
#include "json_loader.h"
#include "itype.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "map_iterator.h"
#include "map_scale_constants.h"
#include "mapbuffer.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "omdata.h"
#include "options.h"
#include "options_helpers.h"
#include "output.h"
#include "overmap.h"
#include "overmap_location.h"
#include "overmap_types.h"
#include "overmapbuffer.h"
#include "point.h"
#include "player_helpers.h"
#include "recipe.h"
#include "regional_settings.h"
#include "rng.h"
#include "test_data.h"
#include "type_id.h"
#include "value_ptr.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "writhing_stalker_ai.h"
#include "worldfactory.h"
#include "zombie_rider_overmap_ai.h"

static const overmap_special_id overmap_special_Cabin( "Cabin" );
static const mtype_id mon_zombie( "mon_zombie" );
static const mtype_id mon_zombie_predator( "mon_zombie_predator" );

TEST_CASE( "human activation keeps a living party together and resumes native bodies once",
           "[overmap][human_activation]" )
{
    using namespace bandit_live_world;
    override_option food( "NO_NPC_FOOD", "false" );
    const int inside_distance = GENERATE( 25, 39, 40, 41 );
    const auto old_world = overmap_buffer.global_state.bandit_live_world;
    const auto old_turn = calendar::turn;
    const auto old_start = calendar::start_of_cataclysm;
    const auto old_player = get_avatar().pos_abs();
    std::vector<shared_ptr_fast<npc>> actors;
    shared_ptr_fast<npc> resident;
    on_out_of_scope restore( [&] {
        for( const auto &actor : actors ) {
            overmap_buffer.remove_npc( actor->getID() );
        }
        if( resident ) {
            overmap_buffer.remove_npc( resident->getID() );
        }
        overmap_buffer.global_state.bandit_live_world = old_world;
        calendar::turn = old_turn;
        calendar::start_of_cataclysm = old_start;
        get_avatar().setpos( old_player, false );
    } );
    calendar::turn = calendar::start_of_cataclysm + 1000_minutes;
    const tripoint_abs_omt player( 140, 145, 0 );
    get_avatar().setpos( project_to<coords::ms>( player ), false );
    auto &world = overmap_buffer.global_state.bandit_live_world;
    world = world_state();
    world.sites.emplace_back();
    auto &site = world.sites.back();
    site.site_id = "human-activation-control";
    site.anchor = player + tripoint( 45, 0, 0 );
    site.living_total = 3;
    site.supply_units = 5;
    site.supply_last_update_minutes = 1000;
    site.supply_accounted_living_total = 3;
    resident = make_shared_fast<npc>();
    clear_character( *resident );
    resident->setID( g->assign_npc_id() );
    resident->spawn_at_precise( project_to<coords::ms>( site.anchor ) );
    overmap_buffer.insert_npc( resident );
    member_record home;
    home.npc_id = resident->getID();
    home.state = member_state::at_home;
    home.home_spawn_tile = resident->pos_abs();
    home.wounded_or_unready = true;
    home.abstract_wound_until_minutes = 1060;
    site.members.push_back( home );
    auto &outing = site.active_outing;
    outing.kind = outing_kind::scout_sortie;
    outing.activity_id = "human-activation-party";
    outing.camp_id = site.site_id;
    outing.generation = 1;
    outing.phase = scout_phase::outbound;
    outing.started_minutes = outing.last_advanced_minutes = outing.last_progress_minutes = 1000;
    outing.expected_return_minutes = 1120;
    outing.missing_deadline_minutes = 1180;
    outing.assessment.observation_started_minutes = 1000;
    outing.assessment.last_progress_minutes = 1000;
    for( const int distance : {
             inside_distance, 41
         } ) {
        auto actor = make_shared_fast<npc>();
        clear_character( *actor );
        actor->setID( g->assign_npc_id() );
        actor->spawn_at_precise( project_to<coords::ms>( player + tripoint( distance, 0, 0 ) ) );
        actor->add_effect( efftype_id( "stunned" ), 3_hours );
        overmap_buffer.insert_npc( actor );
        actors.push_back( actor );
        outing.member_ids.push_back( actor->getID() );
        member_record away;
        away.npc_id = actor->getID();
        away.state = member_state::outbound;
        away.home_spawn_tile = resident->pos_abs();
        site.members.push_back( away );
    }
    outing.leader_id = outing.member_ids.front();
    REQUIRE( outing.is_active() );
    refresh_live_bandit_activation();
    REQUIRE( outing.activation.initialized() );
    CHECK_FALSE( site.home_activation.active );
    CHECK( outing.activation.active == ( inside_distance <= 40 ) );
    // All living partners share this interval, even the body at41. This is
    // genuine native physiology, not a movement or successful-party claim.
    calendar::turn += 1_minutes;
    refresh_live_bandit_activation();
    for( const auto &actor : actors ) {
        CHECK( actor->catch_up_body() == ( inside_distance <= 40 ? 1_minutes : 0_turns ) );
        CHECK( actor->catch_up_body() == 0_turns );
    }
    CHECK( resident->catch_up_body() == 0_turns );
    CHECK( resident->get_last_updated() == calendar::start_of_cataclysm + 1000_minutes );
    if( inside_distance > 40 ) {
        return;
    }
    SECTION( "an inside corpse cannot keep an outside survivor active" ) {
        actors.front()->set_part_hp_cur( bodypart_id( "torso" ), 0 );
        REQUIRE( actors.front()->is_dead() );
        refresh_live_bandit_activation();
        CHECK_FALSE( outing.activation.active );
        CHECK( actors.back()->catch_up_body() == 0_turns );
        // All-confirmed-dead remains eligible for the ordinary terminal
        // casualty owner, rather than creating an immortal frozen party.
        actors.back()->set_part_hp_cur( bodypart_id( "torso" ), 0 );
        REQUIRE( actors.back()->is_dead() );
        refresh_live_bandit_activation();
        CHECK( outing.activation.active );
    }
    SECTION( "freeze reload and resume preserve native physiology and home timers" ) {
        const auto duration_before = actors.back()->get_effect( efftype_id( "stunned" ) ).get_duration();
        const auto updated_before = actors.back()->get_last_updated();
        const std::array needs_before{ actors.back()->get_hunger(), actors.back()->get_thirst(),
                                       actors.back()->get_sleepiness() };
        // Move only the observer. Both real bodies now lie outside the bubble.
        get_avatar().setpos( project_to<coords::ms>( player - tripoint( 50, 0, 0 ) ), false );
        refresh_live_bandit_activation();
        REQUIRE_FALSE( outing.activation.active );
        const auto positions = std::array{ actors.front()->pos_abs(), actors.back()->pos_abs() };
        calendar::turn += 2_days;
        refresh_live_bandit_activation();
        for( std::size_t i = 0; i < actors.size(); ++i ) {
            CHECK( actors[i]->catch_up_body() == 0_turns );
            actors[i]->on_load( &get_map() );
            actors[i]->on_unload();
            actors[i]->npc_update_body();
            CHECK( actors[i]->pos_abs() == positions[i] );
            CHECK( actors[i]->get_last_updated() == updated_before );
        }
        CHECK( actors.back()->get_effect( efftype_id( "stunned" ) ).get_duration() == duration_before );
        CHECK( ( std::array{ actors.back()->get_hunger(), actors.back()->get_thirst(),
                             actors.back()->get_sleepiness() } ) == needs_before );
        CHECK( outing.last_advanced_minutes == 1000 );
        CHECK( outing.expected_return_minutes == 1120 );
        CHECK( outing.assessment.observation_started_minutes == 1000 );
        const int now = to_minutes<int>( calendar::turn - calendar::start_of_cataclysm );
        CHECK( outing.activation.elapsed_minutes( activation_timer::watch_started, 1000, now ) == 1 );
        CHECK( outing.activation.elapsed_turns( activation_timer::expected_return, 1120, now ) < 0 );
        CHECK_FALSE( advance_camp_supply( site, now ) );
        CHECK( site.supply_units == 5 );
        CHECK( site.supply_last_update_minutes == 1000 );
        CHECK( site.home_activation.member_deadline_pending( resident->getID(), 1060, now ) );

        // Serialization canonicalizes only changed derived keys. It cannot
        // mutate runtime state, forgive an unchanged pause, or advance a body.
        const auto clock_bytes = []( const activation_clock &clock ) {
            std::ostringstream bytes;
            JsonOut output( bytes );
            clock.serialize( output );
            return bytes.str();
        };
        const auto entity_clock = [&]( const auto &entity, const std::string &key ) {
            std::ostringstream bytes;
            JsonOut output( bytes );
            entity.serialize( output );
            activation_clock restored;
            JsonObject entity_json = json_loader::from_string( bytes.str() ).get_object();
            entity_json.allow_omitted_members();
            restored.deserialize( entity_json.get_object( key ) );
            return restored;
        };
        const auto frozen_runtime = clock_bytes( outing.activation );
        const auto frozen_saved = entity_clock( outing, "activation" );
        CHECK( clock_bytes( outing.activation ) == frozen_runtime );
        CHECK( frozen_saved.elapsed_turns( activation_timer::watch_started, 1000, now ) ==
               outing.activation.elapsed_turns( activation_timer::watch_started, 1000, now ) );
        CHECK( frozen_saved.timers[static_cast<std::size_t>( activation_timer::watch_started )].suspended_turns > 0 );
        for( const auto &actor : actors ) {
            const int body_turn = to_turns<int>( actor->get_last_updated() - calendar::start_of_cataclysm );
            CHECK( frozen_saved.suspended_body_turns( actor->getID(), body_turn ) ==
                   outing.activation.suspended_body_turns( actor->getID(), body_turn ) );
        }
        // The persisted clock, not an in-memory pause journal, carries the debt.
        std::ostringstream saved;
        JsonOut js( saved );
        outing.activation.serialize( js );
        activation_clock loaded;
        loaded.deserialize( json_loader::from_string( saved.str() ).get_object() );
        outing.activation = loaded;
        get_avatar().setpos( project_to<coords::ms>( player ), false );
        refresh_live_bandit_activation();
        REQUIRE( outing.activation.active );
        for( const auto &actor : actors ) {
            CHECK( actor->catch_up_body() == 0_turns );
            CHECK( actor->get_last_updated() == calendar::turn );
            CHECK( actor->catch_up_body() == 0_turns );
        }
        calendar::turn += 1_minutes;
        refresh_live_bandit_activation();
        for( const auto &actor : actors ) {
            CHECK( actor->catch_up_body() == 1_minutes );
            CHECK( actor->catch_up_body() == 0_turns );
        }
        CHECK( outing.activation.elapsed_minutes( activation_timer::watch_started, 1000, now + 1 ) == 2 );
        // The real home resident and supply owner resume independently, with no
        // missed-day consumption or abstract wound recovery on the first callback.
        get_avatar().setpos( project_to<coords::ms>( site.anchor ), false );
        refresh_live_bandit_activation();
        REQUIRE( site.home_activation.active );
        advance_camp_supply( site, now + 1 );
        CHECK( site.supply_units == 5 );
        CHECK( site.home_activation.member_deadline_pending( resident->getID(), 1060, now + 1 ) );
        calendar::turn += 1_days;
        refresh_live_bandit_activation();
        advance_camp_supply( site, now + 1 + 1440 );
        CHECK( site.supply_units == 2 );
        const auto supplied_runtime = clock_bytes( site.home_activation );
        const auto supplied_saved = entity_clock( site, "home_activation" );
        CHECK( clock_bytes( site.home_activation ) == supplied_runtime );
        const auto supply_age = site.home_activation.elapsed_turns(
                                    activation_timer::supply, site.supply_last_update_minutes, now + 1 + 1440 );
        CHECK( supply_age == 0 );
        CHECK( supplied_saved.elapsed_turns( activation_timer::supply,
                site.supply_last_update_minutes, now + 1 + 1440 ) == supply_age );
        CHECK( supplied_saved.timers[static_cast<std::size_t>( activation_timer::supply )].suspended_turns == 0 );
        const auto resident_age = resident->get_last_updated();
        site.home_activation = supplied_saved;
        refresh_live_bandit_activation();
        advance_camp_supply( site, now + 1 + 1440 );
        CHECK( site.supply_units == 2 );
        CHECK( resident->get_last_updated() == resident_age );
        CHECK( site.home_activation.elapsed_turns( activation_timer::supply,
                site.supply_last_update_minutes, now + 1 + 1440 ) == supply_age );
        CHECK_FALSE( site.home_activation.member_deadline_pending( resident->getID(), 1060,
                now + 1 + 1440 ) );
    }
}

TEST_CASE( "OMT NPC query includes its radius across overmap seams",
           "[overmap][npc_travel_radius]" )
{
    // 151 +/- 40 straddles the 180-OMT seam, but a wrongly unconverted
    // 40-submap enumeration remains on the near side. Mirror at negative
    // coordinates to exercise floor projection on both sides of zero.
    const int sign = GENERATE( -1, 1 );
    const tripoint_abs_omt center( sign * 151, sign * 151, 0 );
    std::vector<character_id> inserted;
    on_out_of_scope remove_inserted( [&] {
        for( const character_id id : inserted ) {
            overmap_buffer.remove_npc( id );
        }
    } );
    std::set<character_id> expected;
    for( const int distance : {
             0, 39, 40, 41
         } ) {
        for( const point direction : {
                 point::east, point::south, point::south_east
             } ) {
            auto actor = make_shared_fast<npc>();
            actor->setID( g->assign_npc_id() );
            const tripoint_abs_omt position = center + tripoint( direction * sign * distance,
                distance == 40 ? 5 : 0 );
            actor->spawn_at_precise( project_to<coords::ms>( position ) );
            REQUIRE( actor->pos_abs_omt() == position );
            overmap_buffer.insert_npc( actor );
            inserted.push_back( actor->getID() );
            if( distance <= 40 ) {
                expected.insert( actor->getID() );
            }
        }
    }
    std::set<character_id> found;
    for( const auto &actor : overmap_buffer.get_npcs_near_omt( center, 40 ) ) {
        if( std::find( inserted.begin(), inserted.end(), actor->getID() ) != inserted.end() ) {
            CHECK( found.insert( actor->getID() ).second );
        }
    }
    CHECK( found == expected );
    // A zero-radius query still means this XY OMT across all floors.
    int at_center = 0;
    for( const auto &actor : overmap_buffer.get_npcs_near_omt( center, 0 ) ) {
        at_center += std::find( inserted.begin(), inserted.end(), actor->getID() ) != inserted.end();
    }
    CHECK( at_center == 3 );
}

class overmap_test_helper
{
    public:
        static void set_highway_connections(
            overmap &om,
            const std::array<tripoint_om_omt, 4> &conns ) {
            om.highway_connections = conns;
        }
        static const std::array<tripoint_om_omt, 4> &get_highway_connections(
            const overmap &om ) {
            return om.highway_connections;
        }
        static bool highway_select_end_points(
            overmap &om, const std::vector<const overmap *> &neighbor_overmaps,
            std::array<tripoint_om_omt, 4> &end_points,
            std::bitset<4> &neighbor_connections ) {
            return om.highway_select_end_points( neighbor_overmaps, end_points,
                                                 neighbor_connections, std::bitset<4>(), 0 );
        }
        static void move_hordes( overmap &om ) {
            om.move_hordes();
        }
};

static std::vector<oter_flags> all_oter_flags()
{
    const int max_flag = static_cast<int>( oter_flags::num_oter_flags );
    std::vector<oter_flags> flags;
    flags.reserve( max_flag );
    for( int i = 0; i < max_flag; ++i ) {
        flags.emplace_back( static_cast<oter_flags>( i ) );
    }
    return flags;
}

static std::vector<std::string> location_flag_strings()
{
    std::unordered_set<std::string> unique_flags;
    for( const overmap_location &loc : overmap_locations::get_all() ) {
        for( const std::string &flag : loc.get_flags() ) {
            unique_flags.insert( flag );
        }
    }
    std::vector<std::string> result( unique_flags.begin(), unique_flags.end() );
    std::sort( result.begin(), result.end() );
    return result;
}

static bool any_terrain_with_flag( const oter_flags flag )
{
    for( const oter_t &ter : overmap_terrains::get_all() ) {
        if( ter.has_flag( flag ) ) {
            return true;
        }
    }
    return false;
}

TEST_CASE( "predator cross-overmap handoff cannot advance twice in one turn",
           "[overmap][hordes][predator]" )
{
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    calendar::turn = calendar::turn_zero;
    clear_overmaps();
    clear_map_and_put_player_underground();

    const point_abs_om source_om_pos = project_to<coords::om>(
                                           get_player_character().pos_abs() ).xy();
    const point_abs_om destination_om_pos = source_om_pos + point::east;
    overmap_buffer.get( source_om_pos );
    overmap_buffer.get( destination_om_pos );
    overmap *source_om = overmap_buffer.get_existing( source_om_pos );
    overmap *destination_om = overmap_buffer.get_existing( destination_om_pos );
    REQUIRE( source_om != nullptr );
    REQUIRE( destination_om != nullptr );

    const tripoint_abs_ms source = project_combine( source_om_pos,
                                   tripoint_om_ms( OMAPX * 2 * SEEX - 1, 20 * SEEX, 0 ) );
    const auto old_avatar = get_avatar().pos_abs();
    on_out_of_scope restore_avatar( [&]() { get_avatar().setpos( old_avatar, false ); } );
    get_avatar().setpos( project_to<coords::ms>( project_to<coords::omt>( source ) - point( 39, 0 ) ), false );
    const tripoint_abs_ms handoff = source + point_rel_ms( 1, 0 );
    const tripoint_abs_ms goal = handoff + point_rel_ms( 1, 0 );
    REQUIRE( project_to<coords::om>( handoff ).xy() == destination_om_pos );
    REQUIRE_FALSE( get_map().inbounds( handoff ) );
    overmap_buffer.set_passable( handoff, true );
    overmap_buffer.set_passable( goal, true );

    point_abs_om ignored_om;
    tripoint_om_ms source_local;
    tripoint_om_ms handoff_local;
    std::tie( ignored_om, source_local ) = project_remain<coords::om>( source );
    std::tie( ignored_om, handoff_local ) = project_remain<coords::om>( handoff );
    for( int dx = -1; dx <= 1; ++dx ) {
        for( int dy = -1; dy <= 1; ++dy ) {
            const tripoint_abs_ms candidate = source + point_rel_ms( dx, dy );
            if( candidate != handoff ) {
                overmap_buffer.set_passable( candidate, false );
            }
        }
    }
    for( const mtype_id &type : {
             mtype_id( "mon_writhing_stalker" ),
             mtype_id( "mon_zombie_rider" )
         } ) {
        // A short-lived alert horde supplies a native active-map node.  Its
        // public extraction/insertion API lets this test put the predator
        // representation into that same production scheduler bucket without
        // adding a production-only test hook.
        monster carrier( mon_zombie );
        carrier.wander_to( goal, 1 );
        REQUIRE( source_om->hordes.spawn_entity( source, carrier ) );
        horde_map::iterator carrier_node = source_om->hordes.find( source_local );
        REQUIRE( carrier_node != source_om->hordes.end() );
        horde_map::node_type node = source_om->hordes.extract( carrier_node );

        monster original( type );
        original.set_moves( 47 );
        original.wander_to( goal, 9 );
        node.mapped() = horde_entity( original );
        source_om->hordes.insert( std::move( node ) );
        horde_entity *outbound = source_om->entity_at( source_local );
        REQUIRE( outbound != nullptr );
        REQUIRE( outbound->monster_data != nullptr );
        outbound->monster_data->set_hp( 83 );
        outbound->monster_data->add_item( item( itype_id( "rock" ) ) );
        outbound->monster_data->ammo[itype_id( "zombie_rider_tainted_bone_arrow" )] = 0;
        outbound->monster_data->predator_state().handoff_epoch = 5;
        // Native zero-action adoption precedes this one real active interval.
        // Seed current intent only afterward; the tested handoff is not a
        // fabricated backlog grant at first cold binding.
        source_om->hordes.expire_light_interest( calendar::turn );
        calendar::turn += 1_turns;
        outbound->monster_data->predator_state().last_advanced_turn = to_turn<int>( calendar::turn );
        const std::string actor_id = outbound->monster_data->predator_state().actor_id;
        if( type == mtype_id( "mon_writhing_stalker" ) ) {
            writhing_stalker::persistent_state &state = outbound->monster_data->writhing_stalker_state();
            state.phase = writhing_stalker::lifecycle_phase::cooldown;
            state.cooldown_until_turn = to_turn<int>( calendar::turn ) + 20;
        } else {
            zombie_rider_overmap_ai::rider_pursuit_state &state =
                outbound->monster_data->zombie_rider_pursuit_state();
            state.observe( 42, goal, to_turn<int>( calendar::turn ) - 1 );
        }
        const int expected_moves = 47 + type->speed - 100;

        // This native friend-access helper invokes the source scheduler only,
        // then the receiving scheduler below in the same calendar turn.
        overmap_test_helper::move_hordes( *source_om );
        horde_entity *const source_after = source_om->entity_at( source_local );
        CAPTURE( source_after != nullptr ? source_after->tracking_intensity : -1 );
        CAPTURE( source_after != nullptr ? source_after->moves : -1 );
        CAPTURE( source_after != nullptr ? source_after->last_processed : calendar::turn_zero );
        REQUIRE( source_om->entity_at( source_local ) == nullptr );
        horde_entity *received = destination_om->entity_at( handoff_local );
        REQUIRE( received != nullptr );
        REQUIRE( received->monster_data != nullptr );
        CHECK( received->destination == goal );
        CHECK( received->tracking_intensity == 8 );
        CHECK( received->moves == expected_moves );
        CHECK( received->last_processed == calendar::turn );
        CHECK( received->monster_data->pos_abs() == handoff );
        CHECK( received->monster_data->get_hp() == 83 );
        CHECK( received->monster_data->inv.size() == 1 );
        CHECK( received->monster_data->inv.front().typeId() == itype_id( "rock" ) );
        CHECK( received->monster_data->ammo[itype_id( "zombie_rider_tainted_bone_arrow" )] == 0 );
        CHECK( received->monster_data->predator_state().actor_id == actor_id );
        CHECK( received->monster_data->predator_state().handoff_epoch == 5 );
        if( type == mtype_id( "mon_writhing_stalker" ) ) {
            const writhing_stalker::persistent_state &state =
                received->monster_data->writhing_stalker_state();
            CHECK( state.phase == writhing_stalker::lifecycle_phase::cooldown );
            CHECK( state.cooldown_until_turn == to_turn<int>( calendar::turn ) + 20 );
        } else {
            const zombie_rider_overmap_ai::rider_pursuit_state &state =
                received->monster_data->zombie_rider_pursuit_state();
            CHECK( state.target_identity == 42 );
            CHECK( state.has_last_observed_position );
            CHECK( state.last_observed_position == goal );
        }

        // This is the material guard: the receiving OMap is visited again in
        // the same turn, and must retain rather than debit/move the handoff.
        overmap_test_helper::move_hordes( *destination_om );
        received = destination_om->entity_at( handoff_local );
        REQUIRE( received != nullptr );
        CHECK( overmap_buffer.entity_at( goal ) == nullptr );
        CHECK( received->tracking_intensity == 8 );
        CHECK( received->moves == expected_moves );
        CHECK( received->last_processed == calendar::turn );
        CHECK( received->monster_data->pos_abs() == handoff );
        CHECK( received->monster_data->get_hp() == 83 );
        CHECK( received->monster_data->ammo[itype_id( "zombie_rider_tainted_bone_arrow" )] == 0 );
        CHECK( received->monster_data->predator_state().actor_id == actor_id );
        CHECK( received->monster_data->predator_state().handoff_epoch == 5 );

        source_om->hordes.clear();
        destination_om->hordes.clear();
    }
}

TEST_CASE( "just-evolved predator uses transactional ownership handoff",
           "[overmap][hordes][predator][evolution]" )
{
    restore_on_out_of_scope restore_calendar_turn( calendar::turn );
    restore_on_out_of_scope restore_start_of_cataclysm( calendar::start_of_cataclysm );
    clear_overmaps();
    clear_map_and_put_player_underground();
    calendar::start_of_cataclysm = calendar::turn_zero;
    calendar::turn = calendar::start_of_cataclysm +
                     zombie_rider_overmap_ai::mature_world_gate_days() * 1_days + 10000_days;

    const point_abs_om source_om_pos = project_to<coords::om>(
                                           get_player_character().pos_abs() ).xy();
    const point_abs_om destination_om_pos = source_om_pos + point::east;
    overmap_buffer.get( source_om_pos );
    overmap_buffer.get( destination_om_pos );
    overmap *source_om = overmap_buffer.get_existing( source_om_pos );
    overmap *destination_om = overmap_buffer.get_existing( destination_om_pos );
    REQUIRE( source_om != nullptr );
    REQUIRE( destination_om != nullptr );

    const tripoint_abs_ms source = project_combine( source_om_pos,
                                   tripoint_om_ms( OMAPX * 2 * SEEX - 1, 20 * SEEX, 0 ) );
    const tripoint_abs_ms handoff = source + point_rel_ms( 1, 0 );
    REQUIRE( project_to<coords::om>( handoff ).xy() == destination_om_pos );
    REQUIRE_FALSE( get_map().inbounds( handoff ) );
    overmap_buffer.set_passable( handoff, true );

    point_abs_om ignored_om;
    tripoint_om_ms source_local;
    tripoint_om_ms handoff_local;
    std::tie( ignored_om, source_local ) = project_remain<coords::om>( source );
    std::tie( ignored_om, handoff_local ) = project_remain<coords::om>( handoff );
    for( int dx = -1; dx <= 1; ++dx ) {
        for( int dy = -1; dy <= 1; ++dy ) {
            const tripoint_abs_ms candidate = source + point_rel_ms( dx, dy );
            if( candidate != handoff ) {
                overmap_buffer.set_passable( candidate, false );
            }
        }
    }

    unsigned int evolution_seed = 0;
    for( unsigned int seed = 1; seed != 10000 && evolution_seed == 0; ++seed ) {
        rng_set_engine_seed( seed );
        horde_entity probe( mon_zombie_predator );
        if( probe.advance_evolution( source ) && probe.monster_data &&
            probe.monster_data->is_caol_predator() ) {
            evolution_seed = seed;
        }
    }
    REQUIRE( evolution_seed != 0 );
    rng_set_engine_seed( evolution_seed );

    // Use the production active bucket, but retain a type-only predator.  The
    // mature world age makes the normal monster::try_upgrade path select the
    // configured predator -> rider endpoint without poly() or a debug spawn.
    monster carrier( mon_zombie );
    carrier.wander_to( handoff, 1 );
    REQUIRE( source_om->hordes.spawn_entity( source, carrier ) );
    horde_map::iterator carrier_node = source_om->hordes.find( source_local );
    REQUIRE( carrier_node != source_om->hordes.end() );
    horde_map::node_type node = source_om->hordes.extract( carrier_node );
    horde_entity lightweight( mon_zombie_predator );
    lightweight.destination = handoff;
    lightweight.tracking_intensity = 9;
    lightweight.moves = 47;
    node.mapped() = std::move( lightweight );
    source_om->hordes.insert( std::move( node ) );
    monster blocker( mon_zombie );
    blocker.wander_to( handoff, 1 );
    REQUIRE( destination_om->hordes.spawn_entity( handoff, blocker ) );
    REQUIRE( destination_om->entity_at( handoff_local ) != nullptr );
    // An occupied transfer candidate forces the prepared rider to be
    // discarded.  The source must remain the original lightweight predator,
    // proving that evolution did not bypass the rollback snapshot.
    overmap_test_helper::move_hordes( *source_om );
    horde_entity *retained = source_om->entity_at( source_local );
    CAPTURE( retained != nullptr );
    REQUIRE( retained != nullptr );
    CHECK( retained->get_type()->id == mon_zombie_predator );
    CHECK( retained->get_type()->id == mon_zombie_predator );
    CHECK( retained->destination == handoff );
    CHECK( retained->last_processed == calendar::turn );

    source_om->hordes.clear();
    destination_om->hordes.clear();
}

TEST_CASE( "oter_flags_string_round_trip", "[overmap][flags]" )
{
    const auto &flag_map = io::get_enum_lookup_map<oter_flags>();
    const std::vector<oter_flags> flags = all_oter_flags();

    CHECK( flag_map.size() == flags.size() );

    std::unordered_set<std::string> seen_strings;
    for( const oter_flags flag : flags ) {
        const std::string flag_string = io::enum_to_string( flag );
        CAPTURE( flag_string );
        CHECK_FALSE( flag_string.empty() );
        CHECK( flag_map.count( flag_string ) == 1 );
        CHECK( io::string_to_enum<oter_flags>( flag_string ) == flag );
        CHECK( seen_strings.emplace( flag_string ).second );
    }
}

TEST_CASE( "overmap_location_flags_are_valid", "[overmap][flags]" )
{
    const std::vector<std::string> flags = location_flag_strings();

    for( const std::string &flag : flags ) {
        CAPTURE( flag );
        CHECK( io::enum_is_valid<oter_flags>( flag ) );
        CHECK( io::string_to_enum_optional<oter_flags>( flag ).has_value() );
    }
}

TEST_CASE( "overmap_location_flags_match_terrain_flags", "[overmap][flags]" )
{
    const std::vector<std::string> flags = location_flag_strings();
    const auto &flag_map = io::get_enum_lookup_map<oter_flags>();

    for( const std::string &flag : flags ) {
        const auto iter = flag_map.find( flag );
        if( iter == flag_map.end() ) {
            continue;
        }
        CAPTURE( flag );
        CHECK( any_terrain_with_flag( iter->second ) );
    }
}

TEST_CASE( "set_and_get_overmap_scents", "[overmap]" )
{
    std::unique_ptr<overmap> test_overmap = std::make_unique<overmap>( point_abs_om() );

    // By default there are no scents set.
    for( int x = 0; x < 180; ++x ) {
        for( int y = 0; y < 180; ++y ) {
            for( int z = -10; z < 10; ++z ) {
                REQUIRE( test_overmap->scent_at( { x, y, z } ).creation_time ==
                         calendar::before_time_starts );
            }
        }
    }

    time_point creation_time = calendar::turn_zero + 50_turns;
    scent_trace test_scent( creation_time, 90 );
    test_overmap->set_scent( { 75, 85, 0 }, test_scent );
    REQUIRE( test_overmap->scent_at( { 75, 85, 0} ).creation_time == creation_time );
    REQUIRE( test_overmap->scent_at( { 75, 85, 0} ).initial_strength == 90 );
}

TEST_CASE( "default_overmap_generation_always_succeeds", "[overmap][slow]" )
{
    overmap_buffer.clear();
    const int city_size = overmap_buffer.get_default_settings(
                              point_abs_om() ).get_settings_city().city_size;
    int overmaps_to_construct = 10;
    for( const point_abs_om &candidate_addr : closest_points_first( point_abs_om(), 10 ) ) {
        // Skip populated overmaps.
        if( overmap_buffer.has( candidate_addr ) ) {
            continue;
        }
        overmap_special_batch test_specials = overmap_specials::get_default_batch( candidate_addr,
                                              city_size );
        overmap_buffer.create_custom_overmap( candidate_addr, test_specials );
        for( const overmap_special_placement &special_placement : test_specials ) {
            const overmap_special *special = special_placement.special_details;
            INFO( "In attempt #" << overmaps_to_construct
                  << " failed to place " << special->id.str() );
            int min_occur = special->get_constraints().occurrences.min;
            CHECK( min_occur <= special_placement.instances_placed );
        }
        if( --overmaps_to_construct <= 0 ) {
            break;
        }
    }
}

TEST_CASE( "default_overmap_generation_has_non_mandatory_specials_at_origin", "[overmap][slow]" )
{
    overmap_buffer.clear();
    on_out_of_scope clear_overmaps_after_test( []() {
        overmap_buffer.clear();
    } );
    // Other tests can save the shared world.  clear() drops RAM state but does
    // not delete overmap files, and populate() would load one instead of using
    // this test's custom special batch.
    point_abs_om origin;
    bool found_unsaved_origin = false;
    for( const point_abs_om &candidate : closest_points_first( point_abs_om(), 50 ) ) {
        bool block_is_unsaved = true;
        for( const point_abs_om &nearby : closest_points_first( candidate, 2 ) ) {
            if( overmap_buffer.has( nearby ) ) {
                block_is_unsaved = false;
                break;
            }
        }
        if( block_is_unsaved ) {
            origin = candidate;
            found_unsaved_origin = true;
            break;
        }
    }
    REQUIRE( found_unsaved_origin );

    const overmap_special_id mandatory_id( "test_impossible_mandatory_special" );
    const overmap_special_id optional_id( "test_optional_special" );
    const cata::flat_set<overmap_location_id> open_air_locations = {
        overmap_location_id( "open_air" )
    };
    const overmap_special_terrain open_air_terrain(
        tripoint_rel_omt( 0, 0, 1 ), oter_str_id( "open_air" ),
        open_air_locations, {} );
    overmap_special mandatory( mandatory_id, open_air_terrain );
    overmap_special optional( optional_id, open_air_terrain );

    overmap_special_placement_constraints &mandatory_constraints =
        const_cast<overmap_special_placement_constraints &>( mandatory.get_constraints() );
    mandatory_constraints.city_size.min = 999;
    mandatory_constraints.city_size.max = 999;
    mandatory_constraints.occurrences.min = 1;
    mandatory_constraints.occurrences.max = 1;

    overmap_special_placement_constraints &optional_constraints =
        const_cast<overmap_special_placement_constraints &>( optional.get_constraints() );
    optional_constraints.occurrences.min = 0;
    optional_constraints.occurrences.max = 1;

    const std::vector<const overmap_special *> specials = { &mandatory, &optional };
    overmap_special_batch test_specials = overmap_special_batch( origin, specials );

    // The impossible mandatory special may spill placement attempts into nearby
    // overmaps.  The always-placeable optional marker must still be placed on
    // the origin overmap after that recursion unwinds.
    overmap_buffer.create_custom_overmap( origin, test_specials );

    bool created_neighbor = false;
    for( const point_abs_om &nearby : closest_points_first( origin, 2 ) ) {
        if( nearby != origin && overmap_buffer.get_existing( nearby ) != nullptr ) {
            created_neighbor = true;
            break;
        }
    }
    REQUIRE( created_neighbor );

    overmap *test_overmap = overmap_buffer.get_existing( origin );
    REQUIRE( test_overmap != nullptr );

    bool found_optional = false;
    for( int x = 0; x < OMAPX; ++x ) {
        for( int y = 0; y < OMAPY; ++y ) {
            if( test_overmap->overmap_special_at( { x, y, 1 } ) == optional_id ) {
                found_optional = true;
            }
        }
    }

    CHECK( found_optional );
}

TEST_CASE( "overmap_special_instance_origins_roundtrip", "[overmap][save]" )
{
    const point_abs_om origin{};
    const overmap_special &single_cabin = overmap_special_Cabin.obj();
    const city no_city;
    const std::array<tripoint_om_omt, 2> placement_origins = {
        tripoint_om_omt( 30, 30, 0 ), tripoint_om_omt( 90, 90, 0 )
    };
    std::map<tripoint_om_omt, int> expected_origin_counts;

    // Place directly on an isolated overmap so this serialization test does not
    // depend on random world terrain or overmaps left in the shared test buffer.
    std::unique_ptr<overmap> test_overmap = std::make_unique<overmap>( origin );
    for( const tripoint_om_omt &placement_origin : placement_origins ) {
        const std::vector<tripoint_om_omt> placed = test_overmap->place_special(
                    single_cabin, placement_origin, om_direction::type::north, no_city, false, true );
        REQUIRE( !placed.empty() );
        expected_origin_counts[placement_origin] = static_cast<int>( placed.size() );
    }

    const auto collect_origin_counts = []( const overmap & om ) {
        std::map<tripoint_om_omt, int> counts;
        for( int x = 0; x < OMAPX; ++x ) {
            for( int y = 0; y < OMAPY; ++y ) {
                for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; ++z ) {
                    const tripoint_om_omt p{ x, y, z };
                    const std::optional<overmap_special_id> special = om.overmap_special_at( p );
                    if( special != overmap_special_Cabin ) {
                        continue;
                    }
                    const std::optional<tripoint_om_omt> placement_origin = om.overmap_special_origin_at( p );
                    REQUIRE( placement_origin.has_value() );
                    counts[*placement_origin]++;
                }
            }
        }
        return counts;
    };

    const std::map<tripoint_om_omt, int> live_origin_counts = collect_origin_counts( *test_overmap );
    CHECK( live_origin_counts == expected_origin_counts );

    std::ostringstream serialized;
    test_overmap->serialize( serialized );
    CHECK( serialized.str().find( "\"origin\"" ) != std::string::npos );

    std::unique_ptr<overmap> reloaded = std::make_unique<overmap>( origin );
    std::istringstream serialized_input( serialized.str() );
    reloaded->unserialize( serialized_input );

    CHECK( collect_origin_counts( *reloaded ) == live_origin_counts );
}

TEST_CASE( "horde_light_interest_save_roundtrip_and_legacy_entry",
           "[overmap][save][hordes][light]" )
{
    const point_abs_om origin{};
    const tripoint_om_ms local_position( 60, 60, 0 );
    const tripoint_abs_ms position = project_combine( origin, local_position );
    const tripoint_abs_ms source = position + point_rel_ms( 24, 0 );
    std::unique_ptr<overmap> original = std::make_unique<overmap>( origin );
    const std::optional<std::unordered_map<tripoint_abs_ms, horde_entity>::iterator> inserted =
        original->hordes.spawn_entity( position, mon_zombie );
    REQUIRE( inserted.has_value() );
    horde_entity &entity = ( *inserted )->second;
    entity.destination = source;
    entity.tracking_intensity = 4;
    entity.light_source = source;
    entity.light_sample_id = "save-light@40";
    entity.light_observed = calendar::turn_zero + 40_turns;
    entity.light_expires = calendar::turn_zero + 46_turns;
    entity.light_interest_strength = 4;

    std::ostringstream serialized;
    original->serialize( serialized );
    const std::string current_save = serialized.str();
    std::unique_ptr<overmap> reloaded = std::make_unique<overmap>( origin );
    std::istringstream current_input( current_save );
    reloaded->unserialize( current_input );
    horde_entity *loaded = reloaded->entity_at( local_position );
    REQUIRE( loaded != nullptr );
    CHECK( loaded->light_source == source );
    CHECK( loaded->light_sample_id == "save-light@40" );
    CHECK( loaded->light_observed == calendar::turn_zero + 40_turns );
    CHECK( loaded->light_expires == calendar::turn_zero + 46_turns );
    CHECK( loaded->light_interest_strength == 4 );

    // Pre-light-interest saves end the horde array entry immediately after
    // `moves`; removing the optional object reproduces that layout.
    std::string legacy_save = current_save;
    const size_t horde_map_start = legacy_save.find( "\"horde_map\"" );
    const size_t sample_id = legacy_save.find( "\"sample_id\"", horde_map_start );
    REQUIRE( horde_map_start != std::string::npos );
    REQUIRE( sample_id != std::string::npos );
    const size_t memory_start = legacy_save.rfind( '{', sample_id );
    const size_t memory_end = legacy_save.find( '}', sample_id );
    const size_t preceding_separator = legacy_save.rfind( ',', memory_start );
    REQUIRE( memory_start != std::string::npos );
    REQUIRE( memory_end != std::string::npos );
    REQUIRE( preceding_separator != std::string::npos );
    legacy_save.erase( preceding_separator, memory_end - preceding_separator + 1 );

    std::unique_ptr<overmap> legacy_loaded = std::make_unique<overmap>( origin );
    std::istringstream legacy_input( legacy_save );
    legacy_loaded->unserialize( legacy_input );
    horde_entity *legacy = legacy_loaded->entity_at( local_position );
    REQUIRE( legacy != nullptr );
    CHECK( legacy->light_sample_id.empty() );
    CHECK( legacy->light_expires == calendar::turn_zero );
    CHECK( legacy->light_interest_strength == 0 );
}

TEST_CASE( "is_ot_match", "[overmap][terrain]" )
{
    SECTION( "exact match" ) {
        // Matches the complete string
        // NOLINTNEXTLINE(cata-ot-match)
        CHECK( is_ot_match( "forest", oter_id( "forest" ), ot_match_type::exact ) );
        // NOLINTNEXTLINE(cata-ot-match)
        CHECK( is_ot_match( "forest_thick", oter_id( "forest_thick" ), ot_match_type::exact ) );

        // Does not exactly match if rotation differs
        // NOLINTNEXTLINE(cata-ot-match)
        CHECK_FALSE( is_ot_match( "sub_station", oter_id( "sub_station_north" ), ot_match_type::exact ) );
        // NOLINTNEXTLINE(cata-ot-match)
        CHECK_FALSE( is_ot_match( "sub_station", oter_id( "sub_station_south" ), ot_match_type::exact ) );
    }

    SECTION( "type match" ) {
        // Matches regardless of rotation
        // NOLINTNEXTLINE(cata-ot-match)
        CHECK( is_ot_match( "sub_station", oter_id( "sub_station_north" ), ot_match_type::type ) );
        // NOLINTNEXTLINE(cata-ot-match)
        CHECK( is_ot_match( "sub_station", oter_id( "sub_station_south" ), ot_match_type::type ) );
        // NOLINTNEXTLINE(cata-ot-match)
        CHECK( is_ot_match( "sub_station", oter_id( "sub_station_east" ), ot_match_type::type ) );
        // NOLINTNEXTLINE(cata-ot-match)
        CHECK( is_ot_match( "sub_station", oter_id( "sub_station_west" ), ot_match_type::type ) );

        // Does not match if base type does not match
        // NOLINTNEXTLINE(cata-ot-match)
        CHECK_FALSE( is_ot_match( "forest", oter_id( "forest_thick" ), ot_match_type::type ) );
        // NOLINTNEXTLINE(cata-ot-match)
        CHECK_FALSE( is_ot_match( "sub_station", oter_id( "sewer_sub_station" ), ot_match_type::type ) );
    }

    SECTION( "prefix match" ) {
        // Matches the complete string
        CHECK( is_ot_match( "forest", oter_id( "forest" ), ot_match_type::prefix ) );
        CHECK( is_ot_match( "forest_thick", oter_id( "forest_thick" ),
                            ot_match_type::prefix ) );

        // Prefix matches when an underscore separator exists
        CHECK( is_ot_match( "forest", oter_id( "forest_thick" ), ot_match_type::prefix ) );
        CHECK( is_ot_match( "underground", oter_id( "underground_sub_station" ), ot_match_type::prefix ) );

        // Prefix itself may contain underscores
        CHECK( is_ot_match( "sewer_end", oter_id( "sewer_end_north" ), ot_match_type::prefix ) );
        CHECK( is_ot_match( "test_forest_very", oter_id( "test_forest_very_thick" ),
                            ot_match_type::prefix ) );

        // Prefix does not match without an underscore separator
        CHECK_FALSE( is_ot_match( "fore", oter_id( "forest" ), ot_match_type::prefix ) );
        CHECK_FALSE( is_ot_match( "fore", oter_id( "forest_thick" ), ot_match_type::prefix ) );

        // Prefix does not match the middle or end
        CHECK_FALSE( is_ot_match( "sub", oter_id( "sewer_sub_station" ), ot_match_type::prefix ) );
        CHECK_FALSE( is_ot_match( "station", oter_id( "sewer_sub_station" ), ot_match_type::prefix ) );
    }

    SECTION( "contains match" ) {
        // Matches the complete string
        CHECK( is_ot_match( "forest", oter_id( "forest" ), ot_match_type::contains ) );
        CHECK( is_ot_match( "forest_thick", oter_id( "forest_thick" ),
                            ot_match_type::contains ) );

        // Matches the beginning/middle/end of an underscore-delimited id
        CHECK( is_ot_match( "sewer", oter_id( "sewer_sub_station" ), ot_match_type::contains ) );
        CHECK( is_ot_match( "sub", oter_id( "sewer_sub_station" ), ot_match_type::contains ) );
        CHECK( is_ot_match( "station", oter_id( "sewer_sub_station" ), ot_match_type::contains ) );

        // Matches the beginning/middle/end without undercores as well
        CHECK( is_ot_match( "sewe", oter_id( "sewer_sub_station" ), ot_match_type::contains ) );
        CHECK( is_ot_match( "er_su", oter_id( "sewer_sub_station" ), ot_match_type::contains ) );
        CHECK( is_ot_match( "_sub_", oter_id( "sewer_sub_station" ), ot_match_type::contains ) );
        CHECK( is_ot_match( "tion", oter_id( "sewer_sub_station" ), ot_match_type::contains ) );

        // Does not match if substring is not contained
        CHECK_FALSE( is_ot_match( "forest", oter_id( "sewer_sub_station" ), ot_match_type::contains ) );
        CHECK_FALSE( is_ot_match( "forestry", oter_id( "forest" ), ot_match_type::contains ) );
    }
}

TEST_CASE( "mutable_overmap_placement", "[overmap][slow]" )
{
    const overmap_special &special =
        *overmap_special_id( GENERATE( "test_crater", "test_microlab" ) );
    const city cit;

    constexpr int num_overmaps = 100;
    constexpr int num_trials_per_overmap = 100;

    global_variables &globvars = get_globals();
    globvars.clear_global_values();

    for( int j = 0; j < num_overmaps; ++j ) {
        // overmap objects are really large, so we don't want them on the
        // stack.  Use unique_ptr and put it on the heap
        std::unique_ptr<overmap> om = std::make_unique<overmap>( point_abs_om::zero );
        om_direction::type dir = om_direction::type::north;

        int successes = 0;

        for( int i = 0; i < num_trials_per_overmap; ++i ) {
            tripoint_om_omt try_pos( rng( 0, OMAPX - 1 ), rng( 0, OMAPY - 1 ), 0 );

            // This test can get very spammy, so abort once an error is
            // observed
            if( debug_has_error_been_observed() ) {
                return;
            }

            if( om->can_place_special( special, try_pos, dir, false ) ) {
                std::vector<tripoint_om_omt> placed_points =
                    om->place_special( special, try_pos, dir, cit, false, false );
                CHECK( !placed_points.empty() );
                ++successes;
            }
        }

        CAPTURE( special.id.str() );
        CHECK( successes > num_trials_per_overmap / 2 );
    }
}

static bool tally_items( std::unordered_map<itype_id, float> &global_item_count,
                         std::unordered_map<itype_id, int> &item_count, tinymap &tm )
{
    bool found = false;
    for( const tripoint_omt_ms &p : tm.points_on_zlevel() ) {
        for( item &i : tm.i_at( p ) ) {
            std::unordered_map<itype_id, float>::iterator iter = global_item_count.find( i.typeId() );
            if( iter != global_item_count.end() ) {
                found = true;
                item_count.emplace( i.typeId(), 0 ).first->second += i.count();
            }
            for( const item *it : i.all_items_ptr() ) {
                iter = global_item_count.find( it->typeId() );
                if( iter != global_item_count.end() ) {
                    found = true;
                    item_count.emplace( i.typeId(), 0 ).first->second += i.count();
                }
            }
        }
        if( const optional_vpart_position ovp = tm.veh_at( p ) ) {
            vehicle *const veh = &ovp->vehicle();
            for( const int elem : veh->parts_at_relative( ovp->mount_pos(), true ) ) {
                const vehicle_part &vp = veh->part( elem );
                for( item &i : veh->get_items( vp ) ) {
                    std::unordered_map<itype_id, float>::iterator iter = global_item_count.find( i.typeId() );
                    if( iter != global_item_count.end() ) {
                        found = true;
                        item_count.emplace( i.typeId(), 0 ).first->second += i.count();
                    }
                    for( const item *it : i.all_items_ptr() ) {
                        iter = global_item_count.find( it->typeId() );
                        if( iter != global_item_count.end() ) {
                            found = true;
                            item_count.emplace( i.typeId(), 0 ).first->second += i.count();
                        }
                    }
                }
            }
        }
    }
    return found;
}

TEST_CASE( "enumerate_items", "[.]" )
{
    for( const itype *id : Item_factory::find(
    []( const itype & type ) -> bool {
    return !!type.gun;
} ) ) {
        printf( "%s ", id->gun->skill_used.str().c_str() );
        printf( "%s\n", id->get_id().str().c_str() );
    }
}

static void finalize_item_counts( std::unordered_map<itype_id, float> &item_counts )
{
    for( std::pair<const std::string, item_demographic_test_data> &category :
         test_data::item_demographics ) {
        // Scan for match ing ammo types and shim them into the provided data so
        // the test doesn't break every time we add a new item variant.
        // Ammo has a lot of things in it we don't consider "real ammo" so there's a list
        // of item types to ignore that are applied here.
        if( category.first == "ammo" ) {
            for( const itype *id : Item_factory::find( []( const itype & type ) -> bool {
            return !!type.ammo;
        } ) ) {
                if( category.second.ignored_items.find( id->get_id() ) != category.second.ignored_items.end() ) {
                    continue;
                }
                category.second.item_weights[id->get_id()] = 1;
                auto ammotype_map_iter = category.second.groups.find( id->ammo->type.str() );
                if( ammotype_map_iter == category.second.groups.end() ) {
                    // If there's no matching ammotype in the test data,
                    // stick the item in the "other" group.
                    category.second.groups["other"].second[id->get_id()];
                } else {
                    // If there is a matching ammotype and the item id isn't already populated,
                    // add it.
                    if( ammotype_map_iter->second.second.find( id->get_id() ) ==
                        ammotype_map_iter->second.second.end() ) {
                        ammotype_map_iter->second.second[id->get_id()] = 1;
                    }
                }
            }
        } else if( category.first == "gun" ) {
            for( const itype *id : Item_factory::find( []( const itype & type ) -> bool {
            return !!type.gun;
        } ) ) {
                if( category.second.ignored_items.find( id->get_id() ) != category.second.ignored_items.end() ) {
                    continue;
                }
                category.second.item_weights[id->get_id()] = 1; // ???
                for( const ammotype &ammotype_id : id->gun->ammo ) {
                    auto ammotype_map_iter = category.second.groups.find( ammotype_id.str() );
                    if( ammotype_map_iter == category.second.groups.end() ) {
                        // If there's no matching ammotype in the test data,
                        // stick the item in the "other" group.
                        category.second.groups["other"].second[id->get_id()];
                    } else {
                        // If there is a matching ammotype and the item id isn't already populated,
                        // add it.
                        if( ammotype_map_iter->second.second.find( id->get_id() ) ==
                            ammotype_map_iter->second.second.end() ) {
                            ammotype_map_iter->second.second[id->get_id()] = 1; // ???
                        }
                    }
                }
            }
        }
        for( const std::pair<const itype_id, int> &demographics : category.second.item_weights ) {
            item_counts[demographics.first] = 0.0;
        }
    }
}

// Toggle this to enable the (very very very expensive) item demographics test.
static bool enable_item_demographics = false;

TEST_CASE( "overmap_terrain_coverage", "[overmap][slow]" )
{
    // The goal of this test is to generate a lot of overmaps, and count up how
    // many times we see each terrain, so that we can check that everything
    // generates at least sometimes.
    struct omt_stats {
        explicit omt_stats( const tripoint_abs_omt &p ) : last_observed( p ) {}

        tripoint_abs_omt last_observed;
        int count = 0;
        int samples = 0;
        bool found = false;
        std::unordered_map<itype_id, int> item_counts;
    };
    std::unordered_map<oter_type_id, omt_stats> stats;
    std::unordered_map<itype_id, float> item_counts;
    finalize_item_counts( item_counts );
    map &main_map = get_map();
    const tripoint_abs_ms saved_player_position = get_avatar().pos_abs();
    const tripoint_abs_sm saved_map_origin = main_map.get_abs_sub();
    on_out_of_scope restore_location( [&main_map, saved_player_position, saved_map_origin]() {
        g->place_player_overmap( project_to<coords::omt>( saved_player_position ), false );
        main_map.load( saved_map_origin, false );
        get_avatar().setpos( saved_player_position, false );
    } );
    // Keep this coverage pass in the default inland geography regardless of
    // which map another test left loaded when the suite entered this test.
    const point_abs_om overmap_origin( 0, 0 );
    g->place_player_overmap( { project_to<coords::omt>( overmap_origin + ( 6 * point::north_west ) ), 0 } );
    // Don't inherit overmap state from initialization or previous tests.
    overmap_buffer.clear();
    // Build the set of terrain types we expect to find. Anything still in this
    // set after generation is reported as missing.
    std::unordered_set<oter_type_id> yet_to_be_seen;
    {
        std::unordered_set<oter_type_id> dedup;
        for( const oter_t &ter : overmap_terrains::get_all() ) {
            oter_type_id id = ter.get_type_id();
            oter_type_str_id id_s = id.id();
            if( id_s.is_empty() || id_s.is_null() || !dedup.insert( id ).second ) {
                continue;
            }
            if( id->has_flag( oter_flags::should_not_spawn ) ) {
                continue;
            }
            const recipe_id recipe( id_s.c_str() );
            if( recipe.is_valid() && recipe->is_blueprint() ) {
                continue;
            }
            if( id->has_flag( oter_flags::ocean ) || id->has_flag( oter_flags::highway ) ) {
                continue;
            }
            bool is_whitelisted = false;
            for( const std::regex &wl : test_data::overmap_terrain_coverage_whitelist ) {
                std::cmatch m;
                if( std::regex_match( id_s.c_str(), m, wl ) &&
                    m.prefix().length() == 0 && m.suffix().length() == 0 ) {
                    is_whitelisted = true;
                    break;
                }
            }
            if( !is_whitelisted ) {
                yet_to_be_seen.insert( id );
            }
        }
    }

    const std::size_t initial_expected = yet_to_be_seen.size();
    std::optional<point_abs_om> first_requested_om;
    std::size_t examples_shown = 0;
    std::map<std::string, int> first_request_z0;
    int first_request_z0_tiles = 0;
    const auto report_content = [&]() {
        std::vector<std::pair<std::string, int>> top( first_request_z0.begin(), first_request_z0.end() );
        std::sort( top.begin(), top.end(), []( const auto & a, const auto & b ) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        } );
        std::cout << "batch_worldgen_coverage_content requested=" << ( first_requested_om ?
                  first_requested_om->to_string() : "unrequested" )
                  << " z=0 scanned_tiles=" << first_request_z0_tiles
                  << " distinct=" << first_request_z0.size() << " top=";
        for( std::size_t i = 0; i < std::min<std::size_t>( 8, top.size() ); ++i ) {
            std::cout << top[i].first.substr( 0, 96 ) << ':' << top[i].second << ',';
        }
        std::cout << std::endl;
    };
    std::size_t requested_regions = 0;
    std::size_t scanned_regions = 0;
    std::size_t prior_remaining = initial_expected;
    std::size_t filtered_candidates = 0;
    const auto report_generation = [&]( const char *stage, const int pass ) {
        std::set<std::string> missing_ids;
        for( const auto &id : yet_to_be_seen ) {
            missing_ids.insert( id->id.str() );
        }
        std::cout << "batch_worldgen_coverage stage=" << stage << " pass=" << pass
                  << " origin=" << overmap_origin.to_string()
                  << " requested_regions=" << requested_regions
                  << " scanned_regions=" << scanned_regions
                  << " observed_types=" << stats.size()
                  << " initial_expected=" << initial_expected
                  << " newly_covered=" << ( prior_remaining - yet_to_be_seen.size() )
                  << " remaining=" << yet_to_be_seen.size()
                  << " filtered_candidates=" << filtered_candidates << " first_remaining=";
        int shown = 0;
        for( const auto &id : missing_ids ) {
            if( shown++ == 8 ) {
                break;
            }
            std::cout << id << ',';
        }
        std::cout << '\n';
        prior_remaining = yet_to_be_seen.size();
    };
    on_out_of_scope report_exit( [&]() {
        report_generation( "exit", -1 );
    } );
    // Build reverse map: oter_type_id -> info about specials that produce it.
    // Used for the filtered generation phase and diagnostic logging.
    struct special_spawn_info {
        overmap_special_id id;
        bool is_unique;
        bool is_optional;
        bool needs_water;
        int occ_min;
        int occ_max;
    };
    std::unordered_map<oter_type_id, std::vector<special_spawn_info>> terrain_to_specials;
    const bool region_present = get_options().has_option( "DEFAULT_REGION" );
    std::cout << "batch_worldgen_coverage_input catch_seed=" <<
              Catch::getCurrentContext().getConfig()->rngSeed()
              << " game_seed=" << g->get_seed()
              << " default_region_present=" << region_present
              << " default_region=" << ( region_present ?
                                         get_options().get_option( "DEFAULT_REGION" ).getValue() : "unavailable" )
              << " whitelist=TEST_DATA/overmap_terrain_coverage_test/overmap_terrain_coverage_whitelist.json"
              << " whitelist_entries=" << test_data::overmap_terrain_coverage_whitelist.size()
              << " mods_count=" << world_generator->active_world->active_mod_order.size()
              << " mods=";
    int mods_shown = 0;
    for( const auto &id : world_generator->active_world->active_mod_order ) {
        if( mods_shown++ == 8 ) {
            break;
        }
        std::cout << id.str() << ',';
    }
    std::cout << '\n';
    const auto &default_settings = overmap_buffer.get_default_settings( point_abs_om() );
    const int city_size = default_settings.get_settings_city().city_size;
    std::cout << "batch_worldgen_coverage_settings id=" << default_settings.id.str()
              << " city_size=" << city_size
              << " city_spacing=" << default_settings.get_settings_city().city_spacing << '\n';
    report_generation( "initial", -1 );
    for( const overmap_special &sp : overmap_specials::get_all() ) {
        if( !sp.can_spawn( city_size ) ) {
            continue;
        }
        const overmap_special_placement_constraints &constraints = sp.get_constraints();
        special_spawn_info info;
        info.id = sp.id;
        info.is_unique = sp.has_flag( "OVERMAP_UNIQUE" ) || sp.has_flag( "GLOBALLY_UNIQUE" );
        info.is_optional = constraints.occurrences.min == 0;
        info.needs_water = sp.has_flag( "LAKE" ) || sp.has_flag( "OCEAN" );
        info.occ_min = constraints.occurrences.min;
        info.occ_max = constraints.occurrences.max;

        for( const oter_type_id &tid : sp.get_terrain_type_ids() ) {
            terrain_to_specials[tid].push_back( info );
        }
    }

    constexpr int max_attempts = 3;
    for( int attempt_no = 0; attempt_no < max_attempts && !yet_to_be_seen.empty();
         ++attempt_no ) {
        if( attempt_no > 0 ) {
            overmap_buffer.reset();
        }
        // First we just touch all the overmaps in an area to cause them to generate.
        for( const point_abs_om &om_cur : closest_points_first( overmap_origin, 0, 3 ) ) {
            point_abs_omt omt_start = project_to<coords::omt>( om_cur );
            ++requested_regions;
            std::optional<bandit_live_world_probe::session> request_probe;
            if( requested_regions == 1 ) {
                first_requested_om = om_cur;
                request_probe.emplace( bandit_live_world_probe::collection_mode::transition_events );
                request_probe->set_overmap_request_target( om_cur.x(), om_cur.y() );
            }
            overmap_buffer.ter( { omt_start, 0 } );
            if( request_probe ) {
                const auto &trace = request_probe->result();
                for( const auto &event : trace.transition_events ) {
                    if( event.transition == "overmap_request" ) {
                        std::cout << "batch_worldgen_coverage_branch requested=" << om_cur.to_string()
                                  << " game_seed=" << g->get_seed() << " stage=" << event.outcome
                                  << " " << event.reason << std::endl;
                    }
                }
                std::cout << "batch_worldgen_coverage_branch dropped=" << trace.dropped_transition_events
                          << std::endl;
            }
        }
        // Then we scan the generated overmaps.
        for( const point_abs_om &om_cur : closest_points_first( overmap_origin, 0, 5 ) ) {
            point_abs_omt omt_start = project_to<coords::omt>( om_cur );
            // In the two rows of overmaps outside the central area, special placement logic.
            // Can trigger additional overmap placement, this checcks to see if an overmap
            // Has already been placed, and skips it if it hasn't.
            // This insures we can scan all the overmaps we generate.
            if( overmap_buffer.ter_existing( { omt_start, 0 } ) == oter_id() ) {
                continue;
            }
            ++scanned_regions;
            point_abs_omt omt_end = omt_start + ( point::south_east * OMAPX );
            for( point_abs_omt p = omt_start; p.y() < omt_end.y(); p.y()++ ) {
                for( p.x() = omt_start.x(); p.x() < omt_end.x(); p.x()++ ) {
                    REQUIRE( !main_map.inbounds( tripoint_abs_ms( project_to<coords::ms>( p ), 0 ) ) );
                    for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; ++z ) {
                        tripoint_abs_omt tp( p, z );
                        const oter_id terrain = overmap_buffer.ter( tp );
                        const oter_type_id id = terrain->get_type_id();
                        if( attempt_no == 0 && first_requested_om && om_cur == *first_requested_om && z == 0 ) {
                            ++first_request_z0_tiles;
                            ++first_request_z0[terrain.id().str()];
                        }
                        auto iter_bool = stats.emplace( id, tp );
                        if( iter_bool.second ) {
                            const bool show_example = examples_shown < 8;
                            const bool expected = show_example && yet_to_be_seen.count( id ) > 0;
                            const std::size_t removed = yet_to_be_seen.erase( id );
                            if( show_example ) {
                                ++examples_shown;
                                std::cout << "batch_worldgen_coverage_observed pos=" << tp.to_string()
                                          << " terrain=" << terrain.id().str().substr( 0, 96 )
                                          << " type=" << id.id().str().substr( 0, 96 )
                                          << " expected=" << expected << " removed=" << removed << std::endl;
                            }
                        }
                        iter_bool.first->second.last_observed = tp;
                        ++iter_bool.first->second.count;
                    }
                }
            }
        }
        if( attempt_no == 0 ) {
            report_content();
        }
        report_generation( "normal", attempt_no );
        // The second phase of this test is to perform the tile-level mapgen
        // for each oter_type, in hopes of triggering any errors that might arise
        // with that.
        // In addition, if enable_item_demographics == true,
        // perform extensive mapgen analysis and make assertions about
        // the ratios of selected item occurrence rates.
        for( std::pair<const oter_type_id, omt_stats> &p : stats ) {
            const std::string oter_type_id = p.first->id.str();
            const tripoint_abs_omt pos = p.second.last_observed;
            const int count = p.second.count;
            // Once we find items of interest in an oter_type, increase the sampling for it.
            int sampling_exponent = p.second.found ? 3 : 2;
            int goal_samples = std::pow( std::log( std::max( 10, count ) ), sampling_exponent );
            if( !enable_item_demographics ) {
                goal_samples = 1;
            }
            // We are sampling with logarithmic falloff. Once we pass a threshold where
            // log( total_count ) yields the next-higher integer value, we take another sample.
            int sample_size = goal_samples - p.second.samples;
            if( sample_size <= 0 ) {
                continue;
            }
            CAPTURE( oter_type_id );
            const std::string msg = capture_debugmsg_during( [pos,
            &item_counts, &p, &sample_size, &goal_samples, count, oter_type_id]() {
                for( int i = 0; i < sample_size; ++i ) {
                    // clear the generated maps so we keep getting new results.
                    MAPBUFFER.clear_outside_reality_bubble();
                    smallmap tm;
                    tm.generate( pos, calendar::turn, false );
                    // Map edits without the "mapgen_in_progress" variable set will toggle
                    // player_adjusted_map to true, this should find callers that fail to do so.
                    CHECK( !map_meddler::has_altered_submaps( *tm.cast_to_map() ) );
                    bool found = tally_items( item_counts, p.second.item_counts, tm );
                    if( enable_item_demographics && found && !p.second.found ) {
                        goal_samples = std::pow( std::log( std::max( 10, count ) ), 3 );
                        sample_size = goal_samples - p.second.samples;
                        p.second.found = true;
                    }
                    tm.delete_unmerged_submaps();
                }
            } );
            p.second.samples = goal_samples;
            CAPTURE( msg );
            CAPTURE( msg.empty() );
        }
    }
    // Last attempt's overmaps are still in memory for the forced placement phase.

    // Check that no SHOULD_NOT_SPAWN terrain was generated.
    for( const auto &entry : stats ) {
        if( entry.first->has_flag( oter_flags::should_not_spawn ) ) {
            CAPTURE( entry.first );
            FAIL( "oter_type_id was found in map but had SHOULD_NOT_SPAWN flag" );
        }
    }

    global_variables &globvars = get_globals();
    globvars.clear_global_values();

    // Filtered generation: for terrains still missing after normal generation,
    // run one more attempt with a custom batch containing only the specials that
    // produce those terrains. With all other specials removed from competition,
    // the remaining ones get all 144 sectors to themselves.
    if( !yet_to_be_seen.empty() ) {
        std::vector<const overmap_special *> filtered;
        std::unordered_set<overmap_special_id> seen_ids;
        for( const oter_type_id &tid : yet_to_be_seen ) {
            auto it = terrain_to_specials.find( tid );
            if( it == terrain_to_specials.end() ) {
                continue;
            }
            for( const special_spawn_info &info : it->second ) {
                if( seen_ids.insert( info.id ).second ) {
                    filtered.push_back( &info.id.obj() );
                }
            }
        }

        if( !filtered.empty() ) {
            filtered_candidates = filtered.size();
            overmap_buffer.reset();
            overmap_special_batch custom_batch( overmap_origin, filtered );
            for( const point_abs_om &om_cur :
                 closest_points_first( overmap_origin, 0, 3 ) ) {
                ++requested_regions;
                overmap_buffer.create_custom_overmap( om_cur, custom_batch );
            }
            // Scan the custom overmaps for newly-placed terrains.
            for( const point_abs_om &om_cur :
                 closest_points_first( overmap_origin, 0, 5 ) ) {
                point_abs_omt omt_start = project_to<coords::omt>( om_cur );
                if( overmap_buffer.ter_existing( { omt_start, 0 } ) == oter_id() ) {
                    continue;
                }
                ++scanned_regions;
                point_abs_omt omt_end = omt_start + ( point::south_east * OMAPX );
                for( point_abs_omt p = omt_start; p.y() < omt_end.y(); p.y()++ ) {
                    for( p.x() = omt_start.x(); p.x() < omt_end.x(); p.x()++ ) {
                        for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; ++z ) {
                            tripoint_abs_omt tp( p, z );
                            oter_type_id id = overmap_buffer.ter( tp )->get_type_id();
                            auto iter_bool = stats.emplace( id, tp );
                            if( iter_bool.second ) {
                                yet_to_be_seen.erase( id );
                            }
                            iter_bool.first->second.last_observed = tp;
                            ++iter_bool.first->second.count;
                        }
                    }
                }
            }
            report_generation( "filtered", 0 );
            // Mapgen sampling for newly observed terrains.
            for( std::pair<const oter_type_id, omt_stats> &p : stats ) {
                int goal_samples = 1;
                int sample_size = goal_samples - p.second.samples;
                if( sample_size <= 0 ) {
                    continue;
                }
                const std::string oter_type_id = p.first->id.str();
                const tripoint_abs_omt pos = p.second.last_observed;
                CAPTURE( oter_type_id );
                const std::string msg = capture_debugmsg_during( [pos]() {
                    MAPBUFFER.clear_outside_reality_bubble();
                    smallmap tm;
                    tm.generate( pos, calendar::turn, false );
                    CHECK( !map_meddler::has_altered_submaps( *tm.cast_to_map() ) );
                    tm.delete_unmerged_submaps();
                } );
                p.second.samples = goal_samples;
                CAPTURE( msg );
                CAPTURE( msg.empty() );
            }
            WARN( "Ran filtered generation with " << filtered.size()
                  << " specials for unobserved terrains" );
        }
    }
    overmap_buffer.reset();

    // Final check: anything still in yet_to_be_seen after both normal generation
    // and forced placement must be in the manual whitelist or it's a failure.
    {
        std::vector<oter_type_id> missing( yet_to_be_seen.begin(), yet_to_be_seen.end() );
        size_t num_missing = missing.size();
        CAPTURE( num_missing );
        constexpr size_t max_to_report = 100;
        if( num_missing > max_to_report ) {
            std::shuffle( missing.begin(), missing.end(), rng_get_engine() );
            missing.erase( missing.begin() + max_to_report, missing.end() );
        }
        std::sort( missing.begin(), missing.end() );
        const std::string missing_oter_type_ids = enumerate_as_string( missing,
        []( const oter_type_id & id ) {
            return id->id.str();
        } );
        CAPTURE( missing_oter_type_ids );
        for( const oter_type_id &tid : missing ) {
            auto it = terrain_to_specials.find( tid );
            if( it != terrain_to_specials.end() ) {
                for( const special_spawn_info &info : it->second ) {
                    UNSCOPED_INFO( tid->id.str() << " expected from special "
                                   << info.id.str() << " (occ=" << info.occ_min
                                   << "/" << info.occ_max
                                   << ( info.is_unique ? " unique" : "" )
                                   << ( info.is_optional ? " optional" : "" )
                                   << ( info.needs_water ? " water" : "" )
                                   << ")" );
                }
            }
        }
        INFO( "To resolve errors about missing terrains you can either give the terrain the "
              "SHOULD_NOT_SPAWN flag, intended for terrains that should never spawn, for example "
              "test terrains or work in progress, or tweak the constraints so that the terrain "
              "can spawn more reliably, or add them to the whitelist at "
              "/data/mods/TEST_DATA/overmap_terrain_coverage_test/overmap_terrain_coverage_whitelist.json "
              "intended for terrains that sometimes spawn, but cannot be expected to spawn "
              "reliably enough for this test." );
        CHECK( num_missing == 0 );
    }

    if( !enable_item_demographics ) {
        // This should be SKIP() but we haven't updated to Catch 3.3.0 yet.
        return;;
    }
    // Copy and scale the final results from all oter_types to the global item_count map for analysis.
    for( const std::pair<const oter_type_id, omt_stats> &omt_entry : stats ) {
        REQUIRE( omt_entry.second.samples != 0 );
        float sampling_factor = static_cast<float>( omt_entry.second.count ) /
                                static_cast<float>( omt_entry.second.samples );
        for( std::pair<const itype_id, int> item_entry : omt_entry.second.item_counts ) {
            const itype *item_type = item::find_type( item_entry.first );
            // Adjust this filter to limit output.
            if( !!item_type->gun ) {
                printf( "Found %f %s in %d instances of %s (scaled)\n",
                        static_cast<float>( item_entry.second ) * sampling_factor,
                        item_entry.first.c_str(), omt_entry.second.count,
                        omt_entry.first.id().c_str() );
            }
            item_counts[item_entry.first] += static_cast<float>( item_entry.second ) * sampling_factor;
        }
    }

    // We're asserting ratios for three things here.
    // 1. global weight within a category, so e.g. number of glock 19s / all guns spawned.
    // 2. weight of a sub-category within a category, e.g. number of 9mm firearms / all guns.
    // 3. weight of individual items within a sub-category, e.g. number of glock 19s / all 9mm firearms.
    for( std::pair<const std::string, item_demographic_test_data> &category :
         test_data::item_demographics ) {
        if( category.second.tests.count( "items" ) ) {
            float category_weight_total = 0.0;
            float category_actual_total = 0.0;
            // I don't see an alternative to just walking this twice?
            for( std::pair<const itype_id, int> &item_ratio : category.second.item_weights ) {
                category_weight_total += item_ratio.second;
                category_actual_total += item_counts[item_ratio.first];
            }
            for( std::pair<const itype_id, int> &item_ratio : category.second.item_weights ) {
                float actual_item_count = item_counts[item_ratio.first];
                float actual_item_ratio = actual_item_count / category_actual_total;
                float expected_item_total = item_ratio.second;
                float expected_item_ratio = expected_item_total / category_weight_total;
                CAPTURE( item_ratio.first );
                CAPTURE( actual_item_count );
                CAPTURE( expected_item_total );
                CHECK_THAT( actual_item_ratio, Catch::Matchers::WithinRel( expected_item_ratio, 0.1f ) );
            }
        }
        if( category.second.tests.count( "groups" ) ) {
            float all_type_weight_total = 0.0;
            float all_type_actual_total = 0.0;
            // I don't see an alternative to just walking this twice?
            for( std::pair < const std::string,
                 std::pair<int, std::map<itype_id, int>>> &group : category.second.groups ) {
                all_type_weight_total += group.second.first;
                for( std::pair<const itype_id, int> &item_ratio : group.second.second ) {
                    all_type_actual_total += item_counts[item_ratio.first];
                }
            }
            for( std::pair < const std::string,
                 std::pair<int, std::map<itype_id, int>>> &group : category.second.groups ) {
                float current_type_actual_total = 0.0;
                float current_type_weight_total = group.second.first;
                for( std::pair<const itype_id, int> &item_ratio : group.second.second ) {
                    current_type_actual_total += item_counts[item_ratio.first];
                }
                if( category.second.tests.count( "inner-group" ) ) {
                    for( std::pair<const itype_id, int> &item_ratio : group.second.second ) {
                        float actual_item_count = item_counts[item_ratio.first];
                        float actual_item_ratio = actual_item_count / current_type_actual_total;
                        float expected_item_ratio = item_ratio.second / current_type_weight_total;
                        CAPTURE( category.first );
                        CAPTURE( item_ratio.first );
                        CAPTURE( actual_item_count );
                        CAPTURE( expected_item_ratio );
                        CHECK_THAT( actual_item_ratio,
                                    Catch::Matchers::WithinRel( expected_item_ratio, 0.1f ) );
                    }
                }
                float current_type_expected_ratio = current_type_weight_total / all_type_weight_total;
                float current_type_actual_ratio = current_type_actual_total / all_type_actual_total;
                CAPTURE( category.first );
                CAPTURE( group.first );
                CAPTURE( current_type_expected_ratio );
                CAPTURE( current_type_actual_ratio );
                INFO( "Difference: " << current_type_actual_ratio - current_type_expected_ratio );
                CHECK_THAT( current_type_actual_ratio,
                            Catch::Matchers::WithinRel( current_type_expected_ratio, 0.1f ) );
            }
        }
    }
}

TEST_CASE( "highway_find_intersection_bounds", "[overmap]" )
{
    overmap_buffer.clear();
    highway_intersection_grid &highway_grid =
        overmap_buffer.global_state.highway_intersections;
    highway_grid.set_grid_origin( point_abs_om::zero );
    point_abs_om pos = highway_grid.get_grid_origin();

    const int c_separation = get_option<int>( "HIGHWAY_GRID_COLUMN_SEPARATION" );
    const int r_separation = get_option<int>( "HIGHWAY_GRID_ROW_SEPARATION" );

    const int col_test = c_separation / 2;
    const int row_test = r_separation / 2;
    const int col_test_2 = c_separation * 1.5;
    const int row_test_2 = r_separation * 1.5;

    //check points in 8 directions of origin
    std::vector<std::pair<point_rel_om, point_rel_om>> input_output_pairs = {
        //inside quadrants surrounding origin + 0,0
        { point_rel_om( col_test, 0 ), point_rel_om( 0, 0 ) },
        { point_rel_om( -col_test, 0 ), point_rel_om( -c_separation, 0 ) },
        { point_rel_om( 0, row_test ), point_rel_om( 0, 0 ) },
        { point_rel_om( 0, -row_test ), point_rel_om( 0, -r_separation ) },
        { point_rel_om( col_test, row_test ), point_rel_om( 0, 0 ) },
        { point_rel_om( -col_test, row_test ), point_rel_om( -c_separation, 0 ) },
        { point_rel_om( col_test, -row_test ), point_rel_om( 0, -r_separation ) },
        //on grid points
        { point_rel_om( 0, 0 ), point_rel_om( 0, 0 ) },
        { point_rel_om( -c_separation, 0 ), point_rel_om( -c_separation, 0 ) },
        { point_rel_om( c_separation, 0 ), point_rel_om( c_separation, 0 ) },
        { point_rel_om( 0, -r_separation ), point_rel_om( 0, -r_separation ) },
        { point_rel_om( 0, r_separation ), point_rel_om( 0, r_separation ) },
        //outside quadrants surrounding origin + 0,0
        { point_rel_om( col_test_2, 0 ), point_rel_om( c_separation, 0 ) },
        { point_rel_om( -col_test_2, 0 ), point_rel_om( -c_separation * 2, 0 ) },
        { point_rel_om( 0, row_test_2 ), point_rel_om( 0, r_separation ) },
        { point_rel_om( 0, -row_test_2 ), point_rel_om( 0, -r_separation * 2 ) },
        { point_rel_om( col_test_2, row_test_2 ), point_rel_om( c_separation, r_separation ) },
        { point_rel_om( -col_test_2, row_test_2 ), point_rel_om( -c_separation * 2, r_separation ) },
        { point_rel_om( col_test_2, -row_test_2 ), point_rel_om( c_separation, -r_separation * 2 ) },
        { point_rel_om( -col_test_2, -row_test_2 ), point_rel_om( -c_separation * 2, -r_separation * 2 ) }
    };

    for( const std::pair<point_rel_om, point_rel_om> &p : input_output_pairs ) {
        std::vector<point_abs_om> bounds = highway_grid.find_feature_point_bounds( pos + p.first );
        CHECK( bounds.back() == pos + p.second );
    }

}

TEST_CASE( "highway_connections_default_to_invalid", "[overmap][highway]" )
{
    overmap_buffer.clear();

    point_abs_om pos;
    bool found = false;
    for( const point_abs_om &candidate : closest_points_first( point_abs_om(), 50 ) ) {
        if( !overmap_buffer.has( candidate ) ) {
            pos = candidate;
            found = true;
            break;
        }
    }
    REQUIRE( found );

    // Heap-allocated because overmap's map_layer arrays overflow the stack.
    auto om = std::make_unique<overmap>( pos );
    const auto &conns = overmap_test_helper::get_highway_connections( *om );
    for( int i = 0; i < 4; i++ ) {
        CAPTURE( i );
        CHECK( conns[i] == tripoint_om_omt::invalid );
        CHECK( conns[i] != tripoint_om_omt::zero );
    }
}

TEST_CASE( "highway_neighbor_missing_connections_no_crash", "[overmap][highway]" )
{
    auto target = std::make_unique<overmap>( point_abs_om::zero );
    auto corrupted_neighbor = std::make_unique<overmap>( point_abs_om( 0, -1 ) );
    overmap_test_helper::set_highway_connections( *corrupted_neighbor, {
        tripoint_om_omt::zero, tripoint_om_omt::zero,
        tripoint_om_omt::zero, tripoint_om_omt::zero
    } );

    std::vector<const overmap *> neighbors( 4, nullptr );
    neighbors[0] = corrupted_neighbor.get();
    std::array<tripoint_om_omt, 4> end_points = {
        tripoint_om_omt::invalid, tripoint_om_omt::invalid,
        tripoint_om_omt::invalid, tripoint_om_omt::invalid
    };
    std::bitset<4> neighbor_connections;
    neighbor_connections.set( 0 );

    bool selected = false;
    std::string dmsg = capture_debugmsg_during( [&]() {
        selected = overmap_test_helper::highway_select_end_points(
                       *target, neighbors, end_points, neighbor_connections );
    } );

    CHECK( selected );
    CHECK_FALSE( neighbor_connections[0] );
    CHECK( end_points[0] == tripoint_om_omt::invalid );
    CHECK_THAT( dmsg, !Catch::Contains( "highway connections not initialized" ) );
}

TEST_CASE( "overmap_generation_is_deterministic", "[overmap]" )
{
    // Verify that overmap generation produces identical results when the RNG
    // engine starts from the same state.  Catches stale distribution cache
    // bugs and any other hidden nondeterminism in the generation pipeline.

    // NOLINTNEXTLINE(cata-determinism)
    const cata_default_random_engine saved_engine = rng_get_engine();

    map &main_map = get_map();
    const point_abs_om origin = project_to<coords::om>( main_map.get_abs_sub().xy() );

    // Generate a 2x2 cluster of overmaps (origin + 3 neighbors).
    const std::vector<point_abs_om> cluster = closest_points_first( origin, 0, 1 );

    constexpr int runs = 3;
    std::vector<std::vector<oter_id>> snapshots( runs );

    for( int run = 0; run < runs; ++run ) {
        overmap_buffer.clear();
        rng_get_engine() = saved_engine;

        // Touch all overmaps in the cluster to trigger generation.
        for( const point_abs_om &om : cluster ) {
            const point_abs_omt omt = project_to<coords::omt>( om );
            overmap_buffer.ter( { omt, 0 } );
        }

        // Snapshot every z=0 tile across the cluster.
        std::vector<oter_id> &snap = snapshots[run];
        snap.reserve( cluster.size() * OMAPX * OMAPY );
        for( const point_abs_om &om : cluster ) {
            const point_abs_omt omt_start = project_to<coords::omt>( om );
            const point_abs_omt omt_end = omt_start + point::south_east * OMAPX;
            for( int y = omt_start.y(); y < omt_end.y(); ++y ) {
                for( int x = omt_start.x(); x < omt_end.x(); ++x ) {
                    snap.push_back( overmap_buffer.ter( { point_abs_omt( x, y ), 0 } ) );
                }
            }
        }
    }

    // Compare each run against the first.
    for( int run = 1; run < runs; ++run ) {
        REQUIRE( snapshots[run].size() == snapshots[0].size() );
        bool match = true;
        for( size_t i = 0; i < snapshots[0].size(); ++i ) {
            if( snapshots[run][i] != snapshots[0][i] ) {
                // Report the first mismatch with tile coordinates.
                const int tiles_per_om = OMAPX * OMAPY;
                const int om_idx = static_cast<int>( i ) / tiles_per_om;
                const int local = static_cast<int>( i ) % tiles_per_om;
                const int y = local / OMAPX;
                const int x = local % OMAPX;
                CAPTURE( run, om_idx, x, y );
                CAPTURE( snapshots[0][i]->id );
                CAPTURE( snapshots[run][i]->id );
                FAIL( "overmap terrain mismatch between run 0 and run " << run );
                match = false;
                break;
            }
        }
        if( match ) {
            SUCCEED( "run " << run << " matches run 0" );
        }
    }
}
