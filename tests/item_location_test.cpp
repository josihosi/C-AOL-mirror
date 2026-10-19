#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "cata_catch.h"
#include "active_item_cache.h"
#include "character.h"
#include "coordinates.h"
#include "debug.h"
#include "enums.h"
#include "flexbuffer_json.h"
#include "inventory.h"
#include "item.h"
#include "item_location.h"
#include "item_pocket.h"
#include "item_uid.h"
#include "json.h"
#include "json_loader.h"
#include "map.h"
#include "map_helpers.h"
#include "map_selector.h"
#include "pimpl.h"
#include "player_helpers.h"
#include "pocket_type.h"
#include "units.h"
#include "vehicle.h"
#include "vehicle_selector.h"
#include "veh_type.h"
#include "vpart_position.h"
#include "vpart_range.h"
#include "point.h"
#include "ret_val.h"
#include "rng.h"
#include "type_id.h"
#include "visitable.h"

static const itype_id itype_backpack( "backpack" );
static const itype_id itype_jeans( "jeans" );
static const itype_id itype_tshirt( "tshirt" );
static const vproto_id vehicle_prototype_bicycle( "bicycle" );
static const vproto_id vehicle_prototype_car( "car" );

TEST_CASE( "item_location_can_maintain_reference_despite_item_removal", "[item][item_location]" )
{
    clear_map_without_vision();
    map &m = get_map();
    tripoint_bub_ms pos( 60, 60, 0 );
    m.i_clear( pos );
    m.add_item( pos, item( itype_jeans ) );
    m.add_item( pos, item( itype_jeans ) );
    m.add_item( pos, item( itype_tshirt ) );
    m.add_item( pos, item( itype_jeans ) );
    m.add_item( pos, item( itype_jeans ) );
    map_cursor cursor( pos );
    item *tshirt = nullptr;
    cursor.visit_items( [&tshirt]( item * i, item * ) {
        if( i->typeId() == itype_tshirt ) {
            tshirt = i;
            return VisitResponse::ABORT;
        }
        return VisitResponse::NEXT;
    } );
    REQUIRE( tshirt != nullptr );
    item_location item_loc( cursor, tshirt );
    REQUIRE( item_loc->typeId() == itype_tshirt );
    for( int j = 0; j < 4; ++j ) {
        // Delete up to 4 random jeans
        map_stack stack = m.i_at( pos );
        REQUIRE( !stack.empty() );
        item *i = &random_entry_opt( stack )->get();
        if( i->typeId() == itype_jeans ) {
            m.i_rem( pos, i );
        }
    }
    CAPTURE( m.i_at( pos ) );
    REQUIRE( item_loc );
    CHECK( item_loc->typeId() == itype_tshirt );
}

TEST_CASE( "item_location_doesnt_return_stale_map_item", "[item][item_location]" )
{
    clear_map_without_vision();
    map &m = get_map();
    tripoint_bub_ms pos( 60, 60, 0 );
    m.i_clear( pos );
    m.add_item( pos, item( itype_tshirt ) );
    item_location item_loc( map_cursor( pos ), &m.i_at( pos ).only_item() );
    REQUIRE( item_loc->typeId() == itype_tshirt );
    m.i_rem( pos, &*item_loc );
    m.add_item( pos, item( itype_jeans ) );
    CHECK( !item_loc );
}

TEST_CASE( "item_in_container", "[item][item_location]" )
{
    Character &dummy = get_player_character();
    clear_avatar();
    item_location backpack = dummy.i_add( item( itype_backpack ) );
    item jeans( itype_jeans );

    REQUIRE( dummy.has_item( *backpack ) );

    backpack->put_in( jeans, pocket_type::CONTAINER );

    item_location backpack_loc( dummy, & **dummy.wear_item( *backpack ) );

    REQUIRE( dummy.has_item( *backpack_loc ) );

    item_location jeans_loc( backpack_loc, &backpack_loc->only_item() );

    REQUIRE( backpack_loc.where() == item_location::type::character );
    REQUIRE( jeans_loc.where() == item_location::type::container );
    const int obtain_cost_calculation = dummy.item_handling_cost( *jeans_loc, true,
                                        backpack_loc->obtain_cost( *jeans_loc ) );
    CHECK( obtain_cost_calculation == jeans_loc.obtain_cost( dummy ) );

    CHECK( jeans_loc.parent_item() == backpack_loc );
}

// Helper: serialize an item_location to a JSON string
static std::string serialize_item_location( const item_location &loc )
{
    std::ostringstream os;
    JsonOut jsout( os );
    loc.serialize( jsout );
    return os.str();
}

// Helper: deserialize an item_location from a JSON string
static item_location deserialize_item_location( const std::string &json_str )
{
    item_location loc;
    JsonValue jv = json_loader::from_string( json_str );
    JsonObject jo = jv;
    loc.deserialize( jo );
    return loc;
}

TEST_CASE( "items_get_unique_uids", "[item][item_uid]" )
{
    item a( itype_jeans );
    item b( itype_jeans );
    item c( itype_tshirt );

    CHECK( a.uid().is_valid() );
    CHECK( b.uid().is_valid() );
    CHECK( c.uid().is_valid() );

    CHECK( a.uid().get_value() != b.uid().get_value() );
    CHECK( a.uid().get_value() != c.uid().get_value() );
    CHECK( b.uid().get_value() != c.uid().get_value() );

    // Default-constructed item has invalid UID
    item null_item;
    CHECK_FALSE( null_item.uid().is_valid() );
}

TEST_CASE( "item_copies_get_new_uids", "[item][item_uid]" )
{
    item a( itype_jeans );
    int64_t a_uid = a.uid().get_value();
    REQUIRE( a.uid().is_valid() );

    // Copy construction - NOLINT: b is intentionally a copy, not const, to test uid semantics
    item b( a ); // NOLINT(performance-unnecessary-copy-initialization)
    CHECK( b.uid().is_valid() );
    CHECK( b.uid().get_value() != a_uid );
    // Original unchanged
    CHECK( a.uid().get_value() == a_uid );

    // Copy assignment
    item c( itype_tshirt );
    int64_t c_old_uid = c.uid().get_value();
    c = a;
    CHECK( c.uid().is_valid() );
    CHECK( c.uid().get_value() != a_uid );
    CHECK( c.uid().get_value() != c_old_uid );
}

TEST_CASE( "item_uid_survives_serialization", "[item][item_uid]" )
{
    item original( itype_jeans );
    REQUIRE( original.uid().is_valid() );
    int64_t original_uid = original.uid().get_value();

    // Serialize
    std::ostringstream os;
    JsonOut jsout( os );
    original.serialize( jsout );
    std::string json_str = os.str();

    // Deserialize
    item loaded;
    JsonValue jv = json_loader::from_string( json_str );
    JsonObject jo = jv;
    loaded.deserialize( jo );

    CHECK( loaded.uid().is_valid() );
    CHECK( loaded.uid().get_value() == original_uid );
}

TEST_CASE( "item_location_in_container_survives_restack", "[item][item_location][item_uid]" )
{
    clear_map_without_vision();
    map &m = get_map();
    tripoint_bub_ms pos( 60, 60, 0 );
    m.i_clear( pos );

    // Place a backpack on the ground with items inside
    item backpack_item( itype_backpack );
    item jeans1( itype_jeans );
    item tshirt( itype_tshirt );
    item jeans2( itype_jeans );
    backpack_item.put_in( jeans1, pocket_type::CONTAINER );
    backpack_item.put_in( tshirt, pocket_type::CONTAINER );
    backpack_item.put_in( jeans2, pocket_type::CONTAINER );

    m.add_item( pos, backpack_item );
    item &placed_backpack = m.i_at( pos ).only_item();

    // Find the tshirt inside the backpack
    item *tshirt_ptr = nullptr;
    for( item *it : placed_backpack.all_items_container_top() ) {
        if( it->typeId() == itype_tshirt ) {
            tshirt_ptr = it;
            break;
        }
    }
    REQUIRE( tshirt_ptr != nullptr );

    // Create item_location pointing to tshirt inside backpack
    item_location backpack_loc( map_cursor( pos ), &placed_backpack );
    item_location tshirt_loc( backpack_loc, tshirt_ptr );
    REQUIRE( tshirt_loc->typeId() == itype_tshirt );

    // Serialize
    std::string json_str = serialize_item_location( tshirt_loc );

    // Trigger restack (merges the two jeans, changing indices)
    for( item_pocket *pkt : placed_backpack.get_standard_pockets() ) {
        pkt->restack();
    }

    // Deserialize - should find tshirt by UID despite index shift
    item_location loaded = deserialize_item_location( json_str );
    REQUIRE( loaded );
    CHECK( loaded->typeId() == itype_tshirt );
}

TEST_CASE( "item_location_in_container_survives_removal", "[item][item_location][item_uid]" )
{
    clear_map_without_vision();
    map &m = get_map();
    tripoint_bub_ms pos( 60, 60, 0 );
    m.i_clear( pos );

    // Place a backpack on the ground with items inside
    item backpack_item( itype_backpack );
    item jeans_a( itype_jeans );
    item tshirt( itype_tshirt );
    backpack_item.put_in( jeans_a, pocket_type::CONTAINER );
    backpack_item.put_in( tshirt, pocket_type::CONTAINER );

    m.add_item( pos, backpack_item );
    item &placed_backpack = m.i_at( pos ).only_item();

    // Find items
    item *tshirt_ptr = nullptr;
    item *jeans_ptr = nullptr;
    for( item *it : placed_backpack.all_items_container_top() ) {
        if( it->typeId() == itype_tshirt ) {
            tshirt_ptr = it;
        } else if( it->typeId() == itype_jeans ) {
            jeans_ptr = it;
        }
    }
    REQUIRE( tshirt_ptr != nullptr );
    REQUIRE( jeans_ptr != nullptr );

    // Create item_location pointing to tshirt
    item_location backpack_loc( map_cursor( pos ), &placed_backpack );
    item_location tshirt_loc( backpack_loc, tshirt_ptr );

    // Serialize
    std::string json_str = serialize_item_location( tshirt_loc );

    // Remove jeans (shifts indices)
    placed_backpack.remove_item( *jeans_ptr );

    // Deserialize - should find tshirt by UID despite index shift
    item_location loaded = deserialize_item_location( json_str );
    REQUIRE( loaded );
    CHECK( loaded->typeId() == itype_tshirt );
}

TEST_CASE( "item_location_in_container_uid_miss_becomes_nowhere",
           "[item][item_location][item_uid]" )
{
    clear_map_without_vision();
    map &m = get_map();
    tripoint_bub_ms pos( 60, 60, 0 );
    m.i_clear( pos );

    // Place a backpack with a tshirt inside
    item backpack_item( itype_backpack );
    item tshirt( itype_tshirt );
    backpack_item.put_in( tshirt, pocket_type::CONTAINER );

    m.add_item( pos, backpack_item );
    item &placed_backpack = m.i_at( pos ).only_item();

    item *tshirt_ptr = nullptr;
    for( item *it : placed_backpack.all_items_container_top() ) {
        if( it->typeId() == itype_tshirt ) {
            tshirt_ptr = it;
            break;
        }
    }
    REQUIRE( tshirt_ptr != nullptr );

    item_location backpack_loc( map_cursor( pos ), &placed_backpack );
    item_location tshirt_loc( backpack_loc, tshirt_ptr );

    // Serialize
    std::string json_str = serialize_item_location( tshirt_loc );

    // Remove the target item entirely
    placed_backpack.remove_item( *tshirt_ptr );

    // Put a different item so index 0 exists but is wrong
    item jeans_replacement( itype_jeans );
    placed_backpack.put_in( jeans_replacement, pocket_type::CONTAINER );

    // Deserialize - UID present but item gone, should become nowhere
    // (NOT fall back to index and bind to the jeans)
    item_location loaded;
    std::string dmsg = capture_debugmsg_during( [&]() {
        loaded = deserialize_item_location( json_str );
    } );
    CHECK_FALSE( loaded );
    CHECK_FALSE( dmsg.empty() );
}

TEST_CASE( "item_location_on_map_survives_removal", "[item][item_location][item_uid]" )
{
    clear_map_without_vision();
    map &m = get_map();
    tripoint_bub_ms pos( 60, 60, 0 );
    m.i_clear( pos );

    // Place multiple items on the map
    m.add_item( pos, item( itype_jeans ) );
    m.add_item( pos, item( itype_tshirt ) );
    m.add_item( pos, item( itype_jeans ) );

    // Find the tshirt
    item *tshirt_ptr = nullptr;
    item *first_jeans = nullptr;
    for( item &it : m.i_at( pos ) ) {
        if( it.typeId() == itype_tshirt ) {
            tshirt_ptr = &it;
        } else if( it.typeId() == itype_jeans && !first_jeans ) {
            first_jeans = &it;
        }
    }
    REQUIRE( tshirt_ptr != nullptr );
    REQUIRE( first_jeans != nullptr );

    item_location tshirt_loc( map_cursor( pos ), tshirt_ptr );
    REQUIRE( tshirt_loc->typeId() == itype_tshirt );

    // Serialize
    std::string json_str = serialize_item_location( tshirt_loc );

    // Remove the first jeans (shifts indices)
    m.i_rem( pos, first_jeans );

    // Deserialize and access (triggers ensure_unpacked for map locations)
    item_location loaded = deserialize_item_location( json_str );
    REQUIRE( loaded );
    CHECK( loaded->typeId() == itype_tshirt );
}

TEST_CASE( "item_location_deserialize_without_uid_falls_back_to_index",
           "[item][item_location][item_uid]" )
{
    clear_map_without_vision();
    map &m = get_map();
    tripoint_bub_ms pos( 60, 60, 0 );
    m.i_clear( pos );

    // Place items on map
    m.add_item( pos, item( itype_jeans ) );
    m.add_item( pos, item( itype_tshirt ) );

    // Hand-craft old-format JSON (no "uid" field) pointing to index 1
    tripoint_abs_ms abs_pos = m.get_abs( pos );
    std::ostringstream os;
    JsonOut jsout( os );
    jsout.start_object();
    jsout.member( "type", "map" );
    jsout.member( "position", abs_pos );
    jsout.member( "idx", 1 );
    jsout.end_object();
    std::string json_str = os.str();

    // Deserialize - should use index fallback since no UID
    item_location loaded = deserialize_item_location( json_str );
    REQUIRE( loaded );
    CHECK( loaded->typeId() == itype_tshirt );
}

TEST_CASE( "item_location_on_person_survives_restack", "[item][item_location][item_uid]" )
{
    clear_map_without_vision();
    Character &dummy = get_player_character();
    clear_avatar();

    // Add items directly to inventory
    item_location jeans1_loc = dummy.i_add( item( itype_jeans ) );
    item_location tshirt_loc = dummy.i_add( item( itype_tshirt ) );
    item_location jeans2_loc = dummy.i_add( item( itype_jeans ) );

    REQUIRE( tshirt_loc );
    REQUIRE( tshirt_loc->typeId() == itype_tshirt );

    // Serialize the tshirt location
    std::string json_str = serialize_item_location( tshirt_loc );

    // Restack inventory (may merge the two jeans, shifting indices)
    dummy.inv->restack( dummy );

    // Deserialize - should find tshirt by UID
    item_location loaded = deserialize_item_location( json_str );
    REQUIRE( loaded );
    CHECK( loaded->typeId() == itype_tshirt );
}

TEST_CASE( "item_location_on_vehicle_base_item", "[item][item_location][item_uid]" )
{
    clear_map_without_vision();
    map &m = get_map();
    clear_vehicles();

    // Place a simple vehicle
    tripoint_bub_ms veh_pos( 60, 60, 0 );
    vehicle *veh = m.add_vehicle( vehicle_prototype_bicycle, veh_pos, 0_degrees, -1,
                                  veh_spawn_status::PRISTINE );
    REQUIRE( veh != nullptr );

    // Get a vehicle part and create item_location for its base item
    int part_idx = 0;
    const item &base = veh->part( part_idx ).get_base();

    vehicle_cursor cur( *veh, part_idx );
    item_location base_loc( cur, const_cast<item *>( &base ) );
    REQUIRE( base_loc );

    // Serialize - base items omit idx and uid
    std::string json_str = serialize_item_location( base_loc );

    // Verify the serialized JSON omits idx and uid (the base item invariant)
    JsonValue jv_check = json_loader::from_string( json_str );
    JsonObject jo_check = jv_check;
    jo_check.allow_omitted_members();
    CHECK( jo_check.get_string( "type" ) == "vehicle" );
    CHECK_FALSE( jo_check.has_member( "idx" ) );
    CHECK_FALSE( jo_check.has_member( "uid" ) );

    // Deserialize - should resolve via idx < 0 path (no uid needed)
    item_location loaded = deserialize_item_location( json_str );
    REQUIRE( loaded );
    CHECK( loaded->typeId() == base.typeId() );
}

TEST_CASE( "item_location_on_vehicle_cargo_survives_removal",
           "[item][item_location][item_uid]" )
{
    clear_map_without_vision();
    map &m = get_map();
    clear_vehicles();

    // Place a vehicle with cargo -- use a car which has trunk/cargo
    // Status 0 = fully intact (no random damage that could break the cargo part)
    tripoint_bub_ms veh_pos( 60, 60, 0 );
    vehicle *veh = m.add_vehicle( vehicle_prototype_car, veh_pos, 0_degrees, 0,
                                  veh_spawn_status::UNDAMAGED );
    REQUIRE( veh != nullptr );

    // Find the largest cargo part (trunk)
    int cargo_part_idx = -1;
    units::volume best_vol = 0_ml;
    for( const vpart_reference &vpr : veh->get_all_parts() ) {
        if( vpr.has_feature( "CARGO" ) ) {
            units::volume vol = vpr.info().size;
            if( vol > best_vol ) {
                best_vol = vol;
                cargo_part_idx = vpr.part_index();
            }
        }
    }
    REQUIRE( cargo_part_idx >= 0 );

    // Clear any randomly-spawned items so the cargo has room for test items
    vehicle_stack cargo = veh->get_items( veh->part( cargo_part_idx ) );
    while( !cargo.empty() ) {
        cargo.erase( cargo.begin() );
    }

    // Add items to cargo
    std::optional<vehicle_stack::iterator> added_jeans =
        veh->add_item( m, veh->part( cargo_part_idx ), item( itype_jeans ) );
    REQUIRE( added_jeans.has_value() );
    std::optional<vehicle_stack::iterator> added_tshirt =
        veh->add_item( m, veh->part( cargo_part_idx ), item( itype_tshirt ) );
    REQUIRE( added_tshirt.has_value() );

    vehicle_cursor cur( *veh, cargo_part_idx );
    item_location tshirt_loc( cur, & **added_tshirt );
    REQUIRE( tshirt_loc->typeId() == itype_tshirt );

    // Serialize
    std::string json_str = serialize_item_location( tshirt_loc );

    // Remove jeans (shifts indices)
    veh->remove_item( veh->part( cargo_part_idx ), *added_jeans );

    // Deserialize - should find tshirt by UID
    item_location loaded = deserialize_item_location( json_str );
    REQUIRE( loaded );
    CHECK( loaded->typeId() == itype_tshirt );
}

TEST_CASE( "expired_item_location_serializes_as_nowhere", "[item][item_location][item_uid]" )
{
    clear_map_without_vision();
    map &m = get_map();
    tripoint_bub_ms pos( 60, 60, 0 );
    m.i_clear( pos );

    m.add_item( pos, item( itype_tshirt ) );
    item_location loc( map_cursor( pos ), &m.i_at( pos ).only_item() );
    REQUIRE( loc );
    REQUIRE( loc->typeId() == itype_tshirt );

    // Remove the target item, invalidating the safe_reference
    m.i_rem( pos, &*loc );

    // The item_location's safe_reference is now expired.
    // Serializing should produce "type": "null" (nowhere), not a bogus map+idx.
    std::string json_str = serialize_item_location( loc );

    // Check that the serialized JSON is a nowhere location
    JsonValue jv = json_loader::from_string( json_str );
    JsonObject jo = jv;
    std::string type = jo.get_string( "type" );
    CHECK( type == "null" );
}

TEST_CASE( "expired_vehicle_cargo_item_location_serializes_as_nowhere",
           "[item][item_location][item_uid]" )
{
    clear_map_without_vision();
    map &m = get_map();
    clear_vehicles();

    tripoint_bub_ms veh_pos( 60, 60, 0 );
    vehicle *veh = m.add_vehicle( vehicle_prototype_car, veh_pos, 0_degrees, 0,
                                  veh_spawn_status::UNDAMAGED );
    REQUIRE( veh != nullptr );

    // Find the largest cargo part
    int cargo_part_idx = -1;
    units::volume best_vol = 0_ml;
    for( const vpart_reference &vpr : veh->get_all_parts() ) {
        if( vpr.has_feature( "CARGO" ) ) {
            units::volume vol = vpr.info().size;
            if( vol > best_vol ) {
                best_vol = vol;
                cargo_part_idx = vpr.part_index();
            }
        }
    }
    REQUIRE( cargo_part_idx >= 0 );

    // Clear any randomly-spawned items so the cargo has room for test items
    vehicle_stack cargo2 = veh->get_items( veh->part( cargo_part_idx ) );
    while( !cargo2.empty() ) {
        cargo2.erase( cargo2.begin() );
    }

    // Add an item to cargo
    std::optional<vehicle_stack::iterator> added =
        veh->add_item( m, veh->part( cargo_part_idx ), item( itype_tshirt ) );
    REQUIRE( added.has_value() );

    vehicle_cursor cur( *veh, cargo_part_idx );
    item_location loc( cur, & **added );
    REQUIRE( loc );

    // Remove the item, invalidating the safe_reference
    veh->remove_item( veh->part( cargo_part_idx ), *added );

    // Serializing should produce "type": "null", not a partial vehicle object
    // that would be misread as a base item (idx defaults to -1 on load)
    std::string json_str = serialize_item_location( loc );

    JsonValue jv = json_loader::from_string( json_str );
    JsonObject jo = jv;
    std::string type = jo.get_string( "type" );
    CHECK( type == "null" );
}

TEST_CASE( "item_location_in_container_deserialize_without_uid_falls_back_to_index",
           "[item][item_location][item_uid]" )
{
    clear_map_without_vision();
    map &m = get_map();
    tripoint_bub_ms pos( 60, 60, 0 );
    m.i_clear( pos );

    // Place a backpack with items inside
    item backpack_item( itype_backpack );
    backpack_item.put_in( item( itype_jeans ), pocket_type::CONTAINER );
    backpack_item.put_in( item( itype_tshirt ), pocket_type::CONTAINER );

    m.add_item( pos, backpack_item );
    item &placed_backpack = m.i_at( pos ).only_item();

    // First, serialize the parent (backpack on map) normally -- it has a UID
    item_location backpack_loc( map_cursor( pos ), &placed_backpack );

    // Find the actual index of the tshirt in the container
    int tshirt_idx = -1;
    int idx = 0;
    for( const item *it : placed_backpack.all_items_container_top() ) {
        if( it->typeId() == itype_tshirt ) {
            tshirt_idx = idx;
            break;
        }
        idx++;
    }
    REQUIRE( tshirt_idx >= 0 );

    // Hand-craft old-format in_container JSON: has idx but no uid
    std::ostringstream os;
    JsonOut jsout( os );
    jsout.start_object();
    jsout.member( "idx", tshirt_idx );
    jsout.member( "type", "in_container" );
    jsout.member( "parent", backpack_loc );
    jsout.end_object();
    std::string json_str = os.str();

    // Deserialize - should use index fallback since no UID
    item_location loaded = deserialize_item_location( json_str );
    REQUIRE( loaded );
    CHECK( loaded->typeId() == itype_tshirt );
}


TEST_CASE( "item_location_detach_keeps_the_native_storage_identity",
           "[item][item_location][item_uid][companion_repair]" )
{
    clear_map_without_vision();
    clear_vehicles();
    map &here = get_map();
    const bool base_part = GENERATE( false, true );
    vehicle *veh = here.add_vehicle( vehicle_prototype_car, tripoint_bub_ms( 60, 60, 0 ),
                                     0_degrees, 0, veh_spawn_status::UNDAMAGED );
    REQUIRE( veh != nullptr );
    int part = 0;
    item_location selected;
    if( base_part ) {
        const item &base = veh->part( part ).get_base();
        selected = item_location( vehicle_cursor( *veh, part ), const_cast<item *>( &base ) );
    } else {
        part = -1;
        for( const vpart_reference &vpr : veh->get_all_parts() ) {
            if( vpr.has_feature( "CARGO" ) ) {
                part = vpr.part_index();
                break;
            }
        }
        REQUIRE( part >= 0 );
        vehicle_stack cargo = veh->get_items( veh->part( part ) );
        while( !cargo.empty() ) {
            cargo.erase( cargo.begin() );
        }
        item coat( itype_id( "jacket_leather" ), calendar::turn );
        REQUIRE( coat.put_in( item( itype_id( "rock" ), calendar::turn ),
                              pocket_type::CONTAINER ).success() );
        const auto added = veh->add_item( here, veh->part( part ), coat );
        REQUIRE( added.has_value() );
        selected = item_location( vehicle_cursor( *veh, part ), & **added );
    }
    const int64_t uid = selected->uid().get_value();
    const int64_t content_uid = base_part ? 0 : selected->only_item().uid().get_value();
    const itype_id type = selected->typeId();
    item extracted = selected.detach();
    CHECK_FALSE( selected );
    CHECK( extracted.typeId() == type );
    CHECK( extracted.uid().get_value() == uid );
    if( base_part ) {
        CHECK( veh->part( part ).removed );
    } else {
        CHECK( extracted.only_item().uid().get_value() == content_uid );
        CHECK( veh->get_items( veh->part( part ) ).empty() );
    }
}

TEST_CASE( "generic_extraction_preserves_nested_identity_and_expires_storage_cache",
           "[item][item_location][companion_repair][transfer_cache]" )
{
    clear_avatar();
    clear_map_without_vision();
    clear_vehicles();
    map &here = get_map();
    Character &who = get_player_character();
    const tripoint_bub_ms pos( 60, 60, 0 );
    const std::string storage = GENERATE( "map_single", "map_bulk", "vehicle_single",
                                          "vehicle_bulk", "inventory_bulk", "pocket_single",
                                          "worn_bulk", "weapon" );
    CAPTURE( storage );
    item gun( itype_id( "glock_19" ), calendar::turn );
    gun.ammo_set( itype_id( "9mm" ), 3 );
    REQUIRE( gun.magazine_current() != nullptr );
    gun.magazine_current()->active = true;
    REQUIRE( gun.magazine_current()->all_items_top( pocket_type::MAGAZINE ).size() == 1 );
    gun.magazine_current()->all_items_top( pocket_type::MAGAZINE ).front()->active = true;
    item pack( itype_backpack, calendar::turn );
    REQUIRE( pack.put_in( gun, pocket_type::CONTAINER ).success() );
    inventory inv;
    item holder( itype_backpack, calendar::turn );
    item *selected = nullptr;
    std::function<item()> extract;
    if( storage == "map_single" || storage == "map_bulk" ) {
        selected = &here.add_item_or_charges( pos, pack );
        extract = [&]() {
            map_cursor cursor( pos );
            if( storage == "map_single" ) {
                item result = cursor.remove_item( *selected );
                CHECK( here.i_at( pos ).empty() );
                return result;
            }
            auto removed = cursor.remove_items_with( [&]( const item & it ) {
                return &it == selected;
            }, 1 );
            REQUIRE( removed.size() == 1 );
            CHECK( here.i_at( pos ).empty() );
            return std::move( removed.front() );
        };
    } else if( storage == "vehicle_single" || storage == "vehicle_bulk" ) {
        vehicle *veh = here.add_vehicle( vehicle_prototype_car, pos, 0_degrees, 0,
                                         veh_spawn_status::UNDAMAGED );
        REQUIRE( veh != nullptr );
        int part = -1;
        for( const vpart_reference &vpr : veh->get_all_parts() ) {
            if( vpr.has_feature( "CARGO" ) ) {
                part = vpr.part_index();
                break;
            }
        }
        REQUIRE( part >= 0 );
        vehicle_stack cargo = veh->get_items( veh->part( part ) );
        while( !cargo.empty() ) {
            cargo.erase( cargo.begin() );
        }
        const auto added = veh->add_item( here, veh->part( part ), pack );
        REQUIRE( added.has_value() );
        selected = & **added;
        extract = [ &, veh, part]() {
            vehicle_cursor cursor( *veh, part );
            if( storage == "vehicle_single" ) {
                item result = cursor.remove_item( *selected );
                CHECK( veh->get_items( veh->part( part ) ).empty() );
                return result;
            }
            auto removed = cursor.remove_items_with( [&]( const item & it ) {
                return &it == selected;
            }, 1 );
            REQUIRE( removed.size() == 1 );
            CHECK( veh->get_items( veh->part( part ) ).empty() );
            return std::move( removed.front() );
        };
    } else if( storage == "inventory_bulk" ) {
        selected = &inv.add_item( pack );
        extract = [&]() {
            auto removed = inv.remove_items_with( [&]( const item & it ) {
                return &it == selected;
            }, 1 );
            REQUIRE( removed.size() == 1 );
            CHECK( inv.size() == 0 );
            return std::move( removed.front() );
        };
    } else if( storage == "pocket_single" ) {
        REQUIRE( holder.put_in( pack, pocket_type::CONTAINER ).success() );
        selected = &holder.only_item();
        extract = [&]() {
            item result = holder.remove_item( *selected );
            CHECK( holder.all_items_top().empty() );
            return result;
        };
    } else if( storage == "worn_bulk" ) {
        const auto worn = who.wear_item( pack, false );
        REQUIRE( worn.has_value() );
        selected = & **worn;
        extract = [&]() {
            auto removed = who.remove_items_with( [&]( const item & it ) {
                return &it == selected;
            }, 1 );
            REQUIRE( removed.size() == 1 );
            CHECK( who.amount_worn( itype_backpack ) == 0 );
            return std::move( removed.front() );
        };
    } else {
        REQUIRE( who.wield( pack ) );
        selected = who.get_wielded_item().get_item();
        extract = [&]() {
            item result = who.remove_weapon();
            CHECK_FALSE( who.get_wielded_item() );
            return result;
        };
    }
    REQUIRE( selected != nullptr );
    const int64_t uid = selected->uid().get_value();
    const int64_t gun_uid = selected->only_item().uid().get_value();
    const int64_t mag_uid = selected->only_item().magazine_current()->uid().get_value();
    const int64_t ammo_uid = selected->only_item().magazine_current()->all_items_top(
                                 pocket_type::MAGAZINE ).front()->uid().get_value();
    active_item_cache old_cache;
    REQUIRE( old_cache.add( *selected, point_sm_ms( 0, 0 ) ) );
    REQUIRE( old_cache.get().size() == 2 );
    const auto old_root = selected->get_safe_reference();
    item extracted = extract();
    CHECK( extracted.uid().get_value() == uid );
    CHECK( extracted.only_item().uid().get_value() == gun_uid );
    CHECK( extracted.only_item().magazine_current()->uid().get_value() == mag_uid );
    CHECK( extracted.only_item().magazine_current()->all_items_top(
               pocket_type::MAGAZINE ).front()->uid().get_value() == ammo_uid );
    CHECK_FALSE( old_root );
    CHECK( old_cache.get().empty() );
    active_item_cache new_cache;
    REQUIRE( new_cache.add( extracted, point_sm_ms( 1, 0 ) ) );
    CHECK( new_cache.get().size() == 2 );
}
