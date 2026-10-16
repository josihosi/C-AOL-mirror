#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "basecamp.h"
#include "bodypart.h"
#include "bandit_live_world.h"
#include "bandit_live_world_probe.h"
#include "calendar.h"
#include "cached_options.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "cursesdef.h"
#include "debug.h"
#include "do_turn.h"
#include "game.h"
#include "imgui/imgui.h"
#include "item.h"
#include "item_location.h"
#include "json.h"
#include "json_loader.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "npctrade.h"
#include "options.h"
#include "options_helpers.h"
#include "output.h"
#include "overmap.h"
#include "overmapbuffer.h"
#include "player_activity.h"
#include "player_helpers.h"
#include "pocket_type.h"
#include "semantic_surface.h"
#include "trade_ui.h"
#include "type_id.h"
#include "ui_manager.h"
#include "visitable.h"
#include "worldfactory.h"

namespace
{
void copy_json_value( JsonOut &out, const JsonValue &value )
{
    if( value.test_object() ) {
        out.start_object();
        for( const JsonMember &member : value.get_object() ) {
            out.member( member.name() );
            copy_json_value( out, member );
        }
        out.end_object();
    } else if( value.test_array() ) {
        out.start_array();
        for( const JsonValue &entry : value.get_array() ) {
            copy_json_value( out, entry );
        }
        out.end_array();
    } else if( value.test_null() ) {
        out.write_null();
    } else if( value.test_bool() ) {
        out.write( value.get_bool() );
    } else if( value.test_string() ) {
        out.write( value.get_string() );
    } else if( value.test_int() ) {
        out.write( value.get_int64() );
    } else {
        out.write( value.get_float() );
    }
}
#if !defined(TILES)
void diagnostic_environment( const std::string &key, const std::optional<std::string> &value )
{
#if defined(_WIN32)
    REQUIRE( _putenv_s( key.c_str(), value ? value->c_str() : "" ) == 0 );
#else
    REQUIRE( ( value ? setenv( key.c_str(), value->c_str(), 1 ) : unsetenv( key.c_str() ) ) == 0 );
#endif
}
#endif

std::string replace_json_member( const JsonObject &object, const std::string &key,
                                 const std::string &replacement )
{
    object.allow_omitted_members();
    std::ostringstream bytes;
    JsonOut out( bytes );
    out.start_object();
    for( const JsonMember &member : object ) {
        if( member.name() != key ) {
            out.member( member.name() );
            copy_json_value( out, member );
        }
    }
    out.member( key );
    copy_json_value( out, json_loader::from_string( replacement ) );
    out.end_object();
    return bytes.str();
}

template<typename T>
std::string serialized( const T &value )
{
    std::ostringstream bytes;
    JsonOut out( bytes );
    value.serialize( out );
    return bytes.str();
}

class diagnostic_terminal
{
    public:
        diagnostic_terminal() : owns_terminal( !catacurses::stdscr ), owns_imgui( false ) {
            if( owns_terminal ) {
                catacurses::init_interface();
                catacurses::resizeterm();
            }
            owns_imgui = ImGui::GetCurrentContext() == nullptr;
            if( owns_imgui ) {
                ImGui::CreateContext();
            }
            root.emplace();
            root->on_redraw( []( ui_adaptor & ) {} );
            ui_manager::redraw();
        }
        ~diagnostic_terminal() {
            // endwin marks remaining adaptors as shutting down; they then
            // skip deregistration, so retire this reusable fixture's owner first.
            root.reset();
            if( owns_terminal ) {
                catacurses::endwin();
            }
            if( owns_imgui ) {
                ImGui::DestroyContext();
            }
        }
    private:
        bool owns_terminal;
        bool owns_imgui;
        std::optional<ui_adaptor> root;
};
struct diagnostic_boundary : std::exception {};
} // namespace

TEST_CASE( "batch payer reach qualifies nested avatar coins without removing them",
           "[batch_causal_067][batch_gold_067]" )
{
    const std::string geometry = GENERATE( std::string( "eligible" ),
                                           std::string( "outside" ), std::string( "blocked" ) );
    clear_avatar();
    clear_map();
    clear_npcs();
    avatar &you = get_avatar();
    map &here = get_map();
    you.setpos( here, tripoint_bub_ms( 60, 60, 0 ), false );
    npc &payer = spawn_npc( point_bub_ms( 64, 60 ), "test_talker" );
    npc &buyer = spawn_npc( point_bub_ms( 64, 62 ), "test_talker" );
    clear_character( payer );
    clear_character( buyer );
    payer.setpos( here, tripoint_bub_ms( 64, 60, 0 ), false );
    buyer.setpos( here, tripoint_bub_ms( 64, 62, 0 ), false );
    item wallet( itype_id( "wallet" ) );
    REQUIRE( wallet.put_in( item( itype_id( "coin_gold" ) ), pocket_type::CONTAINER ).success() );
    item_location bag = you.i_add( wallet, false );
    REQUIRE( bag );
    item *coin = &bag->only_item();
    const auto uid = coin->uid().get_value();
    if( geometry == "outside" ) {
        payer.setpos( here, tripoint_bub_ms( 100, 60, 0 ), false );
    } else if( geometry == "blocked" ) {
        for( int x = 63; x <= 65; ++x ) {
            for( int y = 59; y <= 61; ++y ) {
                if( x != 64 || y != 60 ) {
                    here.ter_set( tripoint_bub_ms( x, y, 0 ), ter_str_id( "t_concrete_wall" ) );
                }
            }
        }
    }
    const auto rows = encounter_trade_items( payer, buyer, 30, 12, 2 );
    const auto matches = std::count_if( rows.begin(), rows.end(), [uid]( const item_location & row ) {
        return row && row->uid().get_value() == uid;
    } );
    INFO( "payer=" << payer.pos_abs().to_string() << " avatar=" << you.pos_abs().to_string()
          << " geometry=" << geometry << " coin_uid=" << uid << " matching_rows=" << matches );
    CHECK( matches == ( geometry == "eligible" ? 1 : 0 ) );
    CHECK( coin->uid().get_value() == uid );
    CHECK( you.has_item( *coin ) );
#if !defined(TILES)
    diagnostic_terminal terminal;
    semantic_surface_manager manager( "batch-payer-" + geometry );
    semantic_surface_manager_session session( manager );
    semantic_surface_scope world( manager, "world", "World" );
    int panes = 0;
    bool inspected_payer = false;
    manager.set_descriptor_observer( [&]( const auto & screen ) {
        if( screen.kind != "inventory" ) {
            return;
        }
        const bool payer_pane = ++panes > 1;
        if( payer_pane ) {
            inspected_payer = true;
            const bool advertised = std::any_of( screen.valid_actions.begin(), screen.valid_actions.end(),
            [uid]( const auto & action ) {
                return action.id == "inventory.toggle" &&
                       action.stable_id == std::to_string( uid ) && action.enabled;
            } );
            CHECK( advertised == ( geometry == "eligible" ) );
        }
        REQUIRE( manager.submit_request( { manager.run_id(), screen.surface_id, screen.frame_id,
                                           "payer-pane-" + std::to_string( panes ),
                                           payer_pane ? "inventory.cancel" : "trade.switch_pane",
                                           std::nullopt, {} } ) );
    } );
    CHECK_FALSE( npc_trading::trade_to_stash( buyer, payer, you.pos_abs_omt(), 0,
                 "Diagnostic payer sources", 30, 12, nullptr, true, 2 ) );
    CHECK( inspected_payer );
    CHECK( coin->uid().get_value() == uid );
    CHECK( you.has_item( *coin ) );
#endif
}

TEST_CASE( "batch authentic NPC nested activity reference survives its registration boundary",
           "[batch_causal_067][batch_item_owner_067]" )
{
    const bool already_registered = GENERATE( true, false );
    const bool multilevel = GENERATE( false, true );
    const bool legacy_index = GENERATE( false, true );
    CAPTURE( already_registered, multilevel, legacy_index );
    clear_avatar();
    clear_map();
    clear_npcs();
    auto owner = make_shared_fast<npc>();
    owner->normalize();
    owner->setID( g->assign_npc_id(), true );
    owner->spawn_at_precise( get_avatar().pos_abs() + point( 3, 0 ) );
    overmap_buffer.insert_npc( owner );
    const auto id = owner->getID();
    on_out_of_scope remove_owner( [id]() {
        overmap_buffer.remove_npc( id );
    } );
    item pack( itype_id( "backpack" ) );
    if( multilevel ) {
        item inner( itype_id( "purse" ) );
        REQUIRE( inner.put_in( item( itype_id( "tshirt" ) ), pocket_type::CONTAINER ).success() );
        REQUIRE( pack.put_in( inner, pocket_type::CONTAINER ).success() );
    } else {
        REQUIRE( pack.put_in( item( itype_id( "tshirt" ) ), pocket_type::CONTAINER ).success() );
    }
    item_location root = owner->i_add( pack, false );
    REQUIRE( root );
    item_location parent = multilevel ? item_location( root, &root->only_item() ) : root;
    REQUIRE( parent );
    const item_location child( parent, &parent->only_item() );
    const auto parent_uid = parent->uid().get_value();
    const auto child_uid = child->uid().get_value();
    zone_sort_activity_actor actor;
    owner->activity = player_activity( actor );
    const JsonObject npc_json = json_loader::from_string( serialized( *owner ) ).get_object();
    npc_json.allow_omitted_members();
    const JsonObject activity = npc_json.get_object( "activity" );
    activity.allow_omitted_members();
    const JsonObject actor_json = activity.get_object( "actor" );
    actor_json.allow_omitted_members();
    std::string reference = serialized( child );
    if( legacy_index ) {
        // Old-save locations identify every nesting level by its parent index.
        // Strip only reference UIDs; the actual serialized items retain theirs.
        const auto write_legacy = [&]( const auto & self, JsonOut & out,
        const JsonObject & object ) -> void {
            object.allow_omitted_members();
            out.start_object();
            for( const JsonMember &member : object )
            {
                if( member.name() == "uid" ) {
                    continue;
                }
                out.member( member.name() );
                if( member.name() == "parent" ) {
                    self( self, out, member.get_object() );
                } else {
                    copy_json_value( out, member );
                }
            }
            out.end_object();
        };
        std::ostringstream legacy;
        JsonOut out( legacy );
        write_legacy( write_legacy, out, json_loader::from_string( reference ).get_object() );
        reference = legacy.str();
    }
    const std::string actor_data = replace_json_member( actor_json.get_object( "actor_data" ),
                                   "picked_up_stuff", '[' + reference + ']' );
    const std::string activity_actor = replace_json_member( actor_json, "actor_data", actor_data );
    const std::string new_activity = replace_json_member( activity, "actor", activity_actor );
    const std::string bytes = replace_json_member( npc_json, "activity", new_activity );
    std::string messages;
    if( already_registered ) {
        messages = capture_debugmsg_during( [&]() {
            // Same serialized activity/UIDs after ordinary owner registration;
            // avoid reassigning an existing character ID as a second variable.
            owner->activity.deserialize( json_loader::from_string( new_activity ).get_object() );
        } );
    } else {
        overmap_buffer.remove_npc( id );
        REQUIRE_FALSE( overmap_buffer.find_npc( id ) );
        overmap &om = overmap_buffer.get( project_to<coords::om>( owner->pos_abs() ).xy() );
        messages = capture_debugmsg_during( [&]() {
            om.unserialize( json_loader::from_string( "{\"npcs\":[" + bytes + "]}" ).get_object() );
        } );
    }
    const auto loaded = overmap_buffer.find_npc( id );
    REQUIRE( loaded );
    const JsonObject result = json_loader::from_string( serialized( loaded->activity ) ).get_object();
    result.allow_omitted_members();
    const auto retained_actor = result.get_object( "actor" );
    retained_actor.allow_omitted_members();
    const auto retained_data = retained_actor.get_object( "actor_data" );
    retained_data.allow_omitted_members();
    const auto picked = retained_data.get_array( "picked_up_stuff" );
    INFO( "registered_before_deserialize=" << already_registered << " owner=" << id.get_value()
          << " parent_uid=" << parent_uid << " child_uid=" << child_uid
          << " warnings={" << messages << "} retained_children=" << picked.size() );
    CHECK( messages.empty() );
    CHECK( picked.size() == 1 );
    if( picked.size() == 1 ) {
        item_location restored;
        restored.deserialize( picked.get_object( 0 ) );
        REQUIRE( restored );
        CHECK( restored->uid().get_value() == child_uid );
        CHECK( restored.parent_item()->uid().get_value() == parent_uid );
        CHECK( loaded->has_item( *restored ) );
        if( multilevel ) {
            CHECK( restored.parent_item().parent_item()->uid() == root->uid() );
        }
        const item_location restored_parent = restored.parent_item();
        item_location copied = restored;
        CHECK( copied == restored );
        restored.remove_item();
        CHECK_FALSE( copied );
        CHECK( restored_parent );
    }
}

TEST_CASE( "batch merged roster admission distinguishes logical home and native departure",
           "[batch_causal_067][batch_patrol_origin_067]" )
{
    using namespace bandit_live_world;
    const bool native_at_camp = GENERATE( false, true );
    clear_avatar();
    clear_map();
    clear_npcs();
    auto &world = overmap_buffer.global_state.bandit_live_world;
    restore_on_out_of_scope<time_point> restore_clock( calendar::turn );
    // A daylight setup isolates admission from legitimate nighttime optics.
    set_time( calendar::start_of_cataclysm + 9360_minutes );
    restore_on_out_of_scope<world_state> restore_world( world );
    std::ifstream
    input( "tools/openclaw_harness/fixtures/saves/live-debug/r032_camp_defense_from_locker_v1/save/McWilliams/dimension_data.gsav" );
    REQUIRE( input.good() );
    std::string bytes( std::istreambuf_iterator<char> { input }, std::istreambuf_iterator<char> {} );
    const auto root = json_loader::from_string( bytes.substr( bytes.find( '{' ) ) ).get_object();
    root.allow_omitted_members();
    const auto dimension = root.get_object( "overmapbuffer" );
    dimension.allow_omitted_members();
    world.deserialize( dimension.get_object( "bandit_live_world" ) );
    auto site = std::find_if( world.sites.begin(), world.sites.end(), []( const auto & candidate ) {
        return candidate.anchor == tripoint_abs_omt( 164, 39, 0 );
    } );
    REQUIRE( site != world.sites.end() );
    // The test world's unrelated procedural terrain is not the retained
    // fixture's route. Supply an ordinary bounded passable field footprint,
    // without changing either source save or any Native member after birth.
    for( int x = 157; x <= 171; ++x ) {
        for( int y = 28; y <= 46; ++y ) {
            overmap_buffer.ter_set( tripoint_abs_omt( x, y, 0 ), oter_str_id( "field" ).id() );
        }
    }
    // The saved selection is three OMT away, with two intervening tiles.
    overmap_buffer.ter_set( tripoint_abs_omt( 162, 38, 0 ), oter_str_id( "forest" ).id() );
    std::vector<character_id> owned;
    on_out_of_scope remove_members( [&]() {
        for( const auto id : owned ) {
            overmap_buffer.remove_npc( id );
        }
    } );
    for( int index = 0; index < 2; ++index ) {
        auto member = make_shared_fast<npc>();
        member->normalize();
        const character_id id( 4 + index );
        member->setID( id, true );
        member->set_fac( faction_id( "hells_raiders" ) );
        const auto position = native_at_camp ?
                              project_to<coords::ms>( site->anchor ) + point( 12 + 2 * index, 12 ) :
                              index == 0 ? tripoint_abs_ms( 3372, 1007, 0 ) : tripoint_abs_ms( 3374, 1240, 0 );
        member->spawn_at_precise( position );
        overmap_buffer.insert_npc( member );
        owned.push_back( id );
    }
    const int now = 9360;
    const auto target = site->anchor + point( 0, -4 );
    {
        tinymap target_geometry;
        target_geometry.load( target, false );
    }
    REQUIRE( upsert_terrain_opportunity_lead( *site, target,
             classify_structural_bounty_terrain( "road_ns" ), now ) );
    const auto lead_id = make_terrain_opportunity_lead_id( site->site_id, target, "road" );
    const auto *lead = site->intelligence_map.find_lead( lead_id );
    REQUIRE( lead );
    auto plan = plan_structural_bounty_outing( *site, *lead, now );
    CAPTURE( native_at_camp, plan.notes );
    REQUIRE( plan.valid );
    int budget = 32;
    bandit_live_world_probe::session probe(
        bandit_live_world_probe::collection_mode::transition_events );
    const auto geography = live_bandit_structural_route_read_for_test( *site, plan, budget );
    std::ostringstream optical;
    for( const auto &event : probe.result().transition_events ) {
        if( event.transition == "watch_geometry_read" ) {
            optical << event.reason << '\n';
        }
    }
    CAPTURE( optical.str(), probe.result().dropped_transition_events );
    CAPTURE( geography.summary );
    REQUIRE( refine_structural_outing_route( *site, now, geography, plan ) );
    const site_record before = *site;
    REQUIRE( apply_structural_bounty_outing_plan( *site, plan, now ) );
    REQUIRE( site->active_outing.member_ids == owned );
    const bool publication = persist_live_bandit_local_progress_for_test( before, *site );
    prepare_live_bandit_abstract_scout_travel_for_test();
    std::cout << "batch_patrol_origin native_at_camp=" << native_at_camp
              << " publication=" << publication
              << " native_waypoint=" << site->active_outing.actor_route_waypoint
              << " logical_waypoint=" << site->active_outing.waypoint_index << '\n';
    CHECK( site->active_outing.actor_route_waypoint == ( native_at_camp ? 0 : -1 ) );
    const std::string site_id = site->site_id;
    const int generation = site->active_outing.generation;
    for( const int elapsed : {
             5, 30, 60
         } ) {
        set_time( calendar::start_of_cataclysm + time_duration::from_minutes( now + elapsed ) );
        process_overmap_npc_move_for_test();
        const auto *current = world.find_site( site_id );
        REQUIRE( current );
        const auto &outing = current->active_outing;
        std::cout << "batch_patrol_due native_at_camp=" << native_at_camp
                  << " minute=" << now + elapsed << " generation=" << outing.generation
                  << " phase=" << to_string( outing.phase )
                  << " owner=" << to_string( outing.owner )
                  << " wp=" << outing.waypoint_index
                  << " native_wp=" << outing.actor_route_waypoint
                  << " departure=" << outing.actor_departure_observed
                  << " cursor=" << outing.last_advanced_minutes
                  << " observations=" << outing.observations.size()
                  << " report=" << current->current_scout_report.source_generation;
        if( outing.assessment.interruption ) {
            std::cout << " interrupted=" << outing.assessment.interruption->cause;
        }
        for( const auto id : owned ) {
            const auto actor = overmap_buffer.find_npc( id );
            REQUIRE( actor );
            std::cout << " actor=" << id.get_value() << ':' << actor->pos_abs().to_string()
                      << " goal=" << actor->goal.to_string()
                      << " path=" << actor->omt_path.size()
                      << " body_clock=" << to_turn<int>( actor->get_last_updated() );
        }
        std::cout << '\n';
        CHECK( outing.generation == generation );
        if( !native_at_camp ) {
            CHECK_FALSE( outing.actor_departure_observed );
            CHECK( outing.observations.empty() );
            CHECK_FALSE( current->current_scout_report.is_present() );
        }
    }
}

TEST_CASE( "batch nested references refuse genuinely missing or foreign ownership",
           "[batch_causal_067][batch_item_owner_negative_067]" )
{
    const std::string absent = GENERATE( std::string( "owner" ), std::string( "parent" ),
                                         std::string( "child" ), std::string( "foreign_owner" ) );
    clear_avatar();
    clear_map();
    clear_npcs();
    auto owner = make_shared_fast<npc>();
    owner->normalize();
    owner->setID( g->assign_npc_id(), true );
    owner->spawn_at_precise( get_avatar().pos_abs() + point::east );
    overmap_buffer.insert_npc( owner );
    const auto id = owner->getID();
    std::optional<character_id> foreign;
    on_out_of_scope remove( [&]() {
        if( overmap_buffer.find_npc( id ) ) {
            overmap_buffer.remove_npc( id );
        }
        if( foreign ) {
            overmap_buffer.remove_npc( *foreign );
        }
    } );
    item pack( itype_id( "backpack" ) );
    REQUIRE( pack.put_in( item( itype_id( "tshirt" ) ), pocket_type::CONTAINER ).success() );
    item_location parent = owner->i_add( pack, false );
    item_location child( parent, &parent->only_item() );
    std::string bytes = serialized( child );
    if( absent == "owner" ) {
        overmap_buffer.remove_npc( id );
    } else if( absent == "parent" ) {
        parent.remove_item();
    } else if( absent == "child" ) {
        const auto removed_uid = child->uid();
        child.remove_item();
        // A valid same-index replacement must never satisfy the missing UID.
        REQUIRE( parent->put_in( item( itype_id( "tshirt" ) ), pocket_type::CONTAINER ).success() );
        REQUIRE( parent->only_item().uid() != removed_uid );
    } else {
        auto other = make_shared_fast<npc>();
        other->normalize();
        other->setID( g->assign_npc_id(), true );
        other->spawn_at_precise( get_avatar().pos_abs() + point::west );
        overmap_buffer.insert_npc( other );
        foreign = other->getID();
        const auto object = json_loader::from_string( bytes ).get_object();
        object.allow_omitted_members();
        const auto wrong_parent = replace_json_member( object.get_object( "parent" ), "character",
                                  std::to_string( foreign->get_value() ) );
        bytes = replace_json_member( object, "parent", wrong_parent );
    }
    item_location loaded;
    const auto warnings = capture_debugmsg_during( [&]() {
        loaded.deserialize( json_loader::from_string( bytes ).get_object() );
        // Character-owned nested references resolve on use after registration.
        // A genuine missing/foreign owner or UID must still fail at that boundary.
        CHECK_FALSE( loaded );
    } );
    CAPTURE( absent, warnings );
    CHECK_FALSE( loaded );
    CHECK_FALSE( warnings.empty() );
}

#if !defined(TILES)
TEST_CASE( "batch diagnostic terminal teardown retires its redraw owner",
           "[batch_causal_067][batch_terminal_lifetime_067]" )
{
    const bool initializes_terminal = !catacurses::stdscr;
    std::size_t owners_with_terminal = 0;
    {
        diagnostic_terminal terminal;
        owners_with_terminal = ui_adaptor::ui_stack_size();
        REQUIRE( owners_with_terminal > 0 );
    }
    CAPTURE( initializes_terminal );
    CHECK( ui_adaptor::ui_stack_size() + 1 == owners_with_terminal );
}

TEST_CASE( "batch native epilogue Close changes each concrete owner despite equal kind",
           "[batch_causal_067][batch_epilogue_067]" )
{
    clear_avatar();
    clear_map();
    clear_npcs();
    npc &first = spawn_npc( get_avatar().pos_bub().xy() + point::east, "test_talker" );
    npc &second = spawn_npc( get_avatar().pos_bub().xy() + point::west, "test_talker" );
    restore_on_out_of_scope<bool> restore_test_mode( test_mode );
    test_mode = false;
    diagnostic_terminal terminal;
    semantic_surface_manager manager( "batch-epilogues" );
    semantic_surface_manager_session session( manager );
    semantic_surface_scope parent( manager, "test_exit", "End of epilogue chain" );
    std::vector<semantic_surface_descriptor> screens;
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&]( const auto & receipt ) {
        receipts.push_back( receipt );
    } );
    manager.set_descriptor_observer( [&]( const auto & screen ) {
        if( screen.kind == "scrollable_text" ) {
            screens.push_back( screen );
            if( screens.size() == 2 ) {
                const auto &old = screens.front();
                REQUIRE( manager.submit_request( { manager.run_id(), old.surface_id, old.frame_id,
                                                   "stale-first-epilogue", "scrollable_text.close",
                                                   std::nullopt, {} } ) );
            }
            REQUIRE( manager.submit_request( { manager.run_id(), screen.surface_id, screen.frame_id,
                                               "close-" + std::to_string( screens.size() ),
                                               "scrollable_text.close", std::nullopt, {} } ) );
        }
    } );
    const auto window = []() {
        return catacurses::newwin( 24, 80, point::zero );
    };
    scrollable_text( window, first.disp_name(), first.get_epilogue() );
    scrollable_text( window, second.disp_name(), second.get_epilogue() );
    REQUIRE( screens.size() == 2 );
    REQUIRE( receipts.size() == 3 );
    CHECK( screens[0].kind == screens[1].kind );
    CHECK( screens[0].surface_id != screens[1].surface_id );
    CHECK( receipts[0].accepted );
    CHECK_FALSE( receipts[1].accepted );
    CHECK( receipts[1].rejection_reason == "wrong_surface" );
    CHECK( receipts[2].accepted );
    CHECK( manager.top()->kind == "test_exit" );
}

TEST_CASE( "batch death camera retains its owner across timeout turns",
           "[batch_causal_067][batch_death_owner_067]" )
{
    // Retirement writes a marker before the first successor. Each variant is
    // run in its own ordinary test world/process, never a revived test corpse.
    const char *selected_route = std::getenv( "BATCH_DIAG_DEATH_ROUTE" );
    const std::string route = selected_route ? selected_route : "delayed_old";
    clear_avatar();
    clear_map();
    clear_npcs();
    override_option autosave( "AUTOSAVE", "false" );
    override_option llm( "LLM_INTENT_ENABLE", "false" );
    REQUIRE( world_generator->active_world );
    const auto world = world_generator->active_world->world_name;
    REQUIRE( g->save() );
    REQUIRE( turn_handler::cleanup_at_end() );
    world_generator->set_active_world( nullptr );
    REQUIRE( g->load( world ) );
    const std::string run = "batch-death-" + route;
    std::map<std::string, std::optional<std::string>> previous;
    for( const std::string key : {
             "OPENCLAW_HARNESS_RUN_ID", "OPENCLAW_HARNESS_SEMANTIC_RUN_ID"
         } ) {
        const char *value = std::getenv( key.c_str() );
        previous[key] = value ? std::optional<std::string>( value ) : std::nullopt;
        diagnostic_environment( key, run );
    }
    on_out_of_scope restore_environment( [&]() {
        for( const auto &entry : previous ) {
            diagnostic_environment( entry.first, entry.second );
        }
    } );
    diagnostic_terminal terminal;
    restore_on_out_of_scope<bool> restore_test_mode( test_mode );
    restore_on_out_of_scope<quit_status> restore_quit( g->uquit );
    restore_on_out_of_scope<bool> restore_new_game( g->new_game );
    test_mode = false;
    g->new_game = false;
    get_avatar().set_part_hp_cur( bodypart_id( "torso" ), 0 );
    get_avatar().set_moves( 0 );
    g->uquit = QUIT_WATCH;
    semantic_surface_manager &manager = openclaw_harness_semantic_surface_manager();
    const std::vector<semantic_surface_descriptor> baseline_owners = manager.stack();
    std::vector<semantic_surface_descriptor> cameras;
    bool close_requested = false;
    bool successor_observed = false;
    on_out_of_scope restore_observers( [&]() {
        manager.set_descriptor_observer( {} );
    } );
    manager.set_descriptor_observer( [&]( const semantic_surface_descriptor & screen ) {
        if( screen.kind != "death_camera" ) {
            if( close_requested && g->uquit == QUIT_DIED ) {
                successor_observed = true;
                CHECK( screen.surface_id != cameras.front().surface_id );
                const std::vector<semantic_surface_descriptor> owners = manager.stack();
                CHECK( std::none_of( owners.begin(), owners.end(), []( const semantic_surface_descriptor & owner ) {
                    return owner.kind == "death_camera";
                } ) );
                // The native close has set QUIT_DIED and retired its camera.
                // Stop before the unrelated death-screen and save flow.
                throw diagnostic_boundary();
            }
            return;
        }
        cameras.push_back( screen );
        if( route == "immediate" ) {
            close_requested = true;
            const auto &selected = cameras.front();
            REQUIRE( manager.submit_request( { run, selected.surface_id, selected.frame_id,
                                               "batch-delayed-close", "death_camera.close", std::nullopt, {} } ) );
        }
    } );
    try {
        if( route == "immediate" ) {
            g->do_turn();
        } else {
            // Let the native death camera return through a real TIMEOUT turn.
            // The next close uses either the original advertised frame or
            // the manager's current frame; both describe the same owner.
            const time_point timeout_turn = calendar::turn;
            g->do_turn();
            CHECK( calendar::turn > timeout_turn );
            CHECK( g->uquit == QUIT_WATCH );
            REQUIRE( cameras.size() == 1 );
            REQUIRE( manager.top() );
            CHECK( manager.top()->surface_id == cameras.front().surface_id );
            CHECK( manager.top()->frame_id == cameras.front().frame_id );
            const semantic_surface_descriptor &selected = route == "delayed_old" ?
                    cameras.front() : *manager.top();
            close_requested = true;
            REQUIRE( manager.submit_request( { run, selected.surface_id, selected.frame_id,
                                               "batch-delayed-close", "death_camera.close", std::nullopt, {} } ) );
            g->do_turn();
        }
    } catch( const diagnostic_boundary & ) {
    }
    CHECK( successor_observed );
    CHECK( g->uquit == QUIT_DIED );
    const std::vector<semantic_surface_descriptor> remaining_owners = manager.stack();
    CHECK( std::none_of( remaining_owners.begin(), remaining_owners.end(),
    []( const semantic_surface_descriptor & owner ) {
        return owner.kind == "death_camera";
    } ) );
    REQUIRE( remaining_owners.size() >= baseline_owners.size() );
    for( std::size_t i = 0; i < baseline_owners.size(); ++i ) {
        CHECK( remaining_owners[i].surface_id == baseline_owners[i].surface_id );
    }
    // The boundary intentionally stops inside the first real successor's
    // constructor, after push has appended it but before its scope can own the
    // pop. Unwind only frames introduced by this diagnostic route; the close
    // already proved the camera owner retired through native QUIT_DIED.
    manager.set_descriptor_observer( {} );
    while( manager.stack().size() > baseline_owners.size() ) {
        const std::vector<semantic_surface_descriptor> current_owners = manager.stack();
        REQUIRE( manager.pop( current_owners.back().surface_id ) );
    }
    const std::vector<semantic_surface_descriptor> cleaned_owners = manager.stack();
    REQUIRE( cleaned_owners.size() == baseline_owners.size() );
    for( std::size_t i = 0; i < baseline_owners.size(); ++i ) {
        CHECK( cleaned_owners[i].surface_id == baseline_owners[i].surface_id );
    }
    CHECK( cameras.size() == 1 );
    std::cout << "batch_death route=" << route << " cameras=" << cameras.size()
              << " successor_observed=" << successor_observed
              << " quit=" << static_cast<int>( g->uquit ) << '\n';
}
#endif

TEST_CASE( "batch worker assignment edits the advertised worker on first selection",
           "[batch_causal_067][camp_assignment_identity_067]" )
{
#if !defined(TILES)
    const std::string roster_change = GENERATE( std::string( "unchanged" ),
                                      std::string( "reordered" ), std::string( "removed" ) );
    clear_avatar();
    clear_map_without_vision();
    clear_npcs();
    diagnostic_terminal terminal;
    avatar &you = get_avatar();
    map &here = get_map();
    you.setpos( here, tripoint_bub_ms{ 60, 60, 0 }, false );
    const tripoint_abs_ms camp_abs = here.get_abs( tripoint_bub_ms{ 60, 60, 0 } );
    basecamp camp( "Identity Diagnostic Camp", project_to<coords::omt>( camp_abs ) );
    npc &first = spawn_npc( tripoint_bub_ms{ 61, 60, 0 }.xy(), "test_talker" );
    npc &second = spawn_npc( tripoint_bub_ms{ 62, 60, 0 }.xy(), "test_talker" );
    npc &third = spawn_npc( tripoint_bub_ms{ 63, 60, 0 }.xy(), "test_talker" );
    static const activity_id ACT_CAMP_PATROL( "ACT_CAMP_PATROL" );
    camp.add_assignee( first.getID() );
    camp.add_assignee( second.getID() );
    camp.add_assignee( third.getID() );
    const std::vector<npc_ptr> workers = camp.get_npcs_assigned();
    REQUIRE( workers.size() == 3 );

    // Choose the second row in the actual native roster, regardless of the
    // roster's pointer-based ordering.  The job editor and numeric prompt
    // remain the real uilist/input surfaces owned by the game.
    npc *untouched_worker = workers.front().get();
    npc *requested_worker = workers[1].get();
    npc *successor_worker = workers.back().get();
    REQUIRE( untouched_worker->job.set_task_priority( ACT_CAMP_PATROL, 3 ) );
    REQUIRE( requested_worker->job.set_task_priority( ACT_CAMP_PATROL, 0 ) );
    REQUIRE( successor_worker->job.set_task_priority( ACT_CAMP_PATROL, 5 ) );
    const int untouched_priority = untouched_worker->job.get_priority_of_job( ACT_CAMP_PATROL );
    const int requested_priority = requested_worker->job.get_priority_of_job( ACT_CAMP_PATROL );
    REQUIRE( untouched_priority == 3 );
    REQUIRE( requested_priority == 0 );

    restore_on_out_of_scope<bool> restore_test_mode( test_mode );
    test_mode = false;
    semantic_surface_manager manager( "camp-worker-control-" + roster_change );
    semantic_surface_manager_session session( manager );
    semantic_surface_scope world( manager, "world", "World" );
    int stage = 0;
    bool edited_requested_worker = false;
    bool selected_set_all = false;
    bool submitted_priority = false;
    bool closed = false;
    bool stale_worker_rejected = false;
    manager.set_receipt_observer( [&]( const semantic_action_receipt & receipt ) {
        if( receipt.request_id == "edit-second-worker" ) {
            stale_worker_rejected = !receipt.accepted && receipt.rejection_reason == "stale_worker";
            if( stale_worker_rejected ) {
                REQUIRE( manager.top() );
                const auto &owner = *manager.top();
                REQUIRE( manager.submit_request( { manager.run_id(), owner.surface_id,
                                                   owner.frame_id, "close-after-stale-worker",
                                                   "camp_assignment.close", std::nullopt, {} } ) );
                stage = 6;
                closed = true;
            }
        }
    } );
    manager.set_descriptor_observer( [&]( const semantic_surface_descriptor & screen ) {
        if( screen.kind == "basecamp_worker_assignment" ) {
            if( stage == 0 ) {
                const std::string stable_id = string_format( "character:%d",
                                              requested_worker->getID().get_value() );
                const auto action = std::find_if( screen.valid_actions.begin(),
                                                  screen.valid_actions.end(),
                [&]( const semantic_action_descriptor & candidate ) {
                    return candidate.id == "camp_assignment.edit_worker_jobs" &&
                           candidate.stable_id == stable_id && candidate.enabled;
                } );
                REQUIRE( action != screen.valid_actions.end() );
                REQUIRE( manager.submit_request( { manager.run_id(), screen.surface_id,
                                                   screen.frame_id, "edit-second-worker",
                                                   action->id, action->stable_id, {} } ) );
                if( roster_change == "reordered" ) {
                    // Removing the preceding row shifts the requested worker
                    // from index 1 to index 0 while another worker takes index 1.
                    camp.remove_assignee( untouched_worker->getID() );
                } else if( roster_change == "removed" ) {
                    camp.remove_assignee( requested_worker->getID() );
                }
                stage = 1;
            }
            if( stage == 4 ) {
                CHECK( requested_worker->job.get_priority_of_job( ACT_CAMP_PATROL ) == 8 );
                CHECK( untouched_worker->job.get_priority_of_job( ACT_CAMP_PATROL ) ==
                       untouched_priority );
                CHECK( successor_worker->job.get_priority_of_job( ACT_CAMP_PATROL ) == 5 );
                edited_requested_worker = true;
                const auto action = std::find_if( screen.valid_actions.begin(),
                                                  screen.valid_actions.end(),
                []( const semantic_action_descriptor & candidate ) {
                    return candidate.id == "camp_assignment.close" && candidate.enabled;
                } );
                REQUIRE( action != screen.valid_actions.end() );
                REQUIRE( manager.submit_request( { manager.run_id(), screen.surface_id,
                                                   screen.frame_id, "close-worker-assignment",
                                                   action->id, std::nullopt, {} } ) );
                stage = 5;
                closed = true;
            }
            return;
        }
        if( screen.kind == "menu" && stage == 1 && roster_change != "removed" ) {
            const auto action = std::find_if( screen.valid_actions.begin(),
                                              screen.valid_actions.end(),
            []( const semantic_action_descriptor & candidate ) {
                return candidate.id == "menu.choose" && candidate.enabled &&
                       candidate.label.find( "Set all priorities" ) != std::string::npos;
            } );
            REQUIRE( action != screen.valid_actions.end() );
            REQUIRE( manager.submit_request( { manager.run_id(), screen.surface_id,
                                               screen.frame_id, "set-second-worker-priorities",
                                               action->id, action->stable_id, {} } ) );
            selected_set_all = true;
            stage = 2;
            return;
        }
        if( screen.kind == "menu" && stage == 3 ) {
            const auto action = std::find_if( screen.valid_actions.begin(),
            screen.valid_actions.end(), []( const semantic_action_descriptor & candidate ) {
                return candidate.id == "menu.cancel" && candidate.enabled;
            } );
            REQUIRE( action != screen.valid_actions.end() );
            REQUIRE( manager.submit_request( { manager.run_id(), screen.surface_id,
                                               screen.frame_id, "return-to-worker-roster",
                                               action->id, std::nullopt, {} } ) );
            stage = 4;
            return;
        }
        if( screen.kind == "string_prompt" && stage == 2 ) {
            REQUIRE( manager.submit_request( { manager.run_id(), screen.surface_id,
                                               screen.frame_id, "set-priority-eight",
                                               "prompt.submit", std::nullopt,
            { { "text", "8" } } } ) );
            submitted_priority = true;
            stage = 3;
            return;
        }
    } );
    on_out_of_scope clear_descriptor_observer( [&]() {
        manager.set_descriptor_observer( {} );
        manager.set_receipt_observer( {} );
    } );

    camp.job_assignment_ui();

    CHECK( stage == ( roster_change == "removed" ? 6 : 5 ) );
    CHECK( selected_set_all == ( roster_change != "removed" ) );
    CHECK( submitted_priority == ( roster_change != "removed" ) );
    CHECK( edited_requested_worker == ( roster_change != "removed" ) );
    CHECK( closed );
    CHECK( stale_worker_rejected == ( roster_change == "removed" ) );
    CHECK( requested_worker->job.get_priority_of_job( ACT_CAMP_PATROL ) ==
           ( roster_change == "removed" ? requested_priority : 8 ) );
    CHECK( untouched_worker->job.get_priority_of_job( ACT_CAMP_PATROL ) == untouched_priority );
    CHECK( successor_worker->job.get_priority_of_job( ACT_CAMP_PATROL ) == 5 );
#endif
}


TEST_CASE( "batch repeated options initialization retains grouped interface definitions",
           "[batch_causal_067][batch_options_registry_067]" )
{
    // A local manager isolates the registry contract from shared test state.
    options_manager local = get_options();
    REQUIRE( local.has_option( "ITEM_HEALTH" ) );
    const bool region_before = local.has_option( "DEFAULT_REGION" );
    const std::string messages = capture_debugmsg_during( [&]() {
        local.init();
    } );
    INFO( "initialization warnings=" << messages.size()
          << " duplicate_group=" << ( messages.find( "already exists" ) != std::string::npos ) );
    std::cout << "batch_options_reinit item_before=1 item_after=" << local.has_option( "ITEM_HEALTH" )
              << " region_before=" << region_before
              << " region_after=" << local.has_option( "DEFAULT_REGION" )
              << " duplicate_group=" << ( messages.find( "already exists" ) != std::string::npos ) << '\n';
    CHECK( local.has_option( "ITEM_HEALTH" ) );
}

TEST_CASE( "batch invisible monster collision preserves melee sight admission",
           "[batch_causal_067][batch_monster_sight_guard_067]" )
{
    clear_map();
    clear_avatar();
    const time_point previous_turn = calendar::turn;
    on_out_of_scope restore_turn( [previous_turn]() {
        calendar::turn = previous_turn;
    } );
    calendar::turn = calendar::turn_zero + 12_hours;
    map &here = get_map();
    get_avatar().setpos( here, tripoint_bub_ms( 50, 50, 0 ) );
    monster &attacker = spawn_test_monster( "mon_zombie_scientist",
                                            tripoint_bub_ms( 65, 65, 0 ) );
    const bool invisible = GENERATE( true, false );
    monster &target = spawn_test_monster( invisible ? "mon_star_vampire" : "mon_dog",
                                          tripoint_bub_ms( 66, 65, 0 ) );
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0 );
    CAPTURE( invisible, attacker.pos_abs(), target.pos_abs() );
    REQUIRE( attacker.pos_abs().z() == target.pos_abs().z() );
    REQUIRE( attacker.is_adjacent( &target, true ) );
    REQUIRE( attacker.attitude_to( target ) == Creature::Attitude::HOSTILE );
    REQUIRE( attacker.sees( here, target ) == !invisible );
    const int original_hp = target.get_hp();
    for( int attempt = 0; attempt < 2; ++attempt ) {
        attacker.set_moves( 200 );
        bool attacked = false;
        const std::string warning = capture_debugmsg_during( [&]() {
            attacked = attacker.attack_at( target.pos_bub() );
        } );
        CAPTURE( attempt, warning );
        CHECK( attacked == !invisible );
        CHECK( ( attacker.get_moves() < 200 ) == !invisible );
        CHECK( warning.empty() );
        if( invisible ) {
            CHECK( target.get_hp() == original_hp );
        }
    }
}

TEST_CASE( "batch unseen occupied movement step recovers after ordinary target departure",
           "[batch_causal_067][batch_monster_sight_guard_067]" )
{
    clear_map();
    clear_avatar();
    map &here = get_map();
    get_avatar().setpos( here, tripoint_bub_ms( 50, 50, 0 ) );
    monster &attacker = spawn_test_monster( "mon_zombie_scientist",
                                            tripoint_bub_ms( 65, 65, 0 ) );
    monster &target = spawn_test_monster( "mon_star_vampire",
                                          tripoint_bub_ms( 66, 65, 0 ) );
    const tripoint_bub_ms occupied = target.pos_bub();
    REQUIRE_FALSE( attacker.sees( here, target ) );
    const int hp_before = target.get_hp();
    attacker.set_dest( target.pos_abs() );
    attacker.set_moves( 200 );
    const std::string warning = capture_debugmsg_during( [&]() {
        attacker.move();
    } );
    CAPTURE( warning, attacker.pos_abs(), target.pos_abs(), attacker.get_moves() );
    CHECK( warning.empty() );
    CHECK( attacker.get_moves() < 200 );
    CHECK( target.get_hp() == hp_before );
    CHECK_FALSE( attacker.has_effect( efftype_id( "stumbled_into_invisible" ) ) );
    CHECK_FALSE( here.has_field_at( occupied, field_type_str_id( "fd_last_known" ) ) );
    REQUIRE( target.move_to( target.pos_bub() + tripoint::north ) );
    REQUIRE( target.move_to( target.pos_bub() + tripoint::north ) );
    REQUIRE( rl_dist( attacker.pos_abs(), target.pos_abs() ) > 1 );
    const tripoint_bub_ms before = attacker.pos_bub();
    const tripoint_bub_ms goal = before + tripoint::east;
    REQUIRE( here.passable( goal ) );
    attacker.set_dest( here.get_abs( goal ) );
    attacker.set_moves( 200 );
    const std::string after = capture_debugmsg_during( [&]() {
        attacker.move();
    } );
    CHECK( after.empty() );
    CHECK( attacker.pos_bub() == goal );
    CHECK( target.get_hp() == hp_before );
    CHECK_FALSE( here.has_field_at( occupied, field_type_str_id( "fd_last_known" ) ) );
}

TEST_CASE( "batch monster sight admission preserves attack restrictions and hallucinations",
           "[batch_causal_067][batch_monster_sight_guard_067]" )
{
    clear_map();
    clear_avatar();
    map &here = get_map();
    get_avatar().setpos( here, tripoint_bub_ms( 50, 50, 0 ) );
    monster &attacker = spawn_test_monster( "mon_zombie_scientist",
                                            tripoint_bub_ms( 65, 65, 0 ) );
    const std::string mode = GENERATE( "pacifist", "no_melee", "attackmon",
                                       "unseen_attackmon", "hallucination" );
    monster &target = spawn_test_monster(
                          mode == "unseen_attackmon" || mode == "hallucination" ?
                          "mon_star_vampire" : "mon_dog", tripoint_bub_ms( 66, 65, 0 ) );
    const mtype *const original_type = attacker.type;
    mtype restricted_type = *original_type;
    on_out_of_scope restore_type( [&]() {
        attacker.type = original_type;
    } );
    attacker.type = &restricted_type;
    if( mode == "pacifist" ) {
        restricted_type.set_flag( mon_flag_str_id( "PACIFIST" ) );
    } else if( mode == "no_melee" ) {
        restricted_type.melee_dice = 0;
    } else if( mode == "attackmon" || mode == "unseen_attackmon" ) {
        restricted_type.set_flag( mon_flag_str_id( "ATTACKMON" ) );
        target.faction = attacker.faction;
        REQUIRE( attacker.attitude_to( target ) != Creature::Attitude::HOSTILE );
    } else {
        attacker.hallucination = true;
        target.hallucination = true;
    }
    if( mode == "unseen_attackmon" || mode == "hallucination" ) {
        REQUIRE_FALSE( attacker.sees( here, target ) );
    }
    const bool expected = mode == "attackmon" || mode == "hallucination";
    const int hp_before = target.get_hp();
    bool attacked = false;
    const std::string warning = capture_debugmsg_during( [&]() {
        attacked = attacker.attack_at( target.pos_bub() );
    } );
    CAPTURE( mode, warning );
    CHECK( warning.empty() );
    CHECK( attacked == expected );
    if( !expected ) {
        CHECK( target.get_hp() == hp_before );
    }
    CHECK_FALSE( attacker.has_effect( efftype_id( "stumbled_into_invisible" ) ) );
    CHECK_FALSE( here.has_field_at( target.pos_bub(), field_type_str_id( "fd_last_known" ) ) );
}

TEST_CASE( "batch unseen monster occupancy does not cross walls or floors",
           "[batch_causal_067][batch_monster_sight_guard_067]" )
{
    clear_map();
    clear_avatar();
    map &here = get_map();
    get_avatar().setpos( here, tripoint_bub_ms( 50, 50, 0 ) );
    const bool upper_floor = GENERATE( false, true );
    const tripoint_bub_ms start( 65, 65, 0 );
    const tripoint_bub_ms destination = upper_floor ?
                                        tripoint_bub_ms( 65, 65, 1 ) : tripoint_bub_ms( 67, 65, 0 );
    if( upper_floor ) {
        here.ter_set( destination, ter_id( "t_floor" ) );
    } else {
        here.ter_set( tripoint_bub_ms( 66, 65, 0 ), ter_id( "t_rock" ) );
    }
    monster &attacker = spawn_test_monster( "mon_zombie_scientist", start );
    monster &target = spawn_test_monster( "mon_dog", destination );
    here.invalidate_map_cache( 0 );
    here.invalidate_map_cache( destination.z() );
    here.build_map_cache( 0 );
    here.build_map_cache( destination.z() );
    REQUIRE_FALSE( attacker.sees( here, target ) );
    const int hp_before = target.get_hp();
    bool attacked = false;
    const std::string warning = capture_debugmsg_during( [&]() {
        attacked = attacker.attack_at( destination );
        attacker.set_dest( target.pos_abs() );
        attacker.set_moves( 200 );
        attacker.move();
    } );
    CAPTURE( upper_floor, warning, attacker.pos_abs(), target.pos_abs() );
    CHECK( warning.empty() );
    CHECK_FALSE( attacked );
    CHECK( target.get_hp() == hp_before );
    CHECK( attacker.pos_bub().z() == start.z() );
    CHECK( attacker.pos_bub() != destination );
    CHECK_FALSE( attacker.has_effect( efftype_id( "stumbled_into_invisible" ) ) );
    CHECK_FALSE( here.has_field_at( destination, field_type_str_id( "fd_last_known" ) ) );
}
