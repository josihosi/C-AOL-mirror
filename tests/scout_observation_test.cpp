#include <algorithm>
#include <cmath>
#include <memory>
#include "lightmap.h"
#include "level_cache.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include "cata_catch.h"
#include "cata_path.h"
#include "json.h"
#include "json_loader.h"
#include "mapbuffer.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "string_formatter.h"
#include "submap.h"
#include "scout_observation.h"
#include "avatar.h"
#include "bandit_live_world.h"
#include "monster.h"
#include "mtype.h"
#include "faction.h"
#include "field_type.h"
#include "calendar.h"
#include "cata_scope_helpers.h"
#include "effect.h"
#include "game.h"
#include "map.h"
#include "line.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "npc.h"
#include "options_helpers.h"
#include "player_helpers.h"
#include "weather.h"
#include "vehicle.h"
#include "units.h"
#include "item.h"
#include "memory_fast.h"
#include "pocket_type.h"
#include "weather_type.h"
#include "worldfactory.h"
#include "zzip.h"

std::vector<tripoint_abs_omt> live_bandit_member_route_to_for_test(
    const npc &member, const bandit_live_world::site_record &site,
    const tripoint_abs_omt &destination );

TEST_CASE( "stationary binocular optics preserves physical visibility at three OMT",
           "[binocular_056][visibility]" )
{
    const auto saved_turn = calendar::turn;
    const auto saved_position = get_avatar().pos_abs();
    on_out_of_scope restore( [&]() {
        get_avatar().setpos( saved_position, false );
        set_time( saved_turn );
    } );
    clear_avatar();
    clear_npcs();
    clear_map_with_vision( -1, 4, false );
    set_time_to_day();
    scoped_weather_override weather( weather_type_id( "clear" ) );
    map &here = get_map();
    npc observer;
    observer.normalize();
    observer.set_fake( true );
    observer.recalc_sight_limits();
    observer.setpos( here, tripoint_bub_ms( 12, 66, 0 ), false );
    avatar &target = get_avatar();
    target.setpos( here, tripoint_bub_ms( 84, 66, 0 ), false );
    here.build_map_cache( 0 );
    REQUIRE( rl_dist( observer.pos_abs_omt().xy(), target.pos_abs_omt().xy() ) == 3 );
    CHECK_FALSE( observer.sees_without_clairvoyance( here, target ) );
    const auto first_read = scout_observation::read_character( observer, target );
    CAPTURE( first_read.reason, first_read.known, here.is_outside( target.pos_bub( here ) ),
             here.ambient_light_at( target.pos_bub( here ) ), observer.sight_range( 100.0f, 100.0f ),
             observer.is_blind(), observer.in_sleep_state(), target.is_invisible() );
    REQUIRE( first_read.visible );
    npc person;
    person.normalize();
    person.set_fake( true );
    person.recalc_sight_limits();
    person.setpos( here, tripoint_bub_ms( 84, 67, 0 ), false );
    CHECK( scout_observation::read_character( observer, person ).visible );

    SECTION( "existing exterior actors remain scout visible outside player vision" ) {
        // Establish the player's ordinary view before the read; do not reveal cells
        // or write seen caches to make the binocular result succeed.
        target.setpos( here, tripoint_bub_ms( 12, 12, 0 ), false );
        here.build_map_cache( 0 );
        here.update_visibility_cache( 0 );
        REQUIRE_FALSE( target.sees( here, person ) );
        const auto &cache = here.get_cache_ref( 0 );
        REQUIRE( cache.seen_cache[person.pos_bub( here ).xy()] == 0.0f );
        const auto before_seen = cache.seen_cache;
        const auto before_camera = cache.camera_cache;
        const auto before_memory_terrain = cache.map_memory_cache_ter;
        const auto before_memory_decoration = cache.map_memory_cache_dec;
        const auto player_position = target.pos_abs();
        const auto existing = scout_observation::read_character( observer, person );
        CHECK( existing.known );
        CHECK( existing.visible );
        bool unchanged_seen = true;
        bool unchanged_camera = true;
        for( size_t x = 0; x < decltype( before_seen )::size_x; ++x ) {
            unchanged_seen = unchanged_seen && cache.seen_cache[x] == before_seen[x];
            unchanged_camera = unchanged_camera && cache.camera_cache[x] == before_camera[x];
        }
        CHECK( unchanged_seen );
        CHECK( unchanged_camera );
        CHECK( cache.map_memory_cache_ter == before_memory_terrain );
        CHECK( cache.map_memory_cache_dec == before_memory_decoration );
        CHECK( target.pos_abs() == player_position );
        CHECK_FALSE( target.sees( here, person ) );
    }
    SECTION( "lookout and intervening forest are ignored but target entry stays physical" ) {
        scout_observation::site_reader reader;
        const std::vector<tripoint_abs_omt> footprint = { person.pos_abs_omt() };
        REQUIRE( reader.read_character( observer, person, footprint ).visible );
        REQUIRE( reader.read_signal( observer, observer.pos_abs(), person.pos_abs(), footprint ).visible );
        here.ter_set( observer.pos_bub( here ), ter_str_id( "t_tree" ) );
        for( int x = 24; x < 72; ++x ) {
            here.ter_set( tripoint_bub_ms( x, 66, 0 ), ter_str_id( "t_tree" ) );
            here.ter_set( tripoint_bub_ms( x, 67, 0 ), ter_str_id( "t_tree" ) );
        }
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        scout_observation::site_reader forest;
        CHECK( forest.read_character( observer, person, footprint ).visible );
        CHECK( forest.read_signal( observer, observer.pos_abs(), person.pos_abs(), footprint ).visible );
        here.ter_set( tripoint_bub_ms( 72, 67, 0 ), ter_str_id( "t_tree" ) );
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        scout_observation::site_reader target_blocked;
        CHECK_FALSE( target_blocked.read_character( observer, person, footprint ).visible );
        CHECK_FALSE( target_blocked.read_signal( observer, observer.pos_abs(), person.pos_abs(),
                     footprint ).visible );
    }
    SECTION( "an unavailable lookout and corridor do not hide an existing target" ) {
        observer.setpos( here.get_abs( tripoint_bub_ms( 156, 66, 0 ) ), false );
        REQUIRE_FALSE( here.inbounds( observer.pos_abs() ) );
        const auto missing = project_to<coords::sm>( observer.pos_abs() );
        REQUIRE( MAPBUFFER.lookup_submap_existing( missing ) == nullptr );
        const auto before_player = target.pos_abs();
        scout_observation::site_reader reader;
        const auto actual = reader.read_character( observer, person );
        CHECK( actual.known );
        CHECK( actual.visible );
        CHECK( reader.read_signal( observer, observer.pos_abs(), person.pos_abs(),
        { person.pos_abs_omt() } ).visible );
        CHECK( reader.cached_view_count() == 0 );
        CHECK( target.pos_abs() == before_player );
        CHECK( MAPBUFFER.lookup_submap_cached( missing ) == nullptr );
        // The original bearing enters the east edge of this footprint at x95.
        here.ter_set( tripoint_bub_ms( 95, 66, 0 ), ter_str_id( "t_wall" ) );
        here.ter_set( tripoint_bub_ms( 95, 67, 0 ), ter_str_id( "t_wall" ) );
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        CHECK_FALSE( reader.read_character( observer, person ).visible );
    }
    SECTION( "long diagonal rays use the configured native distance" ) {
        const bool previous_distance = trigdist;
        const bool circular = GENERATE( false, true );
        trigdist = circular;
        on_out_of_scope restore_distance( [&]() {
            trigdist = previous_distance;
        } );
        observer.setpos( here, tripoint_bub_ms( 0, 0, 0 ), false );
        target.setpos( here, tripoint_bub_ms( 95, 71, 0 ), false );
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        REQUIRE( square_dist( observer.pos_abs_omt().raw(), target.pos_abs_omt().raw() ) == 3 );
        CHECK_FALSE( observer.sees_without_clairvoyance( here, target ) );
        CHECK( here.sees_with_optics( observer.pos_bub( here ), target.pos_bub( here ), 120, 2.0f ) );
        CHECK( scout_observation::read_character( observer, target ).visible );
    }
    SECTION( "native vision-only barriers and cumulative smoke stay opaque to optics" ) {
        const bool smoke = GENERATE( false, true );
        for( int x = 16; x <= 28; ++x ) {
            if( smoke ) {
                here.add_field( tripoint_bub_ms( x, 66, 0 ), field_type_id( "fd_smoke" ), 2 );
            }
        }
        if( !smoke ) {
            here.ter_set( tripoint_bub_ms( 20, 66, 0 ), ter_str_id( "t_door_glass_frosted_c" ) );
        }
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        CHECK( scout_observation::read_character( observer, target ).visible );
        if( smoke ) {
            for( int x = 74; x <= 82; ++x ) {
                here.add_field( tripoint_bub_ms( x, 66, 0 ), field_type_id( "fd_smoke" ), 2 );
            }
        } else {
            here.ter_set( tripoint_bub_ms( 76, 66, 0 ), ter_str_id( "t_door_glass_frosted_c" ) );
        }
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        CHECK_FALSE( scout_observation::read_character( observer, target ).visible );
        person.setpos( here, tripoint_bub_ms( 32, 66, 0 ), false );
        target.setpos( here, tripoint_bub_ms( 12, 66, 0 ), false );
        here.build_map_cache( 0 );
        CHECK_FALSE( target.sees( here, person ) );
        CHECK_FALSE( observer.sees_with_optics( here, person, 2.0f ) );
        CHECK( scout_observation::read_character( observer, person ).visible );
    }
    SECTION( "wall blocks the same clear daylight ray" ) {
        here.ter_set( tripoint_bub_ms( 76, 66, 0 ), ter_str_id( "t_wall" ) );
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        CHECK_FALSE( scout_observation::read_character( observer, target ).visible );
    }
    SECTION( "actual roofs retain z and floors obstruct" ) {
        const int roof = GENERATE( 1, 3 );
        target.setpos( here, tripoint_bub_ms( 84, 66, roof ), false );
        here.ter_set( target.pos_bub( here ), ter_str_id( "t_flat_roof" ) );
        here.invalidate_map_cache( roof );
        here.build_map_cache( roof );
        CHECK( scout_observation::read_character( observer, target ).visible );
        person.setpos( here, tripoint_bub_ms( 84, 67, roof ), false );
        here.ter_set( person.pos_bub( here ), ter_str_id( "t_flat_roof" ) );
        here.invalidate_map_cache( roof );
        here.build_map_cache( roof );
        const auto physical_line = scout_observation::site_sightline( observer.pos_abs(),
                                   person.pos_abs() );
        REQUIRE( physical_line.size() == 72 );
        CHECK( physical_line.back() == person.pos_abs() );
        const std::vector<tripoint_abs_omt> roof_footprint = { person.pos_abs_omt() };
        const auto roof_person = scout_observation::read_character( observer, person );
        CAPTURE( roof, roof_person.reason, here.ambient_light_at( person.pos_bub( here ) ),
                 here.ledge_coverage( observer, person.pos_bub( here ),
                                      &roof_footprint ) );
        CHECK( roof_person.visible );
        // An intervening solid wall at every ray height blocks the exposed target.
        for( int z = 0; z <= roof; ++z ) {
            for( int y = 62; y <= 70; ++y ) {
                here.ter_set( tripoint_bub_ms( 76, y, z ), ter_str_id( "t_wall" ) );
            }
            here.invalidate_map_cache( z );
        }
        here.build_map_cache( roof );
        CHECK_FALSE( scout_observation::read_character( observer, target ).visible );
        CHECK_FALSE( scout_observation::read_character( observer, person ).visible );
    }
    SECTION( "opaque floors and the far side of a house block exterior actors" ) {
        target.setpos( here, tripoint_bub_ms( 84, 66, 3 ), false );
        here.ter_set( target.pos_bub( here ), ter_str_id( "t_flat_roof" ) );
        for( int z = 1; z <= 3; ++z ) {
            for( int x = 74; x <= 82; ++x ) {
                for( int y = 62; y <= 70; ++y ) {
                    here.ter_set( tripoint_bub_ms( x, y, z ), ter_str_id( "t_floor" ) );
                }
            }
            here.invalidate_map_cache( z );
        }
        here.build_map_cache( 3 );
        CHECK_FALSE( scout_observation::read_character( observer, target ).visible );
    }
    SECTION( "indoor occupants stay unknown even through a clear open doorway" ) {
        here.ter_set( target.pos_bub( here ), ter_str_id( "t_floor" ) );
        here.ter_set( target.pos_bub( here ) + tripoint( 0, 0, 1 ), ter_str_id( "t_floor" ) );
        here.invalidate_map_cache( 0 );
        here.invalidate_map_cache( 1 );
        here.build_map_cache( 0 );
        REQUIRE_FALSE( here.is_outside( target.pos_bub( here ) ) );
        const auto hidden = scout_observation::read_character( observer, target );
        CHECK_FALSE( hidden.visible );
        CHECK( hidden.reason == "interior_unknown" );
    }
    SECTION( "optics cannot reveal an invisible native actor" ) {
        target.set_mutation( trait_id( "DEBUG_CLOAK" ) );
        REQUIRE( target.is_invisible() );
        CHECK_FALSE( scout_observation::read_character( observer, target ).visible );
        CHECK_FALSE( observer.sees_without_clairvoyance( here, target ) );
        target.unset_mutation( trait_id( "DEBUG_CLOAK" ) );
    }
    SECTION( "native avatar stealth still reduces binocular recognition distance" ) {
        target.set_mutation( trait_id( "CRAFTY" ) );
        target.set_mutation( trait_id( "CAMO2" ) );
        target.recalculate_enchantment_cache();
        REQUIRE( target.visibility() == 40 );
        CHECK_FALSE( scout_observation::read_character( observer, target ).visible );
        CHECK_FALSE( observer.sees_without_clairvoyance( here, target ) );
        target.unset_mutation( trait_id( "CRAFTY" ) );
        target.unset_mutation( trait_id( "CAMO2" ) );
        target.recalculate_enchantment_cache();
        CHECK( scout_observation::read_character( observer, target ).visible );
    }
    SECTION( "fresh native eye impairment applies before cached sight limits" ) {
        observer.set_part_hp_cur( bodypart_id( "torso" ), 1 );
        REQUIRE_FALSE( observer.is_dead_state() );
        REQUIRE( scout_observation::read_character( observer, target ).visible );
        const int cached_range = observer.sight_range( 100.0f, 100.0f );
        REQUIRE( cached_range >= MAX_VIEW_DISTANCE );
        observer.add_effect( efftype_id( "boomered" ), 1_hours );
        REQUIRE_FALSE( observer.is_blind() );
        REQUIRE( observer.sight_range( 100.0f, 100.0f ) == cached_range );
        CHECK_FALSE( scout_observation::read_character( observer, target ).visible );
        scout_observation::site_reader reader;
        CHECK_FALSE( reader.read_signal( observer, observer.pos_abs(), target.pos_abs(),
        { target.pos_abs_omt() } ).visible );
        CHECK( observer.sight_range( 100.0f, 100.0f ) == cached_range );
        CHECK( observer.get_part_hp_cur( bodypart_id( "torso" ) ) == 1 );
        observer.set_mutation( trait_id( "PER_SLIME_OK" ) );
        observer.recalc_sight_limits();
        REQUIRE( observer.has_trait( trait_id( "PER_SLIME_OK" ) ) );
        CHECK( scout_observation::read_character( observer, target ).visible );
        CHECK( reader.read_signal( observer, observer.pos_abs(), target.pos_abs(),
        { target.pos_abs_omt() } ).visible );
        CHECK( observer.get_part_hp_cur( bodypart_id( "torso" ) ) == 1 );
        observer.unset_mutation( trait_id( "PER_SLIME_OK" ) );
        observer.recalc_sight_limits();
        CHECK_FALSE( scout_observation::read_character( observer, target ).visible );
    }
    SECTION( "sleep and blindness do not acquire people" ) {
        observer.add_effect( efftype_id( "sleep" ), 1_hours );
        CHECK_FALSE( scout_observation::read_character( observer, target ).visible );
        observer.remove_effect( efftype_id( "sleep" ) );
        observer.add_effect( efftype_id( "blind" ), 1_hours );
        CHECK_FALSE( scout_observation::read_character( observer, target ).visible );
    }
    SECTION( "dense weather reduces the optical range" ) {
        scoped_weather_override fog( weather_type_id( "fog" ) );
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        CHECK_FALSE( scout_observation::read_character( observer, target ).visible );
    }
    SECTION( "unlit night targets disappear while illuminated targets remain visible" ) {
        set_time( calendar::turn_zero );
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        CHECK_FALSE( scout_observation::read_character( observer, target ).visible );
        scout_observation::site_reader unlit_signal;
        CHECK_FALSE( unlit_signal.read_signal( observer, observer.pos_abs(), target.pos_abs(),
        { target.pos_abs_omt() } ).visible );
        here.add_field( target.pos_bub( here ), field_type_id( "fd_fire" ), 3 );
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        CHECK( scout_observation::read_character( observer, target ).visible );
        scout_observation::site_reader planner;
        CHECK( planner.read( observer, observer.pos_abs(), target.pos_abs() ).visible );
        CHECK( planner.read_signal( observer, observer.pos_abs(), target.pos_abs(),
        { target.pos_abs_omt() } ).visible );
    }
    SECTION( "unavailable geometry stays unknown without map generation" ) {
        observer.setpos( tripoint_abs_ms( 987654, 987654, 0 ), false );
        target.setpos( tripoint_abs_ms( 987726, 987654, 0 ), false );
        const auto read = scout_observation::read_character( observer, target );
        CHECK_FALSE( read.known );
        CHECK_FALSE( read.visible );
        CHECK( read.reason == "geometry_unavailable" );
    }
}

TEST_CASE( "site association follows only an actually observed leaving identity",
           "[binocular_056][site_association]" )
{
    const std::vector<tripoint_abs_omt> site = { tripoint_abs_omt( 10, 10, 0 ) };
    CHECK( scout_observation::associated_with_site( tripoint_abs_omt( 10, 10, 3 ), site ) );
    CHECK_FALSE( scout_observation::associated_with_site( tripoint_abs_omt( 11, 10, 0 ), site ) );
    CHECK( scout_observation::associated_with_site( tripoint_abs_omt( 11, 10, 0 ), site,
    { tripoint_abs_omt( 10, 10, 1 ) } ) );
    CHECK_FALSE( scout_observation::associated_with_site( tripoint_abs_omt( 12, 10, 0 ), site,
    { tripoint_abs_omt( 10, 10, 1 ) } ) );
}

TEST_CASE( "loaded target remains visible to an abstract scout through existing geometry",
           "[binocular_056][abstract_visibility]" )
{
    const auto saved_map_origin = get_map().get_abs_sub();
    clear_avatar();
    clear_npcs();
    clear_map_with_vision( -1, 1, false );
    const auto saved_turn = calendar::turn;
    const auto saved_position = get_avatar().pos_abs();
    on_out_of_scope restore( [&]() {
        if( get_map().get_abs_sub() != saved_map_origin ) {
            get_map().load( saved_map_origin, false, false );
        }
        get_avatar().setpos( saved_position, false );
        set_time( saved_turn );
    } );
    set_time_to_day();
    scoped_weather_override weather( weather_type_id( "clear" ) );
    map &here = get_map();
    // Fixed bubble coordinates below describe target-local obstacles only when
    // the window starts on an OMT boundary. Ordinary player relocation can leave
    // an odd submap origin; preserve that incoming window on scope exit.
    const auto aligned_origin = project_to<coords::sm>( project_to<coords::omt>( here.get_abs_sub() ) );
    if( here.get_abs_sub() != aligned_origin ) {
        here.load( aligned_origin, false, false );
    }
    npc observer;
    observer.normalize();
    observer.set_fake( true );
    observer.recalc_sight_limits();
    observer.setpos( here, tripoint_bub_ms( 12, 66, 0 ), false );
    get_avatar().setpos( here, tripoint_bub_ms( 84, 66, 0 ), false );
    here.build_map_cache( 0 );
    REQUIRE( scout_observation::read_character( observer, get_avatar() ).visible );
    // Prepare controlled existing submaps. Only the test generates this fixture;
    // the production reader subsequently uses existing-only retrieval and never calls load.
    map fixture;
    fixture.load( here.get_abs_sub() + point( -6, 0 ), false );
    // Native illumination needs the same full map extent around each remote
    // observer. Generate this explicit test context only; the reader never does.
    std::vector<std::unique_ptr<map>> lighting_context;
    for( const point &offset : {
             point( -7, -4 ), point( -7, 4 ),
             point( -6, -4 ), point( -6, 4 )
         } ) {
        auto context = std::make_unique<map>();
        context->load( here.get_abs_sub() + offset, false );
        for( int x = 0; x < MAPSIZE * SEEX; ++x ) {
            for( int y = 0; y < MAPSIZE * SEEY; ++y ) {
                context->ter_set( tripoint_bub_ms( x, y, 0 ), ter_str_id( "t_grass" ) );
                context->ter_set( tripoint_bub_ms( x, y, 1 ), ter_str_id( "t_open_air" ) );
                context->furn_set( tripoint_bub_ms( x, y, 0 ), furn_str_id( "f_null" ) );
                context->field_at( tripoint_bub_ms( x, y, 0 ) ).clear();
            }
        }
        lighting_context.push_back( std::move( context ) );
    }
    for( int x = 0; x < MAPSIZE * SEEX; ++x ) {
        for( int y = 0; y < MAPSIZE * SEEY; ++y ) {
            fixture.ter_set( tripoint_bub_ms( x, y, 0 ), ter_str_id( "t_grass" ) );
            fixture.ter_set( tripoint_bub_ms( x, y, 1 ), ter_str_id( "t_open_air" ) );
            fixture.furn_set( tripoint_bub_ms( x, y, 0 ), furn_str_id( "f_null" ) );
            fixture.field_at( tripoint_bub_ms( x, y, 0 ) ).clear();
        }
    }
    here.invalidate_map_cache( 0 );
    here.invalidate_map_cache( 1 );
    here.build_map_cache( 0 );
    observer.setpos( here.get_abs( tripoint_bub_ms( -12, 66, 0 ) ), false );
    get_avatar().setpos( here, tripoint_bub_ms( 60, 66, 0 ), false );
    REQUIRE_FALSE( here.inbounds( observer.pos_abs() ) );
    REQUIRE( here.inbounds( get_avatar().pos_abs() ) );
    REQUIRE( rl_dist( observer.pos_abs_omt().xy(), get_avatar().pos_abs_omt().xy() ) == 3 );
    const auto abstract = scout_observation::read_character( observer, get_avatar() );
    CAPTURE( abstract.reason );
    CHECK( abstract.known );
    CHECK( abstract.visible );
    SECTION( "one planning decision reuses overlapping existing geometry" ) {
        scout_observation::site_reader reader;
        CHECK( reader.read_character( observer, get_avatar() ).visible );
        REQUIRE( reader.cached_view_count() == 0 );
        CHECK( reader.cached_vehicle_scan_count() == 0 );
        get_avatar().set_mutation( trait_id( "DEBUG_CLOAK" ) );
        CHECK_FALSE( reader.read_character( observer, get_avatar() ).visible );
        get_avatar().unset_mutation( trait_id( "DEBUG_CLOAK" ) );
        CHECK( reader.read_character( observer, get_avatar() ).visible );
        CHECK( reader.cached_view_count() == 0 );
        const auto next_origin = here.get_abs( tripoint_bub_ms( -12, 78, 0 ) );
        const auto next_target = here.get_abs( tripoint_bub_ms( 60, 78, 0 ) );
        CHECK( reader.read( observer, next_origin, next_target ).visible );
        CHECK( reader.cached_view_count() == 0 );
        CHECK( reader.cached_vehicle_scan_count() == 0 );
        // A different decision constructs fresh caches from current physical geometry.
        fixture.ter_set( fixture.get_bub( here.get_abs( tripoint_bub_ms( 54, 78, 0 ) ) ),
                         ter_str_id( "t_wall" ) );
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        scout_observation::site_reader next_decision;
        CHECK_FALSE( next_decision.read( observer, next_origin, next_target ).visible );
    }
    SECTION( "remote ledge floors match native visibility" ) {
        observer.setpos( here.get_abs( tripoint_bub_ms( -24, 66, 0 ) ), false );
        get_avatar().setpos( here, tripoint_bub_ms( 48, 66, 1 ), false );
        fixture.ter_set( fixture.get_bub( get_avatar().pos_abs() ), ter_str_id( "t_flat_roof" ) );
        fixture.ter_set( fixture.get_bub( here.get_abs( tripoint_bub_ms( -20, 66, 1 ) ) ),
                         ter_str_id( "t_flat_roof" ) );
        fixture.invalidate_map_cache( 0 );
        fixture.invalidate_map_cache( 1 );
        fixture.build_map_cache( 1 );
        here.invalidate_map_cache( 1 );
        here.build_map_cache( 1 );
        CHECK_FALSE( observer.sees_with_optics( fixture, get_avatar(), 2.0f ) );
        const auto remote = scout_observation::read_character( observer, get_avatar() );
        CAPTURE( remote.reason );
        CHECK( remote.known );
        CHECK( remote.visible );
        // The old distant floor stays irrelevant; an actual target-local wall blocks.
        get_avatar().setpos( here, tripoint_bub_ms( 60, 66, 1 ), false );
        here.ter_set( tripoint_bub_ms( 54, 66, 1 ), ter_str_id( "t_wall" ) );
        here.invalidate_map_cache( 1 );
        here.build_map_cache( 1 );
        CHECK_FALSE( scout_observation::read_character( observer, get_avatar() ).visible );
    }
    SECTION( "abstract physical interruptions survive transient-cache reload" ) {
        bandit_live_world::site_record site;
        auto &outing = site.active_outing;
        observer.setID( character_id( 956220 ), true );
        outing.schema_version = 10;
        outing.kind = bandit_live_world::outing_kind::structural_sortie;
        outing.phase = bandit_live_world::scout_phase::observing;
        outing.owner = bandit_live_world::simulation_owner::abstract;
        outing.selected_watch_kind = bandit_live_world::structural_watch_kind::exact;
        outing.selected_watch_omt = observer.pos_abs_omt();
        outing.target_footprint = { get_avatar().pos_abs_omt() };
        outing.member_ids = { observer.getID() };
        CAPTURE( observer.is_dead_state(), observer.is_blind(), observer.in_sleep_state(),
                 observer.path.size(), observer.omt_path.size(), observer.mission,
                 observer.goal, observer.get_attitude(), observer.get_ai_danger(),
                 observer.pos_abs(), outing.selected_watch_omt,
                 bandit_live_world::target_footprint_watch_distance( outing.selected_watch_omt,
                         outing.target_footprint ), outing.member_is_resolved( observer.getID() ) );
        REQUIRE( scout_observation::watching_member( site, observer ) );
        fixture.add_field( fixture.get_bub( observer.pos_abs() ), field_type_id( "fd_fire" ), 3 );
        CHECK_FALSE( scout_observation::watching_member( site, observer ) );
        fixture.delete_field( fixture.get_bub( observer.pos_abs() ), field_type_id( "fd_fire" ) );
        REQUIRE( scout_observation::watching_member( site, observer ) );
        fixture.invalidate_map_cache( 0 );
        fixture.build_map_cache( 0 );
        SECTION( "resident hostile NPC interrupts outside the active player list" ) {
            observer.set_fac( faction_id( "hells_raiders" ) );
            auto enemy = make_shared_fast<npc>();
            enemy->normalize();
            enemy->setID( character_id( 956225 ), true );
            enemy->set_fac( faction_id( "your_followers" ) );
            enemy->spawn_at_precise( fixture.get_abs( observer.pos_bub( fixture ) + point( 4, 0 ) ) );
            overmap_buffer.insert_npc( enemy );
            on_out_of_scope remove_resident( [&]() {
                overmap_buffer.remove_npc( enemy->getID() );
            } );
            REQUIRE_FALSE( enemy->is_active() );
            REQUIRE( observer.attitude_to( *enemy ) == Creature::Attitude::HOSTILE );
            REQUIRE( observer.sees_without_clairvoyance( fixture, *enemy ) );
            CHECK_FALSE( scout_observation::watching_member( site, observer ) );
            enemy->add_effect( efftype_id( "invisibility" ), 1_hours );
            CHECK( scout_observation::watching_member( site, observer ) );
            observer.set_fac( faction_id( "no_faction" ) );
        }
        SECTION( "ordinary hostile avatar perception interrupts without a temporary seen cache" ) {
            observer.set_fac( faction_id( "hells_raiders" ) );
            get_avatar().setpos( here, tripoint_bub_ms( 2, 66, 0 ), false );
            REQUIRE( observer.attitude_to( get_avatar() ) == Creature::Attitude::HOSTILE );
            REQUIRE( observer.sees_with_optics( fixture, get_avatar(), 1.0f ) );
            CHECK_FALSE( scout_observation::watching_member( site, observer ) );
            const bool partial_smoke = GENERATE( false, true );
            if( partial_smoke ) {
                for( int x = -10; x <= -4; ++x ) {
                    fixture.add_field( fixture.get_bub( here.get_abs( tripoint_bub_ms( x, 66, 0 ) ) ),
                                       field_type_id( "fd_smoke" ), 2 );
                }
                fixture.invalidate_map_cache( 0 );
                fixture.build_map_cache( 0 );
                REQUIRE_FALSE( observer.sees_with_optics( fixture, get_avatar(), 1.0f ) );
                REQUIRE( observer.sees_without_clairvoyance_physical( fixture, get_avatar() ) );
                CHECK_FALSE( scout_observation::watching_member( site, observer ) );
            }
            get_avatar().set_mutation( trait_id( "DEBUG_CLOAK" ) );
            CHECK( scout_observation::watching_member( site, observer ) );
            get_avatar().unset_mutation( trait_id( "DEBUG_CLOAK" ) );
            get_avatar().setpos( here, tripoint_bub_ms( 60, 66, 0 ), false );
            CHECK( scout_observation::watching_member( site, observer ) );
            observer.set_fac( faction_id( "no_faction" ) );
        }
        monster *enemy = g->place_critter_at( mtype_id( "mon_zombie" ), tripoint_bub_ms( 2, 66, 0 ) );
        REQUIRE( enemy );
        on_out_of_scope remove_enemy( [&]() {
            g->clear_zombies();
        } );
        fixture.invalidate_map_cache( 0 );
        fixture.build_map_cache( 0 );
        REQUIRE( observer.sees_without_clairvoyance( fixture, *enemy ) );
        CHECK_FALSE( scout_observation::watching_member( site, observer ) );
        SECTION( "remote ordinary threat recognition uses its actual observer light" ) {
            const bool lamp_at_night = GENERATE( false, true );
            if( lamp_at_night ) {
                set_time( calendar::turn_zero );
                fixture.add_field( observer.pos_bub( fixture ), field_type_id( "fd_biolum" ), 3 );
                fixture.invalidate_map_cache( 0 );
                fixture.build_map_cache( 0 );
                here.invalidate_map_cache( 0 );
                here.build_map_cache( 0 );
            }
            const float origin_light = fixture.ambient_light_at( observer.pos_bub( fixture ) );
            const float wrong_origin_light = here.ambient_light_at( observer.pos_bub( here ) );
            REQUIRE( origin_light > wrong_origin_light );
            const int distance = rl_dist( observer.pos_abs(), enemy->pos_abs() );
            const float target_light = std::sqrt( observer.get_vision_threshold( origin_light ) *
                                                  observer.get_vision_threshold( wrong_origin_light ) ) *
                                       std::exp( LIGHT_TRANSPARENCY_OPEN_AIR * distance );
            REQUIRE( observer.sight_range( target_light, origin_light ) < distance );
            REQUIRE( observer.sight_range( target_light, wrong_origin_light ) >= distance );
            const auto target = enemy->pos_bub( here );
            auto &cache = const_cast<level_cache &>( here.get_cache_ref( 0 ) );
            cache.lm[target.x()][target.y()] = four_quadrants( target_light );
            auto &fixture_cache = const_cast<level_cache &>( fixture.get_cache_ref( 0 ) );
            const auto fixture_target = enemy->pos_bub( fixture );
            fixture_cache.lm[fixture_target.x()][fixture_target.y()] = four_quadrants( target_light );
            const bool native_visible = observer.sees_without_clairvoyance( fixture, *enemy );
            if( !lamp_at_night ) {
                CHECK_FALSE( native_visible );
            }
            // Native recognition also permits a target brighter than natural
            // night light; preserve that rule instead of forcing a dark result.
            CHECK( scout_observation::watching_member( site, observer ) == !native_visible );
        }
        SECTION( "ordinary non-avatar perception survives partial smoke and translucent terrain" ) {
            const bool translucent = GENERATE( false, true );
            if( translucent ) {
                fixture.ter_set( fixture.get_bub( here.get_abs( tripoint_bub_ms( -5, 66, 0 ) ) ),
                                 ter_str_id( "t_door_glass_frosted_c" ) );
            } else {
                for( int x = -10; x <= -4; ++x ) {
                    fixture.add_field( fixture.get_bub( here.get_abs( tripoint_bub_ms( x, 66, 0 ) ) ),
                                       field_type_id( "fd_smoke" ), 2 );
                }
            }
            fixture.invalidate_map_cache( 0 );
            fixture.build_map_cache( 0 );
            REQUIRE( observer.sees_without_clairvoyance( fixture, *enemy ) );
            CHECK_FALSE( observer.sees_with_optics( fixture, *enemy, 1.0f ) );
            CHECK_FALSE( scout_observation::watching_member( site, observer ) );
        }
        std::ostringstream bytes;
        JsonOut writer( bytes );
        observer.serialize( writer );
        npc loaded;
        loaded.deserialize( json_loader::from_string( bytes.str() ).get_object() );
        CHECK( loaded.get_ai_danger() == 0 );
        const bool native_threat_visible = observer.sees_without_clairvoyance( fixture, *enemy );
        CHECK( loaded.sees_without_clairvoyance( fixture, *enemy ) == native_threat_visible );
        CHECK( scout_observation::watching_member( site, loaded ) == !native_threat_visible );
    }
    SECTION( "unknown hostile optics do not veto an independently visible target" ) {
        bandit_live_world::site_record site;
        auto &outing = site.active_outing;
        observer.setID( character_id( 956221 ), true );
        outing.schema_version = 10;
        outing.kind = bandit_live_world::outing_kind::structural_sortie;
        outing.job_type = "scout";
        outing.phase = bandit_live_world::scout_phase::observing;
        outing.owner = bandit_live_world::simulation_owner::abstract;
        outing.selected_watch_kind = bandit_live_world::structural_watch_kind::exact;
        outing.selected_watch_omt = observer.pos_abs_omt();
        outing.target_footprint = { get_avatar().pos_abs_omt() };
        outing.member_ids = { observer.getID() };
        monster *enemy = g->place_critter_at( mtype_id( "mon_zombie" ), tripoint_bub_ms( 2, 66, 0 ) );
        REQUIRE( enemy );
        on_out_of_scope remove_enemy( [&]() {
            g->clear_zombies();
        } );
        // Its ray fits the prepared remote map, but its full native-lighting
        // neighbourhood extends into geometry this fixture has not generated.
        enemy->setpos( here.get_abs( tripoint_bub_ms( -60, 66, 0 ) ) );
        REQUIRE( rl_dist( observer.pos_abs(), enemy->pos_abs() ) <= MAX_VIEW_DISTANCE );
        scout_observation::site_reader optics;
        REQUIRE( optics.read_character( observer, get_avatar() ).visible );
        std::string reason;
        CHECK( optics.watching_member( site, observer, &reason ) );
        INFO( reason );
        CHECK( reason == "eligible_hostile_optics_unknown" );
        CHECK( optics.perceived_threats( observer ).empty() );
        std::ostringstream bytes;
        JsonOut writer( bytes );
        observer.serialize( writer );
        npc loaded;
        loaded.deserialize( json_loader::from_string( bytes.str() ).get_object() );
        CHECK( optics.watching_member( site, loaded ) );
        CHECK( optics.read_character( loaded, get_avatar() ).visible );
    }
    SECTION( "a concealed populated horde uses one bounded observer window" ) {
        bandit_live_world::site_record site;
        auto &outing = site.active_outing;
        observer.setID( character_id( 956220 ), true );
        outing.schema_version = 10;
        outing.kind = bandit_live_world::outing_kind::structural_sortie;
        outing.phase = bandit_live_world::scout_phase::observing;
        outing.owner = bandit_live_world::simulation_owner::abstract;
        outing.selected_watch_kind = bandit_live_world::structural_watch_kind::exact;
        outing.selected_watch_omt = observer.pos_abs_omt();
        outing.target_footprint = { get_avatar().pos_abs_omt() };
        outing.member_ids = { observer.getID() };
        for( int y = 54; y <= 78; ++y ) {
            here.ter_set( tripoint_bub_ms( 0, y, 0 ), ter_str_id( "t_wall" ) );
        }
        for( int y = 61; y <= 70; ++y ) {
            for( int x : {
                     2, 4
                 } ) {
                REQUIRE( g->place_critter_at( mtype_id( "mon_zombie" ), tripoint_bub_ms( x, y, 0 ) ) );
            }
        }
        on_out_of_scope remove_horde( [&]() {
            g->clear_zombies();
        } );
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        CHECK( scout_observation::watching_member( site, observer ) );
        here.ter_set( tripoint_bub_ms( 0, 66, 0 ), ter_str_id( "t_grass" ) );
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        CHECK_FALSE( scout_observation::watching_member( site, observer ) );
        here.ter_set( tripoint_bub_ms( 0, 66, 0 ), ter_str_id( "t_wall" ) );
    }
    SECTION( "a loaded vehicle crossing the bubble edge retains opaque geometry without moving" ) {
        vehicle *barrier = here.add_vehicle( vproto_id( "vehicle_camera_test" ),
                                             tripoint_bub_ms( 1, 66, 0 ), 0_degrees, 0, veh_spawn_status::UNDAMAGED );
        REQUIRE( barrier );
        on_out_of_scope remove_vehicle( [&]() {
            here.destroy_vehicle( barrier );
        } );
        const auto position = barrier->pos_abs();
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        const auto blocked_vehicle = scout_observation::read_character( observer, get_avatar() );
        CAPTURE( blocked_vehicle.reason );
        CHECK( blocked_vehicle.known );
        CHECK( blocked_vehicle.visible );
        CHECK( barrier->pos_abs() == position );
    }
    SECTION( "excluded remote vehicle origins still mark their overhanging parts unknown" ) {
        const int ray_x = GENERATE( -13, -12 );
        observer.setpos( here.get_abs( tripoint_bub_ms( ray_x, 30, 0 ) ), false );
        get_avatar().setpos( here.get_abs( tripoint_bub_ms( ray_x, 102, 0 ) ), false );
        REQUIRE( scout_observation::read_character( observer, get_avatar() ).visible );
        observer.setpos( here.get_abs( tripoint_bub_ms( ray_x, 48, 0 ) ), false );
        get_avatar().setpos( here.get_abs( tripoint_bub_ms( ray_x, 78, 0 ) ), false );
        observer.set_fac( faction_id( "hells_raiders" ) );
        observer.setID( character_id( 956220 ), true );
        bandit_live_world::site_record site;
        auto &outing = site.active_outing;
        outing.schema_version = 10;
        outing.kind = bandit_live_world::outing_kind::structural_sortie;
        outing.phase = bandit_live_world::scout_phase::observing;
        outing.owner = bandit_live_world::simulation_owner::abstract;
        outing.selected_watch_kind = bandit_live_world::structural_watch_kind::exact;
        outing.selected_watch_omt = observer.pos_abs_omt();
        outing.target_footprint = { project_to<coords::omt>( here.get_abs( tripoint_bub_ms( 60, 66, 0 ) ) ) };
        outing.member_ids = { observer.getID() };
        REQUIRE_FALSE( scout_observation::watching_member( site, observer ) );
        vehicle *barrier = fixture.add_vehicle( vproto_id( "vehicle_camera_test" ),
                                                fixture.get_bub( here.get_abs( tripoint_bub_ms( ray_x == -13 ? -12 : -13, 66, 0 ) ) ),
                                                0_degrees, 0, veh_spawn_status::UNDAMAGED );
        REQUIRE( barrier );
        on_out_of_scope remove_vehicle( [&]() {
            fixture.destroy_vehicle( barrier );
        } );
        const auto position = barrier->pos_abs();
        const auto blocked = scout_observation::read_character( observer, get_avatar() );
        CAPTURE( blocked.reason );
        CHECK( blocked.known );
        CHECK( blocked.visible );
        CHECK( scout_observation::watching_member( site, observer ) );
        CHECK( barrier->pos_abs() == position );
        REQUIRE( fixture.displace_vehicle( *barrier, tripoint_rel_ms( 0, 12, 0 ) ) );
        const auto target_local = scout_observation::read_character( observer, get_avatar() );
        CHECK_FALSE( target_local.known );
        CHECK_FALSE( target_local.visible );
    }
    SECTION( "opaque vehicle parts beyond the bubble stay unknown" ) {
        observer.setpos( here.get_abs( tripoint_bub_ms( -1, 30, 0 ) ), false );
        get_avatar().setpos( here.get_abs( tripoint_bub_ms( -1, 102, 0 ) ), false );
        REQUIRE( scout_observation::read_character( observer, get_avatar() ).visible );
        vehicle *barrier = here.add_vehicle( vproto_id( "vehicle_camera_test" ),
                                             tripoint_bub_ms( 1, 66, 0 ), 0_degrees, 0, veh_spawn_status::UNDAMAGED );
        REQUIRE( barrier );
        on_out_of_scope remove_vehicle( [&]() {
            here.destroy_vehicle( barrier );
        } );
        REQUIRE( here.displace_vehicle( *barrier, tripoint_rel_ms::west ) );
        const auto position = barrier->pos_abs();
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        const auto blocked = scout_observation::read_character( observer, get_avatar() );
        CAPTURE( blocked.reason );
        CHECK( blocked.known );
        CHECK( blocked.visible );
        CHECK( barrier->pos_abs() == position );
    }
    if( get_avatar().pos_abs() == here.get_abs( tripoint_bub_ms( 60, 66, 0 ) ) ) {
        here.ter_set( tripoint_bub_ms( 54, 66, 0 ), ter_str_id( "t_wall" ) );
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0 );
        const auto blocked = scout_observation::read_character( observer, get_avatar() );
        const auto wall = here.get_abs( tripoint_bub_ms( 54, 66, 0 ) );
        const auto *stored = MAPBUFFER.lookup_submap_existing( project_to<coords::sm>( wall ) );
        point_sm_ms wall_offset;
        auto wall_local = tripoint_bub_ms( 54, 66, 0 );
        const auto *loaded = map_meddler::unsafe_get_submap_at( wall_local, wall_offset );
        INFO( "optical_fixture map=" << here.get_abs_sub() << " remote=" << fixture.get_abs_sub()
              << " wall=" << wall << " terrain=" << here.ter( tripoint_bub_ms( 54, 66, 0 ) ).id()
              << " stored=" << static_cast<bool>( stored ) << " same_submap=" << ( stored == loaded )
              << " stored_wall=" << ( stored ? stored->get_ter( wall_offset ).id().str() : "missing" )
              << " target=" << get_avatar().pos_abs() << " observer=" << observer.pos_abs()
              << " reason=" << blocked.reason );
        CHECK( blocked.known );
        CHECK_FALSE( blocked.visible );
    }
}

namespace
{
class observation_light_fixture : public map
{
    public:
        void observe_light( const int z ) {
            set_lightmap_cache_dirty( z );
            generate_lightmap( z, true );
        }
};
}

TEST_CASE( "read-only native lighting uses actual sources and supplied map coordinates",
           "[binocular_056][native_observation_light]" )
{
    clear_avatar();
    clear_npcs();
    clear_map_with_vision( -1, 2, false );
    const auto saved_turn = calendar::turn;
    on_out_of_scope restore( [&]() {
        clear_avatar();
        set_time( saved_turn );
    } );
    set_time( calendar::turn_zero );
    scoped_weather_override weather( weather_type_id( "clear" ) );
    map &here = get_map();
    observation_light_fixture view;
    view.load( here.get_abs_sub() + point( -6, 0 ), false );
    get_avatar().setpos( here, tripoint_bub_ms( 12, 66, 0 ), false );
    const auto source = get_avatar().pos_bub( view );
    const auto wrong_source = get_avatar().pos_bub( here );
    REQUIRE( view.inbounds( source ) );
    REQUIRE( source != wrong_source );
    for( int x = 0; x < MAPSIZE * SEEX; ++x ) {
        for( int y = 0; y < MAPSIZE * SEEY; ++y ) {
            view.ter_set( tripoint_bub_ms( x, y, 0 ), ter_str_id( "t_grass" ) );
            view.ter_set( tripoint_bub_ms( x, y, 1 ), ter_str_id( "t_open_air" ) );
            view.furn_set( tripoint_bub_ms( x, y, 0 ), furn_str_id( "f_null" ) );
            view.field_at( tripoint_bub_ms( x, y, 0 ) ).clear();
            view.i_clear( tripoint_bub_ms( x, y, 0 ) );
        }
    }
    view.invalidate_map_cache( 0 );
    view.invalidate_map_cache( 1 );
    view.build_map_cache( 0 );
    const float dark = view.ambient_light_at( source );
    item battery( itype_id( "medium_battery_cell" ) );
    battery.ammo_set( battery.ammo_default(), 56 );
    item lamp( itype_id( "flashlight_on" ) );
    lamp.put_in( battery, pocket_type::MAGAZINE_WELL );
    lamp.active = true;
    get_avatar().i_add( lamp );
    REQUIRE( get_avatar().active_light() > 4 );
    REQUIRE_FALSE( get_avatar().has_effect( efftype_id( "haslight" ) ) );
    const auto position = get_avatar().pos_abs();
    const auto turn = calendar::turn;
    view.observe_light( 0 );
    CHECK( view.ambient_light_at( source ) > dark );
    CHECK( view.ambient_light_at( source ) > view.ambient_light_at( wrong_source ) );
    CHECK_FALSE( get_avatar().has_effect( efftype_id( "haslight" ) ) );
    CHECK( get_avatar().pos_abs() == position );
    CHECK( ( calendar::turn == turn ) );
    const float lit = view.ambient_light_at( source );
    view.observe_light( 0 );
    CHECK( view.ambient_light_at( source ) == Approx( lit ) );
    CHECK_FALSE( get_avatar().has_effect( efftype_id( "haslight" ) ) );
    get_avatar().setpos( here, tripoint_bub_ms( 60, 66, 0 ), false );
    REQUIRE_FALSE( view.inbounds( get_avatar().pos_abs() ) );
    const auto outside_position = get_avatar().pos_abs();
    view.observe_light( 0 );
    CHECK( view.ambient_light_at( source ) == Approx( dark ) );
    CHECK_FALSE( get_avatar().has_effect( efftype_id( "haslight" ) ) );
    CHECK( get_avatar().pos_abs() == outside_position );
    clear_avatar();
    view.add_field( source, field_type_id( "fd_biolum" ), 3 );
    view.observe_light( 0 );
    CHECK( view.ambient_light_at( source ) > dark );
    CHECK( view.get_field_intensity( source, field_type_id( "fd_biolum" ) ) == 3 );
    view.remove_field( source, field_type_id( "fd_biolum" ) );
    auto remote = make_shared_fast<npc>();
    remote->normalize();
    remote->setID( character_id( 956224 ), true );
    const auto remote_at = here.get_abs( tripoint_bub_ms( -12, 66, 0 ) );
    remote->spawn_at_omt( project_to<coords::omt>( remote_at ) );
    overmap_buffer.insert_npc( remote );
    remote->setpos( remote_at, false );
    remote->i_add( lamp );
    REQUIRE( remote->active_light() > 4 );
    REQUIRE_FALSE( here.inbounds( remote->pos_abs() ) );
    on_out_of_scope remove_remote( [&]() {
        if( overmap_buffer.find_npc( remote->getID() ) ) {
            overmap_buffer.remove_npc( remote->getID() );
        }
    } );
    REQUIRE_FALSE( remote->is_active() );
    const auto remote_position = remote->pos_abs();
    view.observe_light( 0 );
    CHECK( view.ambient_light_at( remote->pos_bub( view ) ) > dark );
    CHECK_FALSE( remote->has_effect( efftype_id( "haslight" ) ) );
    CHECK( remote->pos_abs() == remote_position );
    overmap_buffer.remove_npc( remote->getID() );
    set_time_to_day();
    view.observe_light( 0 );
    const float daylight = view.ambient_light_at( source );
    for( int x = 0; x < MAPSIZE * SEEX; ++x ) {
        for( int y = 0; y < MAPSIZE * SEEY; ++y ) {
            view.ter_set( tripoint_bub_ms( x, y, 1 ), ter_str_id( "t_flat_roof" ) );
        }
    }
    view.invalidate_map_cache( 1 );
    view.build_map_cache( 0 );
    view.observe_light( 0 );
    CHECK( view.ambient_light_at( source ) < daylight );
}

TEST_CASE( "existing-only submap retrieval never fills omitted serialized geometry",
           "[binocular_056][existing_geometry]" )
{
    const tripoint_abs_sm stored( 100, 100, 0 );
    const tripoint_abs_sm omitted = stored + point( 1, 0 );
    const auto omt = project_to<coords::omt>( stored );
    const auto segment = project_to<coords::seg>( omt );
    const auto directory = PATH_INFO::current_dimension_save_path() / "maps" /
                           string_format( "%d.%d.%d", segment.x(), segment.y(), segment.z() );
    const auto file = directory / string_format( "%d.%d.%d.map", omt.x(), omt.y(), omt.z() );
    REQUIRE_FALSE( std::filesystem::exists( file.get_unrelative_path() ) );
    std::filesystem::create_directories( directory.get_unrelative_path() );
    on_out_of_scope remove_fixture( [&]() {
        std::filesystem::remove( file.get_unrelative_path() );
        std::filesystem::remove( ( directory + ".zzip" ).get_unrelative_path() );
        std::filesystem::remove( directory.get_unrelative_path() );
    } );
    submap sample;
    sample.set_all_ter( ter_str_id( "t_wall" ) );
    std::ostringstream bytes;
    JsonOut writer( bytes );
    writer.start_array();
    writer.start_object();
    writer.member( "version", savegame_version );
    writer.member( "coordinates", stored.raw() );
    sample.store( writer );
    writer.end_object();
    writer.end_array();
    std::ofstream output( file.get_unrelative_path() );
    output << bytes.str();
    output.close();
    if( world_generator->active_world->has_compression_enabled() ) {
        REQUIRE( zzip::create_from_folder( ( directory + ".zzip" ).get_unrelative_path(),
                                           directory.get_unrelative_path(),
                                           ( PATH_INFO::world_base_save_path() / "maps.dict" ).get_unrelative_path() ) );
    }
    mapbuffer existing;
    CHECK_FALSE( existing.lookup_submap_cached( stored ) );
    CHECK_FALSE( existing.lookup_submap_cached( omitted ) );
    const auto count_before = std::distance( MAPBUFFER.begin(), MAPBUFFER.end() );
    REQUIRE( existing.lookup_submap_existing( stored ) );
    CHECK( existing.lookup_submap_cached( stored ) == existing.lookup_submap_existing( stored ) );
    CHECK( existing.lookup_submap_existing( stored )->get_ter( point_sm_ms( 0, 0 ) ) ==
           ter_str_id( "t_wall" ).id() );
    CHECK_FALSE( existing.lookup_submap_existing( omitted ) );
    CHECK_FALSE( existing.lookup_submap_cached( omitted ) );
    CHECK( std::distance( existing.begin(), existing.end() ) == 1 );
    CHECK( std::distance( MAPBUFFER.begin(), MAPBUFFER.end() ) == count_before );
    SECTION( "ordinary loading completes the partial quad without reloading cached terrain" ) {
        overmap_buffer.ter_set( omt, oter_id( "open_air" ) );
        on_out_of_scope remove_global_fixture( [&]() {
            MAPBUFFER.clear_outside_reality_bubble();
        } );
        submap *original = MAPBUFFER.lookup_submap_existing( stored );
        REQUIRE( original );
        REQUIRE( MAPBUFFER.lookup_submap( omitted ) );
        CHECK( MAPBUFFER.lookup_submap( stored ) == original );
        CHECK( original->get_ter( point_sm_ms( 0, 0 ) ) == ter_str_id( "t_wall" ).id() );
        CHECK( std::distance( MAPBUFFER.begin(), MAPBUFFER.end() ) == count_before + 4 );
    }
}

TEST_CASE( "target vehicle queries ignore unrelated resident submaps and vehicles",
           "[binocular_056][bounded_vehicle_work]" )
{
    clear_avatar();
    clear_npcs();
    clear_map_with_vision( -1, 2, false );
    scoped_weather_override weather( weather_type_id( "clear" ) );
    const auto saved_turn = calendar::turn;
    on_out_of_scope cleanup( [&]() {
        MAPBUFFER.clear_outside_reality_bubble();
        set_time( saved_turn );
    } );
    set_time( calendar::turn_zero + 12_hours );
    map &here = get_map();
    {
        map fixture;
        fixture.load( here.get_abs_sub() + point( -6, 0 ), false );
        std::vector<std::unique_ptr<map>> lighting_context;
        for( const point &offset : {
                 point( -7, -4 ), point( -7, 4 ),
                 point( -6, -4 ), point( -6, 4 )
             } ) {
            auto context = std::make_unique<map>();
            context->load( here.get_abs_sub() + offset, false );
            for( int x = 0; x < MAPSIZE * SEEX; ++x ) {
                for( int y = 0; y < MAPSIZE * SEEY; ++y ) {
                    context->ter_set( tripoint_bub_ms( x, y, 0 ), ter_str_id( "t_grass" ) );
                    context->ter_set( tripoint_bub_ms( x, y, 1 ), ter_str_id( "t_open_air" ) );
                    context->furn_set( tripoint_bub_ms( x, y, 0 ), furn_str_id( "f_null" ) );
                    context->field_at( tripoint_bub_ms( x, y, 0 ) ).clear();
                }
            }
            lighting_context.push_back( std::move( context ) );
        }
        for( int x = 0; x < MAPSIZE * SEEX; ++x ) {
            for( int y = 0; y < MAPSIZE * SEEY; ++y ) {
                fixture.ter_set( tripoint_bub_ms( x, y, 0 ), ter_str_id( "t_grass" ) );
                fixture.ter_set( tripoint_bub_ms( x, y, 1 ), ter_str_id( "t_open_air" ) );
                fixture.furn_set( tripoint_bub_ms( x, y, 0 ), furn_str_id( "f_null" ) );
                fixture.field_at( tripoint_bub_ms( x, y, 0 ) ).clear();
                fixture.i_clear( tripoint_bub_ms( x, y, 0 ) );
            }
        }
        fixture.invalidate_map_cache( 0 );
        fixture.invalidate_map_cache( 1 );
        fixture.build_map_cache( 0 );
        npc observer;
        observer.normalize();
        observer.set_fake( true );
        observer.recalc_sight_limits();
        observer.setpos( here.get_abs( tripoint_bub_ms( -12, 30, 0 ) ), false );
        get_avatar().setpos( here.get_abs( tripoint_bub_ms( -12, 102, 0 ) ), false );
        const std::vector<tripoint_abs_omt> footprint = { get_avatar().pos_abs_omt() };
        scout_observation::site_reader before;
        const auto original = before.read_character( observer, get_avatar(), footprint );
        REQUIRE( original.known );
        REQUIRE( original.visible );
        REQUIRE( before.cached_view_count() > 0 );
        const auto original_queries = before.cached_vehicle_cell_query_count();
        const auto original_parts = before.cached_vehicle_part_read_count();
        const auto original_scans = before.cached_vehicle_scan_count();
        for( int i = 0; i < 2048; ++i ) {
            const tripoint_abs_sm pos( 700000 + i, 800000, 0 );
            REQUIRE_FALSE( MAPBUFFER.lookup_submap_cached( pos ) );
            auto sm = std::make_unique<submap>();
            sm->set_all_ter( ter_str_id( "t_grass" ) );
            // Real physical owners in unrelated cells exercise vehicle admission too.
            if( i % 32 == 0 ) {
                auto unrelated = std::make_unique<vehicle>( vproto_id( "vehicle_camera_test" ) );
                unrelated->sm_pos = pos;
                sm->vehicles.push_back( std::move( unrelated ) );
            }
            REQUIRE( MAPBUFFER.add_submap( pos, sm ) );
        }
        scout_observation::site_reader after;
        const auto populated = after.read_character( observer, get_avatar(), footprint );
        CHECK( populated.known == original.known );
        CHECK( populated.visible == original.visible );
        CAPTURE( original_scans, after.cached_vehicle_scan_count(), original_queries,
                 after.cached_vehicle_cell_query_count(), original_parts,
                 after.cached_vehicle_part_read_count() );
        CHECK( original_scans == 0 );
        CHECK( after.cached_vehicle_scan_count() == 0 );
        CHECK( after.cached_vehicle_cell_query_count() == original_queries );
        CHECK( after.cached_vehicle_part_read_count() == original_parts );
    }
}

TEST_CASE( "resident vehicle part index follows native ownership and geometry",
           "[binocular_056][resident_vehicle_index]" )
{
    SECTION( "admission finds a distant overhang without assuming a vehicle size limit" ) {
        mapbuffer resident;
        const tripoint_abs_sm owner( 900000, 900000, 0 );
        auto sm = std::make_unique<submap>();
        auto body = std::make_unique<vehicle>( vproto_id( "vehicle_camera_test" ) );
        vehicle *v = body.get();
        REQUIRE( v->part_count() > 0 );
        // The actual part is twenty OMT from the serialized owner. Admission
        // uses that owner frame even before native load establishes sm_pos.
        v->part( 0 ).mount = point_rel_ms( -480, 0 );
        v->refresh();
        const auto relative = v->abs_part_pos( v->part( 0 ) ) -
                              project_to<coords::ms>( v->sm_pos );
        const auto pos = project_to<coords::ms>( owner ) + relative;
        const auto cell = project_to<coords::sm>( pos );
        REQUIRE( rl_dist( cell.xy(), owner.xy() ) >= 39 );
        sm->vehicles.push_back( std::move( body ) );
        REQUIRE( resident.add_submap( owner, sm ) );
        const auto contains = [&]( const tripoint_abs_sm & at, const tripoint_abs_ms & part_pos ) {
            const auto &parts = resident.vehicle_parts_at( at );
            return std::any_of( parts.begin(), parts.end(), [&]( const auto & entry ) {
                return entry.first == v && entry.second == part_pos;
            } );
        };
        REQUIRE( contains( cell, pos ) );
        SECTION( "part changes refresh the owned index and discard the old cells" ) {
            v->part( 0 ).mount = point_rel_ms( -600, 0 );
            v->refresh();
            CHECK_FALSE( contains( cell, pos ) );
            const auto next_pos = project_to<coords::ms>( owner ) +
                                  ( v->abs_part_pos( v->part( 0 ) ) - project_to<coords::ms>( v->sm_pos ) );
            CHECK( contains( project_to<coords::sm>( next_pos ), next_pos ) );
            v->part( 0 ).removed = true;
            v->refresh();
            CHECK_FALSE( contains( project_to<coords::sm>( next_pos ), next_pos ) );
        }
        SECTION( "move assignment preserves each physical owner until native refresh" ) {
            const auto other_owner = owner + point( 100, 0 );
            auto other_sm = std::make_unique<submap>();
            auto other_body = std::make_unique<vehicle>( vproto_id( "vehicle_camera_test" ) );
            vehicle *other = other_body.get();
            other->sm_pos = other_owner;
            other->part( 0 ).mount = point_rel_ms( -720, 0 );
            other->refresh();
            other_sm->vehicles.push_back( std::move( other_body ) );
            REQUIRE( resident.add_submap( other_owner, other_sm ) );
            const auto source_pos = other->abs_part_pos( other->part( 0 ) );
            REQUIRE_FALSE( resident.vehicle_parts_at( project_to<coords::sm>( source_pos ) ).empty() );
            *v = std::move( *other );
            v->refresh();
            other->refresh();
            CHECK_FALSE( contains( cell, pos ) );
            const auto next_pos = project_to<coords::ms>( owner ) +
                                  ( v->abs_part_pos( v->part( 0 ) ) - project_to<coords::ms>( v->sm_pos ) );
            CHECK( contains( project_to<coords::sm>( next_pos ), next_pos ) );
            CHECK( resident.vehicle_parts_at( project_to<coords::sm>( source_pos ) ).empty() );
            resident.lookup_submap_cached( other_owner )->vehicles.clear();
            CHECK( contains( project_to<coords::sm>( next_pos ), next_pos ) );
        }
        SECTION( "destruction removes every overhanging reference" ) {
            resident.lookup_submap_cached( owner )->vehicles.clear();
            CHECK( resident.vehicle_parts_at( cell ).empty() );
        }
        resident.clear();
        CHECK( resident.vehicle_parts_at( cell ).empty() );
    }
    SECTION( "movement and detachment update native resident cells" ) {
        clear_map();
        map &here = get_map();
        vehicle *v = here.add_vehicle( vproto_id( "vehicle_camera_test" ),
                                       tripoint_bub_ms( 60, 60, 0 ), 0_degrees, 0,
                                       veh_spawn_status::UNDAMAGED );
        REQUIRE( v );
        const auto original = v->abs_part_pos( v->part( 0 ) );
        const auto count = [&]( const tripoint_abs_ms & pos ) {
            const auto &parts = MAPBUFFER.vehicle_parts_at( project_to<coords::sm>( pos ) );
            return std::count_if( parts.begin(), parts.end(), [&]( const auto & entry ) {
                return entry.first == v && entry.second == pos;
            } );
        };
        REQUIRE( count( original ) > 0 );
        REQUIRE( here.displace_vehicle( *v, tripoint_rel_ms( 12, 0, 0 ) ) );
        CHECK( count( original ) == 0 );
        const auto moved = v->abs_part_pos( v->part( 0 ) );
        REQUIRE( count( moved ) > 0 );
        auto detached = here.detach_vehicle( v );
        REQUIRE( detached.get() == v );
        CHECK( count( moved ) == 0 );
        REQUIRE_FALSE( here.place_vehicle( std::move( detached ) ) );
        // Native ownership reattachment, independently of visibility caches.
        CHECK( count( moved ) > 0 );
        here.destroy_vehicle( v );
        CHECK( count( moved ) == 0 );
    }
}

TEST_CASE( "active structural scouts separate coarse hostile optics from real danger",
           "[scout_observation][bandit][npc]" )
{
    const bool cannibal = GENERATE( false, true );
    const time_point old_turn = calendar::turn;
    const tripoint_abs_ms old_avatar_position = get_avatar().pos_abs();
    const bandit_live_world::world_state old_world =
        overmap_buffer.global_state.bandit_live_world;
    g->faction_manager_ptr->create_if_needed();
    clear_map_without_vision();
    clear_avatar();
    clear_npcs();
    g->clear_zombies();
    set_time_to_day();
    on_out_of_scope restore( [ = ]() {
        clear_npcs();
        g->clear_zombies();
        get_avatar().remove_weapon(); // retire this fixture's native attack weapon
        overmap_buffer.global_state.bandit_live_world = old_world;
        get_avatar().setpos( old_avatar_position, false );
        set_time( old_turn );
        sounds::reset_sounds();
    } );
    map &here = get_map();
    npc &observer = spawn_npc( point_bub_ms( 40, 66 ), "test_talker" );
    npc &partner = spawn_npc( point_bub_ms( 42, 66 ), "test_talker" );
    const faction_id camp_faction( cannibal ? "cannibal_camp" : "hells_raiders" );
    observer.set_fac( camp_faction );
    partner.set_fac( camp_faction );
    observer.i_add( item( itype_id( "binoculars" ) ) );
    observer.set_attitude( NPCATT_NULL );
    partner.set_attitude( NPCATT_NULL );
    observer.set_sleepiness( 0 );
    partner.set_sleepiness( 0 );
    get_avatar().setpos( here, tripoint_bub_ms( 112, 66, 0 ), false );
    set_time_to_day();
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0 );

    bandit_live_world::world_state &world =
        overmap_buffer.global_state.bandit_live_world;
    world = bandit_live_world::world_state();
    bandit_live_world::site_record site;
    site.site_id = cannibal ? "scout-optics-cannibal-site" : "scout-optics-bandit-site";
    site.site_kind = cannibal ? bandit_live_world::owned_site_kind::cannibal_camp :
                     bandit_live_world::owned_site_kind::bandit_camp;
    site.profile = cannibal ? bandit_live_world::hostile_site_profile::cannibal_camp :
                   bandit_live_world::hostile_site_profile::camp_style;
    site.living_total = 2;
    site.members = {
        {
            observer.getID(), "test_talker", observer.pos_abs(),
            bandit_live_world::member_state::outbound, false, ""
        },
        {
            partner.getID(), "test_talker", partner.pos_abs(),
            bandit_live_world::member_state::outbound, false, ""
        }
    };
    bandit_live_world::active_outing_state &outing = site.active_outing;
    outing.schema_version = 10;
    outing.kind = bandit_live_world::outing_kind::structural_sortie;
    outing.activity_id = site.site_id + "-active-scout";
    outing.camp_id = site.site_id;
    outing.generation = 1;
    outing.member_ids = { observer.getID(), partner.getID() };
    outing.leader_id = observer.getID();
    outing.job_type = "scout";
    outing.phase = bandit_live_world::scout_phase::observing;
    outing.owner = bandit_live_world::simulation_owner::abstract;
    outing.handoff_epoch = 0;
    outing.started_minutes = 0;
    outing.last_progress_minutes = 0;
    outing.last_advanced_minutes = 0;
    outing.target_omt = get_avatar().pos_abs_omt();
    outing.target_footprint = { outing.target_omt };
    outing.selected_watch_kind = bandit_live_world::structural_watch_kind::exact;
    outing.selected_watch_omt = observer.pos_abs_omt();
    outing.selected_watch_route_cost = 0;
    world.sites.push_back( std::move( site ) );
    bandit_live_world::site_record &active_site = world.sites.front();
    REQUIRE( bandit_live_world::current_external_simulation_cursor( active_site ) );
    REQUIRE( bandit_live_world::active_operation_duty_site_for( world, observer.getID() ) ==
             &active_site );
    SECTION( "current perception and received attacks keep their Native priority" ) {
        monster *coarse_hostile = g->place_critter_at( mtype_id( "mon_dog_gpyrenees" ),
                                  tripoint_bub_ms( 43, 66, 0 ) );
        REQUIRE( coarse_hostile );
        coarse_hostile->anger = 0;
        coarse_hostile->morale = 0;
        REQUIRE( coarse_hostile->has_flag( mon_flag_HIT_AND_RUN ) );
        REQUIRE( observer.sees_without_clairvoyance( here, *coarse_hostile ) );
        REQUIRE( observer.attitude_to( *coarse_hostile ) == Creature::Attitude::HOSTILE );
        REQUIRE( coarse_hostile->attitude( &observer ) == MATT_IGNORE );
        CHECK_FALSE( observer.scouting_creature_requires_response( *coarse_hostile,
                     "test_coarse_hostile_ignore" ) );
        scout_observation::site_reader optics;
        CHECK( optics.watching_member( active_site, observer ) );
        coarse_hostile->add_effect( efftype_id( "run" ), 1_minutes );
        REQUIRE( coarse_hostile->attitude( &observer ) == MATT_FLEE );
        CHECK_FALSE( observer.scouting_creature_requires_response( *coarse_hostile,
                     "test_coarse_hostile_flee" ) );
        CHECK( optics.watching_member( active_site, observer ) );

        g->clear_zombies();
        monster *actual_attacker = g->place_critter_at( mtype_id( "mon_zombie" ),
                                   tripoint_bub_ms( 41, 66, 0 ) );
        REQUIRE( actual_attacker );
        actual_attacker->anger = 100;
        REQUIRE( observer.sees_without_clairvoyance( here, *actual_attacker ) );
        REQUIRE( actual_attacker->attitude( &observer ) == MATT_ATTACK );
        REQUIRE( actual_attacker->type->melee_dice > 0 );
        REQUIRE( actual_attacker->can_reach_to( observer.pos_bub() ) );
        CHECK( observer.scouting_creature_requires_response( *actual_attacker,
                "test_native_melee_threat" ) );
        CHECK_FALSE( optics.watching_member( active_site, observer ) );

        g->clear_zombies();
        get_avatar().setpos( here, observer.pos_bub() + point::north, false );
        get_avatar().set_skill_level( skill_id( "melee" ), 8 );
        get_avatar().set_skill_level( skill_id( "bashing" ), 8 );
        // Random native doctor equipment can absorb every unarmed strike.
        // Establish a real injury without removing the observer's optics or
        // armor, or replacing native combat with direct damage injection.
        item bat( itype_id( "bat" ) );
        REQUIRE( get_avatar().wield( bat ) );
        const int initial_hp = observer.get_hp();
        std::vector<int> attempted_attacks;
        std::vector<int> spent_moves;
        const int starting_dodges = observer.get_dodges_left();
        for( int hit = 0; hit < 5 && observer.get_hp() == initial_hp; ++hit ) {
            get_avatar().set_moves( 100 );
            attempted_attacks.push_back( get_avatar().melee_attack( observer, false ) ? 1 : 0 );
            spent_moves.push_back( 100 - get_avatar().get_moves() );
        }
        CAPTURE( cannibal, attempted_attacks, spent_moves, starting_dodges,
                 observer.get_dodges_left(), get_avatar().get_str(), observer.get_dodge(),
                 get_avatar().pos_abs(), observer.pos_abs(),
                 observer.get_armor_type( damage_type_id( "bash" ), bodypart_id( "torso" ) ),
                 observer.get_armor_type( damage_type_id( "bash" ), bodypart_id( "arm_l" ) ),
                 get_avatar().is_adjacent( &observer, false ) );
        REQUIRE( observer.get_hp() < initial_hp );
        REQUIRE( observer.faction_alarm );
        REQUIRE( observer.faction_alarm->received_attack.value_or( false ) );
        REQUIRE( observer.faction_alarm_requires_response() );

        get_avatar().setpos( here, tripoint_bub_ms( 112, 66, 0 ), false );
        monster *unseen_hostile = g->place_critter_at( mtype_id( "mon_zombie" ),
                                  tripoint_bub_ms( 20, 66, 0 ) );
        REQUIRE( unseen_hostile );
        unseen_hostile->anger = 100;
        unseen_hostile->setpos( here.get_abs( tripoint_bub_ms( -8, 66, 0 ) ) );
        REQUIRE_FALSE( here.inbounds( unseen_hostile->pos_abs() ) );
        REQUIRE( rl_dist( observer.pos_abs(), unseen_hostile->pos_abs() ) <= MAX_VIEW_DISTANCE );
        scout_observation::site_reader unknown_optics;
        const scout_observation::visibility_read unknown = unknown_optics.read(
                    observer, observer.pos_abs(), unseen_hostile->pos_abs() );
        INFO( unknown.reason );
        CHECK_FALSE( unknown.known );
        CHECK( unknown_optics.perceived_threats( observer ).empty() );
        CHECK( observer.faction_alarm_requires_response() );
        CHECK_FALSE( unknown_optics.watching_member( active_site, observer ) );
        CHECK( unknown_optics.read_character( observer, get_avatar() ).visible );
        CHECK( unknown_optics.read_signal( observer, observer.pos_abs(), get_avatar().pos_abs(),
        { get_avatar().pos_abs_omt() } ).visible );

        std::ostringstream bytes;
        JsonOut writer( bytes );
        observer.serialize( writer );
        npc loaded;
        loaded.deserialize( json_loader::from_string( bytes.str() ).get_object() );
        loaded.set_fac( camp_faction );
        CHECK( loaded.getID() == observer.getID() );
        CHECK( bandit_live_world::active_operation_duty_site_for( world, loaded.getID() ) ==
               &active_site );
        CHECK( loaded.faction_alarm_requires_response() );
        CHECK( unknown_optics.read_character( loaded, get_avatar() ).visible );
        CHECK( unknown_optics.read_signal( loaded, loaded.pos_abs(), get_avatar().pos_abs(),
        { get_avatar().pos_abs_omt() } ).visible );
        SECTION( "incapacitated or field-blocked partner retains actual perceived threats" ) {
            const bool field = GENERATE( false, true );
            g->clear_zombies();
            monster *threat = g->place_critter_at( mtype_id( "mon_zombie" ),
                                                   partner.pos_bub() + point::east );
            REQUIRE( threat );
            threat->anger = 100;
            REQUIRE( partner.sees_without_clairvoyance( here, *threat ) );
            REQUIRE( partner.scouting_creature_requires_response( *threat, "partner_route_knowledge" ) );
            REQUIRE( partner.raise_faction_alarm( threat->pos_abs() ) );
            on_out_of_scope remove_field( [&]() {
                if( field ) {
                    here.remove_field( partner.pos_bub(), field_type_id( "fd_fire" ) );
                }
            } );
            if( field ) {
                REQUIRE( here.add_field( partner.pos_bub(), field_type_id( "fd_fire" ), 2 ) );
            } else {
                partner.add_effect( efftype_id( "stunned" ), 1_minutes );
            }
            REQUIRE( partner.sees_without_clairvoyance( here, *threat ) );
            CHECK_FALSE( optics.watching_member( active_site, partner ) );
            const auto positions = optics.perceived_threats( partner );
            CHECK( std::find( positions.begin(), positions.end(), threat->pos_abs() ) != positions.end() );
            partner.fall_asleep( 1_minutes );
            CHECK( optics.perceived_threats( partner ).empty() );
            CHECK( partner.has_active_faction_alarm() );
            CHECK( partner.faction_alarm_requires_response() );
        }
    }
    SECTION( "bounded watch avoidance does not impose an incident-distance wall on home routes" ) {
        const tripoint_abs_omt origin = observer.pos_abs_omt();
        active_site.anchor = origin + point( -2, 0 );
        active_site.footprint = { active_site.anchor };
        std::vector<std::pair<tripoint_abs_omt, oter_id>> saved_terrain;
        for( int y = -3; y <= 3; ++y ) {
            for( int x = -3; x <= 3; ++x ) {
                const auto cell = origin + point( x, y );
                saved_terrain.emplace_back( cell, overmap_buffer.ter( cell ) );
                overmap_buffer.ter_set( cell, oter_id( "field" ) );
            }
        }
        on_out_of_scope restore_terrain( [&]() {
            for( const auto &entry : saved_terrain ) {
                overmap_buffer.ter_set( entry.first, entry.second );
            }
        } );
        // Declared authenticated incident stress input through the existing
        // alarm API, not a claim that this fixture witnessed a remote attack.
        // Neither alarm authentication nor reception bounds incident distance.
        const tripoint_abs_omt remote = origin + point( -10000, 0 );
        REQUIRE( partner.raise_faction_alarm( project_to<coords::ms>( remote ), true ) );
        REQUIRE( partner.has_active_faction_alarm() );
        REQUIRE( partner.faction_alarm_requires_response() );
        const auto toward = origin + point( -1, -1 );
        CHECK( live_bandit_member_route_to_for_test( observer, active_site, toward ).empty() );
        CHECK_FALSE( live_bandit_member_route_to_for_test( observer, active_site,
                     origin + point( 0, -1 ) ).empty() );
        const auto home = live_bandit_member_route_to_for_test( observer, active_site, active_site.anchor );
        REQUIRE_FALSE( home.empty() );
        CHECK( home.front() == active_site.anchor );
        // Native overmap paths retain the source as their last entry; the
        // ordinary mover consumes that entry before taking the preceding step.
        REQUIRE( home.size() > 1 );
        CHECK( home.back() == origin );
        CHECK( rl_dist( home[home.size() - 2], remote ) < rl_dist( origin, remote ) );
        auto returning = active_site;
        returning.active_outing.phase = bandit_live_world::scout_phase::returning_home;
        CHECK_FALSE( live_bandit_member_route_to_for_test( observer, returning, toward ).empty() );
        CHECK( partner.faction_alarm_requires_response() );
    }
}
