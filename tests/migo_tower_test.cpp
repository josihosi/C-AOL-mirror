#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "do_turn.h"
#include "field_type.h"
#include "game.h"
#include "horde_entity.h"
#include "json.h"
#include "json_loader.h"
#include "live_light.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "mapbuffer.h"
#include "migo_population_native.h"
#include "migo_tower_ai.h"
#include "migo_tower_capability.h"
#include "scout_observation.h"
#include "mtype.h"
#include "item.h"
#include "migo_tower_native.h"
#include "monster.h"
#include "npc.h"
#include "omdata.h"
#include "overmap.h"
#include "overmapbuffer.h"
#include "player_helpers.h"
#include "physical_navigation.h"
#include "submap.h"

TEST_CASE( "migo_signal_location_and_observer_persist_without_actor_knowledge",
           "[migo_tower][serialization]" )
{
    migo_tower::world_state world;
    world.last_signal_scan = 450;
    migo_tower::site_state site;
    site.site_id = "ordinary-site";
    site.anchor = tripoint_abs_omt( 8, 2, 0 );
    site.last_sense_turn = 450;
    site.signal = migo_tower::signal_record{ "physical-sample", "smoke",
        tripoint_abs_omt( 2, 2, 0 ), std::nullopt,
        "real-member", tripoint_abs_ms( 200, 60, 3 ), 450 };
    migo_tower::operation_record operation;
    operation.generation = 9;
    operation.source = *site.signal;
    operation.members = { {"real-member"}, {"second-member", true, true} };
    operation.communicated_losses = {"witnessed-member"};
    operation.advance( 450, 600 );
    operation.advance( 451, 1 );
    operation.advance( 451, 600 );
    site.operation = operation;
    site.next_generation = 10;
    world.sites.push_back( site );
    std::ostringstream output;
    {
        JsonOut json( output );
        world.serialize( json );
    }
    migo_tower::world_state loaded;
    loaded.deserialize( json_loader::from_string( output.str() ) );
    REQUIRE( loaded.find( site.site_id ) );
    const auto &signal = loaded.find( site.site_id )->signal;
    REQUIRE( signal );
    CHECK_FALSE( signal->source_ms );
    CHECK( signal->source_omt == site.signal->source_omt );
    CHECK( signal->observer_member == "real-member" );
    CHECK( signal->observer_position == site.signal->observer_position );
    CHECK( signal->observed_turn == 450 );
    CHECK( loaded.last_signal_scan == 450 );
    REQUIRE( loaded.find( site.site_id )->operation );
    const auto &restored_operation = *loaded.find( site.site_id )->operation;
    CHECK( restored_operation.generation == 9 );
    CHECK( restored_operation.contains( "real-member" ) );
    CHECK_FALSE( restored_operation.contains( "unselected-member" ) );
    CHECK( restored_operation.members[1].settled );
    CHECK( restored_operation.members[1].returned );
    CHECK( restored_operation.sweep.active_elapsed == 1 );
    CHECK( restored_operation.source.observed_turn == 450 );
    CHECK( restored_operation.communicated_losses == std::vector<std::string>{"witnessed-member"} );
    CHECK( loaded.find( site.site_id )->next_generation == 10 );
    CHECK( loaded.find( site.site_id )->last_sense_turn == 450 );
    loaded.deserialize( json_loader::from_string( "{}" ) );
    CHECK( loaded.sites.empty() );
    CHECK( loaded.last_signal_scan == -1 );
    overmap_global_state new_world;
    new_world.migo_towers = world;
    new_world.clear();
    CHECK( new_world.migo_towers.sites.empty() );
    CHECK( new_world.migo_towers.last_signal_scan == -1 );
}

TEST_CASE( "migo_real_signal_receiver_and_map_spawn_share_saved_residents",
           "[migo_tower][migo_population][physical_light][native]" )
{
    const bool load_first = GENERATE( false, true );
    clear_avatar();
    clear_map_without_vision();
    const auto old_turn = calendar::turn;
    on_out_of_scope restore_time( [&]() {
        set_time( old_turn );
    } );
    set_time( calendar::turn_zero + 1200_turns );
    reset_live_light_sample_cache();
    const auto old_populations = overmap_buffer.global_state.migo_populations;
    const auto old_towers = overmap_buffer.global_state.migo_towers;
    const auto old_bandits = overmap_buffer.global_state.bandit_live_world;
    std::set<character_id> old_bandit_ids;
    for( const auto &site : old_bandits.sites ) {
        for( const auto &member : site.members ) {
            old_bandit_ids.insert( member.npc_id );
        }
    }
    overmap_buffer.global_state.migo_towers.clear();
    map &here = get_map();
    const auto old_origin = here.get_abs_sub();
    const auto source_bub = get_avatar().pos_bub();
    const auto source = get_avatar().pos_abs();
    const auto source_omt = project_to<coords::omt>( source );
    const tripoint_abs_omt anchor( source_omt.xy() + point( 4, 0 ), 0 );
    REQUIRE_FALSE( here.inbounds( project_to<coords::ms>( anchor ) ) );
    std::vector<oter_id> old_terrain;
    std::set<character_id> old_npcs;
    for( int z = 0; z < 4; ++z ) {
        const tripoint_abs_omt p( anchor.xy(), z );
        old_terrain.push_back( overmap_buffer.ter( p ) );
        for( const auto &n : overmap_buffer.get_npcs_near_omt( p, 0 ) ) {
            old_npcs.insert( n->getID() );
        }
        overmap_buffer.ter_set( p, oter_str_id( "mi-go_scout_tower_" +
                                                std::to_string( z + 1 ) + "_north" ) );
    }
    const auto geometry = migo_population::geometry_at( anchor );
    REQUIRE( geometry );
    on_out_of_scope restore( [&]() {
        auto &om = overmap_buffer.get( project_to<coords::om>( anchor ).xy() );
        std::vector<std::pair<tripoint_abs_ms, std::string>> residents;
        for( const auto &entry : om.horde_entities() ) {
            if( entry.second->monster_data &&
                entry.second->monster_data->population_identity().site_id == geometry->id ) {
                residents.emplace_back( entry.first,
                                        entry.second->monster_data->population_identity().member_id );
            }
        }
        for( const auto &entry : residents ) {
            CHECK( om.release_monster_payload( entry.first, entry.second ) );
        }
        clear_creatures();
        for( const auto &site : overmap_buffer.global_state.bandit_live_world.sites ) {
            for( const auto &member : site.members ) {
                if( old_bandit_ids.count( member.npc_id ) == 0 ) {
                    overmap_buffer.remove_npc( member.npc_id );
                }
            }
        }
        for( int z = 0; z < 4; ++z ) {
            const tripoint_abs_omt p( anchor.xy(), z );
            for( const auto &n : overmap_buffer.get_npcs_near_omt( p, 0 ) ) {
                if( old_npcs.count( n->getID() ) == 0 ) {
                    overmap_buffer.remove_npc( n->getID() );
                }
            }
            overmap_buffer.ter_set( p, old_terrain[z] );
        }
        here.remove_field( here.get_bub( source ), field_type_id( "fd_smoke" ) );
        here.load( old_origin, false );
        get_avatar().setpos( here, here.get_bub( source ) );
        MAPBUFFER.clear_outside_reality_bubble();
        overmap_buffer.global_state.migo_populations = old_populations;
        overmap_buffer.global_state.migo_towers = old_towers;
        overmap_buffer.global_state.bandit_live_world = old_bandits;
        reset_live_light_sample_cache();
    } );
    // Real mapgen prepares the saved manifest, without entering the tower or
    // calling discover. Both receivers must consume these exact native slots.
    tinymap prepared;
    prepared.load( anchor, false );
    std::set<std::string> saved_ids;
    for( int z = 0; z < 4; ++z ) {
        for( int x = 0; x < 2; ++x ) {
            for( int y = 0; y < 2; ++y ) {
                submap *sm = MAPBUFFER.lookup_submap_existing( project_to<coords::sm>(
                        tripoint_abs_omt( anchor.xy(), z ) ) + point( x, y ) );
                REQUIRE( sm );
                for( const auto &slot : sm->spawns ) {
                    if( slot.population.site_id == geometry->id ) {
                        CHECK( saved_ids.insert( slot.population.member_id ).second );
                    }
                }
            }
        }
    }
    REQUIRE( saved_ids.size() == 8 );
    auto population = [&]() {
        return overmap_buffer.global_state.migo_populations.find( geometry->id );
    };
    REQUIRE( population() );
    REQUIRE_FALSE( population()->seed_committed );
    here.add_field( source_bub, field_type_id( "fd_smoke" ), 3 );
    const auto enter_tower = [&]() {
        // Keep the same source inside the new bubble while the actual main
        // map enters the tower. tinymap deliberately does not expose this
        // spawn API: local coordinates belong to the game's current map.
        here.load( project_to<coords::sm>( anchor ) - point( 8, 4 ), false );
        get_avatar().setpos( here, here.get_bub( source ) );
        here.spawn_monsters( true );
        for( int z = 0; z < 4; ++z ) {
            here.build_map_cache( z );
        }
    };
    if( load_first ) {
        enter_tower();
        REQUIRE( population()->seed_committed );
    }
    // This invokes the production sampler and delivery owner, not a synthetic
    // packet, direct discover call or future receiver stub.
    run_live_light_delivery_for_test();
    const auto &samples = live_light::samples_for_turn( calendar::turn );
    REQUIRE( std::any_of( samples.begin(), samples.end(), []( const auto & sample ) {
        return sample.mark.kind == "smoke";
    } ) );
    REQUIRE( population()->seed_committed );
    REQUIRE( population()->members.size() == 8 );
    const auto expected_source = project_to<coords::ms>( anchor ) + tripoint( 12, 9, 0 );
    CHECK( population()->source == expected_source );
    std::set<std::string> current_ids;
    for( const auto &member : population()->members ) {
        current_ids.insert( member.token.member_id );
    }
    CHECK( current_ids == saved_ids );
    const auto *tower = overmap_buffer.global_state.migo_towers.find( geometry->id );
    REQUIRE( tower );
    REQUIRE( tower->signal );
    CHECK( tower->signal->source_omt == source_omt );
    CHECK( tower->signal->source_ms == source );
    CHECK( tower->signal->observer_position.z() > 0 );
    CHECK( saved_ids.count( tower->signal->observer_member ) == 1 );
    CHECK( tower->signal->observed_turn == to_turn<int>( calendar::turn ) );
    CHECK( tower->recognized_enemies == 0 ); // The distant night emitter is not an occupant read.
    CHECK( tower->decision.members.size() == 3 );
    CHECK( tower->decision.reason == "uncertain_signal_ordinary_party" );
    REQUIRE( tower->find_source( source_omt ) );
    CHECK( tower->find_source( source_omt )->opposition.empty() );
    CHECK( tower->find_source( source_omt )->completed_generation == 0 );
    CHECK( std::count_if( population()->members.begin(),
    population()->members.end(), []( const auto &m ) {
        return m.assignment == migo_population::duty::dispatched;
    } ) == 3 ); // The actual delivery owner committed the response, without a direct API call.
    REQUIRE( tower->operation );
    CHECK( tower->operation->generation == 1 );
    CHECK( tower->next_generation == 2 );
    CHECK( tower->operation->members.size() == 3 );
    CHECK( tower->operation->party_at_departure.size() == 3 );
    CHECK( tower->operation->sweep.candidates.size() >= 45 );
    CHECK( tower->operation->sweep.candidates.size() <= 46 );
    std::set<tripoint_abs_ms> candidate_positions;
    for( const auto &candidate : tower->operation->sweep.candidates ) {
        CHECK( candidate_positions.insert( candidate.position ).second );
        const auto p = project_to<coords::omt>( candidate.position );
        CHECK( std::abs( p.x() - source_omt.x() ) <= 1 );
        CHECK( std::abs( p.y() - source_omt.y() ) <= 1 );
    }
    CHECK( tower->operation->sweep.pending() == source );
    CHECK_FALSE( migo_tower::begin_response( geometry->id, to_turn<int>( calendar::turn ) ) );
    // A missing cross-file policy record cannot lease a parallel copy of
    // existing away members. Retain their actual payloads/epochs and restore
    // this simulated save-gap operand before any movement.
    auto *mutable_tower = overmap_buffer.global_state.migo_towers.find( geometry->id );
    const auto saved_operation = mutable_tower->operation;
    mutable_tower->operation.reset();
    CHECK_FALSE( migo_tower::begin_response( geometry->id, to_turn<int>( calendar::turn ) ) );
    CHECK( mutable_tower->next_generation == 2 );
    mutable_tower->operation = saved_operation;
    int leased = 0;
    int defenders = 0;
    for( const auto &member : population()->members ) {
        if( member.assignment == migo_population::duty::dispatched ) {
            ++leased;
            CHECK( tower->operation->contains( member.token.member_id ) );
            if( !load_first ) {
                auto &owner = overmap_buffer.get( project_to<coords::om>( member.home ).xy() );
                bool found = false;
                for( const auto &entry : owner.horde_entities() ) {
                    if( entry.second->monster_data && entry.second->monster_data->population_identity().member_id ==
                        member.token.member_id ) {
                        found = true;
                        CHECK( entry.second->is_active() );
                        CHECK( entry.second->tracking_intensity == 0 );
                        CHECK( migo_tower::owns_intent( *entry.second->monster_data,
                                                       migo_population::representation::abstract ) );
                    }
                }
                CHECK( found );
            }
        } else {
            ++defenders;
        }
    }
    CHECK( leased == 3 );
    CHECK( defenders == 5 );
    const auto first_observation = tower->last_sense_turn;
    run_live_light_delivery_for_test();
    if( !load_first ) {
        // The actual scheduler must admit these native non-zombie payloads
        // without manufacturing tracking, and must not service them twice.
        auto &owner = overmap_buffer.get( project_to<coords::om>( anchor ).xy() );
        // Recreate the actual native unload constructor for an unselected
        // home resident with an inherited goal. Its immobile admission must
        // survive servicing somebody else's operation.
        std::optional<tripoint_abs_ms> defender_position;
        std::string defender_id;
        time_point defender_processed;
        for( const auto &entry : owner.horde_entities() ) {
            if( entry.second->monster_data &&
                entry.second->monster_data->population_identity().site_id == geometry->id &&
                !tower->operation->contains( entry.second->monster_data->population_identity().member_id ) ) {
                monster defender( *entry.second->monster_data );
                defender_position = entry.first;
                defender_id = defender.population_identity().member_id;
                defender.set_dest( entry.first + point::east );
                REQUIRE( owner.release_monster_payload( entry.first, defender_id ) );
                auto *retained = owner.retain_monster_payload( entry.first, defender );
                REQUIRE( retained );
                REQUIRE( retained->tracking_intensity == 1000 );
                defender_processed = retained->last_processed;
                break;
            }
        }
        REQUIRE( defender_position );
        calendar::turn += 1_turns;
        migo_tower::service_operations( to_turn<int>( calendar::turn ), 1 );
        overmap_buffer.move_hordes();
        bool defender_found = false;
        for( const auto &entry : owner.horde_entities() ) {
            if( entry.second->monster_data &&
                entry.second->monster_data->population_identity().member_id == defender_id ) {
                defender_found = true;
                CHECK( entry.first == *defender_position );
                CHECK( entry.second->last_processed == defender_processed );
                CHECK( population()->find( defender_id )->assignment == migo_population::duty::available );
            }
        }
        CHECK( defender_found );
        std::vector<std::string> before;
        for( const auto &entry : owner.horde_entities() ) {
            if( entry.second->monster_data && tower->operation->contains(
                    entry.second->monster_data->population_identity().member_id ) ) {
                std::ostringstream saved;
                { JsonOut json( saved ); entry.second->monster_data->serialize( json ); }
                before.push_back( saved.str() );
            }
        }
        REQUIRE( before.size() == 3 );
        overmap_buffer.move_hordes();
        std::vector<std::string> after;
        for( const auto &entry : owner.horde_entities() ) {
            if( entry.second->monster_data && tower->operation->contains(
                    entry.second->monster_data->population_identity().member_id ) ) {
                std::ostringstream saved;
                { JsonOut json( saved ); entry.second->monster_data->serialize( json ); }
                after.push_back( saved.str() );
            }
        }
        std::sort( before.begin(), before.end() );
        std::sort( after.begin(), after.end() );
        CHECK( before == after );
    }
    enter_tower();
    for( monster &body : g->all_monsters() ) {
        if( body.population_identity().site_id == geometry->id &&
            tower->operation->contains( body.population_identity().member_id ) ) {
            REQUIRE( migo_tower::owns_intent( body, migo_population::representation::local ) );
            const int before = body.get_moves();
            const bool already_credited = body.migo_intent()->last_energy_world == to_turn<int>( calendar::turn );
            body.process_turn();
            CHECK( body.get_moves() == before + ( already_credited ? 0 : body.get_speed() ) );
            const int after = body.get_moves();
            body.process_turn();
            CHECK( body.get_moves() == after );
            CHECK( body.migo_intent()->last_native_process_world == to_turn<int>( calendar::turn ) );
        }
    }
    CHECK( population()->members.size() == 8 );
    CHECK( population()->source == expected_source );
    CHECK( overmap_buffer.global_state.migo_towers.find( geometry->id )->last_sense_turn ==
           first_observation );
    for( const auto &member : population()->members ) {
        CHECK( saved_ids.count( member.token.member_id ) == 1 );
    }
    // Lower-level movement proof uses the actual generated body, stairs and
    // resin portals. It is not a selected assault or a live native journey.
    monster *climber = nullptr;
    for( monster &body : g->all_monsters() ) {
        if( body.population_identity().site_id == geometry->id &&
            ( !climber || body.pos_abs().z() > climber->pos_abs().z() ) ) {
            climber = &body;
        }
    }
    REQUIRE( climber );
    REQUIRE( climber->pos_abs().z() == 3 );
    int descents = 0;
    int doors_opened = 0;
    for( int steps = 0; steps < 200 && ( climber->pos_abs().z() != 0 ||
                                         project_to<coords::omt>( climber->pos_abs() ).xy() == anchor.xy() ); ++steps ) {
        calendar::turn += 1_turns;
        climber->process_turn();
        const auto before = climber->pos_abs();
        const auto next = physical_navigation::read_step( *climber, source );
        INFO( next.reason );
        CAPTURE( before, next.position, steps );
        REQUIRE( next.known );
        REQUIRE( next.next != physical_navigation::action::none );
        if( next.next == physical_navigation::action::open_door ) {
            REQUIRE( physical_navigation::commit_door( *climber, next.position ) );
            climber->mod_moves( -next.move_cost );
            ++doors_opened;
            CHECK( climber->pos_abs() == before );
        } else {
            REQUIRE( here.inbounds( next.position ) );
            REQUIRE( climber->move_to( here.get_bub( next.position ) ) );
            CHECK( climber->pos_abs() == next.position );
            descents += climber->pos_abs().z() < before.z() ? 1 : 0;
        }
        CHECK( population()->owns( climber->population_identity(),
                                   migo_population::representation::local ) );
    }
    CHECK( descents == 3 );
    CHECK( doors_opened > 0 );
    CHECK( climber->pos_abs().z() == 0 );
    CHECK( project_to<coords::omt>( climber->pos_abs() ).xy() != anchor.xy() );
    // Supplemental refusal controls reuse the genuinely received packet. The
    // synthetic freshness below earns no new positive emission/receipt proof.
    auto refused_samples = samples;
    calendar::turn += 5_minutes;
    for( auto &sample : refused_samples ) {
        sample.observed_turn = to_turn<int>( calendar::turn );
        sample.source_ms = source + point( 24, 0 ); // Contradicts its original OMT.
    }
    CHECK( migo_tower::receive_signals( refused_samples,
                                        to_turn<int>( calendar::turn ) ).received == 0 );
    CHECK( overmap_buffer.global_state.migo_towers.find( geometry->id )->last_sense_turn ==
           first_observation );
    for( monster &body : g->all_monsters() ) {
        if( body.population_identity().site_id == geometry->id ) {
            body.add_effect( efftype_id( "blind" ), 1_hours );
            REQUIRE_FALSE( body.can_see() );
        }
    }
    calendar::turn += 5_minutes;
    refused_samples = samples;
    for( auto &sample : refused_samples ) {
        sample.observed_turn = to_turn<int>( calendar::turn );
    }
    CHECK( migo_tower::receive_signals( refused_samples,
                                        to_turn<int>( calendar::turn ) ).received == 0 );
    CHECK( overmap_buffer.global_state.migo_towers.find( geometry->id )->last_sense_turn ==
           first_observation );
}


TEST_CASE( "migo_capability_admission_uses_damage_survival_and_real_reserve",
           "[migo_tower]" )
{
    using namespace migo_tower;
    combat_profile ordinary;
    ordinary.known = true;
    ordinary.hp = 100;
    ordinary.speed = 100;
    ordinary.hit = 5;
    ordinary.dodge = 5;
    ordinary.melee.add_damage( damage_type_id( "bash" ), 20 );
    std::vector<candidate> available;
    for( int i = 0; i < 8; ++i ) {
        available.push_back( {"body-" + std::to_string( i ),
                              i < 6 ? "mon_mi_go" : "mon_mi_go_scout", ordinary} );
    }
    const auto baseline = select_party( available, {}, 20, false );
    REQUIRE( baseline.members.size() == 3 );
    CHECK( baseline.members == std::vector<std::string> {"body-0", "body-1", "body-6"} );
    combat_profile enemy = ordinary;
    enemy.hp = 620;
    // Three bodies lose two before inflicting620 damage; four lose only one.
    const auto larger = select_party( available, {enemy}, 1, false );
    REQUIRE( larger.members.size() == 4 );
    CHECK( larger.projection.known );
    CHECK( larger.projection.sufficient );
    CHECK( larger.projection.casualties == 1 );
    CHECK( larger.projection.party_damage_per_second > 0 );
    const auto cautious = select_party( available, {enemy}, 1, true );
    CHECK( cautious.members.empty() ); // Even six cannot defeat before first loss.
    CHECK( cautious.reason == "largest_party_fails_survival_margin" );
    enemy.known = false;
    CHECK( select_party( available, {enemy}, 1, false ).reason == "observed_capability_unknown" );
    enemy.known = true;
    enemy.ranged_range = 10;
    enemy.ranged.add_damage( damage_type_id( "bullet" ), 200 );
    CHECK( select_party( available, {enemy}, 20, false ).members.empty() );
    CHECK( select_party( available, {enemy}, 5, false ).members.empty() );
    // Armor is a damage-channel fact, not a size or generic power rating.
    enemy.ranged = damage_instance();
    enemy.armor.set_resist( damage_type_id( "bash" ), 100 );
    CHECK( select_party( available, {enemy}, 1, false ).members.empty() );
    available.resize( 4 );
    CHECK( select_party( available, {}, 1, false ).members.empty() );
    available.push_back( {"substitute", "mon_mi_go", ordinary} );
    CHECK( select_party( available, {}, 1, false ).members.size() == 3 );
    available.back().capability.hp = 0;
    CHECK( select_party( available, {}, 1, false ).members.empty() );
}


TEST_CASE( "migo_native_capabilities_require_perception_and_ignore_hidden_human_contents",
           "[migo_tower][physical_light][native]" )
{
    clear_avatar();
    clear_map_with_vision( -1, 4, false );
    const auto old_turn = calendar::turn;
    on_out_of_scope restore_scene( [&]() {
        clear_avatar();
        clear_map_without_vision( -1, 4 );
        set_time( old_turn );
    } );
    set_time_to_day();
    map &here = get_map();
    monster observer( mtype_id( "mon_mi_go" ) );
    observer.set_pos_abs_only( here.get_abs( tripoint_bub_ms( 12, 66, 0 ) ) );
    auto &human = get_avatar();
    human.setpos( here, tripoint_bub_ms( 30, 66, 0 ), false );
    REQUIRE( human.wear_item( item( itype_id( "backpack" ) ), false ) );
    here.build_map_cache( 0 );
    scout_observation::site_reader first_read;
    const auto unarmed = migo_tower::observe_enemy( observer, human, first_read );
    REQUIRE( unarmed );
    REQUIRE( unarmed->known );
    CHECK( unarmed->hp > 0 );
    human.set_part_hp_cur( bodypart_id( "torso" ), 1 );
    human.i_add( item( itype_id( "rock" ) ) );
    REQUIRE_FALSE( human.get_wielded_item() );
    scout_observation::site_reader hidden_change;
    const auto same = migo_tower::observe_enemy( observer, human, hidden_change );
    REQUIRE( same );
    CHECK( same->hp == unarmed->hp );
    CHECK( same->melee.total_damage() == unarmed->melee.total_damage() );
    CHECK( same->ranged.total_damage() == unarmed->ranged.total_damage() );
    REQUIRE( itype_id( "glock_19" ).is_valid() );
    item gun( itype_id( "glock_19" ) );
    human.set_wielded_item( gun );
    scout_observation::site_reader armed_read;
    const auto armed = migo_tower::observe_enemy( observer, human, armed_read );
    REQUIRE( armed );
    CHECK( armed->ranged.total_damage() > 0 );
    CHECK( armed->ranged_range > 0 );
    gun.ammo_set( gun.ammo_default() );
    human.set_wielded_item( gun );
    scout_observation::site_reader loaded_read;
    const auto loaded = migo_tower::observe_enemy( observer, human, loaded_read );
    REQUIRE( loaded );
    CHECK( loaded->ranged.total_damage() == armed->ranged.total_damage() );
    CHECK( loaded->ranged_range == armed->ranged_range );
    observer.add_effect( efftype_id( "blind" ), 1_hours );
    scout_observation::site_reader blind_read;
    CHECK_FALSE( migo_tower::observe_enemy( observer, human, blind_read ) );
    observer.remove_effect( efftype_id( "blind" ) );
    for( int y = 0; y < 132; ++y ) {
        here.ter_set( tripoint_bub_ms( 20, y, 0 ), ter_str_id( "t_wall" ) );
    }
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0 );
    scout_observation::site_reader occluded_read;
    CHECK_FALSE( migo_tower::observe_enemy( observer, human, occluded_read ) );
    const auto own_before = migo_tower::own_capability( observer );
    observer.set_hp( 50 );
    const auto own_after = migo_tower::own_capability( observer );
    REQUIRE( own_before.known );
    CHECK( own_after.hp == 50 );
    CHECK( own_after.hp < own_before.hp );
    monster scout( mtype_id( "mon_mi_go_scout" ) );
    const auto scout_profile = migo_tower::own_capability( scout );
    REQUIRE( scout_profile.known );
    CHECK( scout_profile.ranged.total_damage() > 0 );
    CHECK( scout_profile.ranged_range == 30 );
    CHECK( scout_profile.ranged_cost >= 5 * scout.get_speed() );
}

TEST_CASE( "migo_extended_melee_is_exposure_and_saved_threat_memory_is_conservative",
           "[migo_tower][serialization]" )
{
    using namespace migo_tower;
    combat_profile ordinary;
    ordinary.known = true;
    ordinary.hp = 100;
    ordinary.speed = 100;
    ordinary.hit = 5;
    ordinary.dodge = 5;
    ordinary.melee.add_damage( damage_type_id( "bash" ), 20 );
    combat_profile enemy = ordinary;
    enemy.melee.clear();
    melee_option reach;
    reach.damage.add_damage( damage_type_id( "bash" ), 200 );
    reach.hit = 5;
    reach.range = 4;
    reach.no_adjacent = true;
    enemy.melee_options.push_back( reach );
    const std::vector<combat_profile> party( 3, ordinary );
    CHECK( project( party, {enemy}, 1, true ).sufficient );
    CHECK_FALSE( project( party, {enemy}, 5, true ).sufficient );
    threat_scenarios memory;
    remember_observation( memory, {enemy} );
    remember_observation( memory, {} );
    remember_observation( memory, {enemy} );
    REQUIRE( memory.size() == 1 );
    site_state site;
    site.advance( 100, 9000 );
    site.advance( 101, 1 );
    site.advance( 101, 9000 );
    REQUIRE( site.active_elapsed == 1 );
    auto *history = site.remember_source( tripoint_abs_omt( 2, 3, 2 ) );
    REQUIRE( history );
    history->opposition = memory;
    history->latest_evidence = "physically-seen-weapon";
    REQUIRE( history->finish( 1, site.active_elapsed, "returned", {"witnessed-loss"}, false, party ) );
    CHECK_FALSE( history->finish( 1, 99, "returned", {}, false, party ) );
    CHECK( history->caution_level() == 1 );
    CHECK( history->requires_stronger() );
    admission_decision sufficient;
    sufficient.projection.known = true;
    sufficient.projection.sufficient = true;
    CHECK_FALSE( history->retry_ready( 86400, party, sufficient, true ) );
    CHECK_FALSE( history->retry_ready( 86401, party, sufficient, true ) );
    history->latest_evidence = "physically-seen-changed-weapon";
    CHECK( history->retry_ready( 86401, party, sufficient, true ) );
    std::ostringstream output;
    { JsonOut json( output ); site.serialize( json ); }
    site_state loaded;
    loaded.deserialize( json_loader::from_string( output.str() ) );
    const auto *restored = loaded.find_source( tripoint_abs_omt( 2, 3, 0 ) );
    REQUIRE( restored );
    CHECK( restored->caution_level() == 1 );
    CHECK( restored->retry_after_active == 86401 );
    REQUIRE( restored->opposition.size() == 1 );
    CHECK( capability_key( restored->opposition[0] ) == capability_key( memory[0] ) );
    CHECK( restored->opposition[0][0].melee_options[0].no_adjacent );
    CHECK( restored->opposition[0][0].melee_options[0].range == 4 );
    CHECK( loaded.active_elapsed == 1 );
    loaded.advance( 100000, 0 );
    CHECK( loaded.active_elapsed == 1 );
    loaded.advance( 100001, 1 );
    CHECK( loaded.active_elapsed == 2 );
    history = loaded.find_source( tripoint_abs_omt( 2, 3, -1 ) );
    REQUIRE( history );
    REQUIRE( history->finish( 2, 86401, "returned", {"second-loss"}, false, party ) );
    CHECK( history->caution_level() == 2 );
    CHECK( history->retry_after_active == 86401 + 7 * 86400 );
}

TEST_CASE( "migo_member_intent_follows_the_native_payload_and_reload",
           "[migo_tower][serialization][ownership]" )
{
    monster original( mtype_id( "mon_mi_go" ) );
    CHECK_FALSE( original.migo_intent() );
    original.population_identity() = { "site", "resident", 7, tripoint_abs_omt( 2, 2, 0 ) };
    migo_tower::member_intent intent;
    intent.generation = 4;
    intent.home = tripoint_abs_ms( 60, 60, 3 );
    intent.waypoint = tripoint_abs_ms( 72, 60, 0 );
    intent.last_energy_world = 101;
    intent.last_native_process_world = 100;
    original.set_migo_intent( intent );
    horde_entity prepared( original );
    REQUIRE( prepared.monster_data );
    REQUIRE( prepared.monster_data->migo_intent() );
    prepared.monster_data->migo_intent()->retreat();
    prepared.monster_data->migo_intent()->witnessed_losses.push_back( "witnessed-member" );
    // A prepared receiving candidate is a private payload, not a second
    // global motor or a mutation of the retained source on refusal.
    CHECK( original.migo_intent()->phase == migo_tower::member_phase::outbound );
    CHECK( original.migo_intent()->waypoint == intent.waypoint );
    CHECK( original.migo_intent()->witnessed_losses.empty() );
    CHECK( prepared.monster_data->population_identity().epoch == 7 );
    CHECK_FALSE( prepared.monster_data->migo_intent()->waypoint );
    prepared.monster_data->migo_intent()->waypoint = intent.home;
    prepared.monster_data->migo_intent()->retreat();
    CHECK( prepared.monster_data->migo_intent()->waypoint == intent.home );

    std::ostringstream saved;
    { JsonOut json( saved ); prepared.monster_data->serialize( json ); }
    monster loaded;
    loaded.deserialize( json_loader::from_string( saved.str() ) );
    REQUIRE( loaded.migo_intent() );
    CHECK( loaded.population_identity().member_id == "resident" );
    CHECK( loaded.population_identity().epoch == 7 );
    CHECK( loaded.migo_intent()->generation == 4 );
    CHECK( loaded.migo_intent()->phase == migo_tower::member_phase::returning );
    CHECK( loaded.migo_intent()->last_energy_world == 101 );
    CHECK( loaded.migo_intent()->last_native_process_world == 100 );
    CHECK( loaded.migo_intent()->witnessed_losses == std::vector<std::string>{"witnessed-member"} );
    original.clear_migo_intent();
    std::ostringstream old_saved;
    { JsonOut json( old_saved ); original.serialize( json ); }
    loaded.deserialize( json_loader::from_string( old_saved.str() ) );
    CHECK_FALSE( loaded.migo_intent() );
    CHECK( loaded.population_identity().epoch == 7 );
}

TEST_CASE( "migo_native_settlement_does_not_turn_roster_death_into_loss_knowledge",
           "[migo_tower][ownership][native][serialization]" )
{
    const bool still_away = GENERATE( false, true );
    // Component setup supplies leases/death truth, not a dispatched journey or
    // witnessed native casualty. The actual service caller must keep those
    // facts private until communication or its legitimate overdue allowance.
    clear_avatar();
    clear_map_with_vision( -1, 4, false );
    const auto old_turn = calendar::turn;
    const auto old_populations = overmap_buffer.global_state.migo_populations;
    const auto old_towers = overmap_buffer.global_state.migo_towers;
    on_out_of_scope restore( [&]() {
        clear_map_without_vision( -1, 4 );
        overmap_buffer.global_state.migo_populations = old_populations;
        overmap_buffer.global_state.migo_towers = old_towers;
        set_time( old_turn );
    } );
    set_time( calendar::turn_zero + 1500_turns );
    get_avatar().setpos( get_map(), tripoint_bub_ms( 1, 1, 0 ), false );
    const auto home = get_map().get_abs( tripoint_bub_ms( 60, 60, 0 ) );
    const auto anchor = project_to<coords::omt>( home );
    const std::string id = "settlement-knowledge-component";
    migo_population::site_record population;
    population.id = id;
    population.anchor = anchor;
    std::vector<migo_population::member_record> members;
    for( const auto &name : {"returnee", "unseen-one", "unseen-two"} ) {
        migo_population::member_record member;
        member.token = {id, name, 1, anchor};
        member.type = mtype_id( "mon_mi_go" );
        member.home = home;
        member.hp = member.type->hp;
        member.assignment = migo_population::duty::dispatched;
        member.owner = name == std::string( "returnee" ) ?
                       migo_population::representation::local : migo_population::representation::abstract;
        members.push_back( member );
    }
    REQUIRE( population.commit_seed( members, std::nullopt, "", 0, true, false ) );
    REQUIRE( population.record_death( members[1].token ) );
    REQUIRE( population.record_death( members[2].token ) );
    overmap_buffer.global_state.migo_populations.sites.push_back( population );
    migo_tower::site_state site;
    site.site_id = id;
    site.anchor = anchor;
    migo_tower::operation_record operation;
    operation.generation = 1;
    operation.source.source_omt = anchor + point( 2, 0 );
    operation.members = {{"returnee"}, {"unseen-one"}, {"unseen-two"}};
    operation.sweep.candidates = {{get_map().get_abs( tripoint_bub_ms( 110, 60, 0 ) )}};
    operation.overdue_after_active = 10;
    operation.advance( to_turn<int>( calendar::turn ), 0 );
    site.operation = operation;
    REQUIRE( site.remember_source( operation.source.source_omt ) );
    overmap_buffer.global_state.migo_towers.sites.push_back( site );
    monster *body = g->place_critter_at( mtype_id( "mon_mi_go" ),
                                       tripoint_bub_ms( still_away ? 100 : 60, 60, 0 ) );
    REQUIRE( body );
    body->population_identity() = members[0].token;
    migo_tower::member_intent intent;
    intent.generation = 1;
    intent.phase = still_away ? migo_tower::member_phase::outbound : migo_tower::member_phase::returning;
    intent.home = home;
    body->set_migo_intent( intent );
    migo_tower::service_operations( to_turn<int>( calendar::turn ), 0 );
    calendar::turn += 1_turns;
    migo_tower::service_operations( to_turn<int>( calendar::turn ), 1 );
    auto *current = overmap_buffer.global_state.migo_towers.find( id );
    REQUIRE( current );
    REQUIRE( current->operation );
    CHECK( current->operation->members[0].returned == !still_away );
    CHECK( current->operation->members[1].settled );
    CHECK( current->operation->members[2].settled );
    CHECK( current->operation->communicated_losses.empty() );
    CHECK_FALSE( current->operation->resolved );
    CHECK( current->find_source( operation.source.source_omt )->completed_generation == 0 );
    calendar::turn += 9_turns;
    migo_tower::service_operations( to_turn<int>( calendar::turn ), 9 );
    CHECK( current->operation->resolved == !still_away );
    CHECK_FALSE( body->is_dead_state() );
    const auto *history = current->find_source( operation.source.source_omt );
    REQUIRE( history );
    CHECK( history->outcome == "overdue_missing" );
    CHECK( history->casualty_attempts == 0 );
    CHECK( history->overdue_attempts == 1 );
    CHECK( history->last_witnessed_losses.empty() );
    std::ostringstream saved;
    { JsonOut json( saved ); current->serialize( json ); }
    migo_tower::site_state loaded;
    loaded.deserialize( json_loader::from_string( saved.str() ) );
    CHECK( loaded.operation->resolved == !still_away );
    CHECK( loaded.find_source( operation.source.source_omt )->overdue_attempts == 1 );
    migo_tower::service_operations( to_turn<int>( calendar::turn ), 9 );
    CHECK( current->find_source( operation.source.source_omt )->overdue_attempts == 1 );
}

TEST_CASE( "migo_source_history_eviction_cannot_restore_a_blind_retry",
           "[migo_tower][serialization]" )
{
    using namespace migo_tower;
    site_state site;
    for( int i = 0; i < 4; ++i ) {
        auto *history = site.remember_source( tripoint_abs_omt( i, 0, 0 ) );
        REQUIRE( history );
        REQUIRE( history->finish( i + 1, 0, "searched_returned", {}, false, {} ) );
    }
    CHECK_FALSE( site.remember_source( tripoint_abs_omt( 4, 0, 0 ) ) );
    site.active_elapsed = 86400; // explicit component elapsed, no native time proof
    REQUIRE( site.remember_source( tripoint_abs_omt( 4, 0, 0 ) ) );
    CHECK_FALSE( site.find_source( tripoint_abs_omt( 0, 0, 0 ) ) );
    auto *recalled = site.remember_source( tripoint_abs_omt( 0, 0, 1 ) );
    REQUIRE( recalled );
    CHECK( recalled->prior_outcome_unavailable );
    CHECK( recalled->caution_level() == 2 );
    CHECK( recalled->retry_after_active == 8 * 86400 );
    admission_decision sufficient;
    sufficient.projection.known = true;
    sufficient.projection.sufficient = true;
    CHECK_FALSE( recalled->retry_ready( 9 * 86400, {}, sufficient, false ) );
    std::ostringstream saved;
    { JsonOut json( saved ); site.serialize( json ); }
    site_state loaded;
    loaded.deserialize( json_loader::from_string( saved.str() ) );
    REQUIRE( loaded.find_source( tripoint_abs_omt( 0, 0, 0 ) ) );
    CHECK( loaded.find_source( tripoint_abs_omt( 0, 0, 0 ) )->prior_outcome_unavailable );
    CHECK( loaded.prior_sources == site.prior_sources );
}

TEST_CASE( "migo_late_knowledge_resolves_one_overdue_attempt_without_double_loss",
           "[migo_tower][serialization]" )
{
    migo_tower::source_history history;
    history.latest_evidence = "actual-source-facts";
    REQUIRE( history.finish( 1, 100, "overdue_missing", {}, true, {} ) );
    CHECK( history.overdue_attempts == 1 );
    CHECK( history.casualty_attempts == 0 );
    REQUIRE( history.finish( 1, 200, "returned", {"actually-witnessed"}, false, {} ) );
    CHECK( history.overdue_attempts == 0 );
    CHECK( history.casualty_attempts == 1 );
    CHECK( history.caution_level() == 1 );
    const auto deadline = history.retry_after_active;
    CHECK_FALSE( history.finish( 1, 300, "returned", {"actually-witnessed"}, false, {} ) );
    CHECK( history.retry_after_active == deadline );
    REQUIRE( history.finish( 2, 400, "overdue_missing", {}, true, {} ) );
    CHECK( history.caution_level() == 2 );
    REQUIRE( history.finish( 2, 500, "searched_returned", {}, false, {} ) );
    CHECK( history.caution_level() == 1 ); // this late party actually returned, with no new casualty
    CHECK( history.casualty_attempts == 1 );
    CHECK( history.overdue_attempts == 0 );
    CHECK_FALSE( history.finish( 2, 600, "searched_returned", {}, false, {} ) );
}

TEST_CASE( "migo_actual_death_needs_physical_witness_then_home_communication",
           "[migo_tower][native][ownership][serialization]" )
{
    const bool unseen = GENERATE( false, true );
    clear_avatar();
    clear_map_with_vision( -1, 4, false );
    const auto old_turn = calendar::turn;
    const auto old_populations = overmap_buffer.global_state.migo_populations;
    const auto old_towers = overmap_buffer.global_state.migo_towers;
    on_out_of_scope restore( [&]() {
        clear_map_without_vision( -1, 4 );
        overmap_buffer.global_state.migo_populations = old_populations;
        overmap_buffer.global_state.migo_towers = old_towers;
        set_time( old_turn );
    } );
    set_time_to_day();
    map &here = get_map();
    get_avatar().setpos( here, tripoint_bub_ms( 1, 1, 0 ), false );
    const auto home = here.get_abs( tripoint_bub_ms( 60, 60, 0 ) );
    const auto casualty_position = here.get_abs( tripoint_bub_ms( 90, 60, 0 ) );
    const auto anchor = project_to<coords::omt>( home );
    const std::string id = "witness-native-component";
    migo_population::site_record population;
    population.id = id;
    population.anchor = anchor;
    std::vector<migo_population::member_record> members;
    for( const auto &name : {"returnee", "casualty"} ) {
        migo_population::member_record member;
        member.token = {id, name, 1, anchor};
        member.type = mtype_id( "mon_mi_go" );
        member.home = home;
        member.hp = member.type->hp;
        member.owner = migo_population::representation::local;
        member.assignment = migo_population::duty::dispatched;
        members.push_back( member );
    }
    REQUIRE( population.commit_seed( members, std::nullopt, "", 0, true, false ) );
    overmap_buffer.global_state.migo_populations.sites.push_back( population );
    migo_tower::site_state site;
    site.site_id = id;
    site.anchor = anchor;
    migo_tower::operation_record operation;
    operation.generation = 1;
    operation.source.source_omt = project_to<coords::omt>( casualty_position );
    operation.sweep.candidates = {{casualty_position}};
    operation.members = {{"returnee"}, {"casualty"}};
    operation.overdue_after_active = 100000;
    operation.advance( to_turn<int>( calendar::turn ), 0 );
    site.operation = operation;
    REQUIRE( site.remember_source( operation.source.source_omt ) );
    overmap_buffer.global_state.migo_towers.sites.push_back( site );
    monster *carrier = g->place_critter_at( members[0].type, tripoint_bub_ms( 89, 60, 0 ) );
    monster *casualty = g->place_critter_at( members[1].type, tripoint_bub_ms( 90, 60, 0 ) );
    REQUIRE( carrier );
    REQUIRE( casualty );
    carrier->population_identity() = members[0].token;
    casualty->population_identity() = members[1].token;
    migo_tower::member_intent intent;
    intent.generation = 1;
    intent.home = home;
    carrier->set_migo_intent( intent );
    casualty->set_migo_intent( intent );
    if( unseen ) {
        carrier->add_effect( efftype_id( "blind" ), 1_hours );
    }
    if( unseen ) {
        REQUIRE_FALSE( carrier->can_see() ); // native adjacent awareness is not a visual witness
    } else {
        REQUIRE( carrier->sees_without_clairvoyance_physical( here, *casualty ) );
    }
    // Explicit native fatal damage/death is component setup, not a claimed
    // combat victory. The real death hook owns terminal roster/witness facts.
    casualty->set_hp( 0 );
    casualty->die( &here, nullptr );
    casualty->die( &here, nullptr );
    auto *current = overmap_buffer.global_state.migo_towers.find( id );
    REQUIRE( current );
    CHECK( current->operation->communicated_losses.empty() );
    CHECK( carrier->migo_intent()->witnessed_losses.size() == ( unseen ? 0 : 1 ) );
    CHECK( overmap_buffer.global_state.migo_populations.find( id )->find( "casualty" )->owner ==
           migo_population::representation::dead );
    std::ostringstream saved_carrier;
    { JsonOut json( saved_carrier ); carrier->serialize( json ); }
    monster reloaded;
    reloaded.deserialize( json_loader::from_string( saved_carrier.str() ) );
    CHECK( reloaded.migo_intent()->witnessed_losses == carrier->migo_intent()->witnessed_losses );
    carrier->remove_effect( efftype_id( "blind" ) ); // restore sight after the unseen death; no new knowledge
    for( int turns = 0; turns < 100 && !current->operation->members[0].returned; ++turns ) {
        calendar::turn += 1_turns;
        process_monsters_and_npcs_turn_for_test();
        migo_tower::service_operations( to_turn<int>( calendar::turn ), 1 );
    }
    CHECK( current->operation->members[0].returned );
    CHECK( current->operation->resolved == !unseen );
    CHECK( current->operation->communicated_losses.size() == ( unseen ? 0 : 1 ) );
    CHECK( current->find_source( operation.source.source_omt )->casualty_attempts == ( unseen ? 0 : 1 ) );
}

TEST_CASE( "migo_actual_search_motor_excludes_fire_reopens_once_and_visits_connected_floor",
           "[migo_tower][raid_sweep][native][ownership]" )
{
    const bool contained = GENERATE( false, true );
    clear_avatar();
    clear_map_with_vision( -1, 4, false );
    const auto old_turn = calendar::turn;
    const auto old_populations = overmap_buffer.global_state.migo_populations;
    const auto old_towers = overmap_buffer.global_state.migo_towers;
    on_out_of_scope restore( [&]() {
        clear_map_without_vision( -1, 4 );
        overmap_buffer.global_state.migo_populations = old_populations;
        overmap_buffer.global_state.migo_towers = old_towers;
        set_time( old_turn );
    } );
    set_time_to_day();
    map &here = get_map();
    get_avatar().setpos( here, tripoint_bub_ms( 1, 1, 0 ), false );
    const tripoint_bub_ms start( 60, 60, 0 );
    const tripoint_bub_ms first( 64, 60, 0 );
    const tripoint_bub_ms second( 66, 60, 0 );
    const tripoint_bub_ms fire( 64, 61, 0 );
    const tripoint_bub_ms stairs( 67, 60, 0 );
    const auto landing = stairs + tripoint_rel_ms( 0, 0, 1 );
    for( const auto &p : here.points_in_rectangle( tripoint_bub_ms( 64, 58, 1 ),
            tripoint_bub_ms( 70, 62, 1 ) ) ) {
        here.ter_set( p, ter_str_id( "t_floor" ) );
    }
    here.ter_set( stairs, ter_str_id( "t_stairs_up" ) );
    here.ter_set( landing, ter_str_id( "t_stairs_down" ) );
    if( contained ) {
        here.furn_set( fire, furn_str_id( "f_brazier" ) );
    }
    here.add_field( fire, field_type_id( "fd_fire" ), 1 );
    here.invalidate_map_cache( 0 );
    here.invalidate_map_cache( 1 );
    here.build_map_cache( 0 );
    here.build_map_cache( 1 );
    const auto anchor = project_to<coords::omt>( here.get_abs( start ) );
    const std::string id = "search-motor-component";
    migo_population::member_record member;
    member.token = {id, "real-body", 1, anchor};
    member.type = mtype_id( "mon_mi_go" );
    member.home = here.get_abs( start );
    member.owner = migo_population::representation::local;
    member.assignment = migo_population::duty::dispatched;
    member.hp = member.type->hp;
    migo_population::site_record population;
    population.id = id;
    population.anchor = anchor;
    REQUIRE( population.commit_seed( {member}, std::nullopt, "", 0, true, false ) );
    overmap_buffer.global_state.migo_populations.sites.push_back( population );
    migo_tower::site_state site;
    site.site_id = id;
    site.anchor = anchor;
    migo_tower::operation_record operation;
    operation.generation = 1;
    operation.source.kind = "smoke";
    operation.source.source_omt = anchor;
    operation.source.source_ms = here.get_abs( fire );
    operation.members = {{member.token.member_id, false, false, here.get_abs( start )}};
    operation.sweep.candidates = {{here.get_abs( first )}, {here.get_abs( second )}};
    operation.overdue_after_active = 100000;
    operation.advance( to_turn<int>( calendar::turn ), 0 );
    site.operation = operation;
    REQUIRE( site.remember_source( anchor ) );
    overmap_buffer.global_state.migo_towers.sites.push_back( site );
    monster *body = g->place_critter_at( member.type, start );
    REQUIRE( body );
    body->population_identity() = member.token;
    migo_tower::member_intent intent;
    intent.generation = 1;
    intent.home = member.home;
    body->set_migo_intent( intent );
    // Lease/intent is explicit component setup; movement, hazard reads and
    // floor observation below use actual native callers and physical tiles.
    auto *current = overmap_buffer.global_state.migo_towers.find( id );
    REQUIRE( current );
    REQUIRE( migo_tower::owns_intent( *body, migo_population::representation::local ) );
    const auto approach = physical_navigation::read_step( *body, here.get_abs( first ) );
    INFO( approach.reason );
    migo_tower::service_operations( to_turn<int>( calendar::turn ), 0 );
    calendar::turn += 1_turns;
    migo_tower::service_operations( to_turn<int>( calendar::turn ), 1 );
    if( contained ) {
        CHECK( approach.next != physical_navigation::action::none );
        CHECK( current->operation->sweep.candidates[0].state == raid_sweep::disposition::pending );
    } else {
        CHECK( approach.reason == "approach_fire_hazard" );
        CHECK( approach.hazard_revision != 0 );
        CHECK( current->operation->sweep.candidates[0].state == raid_sweep::disposition::hazardous );
        CHECK( current->operation->sweep.last_progress == 0 );
        here.remove_field( fire, field_type_id( "fd_fire" ) ); // declared physical reopening setup
        calendar::turn += 1_turns;
        migo_tower::service_operations( to_turn<int>( calendar::turn ), 1 );
        CHECK( current->operation->sweep.pending() == here.get_abs( first ) );
        CHECK( current->operation->sweep.candidates[0].reconsidered );
        here.add_field( fire, field_type_id( "fd_fire" ), 1 );
        calendar::turn += 1_turns;
        migo_tower::service_operations( to_turn<int>( calendar::turn ), 1 );
        CHECK( current->operation->sweep.candidates[0].state == raid_sweep::disposition::hazardous );
        here.remove_field( fire, field_type_id( "fd_fire" ) );
        calendar::turn += 1_turns;
        migo_tower::service_operations( to_turn<int>( calendar::turn ), 1 );
        CHECK( current->operation->sweep.pending() == here.get_abs( second ) );
    }
    bool arrived_upstairs = false;
    for( int turns = 0; turns < 40 && !arrived_upstairs; ++turns ) {
        calendar::turn += 1_turns;
        process_monsters_and_npcs_turn_for_test();
        migo_tower::service_operations( to_turn<int>( calendar::turn ), 1 );
        REQUIRE_FALSE( body->is_dead_state() );
        arrived_upstairs = std::any_of( current->operation->sweep.candidates.begin(),
        current->operation->sweep.candidates.end(), [&]( const auto &candidate ) {
            return candidate.position == here.get_abs( landing ) &&
                   candidate.state == raid_sweep::disposition::searched;
        } );
    }
    CHECK( arrived_upstairs );
    CHECK( body->pos_abs().z() == 1 );
    CHECK( current->operation->sweep.candidates.size() > 2 );
    CHECK( current->find_source( anchor )->observed_active == contained );
    std::ostringstream saved;
    { JsonOut json( saved ); current->serialize( json ); }
    migo_tower::site_state restored;
    restored.deserialize( json_loader::from_string( saved.str() ) );
    CHECK( restored.operation->sweep.cursor == current->operation->sweep.cursor );
    CHECK( restored.operation->sweep.candidates[0].reconsidered == !contained );
    CHECK( restored.find_source( anchor )->observed_active == contained );
}

TEST_CASE( "migo_operation_scheduler_crosses_remote_overmaps_without_servicing_other_hordes",
           "[migo_tower][native][ownership]" )
{
    // A leased native payload is component setup. The public scheduler owns
    // every physical step, cross-overmap insertion and actual home settlement.
    const auto old_turn = calendar::turn;
    const auto old_populations = overmap_buffer.global_state.migo_populations;
    const auto old_towers = overmap_buffer.global_state.migo_towers;
    const auto first_om = project_to<coords::om>( get_avatar().pos_abs() ).xy() + point( 2, 0 );
    const auto second_om = first_om + point::east;
    overmap &first_owner = overmap_buffer.get( first_om );
    overmap &second_owner = overmap_buffer.get( second_om );
    const tripoint_abs_omt first_cell( project_to<coords::omt>( first_om ) + point( OMAPX - 1, 2 ), 0 );
    const auto second_cell = first_cell + point::east;
    const auto old_first = overmap_buffer.ter_existing( first_cell );
    const auto old_second = overmap_buffer.ter_existing( second_cell );
    const auto start = project_to<coords::ms>( first_cell ) + point( 12, 12 );
    const auto goal = start + point( 18, 0 );
    const auto unrelated_position = start + point( 0, 3 );
    const std::string id = "remote-operation-component";
    std::string unrelated_id;
    on_out_of_scope restore( [&]() {
        for( auto *owner : {&first_owner, &second_owner} ) {
            for( const auto &entry : owner->horde_entities() ) {
                if( entry.second->monster_data &&
                    ( entry.second->monster_data->population_identity().site_id == id ||
                      ( !unrelated_id.empty() &&
                        entry.second->monster_data->handoff_actor_id() == unrelated_id ) ) ) {
                    owner->release_monster_payload( entry.first,
                                                    entry.second->monster_data->handoff_actor_id() );
                }
            }
        }
        overmap_buffer.ter_set( first_cell, old_first );
        overmap_buffer.ter_set( second_cell, old_second );
        overmap_buffer.global_state.migo_populations = old_populations;
        overmap_buffer.global_state.migo_towers = old_towers;
        MAPBUFFER.clear_outside_reality_bubble();
        set_time( old_turn );
    } );
    for( const auto &cell : {first_cell, second_cell} ) {
        overmap_buffer.ter_set( cell, oter_str_id( "field" ) );
        tinymap terrain;
        terrain.load( cell, false );
        for( const auto &p : terrain.points_in_rectangle( tripoint_omt_ms( 0, 0, 0 ),
                tripoint_omt_ms( 23, 23, 0 ) ) ) {
            terrain.ter_set( p, ter_str_id( "t_grass" ) );
            terrain.furn_set( p, furn_str_id::NULL_ID() );
        }
    }
    migo_population::site_record population;
    population.id = id;
    population.anchor = first_cell;
    migo_population::member_record member;
    member.token = {id, "one-owner", 1, first_cell};
    member.type = mtype_id( "mon_mi_go" );
    member.home = start;
    member.owner = migo_population::representation::abstract;
    member.assignment = migo_population::duty::dispatched;
    member.hp = member.type->hp;
    REQUIRE( population.commit_seed( {member}, std::nullopt, "", 0, true, false ) );
    overmap_buffer.global_state.migo_populations.sites.push_back( population );
    migo_tower::site_state site;
    site.site_id = id;
    site.anchor = first_cell;
    migo_tower::operation_record operation;
    operation.generation = 1;
    operation.source.source_omt = second_cell;
    operation.sweep.candidates = {{goal}};
    operation.members = {{member.token.member_id, false, false, start}};
    operation.overdue_after_active = 100000;
    operation.advance( to_turn<int>( calendar::turn ), 0 );
    site.operation = operation;
    REQUIRE( site.remember_source( second_cell ) );
    overmap_buffer.global_state.migo_towers.sites.push_back( site );
    monster body( member.type );
    body.population_identity() = member.token;
    body.set_pos_abs_only( start );
    body.set_moves( 0 );
    migo_tower::member_intent intent;
    intent.generation = 1;
    intent.home = start;
    body.set_migo_intent( intent );
    REQUIRE( first_owner.retain_monster_payload( start, body ) );
    monster unrelated( mtype_id( "mon_writhing_stalker" ) );
    unrelated.ensure_predator_state();
    unrelated_id = unrelated.handoff_actor_id();
    REQUIRE_FALSE( unrelated_id.empty() );
    unrelated.set_pos_abs_only( unrelated_position );
    unrelated.set_dest( unrelated_position + point::east );
    auto *retained = first_owner.retain_monster_payload( unrelated_position, unrelated );
    REQUIRE( retained );
    REQUIRE( retained->is_active() );
    const auto unrelated_processed = retained->last_processed;
    auto *current = overmap_buffer.global_state.migo_towers.find( id );
    REQUIRE( current );
    bool crossed = false;
    bool moved = false;
    for( int turns = 0; turns < 80 && !current->operation->resolved; ++turns ) {
        calendar::turn += 1_turns;
        migo_tower::service_operations( to_turn<int>( calendar::turn ), 1 );
        overmap_buffer.move_hordes();
        int owners = 0;
        for( auto *owner : {&first_owner, &second_owner} ) {
            for( const auto &entry : owner->horde_entities() ) {
                if( !entry.second->monster_data ) {
                    continue;
                }
                const monster &payload = *entry.second->monster_data;
                if( payload.population_identity().site_id == id ) {
                    ++owners;
                    moved |= entry.first != start;
                    crossed |= project_to<coords::om>( entry.first ).xy() == second_om;
                    CHECK( payload.population_identity().epoch == 1 );
                    CHECK( payload.get_hp() == member.hp );
                    const auto position = entry.first;
                    const auto budget = entry.second->moves;
                    overmap_buffer.move_hordes();
                    CHECK( entry.first == position );
                    CHECK( entry.second->moves == budget );
                }
                if( payload.handoff_actor_id() == unrelated_id ) {
                    CHECK( entry.first == unrelated_position );
                    CHECK( entry.second->last_processed == unrelated_processed );
                }
            }
        }
        CHECK( owners == 1 );
    }
    CHECK( moved );
    CHECK( crossed );
    CHECK( current->operation->resolved );
    CHECK( current->operation->members[0].returned );
    const auto *returned = overmap_buffer.global_state.migo_populations.find( id )->find( "one-owner" );
    REQUIRE( returned );
    CHECK( returned->assignment == migo_population::duty::available );
    CHECK( returned->owner == migo_population::representation::abstract );
}

TEST_CASE( "raid_sweep_requires_arrival_and_observation_and_bounds_obstruction",
           "[migo_tower][raid_sweep][serialization]" )
{
    raid_sweep::progress sweep;
    sweep.source = tripoint_abs_omt( 3, 2, 0 );
    sweep.generation = 4;
    const tripoint_abs_ms first( 84, 60, 0 );
    const tripoint_abs_ms second( 85, 60, 1 );
    sweep.candidates = { {first}, {second} };
    sweep.advance( 100, 9000 );
    CHECK( sweep.active_elapsed == 0 );
    sweep.advance( 10000, 0 );
    CHECK( sweep.active_elapsed == 0 );
    sweep.advance( 11799, 1799 );
    CHECK_FALSE( sweep.obstructed() );
    CHECK_FALSE( sweep.arrive( first, false ) );
    CHECK_FALSE( sweep.arrive( second, true ) );
    sweep.advance( 11800, 1 );
    CHECK( sweep.obstructed() );
    REQUIRE( sweep.exclude( raid_sweep::disposition::hazardous, 7 ) );
    CHECK( sweep.last_progress == 0 );
    CHECK_FALSE( sweep.obstructed() ); // a different candidate gets its bounded attempt
    CHECK_FALSE( sweep.reconsider( first, 7 ) );
    REQUIRE( sweep.reconsider( first, 8 ) );
    CHECK( sweep.pending() == first );
    CHECK_FALSE( sweep.obstructed() ); // one material-change retry has a bounded allowance
    CHECK( sweep.last_progress == 0 ); // it still is not physical search progress
    sweep.made_progress(); // caller has actually moved or engaged
    CHECK_FALSE( sweep.obstructed() );
    REQUIRE( sweep.arrive( first, true ) );
    CHECK( sweep.candidates[0].state == raid_sweep::disposition::searched );
    CHECK( sweep.pending() == second );
    CHECK_FALSE( sweep.reconsider( first, 9 ) );
    REQUIRE( sweep.exclude( raid_sweep::disposition::unreachable, 11 ) );
    CHECK_FALSE( sweep.pending() );
    CHECK_FALSE( sweep.arrive( second, true ) );
    CHECK_FALSE( sweep.reconsider( second, 12 ) );
    std::ostringstream saved;
    { JsonOut json( saved ); sweep.serialize( json ); }
    raid_sweep::progress restored;
    restored.deserialize( json_loader::from_string( saved.str() ) );
    CHECK_FALSE( restored.pending() );
    CHECK( restored.generation == 4 );
    CHECK( restored.candidates[0].reconsidered );
    CHECK( restored.candidates[0].geometry_revision == 8 );
    CHECK( restored.candidates[1].state == raid_sweep::disposition::unreachable );
    CHECK( restored.active_elapsed == 1800 );
    restored.advance( 11800, 1800 );
    CHECK( restored.active_elapsed == 1800 );
    restored.advance( 11801, 1800 );
    CHECK( restored.active_elapsed == 1801 ); // never admit more than the real interval

    raid_sweep::progress changed;
    changed.candidates = { {first} };
    REQUIRE( changed.exclude( raid_sweep::disposition::hazardous, 7 ) );
    REQUIRE( changed.reconsider( first, 8 ) );
    REQUIRE( changed.exclude( raid_sweep::disposition::hazardous, 8 ) );
    CHECK_FALSE( changed.reconsider( first, 9 ) );
    CHECK_FALSE( changed.pending() );
}

TEST_CASE( "migo_native_loop_boundary_handoff_yields_released_handle_and_preserves_refusal",
           "[migo_tower][ownership][native]" )
{
    // This isolates the real movement/placement caller. Seeded leases and
    // intent are setup, not signal, admission, full journey or live proof.
    const bool occupied_receiver = GENERATE( false, true );
    clear_avatar();
    clear_map_without_vision();
    const auto old_turn = calendar::turn;
    const auto old_populations = overmap_buffer.global_state.migo_populations;
    const auto old_towers = overmap_buffer.global_state.migo_towers;
    const auto old_bandits = overmap_buffer.global_state.bandit_live_world;
    overmap_buffer.global_state.bandit_live_world = {};
    set_time( calendar::turn_zero + 1400_turns );
    map &here = get_map();
    const auto old_origin = here.get_abs_sub();
    const tripoint_bub_ms edge( here.getmapsize() * SEEX - 1, 60, 0 );
    const auto start = here.get_abs( edge );
    const auto outside = start + point::east;
    const auto home = start + point( 26, 0 );
    const auto anchor = project_to<coords::omt>( home );
    const auto near_cell = project_to<coords::omt>( outside );
    const oter_id old_near_terrain = overmap_buffer.ter_existing( near_cell );
    const oter_id old_home_terrain = overmap_buffer.ter_existing( anchor );
    overmap_buffer.ter_set( near_cell, oter_str_id( "field" ) );
    overmap_buffer.ter_set( anchor, oter_str_id( "field" ) );
    const std::string id = "motor-fixture@" + anchor.to_string_writable();
    const std::string member_id = id + ":returnee";
    const std::string blocker_id = id + ":blocker";
    overmap &owner = overmap_buffer.get( project_to<coords::om>( outside ).xy() );
    on_out_of_scope restore( [&]() {
        clear_avatar();
        clear_map_without_vision();
        for( const auto &entry : owner.horde_entities() ) {
            if( entry.second->monster_data && entry.second->monster_data->population_identity().site_id == id ) {
                owner.release_monster_payload( entry.first,
                                               entry.second->monster_data->population_identity().member_id );
            }
        }
        here.load( old_origin, false );
        get_avatar().setpos( here, tripoint_bub_ms( 60, 60, 0 ) );
        overmap_buffer.ter_set( near_cell, old_near_terrain );
        overmap_buffer.ter_set( anchor, old_home_terrain );
        overmap_buffer.global_state.migo_populations = old_populations;
        overmap_buffer.global_state.migo_towers = old_towers;
        overmap_buffer.global_state.bandit_live_world = old_bandits;
        MAPBUFFER.clear_outside_reality_bubble();
        set_time( old_turn );
    } );
    for( const auto &cell : {near_cell, anchor} ) {
        tinymap terrain;
        terrain.load( cell, false );
        for( const auto &p : terrain.points_in_rectangle( tripoint_omt_ms( 0, 0, 0 ),
                tripoint_omt_ms( 2 * SEEX - 1, 2 * SEEY - 1, 0 ) ) ) {
            terrain.ter_set( p, ter_str_id( "t_grass" ) );
            terrain.furn_set( p, furn_str_id::NULL_ID() );
        }
    }
    get_avatar().setpos( here, tripoint_bub_ms( 1, 1, 0 ) );
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0 );
    migo_population::site_record population;
    population.id = id;
    population.anchor = anchor;
    migo_population::member_record resident;
    resident.token = {id, member_id, 1, anchor};
    resident.type = mtype_id( "mon_mi_go" );
    resident.owner = migo_population::representation::local;
    resident.assignment = migo_population::duty::returning;
    resident.home = home;
    resident.hp = resident.type->hp;
    auto blocker = resident;
    blocker.token.member_id = blocker_id;
    blocker.owner = migo_population::representation::abstract;
    blocker.assignment = migo_population::duty::available;
    REQUIRE( population.commit_seed( {resident, blocker}, std::nullopt, "", 0, true, false ) );
    overmap_buffer.global_state.migo_populations.sites.push_back( population );
    migo_tower::site_state tower;
    tower.site_id = id;
    tower.anchor = anchor;
    migo_tower::operation_record operation;
    operation.generation = 1;
    operation.source.source_omt = project_to<coords::omt>( start );
    operation.members = { {member_id} };
    operation.advance( to_turn<int>( calendar::turn ), 0 );
    tower.operation = operation;
    tower.next_generation = 2;
    overmap_buffer.global_state.migo_towers.sites.push_back( tower );
    monster *body = g->place_critter_at( mtype_id( "mon_mi_go" ), edge );
    REQUIRE( body );
    body->population_identity() = resident.token;
    body->set_moves( 0 );
    migo_tower::member_intent intent;
    intent.generation = 1;
    intent.phase = migo_tower::member_phase::returning;
    intent.home = home;
    body->set_migo_intent( intent );
    // Native contact is an action; simply retaining a visible enemy is not
    // search progress. The returning member must resume home outside its
    // actual immediate melee reach, rather than start chasing that enemy.
    get_avatar().set_str_base( 30 );
    get_avatar().recalc_hp();
    get_avatar().set_all_parts_hp_to_max(); // durable fixture target, no victory assertion
    get_avatar().setpos( here, edge - point::east );
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0 );
    REQUIRE( body->sees( here, get_avatar() ) );
    REQUIRE( body->attitude( &get_avatar() ) == MATT_ATTACK );
    body->set_moves( 100 );
    body->plan();
    CHECK( body->migo_intent()->last_combat_world < to_turn<int>( calendar::turn ) );
    body->move();
    CHECK( body->migo_intent()->last_combat_world == to_turn<int>( calendar::turn ) );
    REQUIRE_FALSE( get_avatar().is_dead_state() );
    CHECK( body->pos_abs() == start );
    get_avatar().setpos( here, edge - point( 5, 0 ) );
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0 );
    REQUIRE( body->sees( here, get_avatar() ) );
    REQUIRE_FALSE( body->is_adjacent( &get_avatar(), true ) );
    REQUIRE( migo_tower::own_capability( *body ).ranged_range == 0 );
    body->set_moves( 100 );
    body->plan();
    CHECK( body->migo_intent()->phase == migo_tower::member_phase::returning );
    CHECK( body->get_dest() == outside );
    // Reset only this transfer fixture's initial budget/target setup. The
    // actual near/far native decisions above retain their own scope.
    get_avatar().setpos( here, tripoint_bub_ms( 1, 1, 0 ) );
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0 );
    body->set_moves( 0 );
    body->unset_dest();
    monster blocking( mtype_id( "mon_mi_go" ) );
    blocking.population_identity() = blocker.token;
    const auto blocker_position = occupied_receiver ? outside : outside + point( 0, 2 );
    blocking.set_pos_abs_only( blocker_position );
    REQUIRE( owner.retain_monster_payload( blocker_position, blocking ) );
    // Actual acid at the departure would affect a released HP0 handle again
    // if monmove continued native field/death processing after the handoff.
    here.add_field( edge, field_type_id( "fd_acid" ), 1 );
    process_monsters_and_npcs_turn_for_test();
    auto *ledger = overmap_buffer.global_state.migo_populations.find( id );
    REQUIRE( ledger );
    auto *record = ledger->find( member_id );
    REQUIRE( record );
    if( occupied_receiver ) {
        CHECK( record->owner == migo_population::representation::local );
        CHECK( record->token.epoch == 1 );
        CHECK( body->pos_abs() == start );
        CHECK( body->get_moves() == body->get_speed() );
        CHECK( body->migo_intent()->last_refused_world == to_turn<int>( calendar::turn ) );
        const int budget = body->get_moves();
        calendar::turn += 1_turns;
        body->process_turn();
        CHECK( body->get_moves() == budget ); // no accumulated refused-step backlog
        REQUIRE( owner.release_monster_payload( outside, blocker_id ) );
        // Setup removes the receiving obstruction; this is not proof of the
        // blocker's autonomous travel, death or population loss.
        blocking.set_pos_abs_only( outside + point( 0, 2 ) );
        REQUIRE( owner.retain_monster_payload( blocking.pos_abs(), blocking ) );
        here.remove_field( edge, field_type_id( "fd_acid" ) );
        process_monsters_and_npcs_turn_for_test();
    }
    CHECK( record->owner == migo_population::representation::abstract );
    CHECK( record->token.epoch == 2 );
    CHECK( record->hp > 0 );
    CHECK( ledger->living_count() == 2 );
    CHECK( std::none_of( here.i_at( edge ).begin(), here.i_at( edge ).end(), []( const item &i ) {
        return i.is_corpse();
    } ) );
    monster *payload = nullptr;
    int copies = 0;
    for( const auto &entry : owner.horde_entities() ) {
        if( entry.second->monster_data && entry.second->monster_data->population_identity().member_id == member_id ) {
            ++copies;
            CHECK( entry.first == outside );
            payload = entry.second->monster_data.get();
        }
    }
    REQUIRE( copies == 1 );
    REQUIRE( payload );
    CHECK( payload->get_hp() > 0 );
    const int budget = payload->get_moves();
    const int hp = payload->get_hp();
    here.load( old_origin + point( 1, 0 ), false );
    get_avatar().setpos( here, tripoint_bub_ms( 1, 1, 0 ) );
    overmap_buffer.spawn_monster( project_to<coords::sm>( outside ) );
    monster *reentered = nullptr;
    for( monster &loaded : g->all_monsters() ) {
        if( loaded.population_identity().member_id == member_id ) {
            REQUIRE_FALSE( reentered );
            reentered = &loaded;
        }
    }
    REQUIRE( reentered );
    CHECK( record->owner == migo_population::representation::local );
    CHECK( reentered->population_identity().epoch == 3 );
    CHECK( reentered->pos_abs() == outside );
    reentered->process_turn();
    CHECK( reentered->get_moves() == budget );
    CHECK( reentered->get_hp() == hp );
    calendar::turn += 1_turns;
    reentered->process_turn();
    CHECK( reentered->get_moves() == budget + reentered->get_speed() );
}
