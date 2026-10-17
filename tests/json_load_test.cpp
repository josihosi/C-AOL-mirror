#include "calendar.h"
#include "color.h"
#include "debug.h"
#include "json_loader.h"
#include "item.h"
#include "json.h"
#include "damage.h"
#include "dialogue_helpers.h"
#include "cata_catch.h"
#include "itype.h"
#include "mattack_common.h"
#include "mtype.h"

static const damage_type_id damage_cut( "cut" );
static const damage_type_id damage_heat( "heat" );

static const itype_id itype_test_copyfrom_base( "test_copyfrom_base" );
static const itype_id itype_test_copyfrom_overwrite( "test_copyfrom_overwrite" );

static const mtype_id mon_test_base( "mon_test_base" );
static const mtype_id mon_test_overwrite( "mon_test_overwrite" );

TEST_CASE( "damage_instance_load_does_not_extend", "[json][damage][load]" )
{
    REQUIRE( itype_test_copyfrom_base.is_valid() );
    REQUIRE( itype_test_copyfrom_overwrite.is_valid() );


    REQUIRE( itype_test_copyfrom_base->gun );
    REQUIRE( itype_test_copyfrom_overwrite->gun );

    const damage_instance &base = itype_test_copyfrom_base->gun->damage;
    const damage_instance &overwrite = itype_test_copyfrom_overwrite->gun->damage;
    REQUIRE_FALSE( base.empty() );
    REQUIRE_FALSE( overwrite.empty() );

    CHECK_FALSE( base == overwrite );

    CHECK( base.total_damage() == 3.f );
    CHECK( base.type_damage( damage_heat ) == 3.f );
    CHECK( base.type_damage( damage_cut ) == 0.f );

    CHECK( overwrite.total_damage() == 5.f );
    CHECK( overwrite.type_damage( damage_cut ) == 5.f );
    CHECK( overwrite.type_damage( damage_heat ) == 0.f );
}

TEST_CASE( "item_name_inherited_from_copy_from", "[json][item][load]" )
{
    REQUIRE( itype_test_copyfrom_base.is_valid() );
    REQUIRE( itype_test_copyfrom_overwrite.is_valid() );

    CHECK( itype_test_copyfrom_overwrite->nname( 1 ) == "base" );
}

TEST_CASE( "monster_special_attack_cooldown_inheritance", "[json][monster][load]" )
{
    REQUIRE( mon_test_base.is_valid() );
    REQUIRE( mon_test_overwrite.is_valid() );

    const std::map<std::string, mtype_special_attack> &base_attacks = mon_test_base->special_attacks;
    const std::map<std::string, mtype_special_attack> &overwrite_attacks =
        mon_test_overwrite->special_attacks;
    REQUIRE( base_attacks.find( "TEST_COOLDOWN" ) != base_attacks.end() );
    REQUIRE( overwrite_attacks.find( "TEST_COOLDOWN" ) != overwrite_attacks.end() );

    const dbl_or_var &base = base_attacks.at( "TEST_COOLDOWN" )->cooldown;
    const dbl_or_var &overwrite = overwrite_attacks.at( "TEST_COOLDOWN" )->cooldown;

    REQUIRE( base.is_constant() );
    REQUIRE( overwrite.is_constant() );

    CHECK( base.constant() == 5.0 );
    CHECK( overwrite.constant() == 7.0 );
}

TEST_CASE( "item_color_defaults_and_inheritance", "[json][item][color]" )
{
    itype definition;
    SECTION( "a new item with no color keeps the documented white default" ) {
        const JsonObject data = json_loader::from_string(
                                    R"({"id":"test_color_default","name":"test item","symbol":","})" );
        definition.load( data, "dda" );
        CHECK( definition.color == c_white );
        const std::string warning = capture_debugmsg_during( [&]() {
            CHECK( invert_color( definition.color ) == invert_color( c_white ) );
        } );
        CHECK( warning.empty() );
    }
    SECTION( "explicit and inherited colors remain authoritative" ) {
        const JsonObject parent = json_loader::from_string(
                                      R"({"id":"test_color_parent","name":"test item","symbol":",","color":"red"})" );
        definition.load( parent, "dda" );
        REQUIRE( definition.color == c_red );
        definition.was_loaded = true;
        const JsonObject child = json_loader::from_string(
                                     R"({"id":"test_color_child","copy-from":"test_color_parent"})" );
        definition.load( child, "dda" );
        CHECK( definition.color == c_red );
        const JsonObject override_color = json_loader::from_string(
                    R"({"id":"test_color_child","color":"blue"})" );
        definition.load( override_color, "dda" );
        CHECK( definition.color == c_blue );
    }
}

TEST_CASE( "colorless_plastic_sheet_can_be_highlighted", "[json][item][color]" )
{
    const item sheet( itype_id( "plastic_sheet_small" ), calendar::turn );
    CHECK( sheet.color() == c_white );
    const std::string warning = capture_debugmsg_during( [&]() {
        CHECK( invert_color( sheet.color() ) == invert_color( c_white ) );
        CHECK( hilite( sheet.color() ) == hilite( c_white ) );
    } );
    CHECK( warning.empty() );
}

TEST_CASE( "invalid_item_color_is_still_reported", "[json][item][color]" )
{
    itype definition;
    const JsonObject data = json_loader::from_string(
                                R"({"id":"test_invalid_item_color","name":"test item","color":"nonexistent_test_color"})" );
    const std::string warning = capture_debugmsg_during( [&]() {
        definition.load( data, "dda" );
    } );
    CHECK( warning.find( "couldn't parse color" ) != std::string::npos );
    CHECK( definition.color == c_unset );
}
