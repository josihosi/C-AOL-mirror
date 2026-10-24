#include <array>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "avatar.h"
#include "avatar_action.h"
#include "bodypart.h"
#include "cata_scope_helpers.h"
#include "creature_tracker.h"
#include "json.h"
#include "json_loader.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "monster.h"
#include "overmapbuffer.h"
#include "predator_state.h"
#include "writhing_stalker_ai.h"
#include "calendar.h"
#include "cata_catch.h"
#include "character_attire.h"
#include "coordinates.h"
#include "enum_bitset.h"
#include "enums.h"
#include "flag.h"
#include "game.h"
#include "item.h"
#include "item_category.h"
#include "item_factory.h"
#include "item_location.h"
#include "item_tname.h"
#include "itype.h"
#include "material.h"
#include "math_defines.h"
#include "monstergenerator.h"
#include "mtype.h"
#include "player_helpers.h"
#include "pocket_type.h"
#include "point.h"
#include "requirements.h"
#include "ret_val.h"
#include "string_formatter.h"
#include "subbodypart.h"
#include "test_data.h"
#include "type_id.h"
#include "units.h"
#include "value_ptr.h"

static const flag_id json_flag_COLD( "COLD" );
static const flag_id json_flag_FILTHY( "FILTHY" );
static const flag_id json_flag_FIX_NEARSIGHT( "FIX_NEARSIGHT" );
static const flag_id json_flag_HOT( "HOT" );

static const item_category_id item_category_clothing( "clothing" );
static const item_category_id item_category_container( "container" );
static const item_category_id item_category_food( "food" );
static const item_category_id item_category_guns( "guns" );
static const item_category_id item_category_spare_parts( "spare_parts" );
static const item_category_id item_category_tools( "tools" );

static const itype_id itype_10gal_hat( "10gal_hat" );
static const itype_id itype_arm_splint( "arm_splint" );
static const itype_id itype_arm_warmers( "arm_warmers" );
static const itype_id itype_armor_mc_lightplate( "armor_mc_lightplate" );
static const itype_id itype_aspirin( "aspirin" );
static const itype_id itype_attachable_ear_muffs( "attachable_ear_muffs" );
static const itype_id itype_backpack( "backpack" );
static const itype_id itype_bag_plastic( "bag_plastic" );
static const itype_id itype_barrel_glock_short( "barrel_glock_short" );
static const itype_id itype_battery( "battery" );
static const itype_id itype_bottle_plastic_small( "bottle_plastic_small" );
static const itype_id itype_butter( "butter" );
static const itype_id itype_cash_card( "cash_card" );
static const itype_id itype_chem_black_powder( "chem_black_powder" );
static const itype_id itype_chem_muriatic_acid( "chem_muriatic_acid" );
static const itype_id itype_duffelbag( "duffelbag" );
static const itype_id itype_efile_photos( "efile_photos" );
static const itype_id itype_efile_recipes( "efile_recipes" );
static const itype_id itype_glock_19( "glock_19" );
static const itype_id itype_hammer( "hammer" );
static const itype_id itype_hat_hard( "hat_hard" );
static const itype_id itype_jeans( "jeans" );
static const itype_id itype_legrig( "legrig" );
static const itype_id itype_money( "money" );
static const itype_id itype_neccowafers( "neccowafers" );
static const itype_id itype_rocuronium( "rocuronium" );
static const itype_id itype_shoulder_strap( "shoulder_strap" );
static const itype_id itype_single_malt_whiskey( "single_malt_whiskey" );
static const itype_id itype_software_hacking( "software_hacking" );
static const itype_id itype_test_armguard( "test_armguard" );
static const itype_id itype_test_backpack( "test_backpack" );
static const itype_id itype_test_consolidate( "test_consolidate" );
static const itype_id itype_test_duffelbag( "test_duffelbag" );
static const itype_id itype_test_exo_lense_module( "test_exo_lense_module" );
static const itype_id itype_test_liquid( "test_liquid" );
static const itype_id itype_test_modular_exosuit( "test_modular_exosuit" );
static const itype_id itype_test_mp3( "test_mp3" );
static const itype_id itype_test_rock( "test_rock" );
static const itype_id itype_test_smart_phone( "test_smart_phone" );
static const itype_id itype_test_waterproof_bag( "test_waterproof_bag" );
static const itype_id itype_towel( "towel" );
static const itype_id itype_usb_drive( "usb_drive" );
static const itype_id itype_walnut( "walnut" );
static const itype_id itype_water( "water" );
static const itype_id itype_win70( "win70" );
static const itype_id itype_wrapper( "wrapper" );

static const json_character_flag json_flag_DEAF( "DEAF" );

static const mod_id MOD_INFORMATION_test_data( "test_data" );

static const recipe_id recipe_2byarm_guard( "2byarm_guard" );
static const recipe_id recipe_armguard_chitin( "armguard_chitin" );
static const recipe_id recipe_balclava( "balclava" );

TEST_CASE( "item_volume", "[item]" )
{
    // Need to pick some item here which is count_by_charges and for which each
    // charge is at least 1_ml.  Battery works for now.
    item i( itype_battery, calendar::turn_zero, item::default_charges_tag() );
    REQUIRE( i.count_by_charges() );
    // Would be better with Catch2 generators
    const units::volume big_volume = units::from_milliliter( std::numeric_limits<int>::max() / 2 );
    for( units::volume v : {
             0_ml, 1_ml, i.volume(), big_volume
         } ) {
        INFO( "checking batteries that fit in " << v );
        const int charges_that_should_fit = i.charges_per_volume( v );
        i.charges = charges_that_should_fit;
        CHECK( i.volume() <= v ); // this many charges should fit
        i.charges++;
        CHECK( i.volume() > v ); // one more charge should not fit
    }
}

TEST_CASE( "simple_item_layers", "[item]" )
{
    CHECK( item( itype_arm_warmers ).get_layer().front() == layer_level::SKINTIGHT );
    CHECK( item( itype_10gal_hat ).get_layer().front() == layer_level::NORMAL );
    // intentionally no waist layer check since it is obsoleted
    CHECK( item( itype_armor_mc_lightplate ).get_layer().front() == layer_level::OUTER );
    CHECK( item( itype_legrig ).get_layer().front() == layer_level::BELTED );
}

TEST_CASE( "gun_layer", "[item]" )
{
    item gun( itype_win70 );
    item mod( itype_shoulder_strap );
    CHECK( gun.is_gunmod_compatible( mod ).success() );
    gun.put_in( mod, pocket_type::MOD );
    CHECK( gun.get_layer().front() == layer_level::BELTED );
    CHECK( gun.get_category_of_contents().id == item_category_guns );
}

TEST_CASE( "gun_with_gunmod_price_sums_components", "[item][price][gunmod]" )
{
    item gun( itype_glock_19 );
    item mod( itype_barrel_glock_short );
    REQUIRE( gun.is_gunmod_compatible( mod ).success() );
    const int gun_pre = gun.price_no_contents( false );
    const int mod_pre = mod.price_no_contents( false );
    const int gun_post = gun.price_no_contents( true );
    const int mod_post = mod.price_no_contents( true );
    gun.put_in( mod, pocket_type::MOD );
    CHECK( gun.price( false ) == gun_pre + mod_pre );
    CHECK( gun.price( true ) == gun_post + mod_post );
}

TEST_CASE( "stacking_cash_cards", "[item]" )
{
    // Differently-charged cash cards should stack if neither is zero.
    item cash0( itype_cash_card, calendar::turn_zero );
    item cash1( itype_cash_card, calendar::turn_zero );
    item cash2( itype_cash_card, calendar::turn_zero );
    cash1.put_in( item( itype_money, calendar::turn_zero, 1 ), pocket_type::MAGAZINE );
    cash2.put_in( item( itype_money, calendar::turn_zero, 2 ), pocket_type::MAGAZINE );
    CHECK( !cash0.stacks_with( cash1 ) );
    //CHECK( cash1.stacks_with( cash2 ) ); Enable this once cash card stacking is brought back.
}

// second minute hour day week season year

TEST_CASE( "stacking_over_time", "[item]" )
{
    item A( itype_neccowafers );
    item B( itype_neccowafers );

    GIVEN( "Two items with the same birthday" ) {
        REQUIRE( A.stacks_with( B ) );
        WHEN( "the items are aged different numbers of seconds" ) {
            A.mod_rot( A.type->comestible->spoils - 1_turns );
            B.mod_rot( B.type->comestible->spoils - 3_turns );
            THEN( "they don't stack" ) {
                CHECK( !A.stacks_with( B ) );
            }
        }
        WHEN( "the items are aged the same to the minute but different numbers of seconds" ) {
            A.mod_rot( A.type->comestible->spoils - 5_minutes );
            B.mod_rot( B.type->comestible->spoils - 5_minutes );
            B.mod_rot( -5_turns );
            THEN( "they stack" ) {
                CHECK( A.stacks_with( B ) );
            }
        }
        WHEN( "the items are aged a few seconds different but different minutes" ) {
            A.mod_rot( A.type->comestible->spoils - 5_minutes );
            B.mod_rot( B.type->comestible->spoils - 5_minutes );
            B.mod_rot( 5_turns );
            THEN( "they don't stack" ) {
                CHECK( !A.stacks_with( B ) );
            }
        }
        WHEN( "the items are aged the same to the hour but different numbers of minutes" ) {
            A.mod_rot( A.type->comestible->spoils - 5_hours );
            B.mod_rot( B.type->comestible->spoils - 5_hours );
            B.mod_rot( -5_minutes );
            THEN( "they stack" ) {
                CHECK( A.stacks_with( B ) );
            }
        }
        WHEN( "the items are aged a few seconds different but different hours" ) {
            A.mod_rot( A.type->comestible->spoils - 5_hours );
            B.mod_rot( B.type->comestible->spoils - 5_hours );
            B.mod_rot( 5_turns );
            THEN( "they don't stack" ) {
                CHECK( !A.stacks_with( B ) );
            }
        }
        WHEN( "the items are aged the same to the day but different numbers of seconds" ) {
            A.mod_rot( A.type->comestible->spoils - 3_days );
            B.mod_rot( B.type->comestible->spoils - 3_days );
            B.mod_rot( -5_turns );
            THEN( "they stack" ) {
                CHECK( A.stacks_with( B ) );
            }
        }
        WHEN( "the items are aged a few seconds different but different days" ) {
            A.mod_rot( A.type->comestible->spoils - 3_days );
            B.mod_rot( B.type->comestible->spoils - 3_days );
            B.mod_rot( 5_turns );
            THEN( "they don't stack" ) {
                CHECK( !A.stacks_with( B ) );
            }
        }
        WHEN( "the items are aged the same to the week but different numbers of seconds" ) {
            A.mod_rot( A.type->comestible->spoils - 7_days );
            B.mod_rot( B.type->comestible->spoils - 7_days );
            B.mod_rot( -5_turns );
            THEN( "they stack" ) {
                CHECK( A.stacks_with( B ) );
            }
        }
        WHEN( "the items are aged a few seconds different but different weeks" ) {
            A.mod_rot( A.type->comestible->spoils - 7_days );
            B.mod_rot( B.type->comestible->spoils - 7_days );
            B.mod_rot( 5_turns );
            THEN( "they don't stack" ) {
                CHECK( !A.stacks_with( B ) );
            }
        }
        WHEN( "the items are aged the same to the season but different numbers of seconds" ) {
            A.mod_rot( A.type->comestible->spoils - calendar::season_length() );
            B.mod_rot( B.type->comestible->spoils - calendar::season_length() );
            B.mod_rot( -5_turns );
            THEN( "they stack" ) {
                CHECK( A.stacks_with( B ) );
            }
        }
        WHEN( "the items are aged a few seconds different but different seasons" ) {
            A.mod_rot( A.type->comestible->spoils - calendar::season_length() );
            B.mod_rot( B.type->comestible->spoils - calendar::season_length() );
            B.mod_rot( 5_turns );
            THEN( "they don't stack" ) {
                CHECK( !A.stacks_with( B ) );
            }
        }
    }
}

TEST_CASE( "liquids_at_different_temperatures", "[item][temperature][stack][combine]" )
{
    item liquid_hot( itype_test_liquid );
    item liquid_cold( itype_test_liquid );
    item liquid_filthy( itype_test_liquid );

    // heat_up/cold_up sets temperature of item and corresponding HOT/COLD flags
    liquid_hot.heat_up(); // 60 C (333.15 K)
    liquid_cold.cold_up(); // 3 C (276.15 K)
    liquid_filthy.cold_up(); // 3 C (276.15 K)
    liquid_filthy.set_flag( json_flag_FILTHY );

    REQUIRE( units::to_kelvin( liquid_hot.temperature ) == Approx( 333.15 ) );
    REQUIRE( units::to_kelvin( liquid_cold.temperature ) == Approx( 276.15 ) );
    REQUIRE( liquid_hot.has_flag( json_flag_HOT ) );
    REQUIRE( liquid_cold.has_flag( json_flag_COLD ) );

    SECTION( "liquids at the same temperature can stack together" ) {
        CHECK( liquid_cold.stacks_with( liquid_cold ) );
        CHECK( liquid_hot.stacks_with( liquid_hot ) );
    }

    SECTION( "liquids at different temperature do not stack" ) {
        // Items with different flags do not stack
        CHECK_FALSE( liquid_cold.stacks_with( liquid_hot ) );
        CHECK_FALSE( liquid_hot.stacks_with( liquid_cold ) );
    }

    SECTION( "liquids at different temperature can be combined" ) {
        CHECK( liquid_cold.can_combine( liquid_hot ) );
        CHECK( liquid_hot.can_combine( liquid_cold ) );
    }

    SECTION( "liquids with different flags can not be combined" ) {
        CHECK( !liquid_cold.can_combine( liquid_filthy ) );
        CHECK( !liquid_filthy.can_combine( liquid_cold ) );
    }
}

static void assert_minimum_length_to_volume_ratio( const item &target )
{
    if( target.type->get_id().is_null() ) {
        return;
    }
    CAPTURE( target.type->get_id() );
    CAPTURE( target.volume() );
    CAPTURE( target.base_volume() );
    CAPTURE( target.type->volume );
    if( target.made_of( phase_id::LIQUID ) || target.is_soft() ) {
        CHECK( target.length() == 0_mm );
        return;
    }
    if( target.volume() == 0_ml ) {
        CHECK( target.length() == -1_mm );
        return;
    }
    if( target.volume() == 1_ml ) {
        CHECK( target.length() >= 0_mm );
        return;
    }
    // Minimum possible length is if the item is a sphere.
    const float minimal_diameter = std::cbrt( ( 3.0 * units::to_milliliter( target.base_volume() ) ) /
                                   ( 4.0 * M_PI ) );
    CHECK( units::to_millimeter( target.length() ) >= minimal_diameter * 10.0 );
}

TEST_CASE( "item_length_sanity_check", "[item]" )
{
    for( const itype *type : item_controller->all() ) {
        const item sample( type, calendar::turn_zero, item::solitary_tag {} );
        assert_minimum_length_to_volume_ratio( sample );
    }
}

TEST_CASE( "corpse_length_sanity_check", "[item]" )
{
    for( const mtype &type : MonsterGenerator::generator().get_all_mtypes() ) {
        INFO( type.id.str() )
        const item sample = item::make_corpse( type.id );
        assert_minimum_length_to_volume_ratio( sample );
    }
}

static void check_spawning_in_container( const itype_id &item_type )
{
    item test_item( item_type );
    REQUIRE( test_item.type->default_container );
    item container_item = test_item.in_its_container( 1 );
    CHECK( container_item.typeId() == *test_item.type->default_container );
    if( container_item.is_container() ) {
        CHECK( container_item.has_item_with( [&test_item]( const item & it ) {
            return it.typeId() == test_item.typeId();
        } ) );
    } else if( test_item.is_software() ) {
        REQUIRE( container_item.is_estorage() );
        const std::vector<const item *> softwares = container_item.softwares();
        CHECK( !softwares.empty() );
        for( const item *itm : softwares ) {
            CHECK( itm->typeId() == test_item.typeId() );
        }
    } else {
        FAIL( "Not container or software storage." );
    }
}

TEST_CASE( "items_spawn_in_their_default_containers", "[item]" )
{
    check_spawning_in_container( itype_water );
    check_spawning_in_container( itype_single_malt_whiskey );
    check_spawning_in_container( itype_rocuronium );
    check_spawning_in_container( itype_chem_muriatic_acid );
    check_spawning_in_container( itype_chem_black_powder );
}

TEST_CASE( "item_variables_round-trip_accurately", "[item]" )
{
    item i( itype_water );
    i.set_var( "A", 17 );
    CHECK( i.get_var( "A", 0 ) == 17 );
    i.set_var( "B", 0.125 );
    CHECK( i.get_var( "B", 0.0 ) == 0.125 );
    i.set_var( "C", tripoint_abs_ms( 2, 3, 4 ) );
    CHECK( i.get_var( "C", tripoint_abs_ms::zero ) == tripoint_abs_ms( 2, 3, 4 ) );
}

static item make_photo_gallery( int n_photos )
{
    item gallery( itype_efile_photos );
    std::vector<item::extended_photo_def> photos;
    photos.reserve( n_photos );
    for( int i = 0; i < n_photos; ++i ) {
        item::extended_photo_def photo;
        photo.quality = 3;
        photo.name = string_format( "test_photo_%d", i );
        photo.description = "A test photo for stacking comparison.";
        photos.push_back( photo );
    }
    gallery.write_extended_photos( photos, "CAMERA_EXTENDED_PHOTOS" );
    return gallery;
}

TEST_CASE( "efile_photos_total_photos_count_cache", "[item][estorage]" )
{
    SECTION( "fresh write caches the count" ) {
        item gallery = make_photo_gallery( 5 );
        CHECK( gallery.has_var( "CAMERA_EXTENDED_PHOTOS_count" ) );
        CHECK( gallery.get_var( "CAMERA_EXTENDED_PHOTOS_count", 0 ) == 5 );
        CHECK( gallery.total_photos() == 5 );
    }

    SECTION( "legacy save without cache var lazy-fills on first call" ) {
        item gallery = make_photo_gallery( 7 );
        gallery.erase_var( "CAMERA_EXTENDED_PHOTOS_count" );
        REQUIRE_FALSE( gallery.has_var( "CAMERA_EXTENDED_PHOTOS_count" ) );
        CHECK( gallery.total_photos() == 7 );
        CHECK( gallery.has_var( "CAMERA_EXTENDED_PHOTOS_count" ) );
        CHECK( gallery.get_var( "CAMERA_EXTENDED_PHOTOS_count", 0 ) == 7 );
    }

    SECTION( "two galleries with same photos stack" ) {
        item a = make_photo_gallery( 3 );
        item b = make_photo_gallery( 3 );
        stacking_info info = a.stacks_with( b );
        CHECK( static_cast<bool>( info ) );
        CHECK( info.bits[tname::segments::EMEMORY] );
    }

    SECTION( "mixed cache state still stacks" ) {
        // Cache key is derived; VARS must ignore it or lazy-fill desyncs split stacks.
        item a = make_photo_gallery( 4 );
        item b = make_photo_gallery( 4 );
        b.erase_var( "CAMERA_EXTENDED_PHOTOS_count" );
        REQUIRE( a.has_var( "CAMERA_EXTENDED_PHOTOS_count" ) );
        REQUIRE_FALSE( b.has_var( "CAMERA_EXTENDED_PHOTOS_count" ) );
        stacking_info info = a.stacks_with( b );
        CHECK( static_cast<bool>( info ) );
        CHECK( info.bits[tname::segments::VARS] );
    }

    SECTION( "differing photo counts do not stack" ) {
        item a = make_photo_gallery( 3 );
        item b = make_photo_gallery( 5 );
        REQUIRE( a.total_photos() != b.total_photos() );
        stacking_info info = a.stacks_with( b );
        CHECK_FALSE( static_cast<bool>( info ) );
        CHECK_FALSE( info.bits[tname::segments::VARS] );
    }

    SECTION( "containers with differing nested galleries report EMEMORY mismatch" ) {
        // EMEMORY bit drives aggregated free-mem display, must reflect real mismatch.
        item a( itype_usb_drive );
        item b( itype_usb_drive );
        REQUIRE( a.put_in( make_photo_gallery( 3 ), pocket_type::E_FILE_STORAGE ).success() );
        REQUIRE( b.put_in( make_photo_gallery( 5 ), pocket_type::E_FILE_STORAGE ).success() );
        stacking_info info = a.stacks_with( b );
        CHECK_FALSE( static_cast<bool>( info ) );
        CHECK_FALSE( info.bits[tname::segments::EMEMORY] );
    }

    SECTION( "occupied_ememory sums every top-level efile" ) {
        item drive( itype_usb_drive );
        const item g1 = make_photo_gallery( 2 );
        const item g2 = make_photo_gallery( 5 );
        const units::ememory expected = g1.ememory_size() + g2.ememory_size();
        REQUIRE( drive.put_in( g1, pocket_type::E_FILE_STORAGE ).success() );
        REQUIRE( drive.put_in( g2, pocket_type::E_FILE_STORAGE ).success() );
        const bool sum_matches = drive.occupied_ememory() == expected;
        CHECK( sum_matches );
    }

    SECTION( "estorage and non-estorage pair fails EMEMORY bit" ) {
        // Aggregated headers AND this bit; mixed pairs must drop it.
        item drive( itype_usb_drive );
        item rock( itype_test_rock );
        REQUIRE( drive.is_estorage() );
        REQUIRE_FALSE( rock.is_estorage() );
        stacking_info info = drive.stacks_with( rock );
        CHECK_FALSE( info.bits[tname::segments::EMEMORY] );
    }

    SECTION( "non-estorage pair short-circuits EMEMORY bit to true" ) {
        item a( itype_test_rock );
        item b( itype_test_rock );
        REQUIRE_FALSE( a.is_estorage() );
        stacking_info info = a.stacks_with( b );
        CHECK( info.bits[tname::segments::EMEMORY] );
    }
}

static item make_recipe_book( const std::set<recipe_id> &recipes )
{
    item book( itype_efile_recipes );
    book.set_saved_recipes( recipes );
    return book;
}

TEST_CASE( "efile_recipes_count_cache", "[item][estorage]" )
{
    const std::set<recipe_id> two = { recipe_balclava, recipe_2byarm_guard };
    const std::set<recipe_id> three = { recipe_balclava, recipe_2byarm_guard, recipe_armguard_chitin };

    SECTION( "set_saved_recipes caches the count" ) {
        item book = make_recipe_book( two );
        CHECK( book.has_var( "EIPC_RECIPES_count" ) );
        CHECK( book.get_var( "EIPC_RECIPES_count", 0 ) == 2 );
    }

    SECTION( "legacy save without cache var lazy-fills via ememory_size" ) {
        item book = make_recipe_book( three );
        book.erase_var( "EIPC_RECIPES_count" );
        REQUIRE_FALSE( book.has_var( "EIPC_RECIPES_count" ) );
        const bool nonzero = book.ememory_size().value() > 0;
        REQUIRE( nonzero );
        CHECK( book.has_var( "EIPC_RECIPES_count" ) );
        CHECK( book.get_var( "EIPC_RECIPES_count", 0 ) == 3 );
    }

    SECTION( "broken e-file reports zero ememory_size" ) {
        item book = make_recipe_book( two );
        book.set_flag( flag_ITEM_BROKEN );
        REQUIRE( book.is_broken_on_active() );
        CHECK( book.ememory_size().value() == 0 );
    }

    SECTION( "lazy fill deduplicates malformed CSV" ) {
        item book( itype_efile_recipes );
        book.set_var( "EIPC_RECIPES", std::string( ",balclava,balclava," ) );
        REQUIRE_FALSE( book.has_var( "EIPC_RECIPES_count" ) );
        const bool nonzero = book.ememory_size().value() > 0;
        REQUIRE( nonzero );
        CHECK( book.get_var( "EIPC_RECIPES_count", 0 ) == 1 );
    }

    SECTION( "set_saved_recipes while broken caches steady-state count" ) {
        item book( itype_efile_recipes );
        book.set_flag( flag_ITEM_BROKEN );
        REQUIRE( book.is_broken_on_active() );
        book.set_saved_recipes( two );
        CHECK( book.get_var( "EIPC_RECIPES_count", 0 ) == 2 );
        book.unset_flag( flag_ITEM_BROKEN );
        REQUIRE_FALSE( book.is_broken_on_active() );
        const bool nonzero = book.ememory_size().value() > 0;
        CHECK( nonzero );
    }

    SECTION( "mixed cache state still stacks" ) {
        item a = make_recipe_book( two );
        item b = make_recipe_book( two );
        b.erase_var( "EIPC_RECIPES_count" );
        REQUIRE( a.has_var( "EIPC_RECIPES_count" ) );
        REQUIRE_FALSE( b.has_var( "EIPC_RECIPES_count" ) );
        stacking_info info = a.stacks_with( b );
        CHECK( static_cast<bool>( info ) );
        CHECK( info.bits[tname::segments::VARS] );
    }
}

TEST_CASE( "water_affect_items_while_swimming_check", "[item][water][swimming]" )
{
    avatar &guy = get_avatar();
    clear_avatar();

    GIVEN( "an item with flag WATER_DISSOLVE" ) {

        REQUIRE( item( itype_aspirin ).has_flag( flag_WATER_DISSOLVE ) );

        WHEN( "item in hand" ) {
            guy.unwield();
            guy.clear_worn();

            item aspirin( itype_aspirin );

            REQUIRE( guy.wield( aspirin ) );

            THEN( "should dissolve in water" ) {
                g->water_affect_items( guy );
                CHECK_FALSE( guy.has_item_with_flag( flag_WATER_DISSOLVE ) );
            }
        }

        WHEN( "item in backpack" ) {
            guy.unwield();
            guy.clear_worn();

            item aspirin( itype_aspirin );
            item backpack( itype_backpack );

            backpack.put_in( aspirin, pocket_type::CONTAINER );

            REQUIRE( guy.wear_item( backpack ) );

            THEN( "should dissolve in water" ) {
                g->water_affect_items( guy );
                CHECK_FALSE( guy.has_item_with_flag( flag_WATER_DISSOLVE ) );
            }
        }

        WHEN( "item in small plastic bottle" ) {
            guy.unwield();
            guy.clear_worn();

            item aspirin( itype_aspirin );
            item bottle_small( itype_bottle_plastic_small );

            bottle_small.put_in( aspirin, pocket_type::CONTAINER );

            REQUIRE( guy.wield( bottle_small ) );

            THEN( "should not dissolve in water" ) {
                g->water_affect_items( guy );
                CHECK( guy.has_item_with_flag( flag_WATER_DISSOLVE ) );
            }
        }

        WHEN( "item in backpack inside duffel bag" ) {
            guy.unwield();
            guy.clear_worn();

            item aspirin( itype_aspirin );
            item backpack( itype_backpack );
            item duffelbag( itype_duffelbag );

            backpack.put_in( aspirin, pocket_type::CONTAINER );
            duffelbag.put_in( backpack, pocket_type::CONTAINER );

            REQUIRE( guy.wear_item( duffelbag ) );

            THEN( "should dissolve in water" ) {
                g->water_affect_items( guy );
                CHECK_FALSE( guy.has_item_with_flag( flag_WATER_DISSOLVE ) );
            }
        }

        WHEN( "item in backpack inside body bag" ) {
            guy.unwield();
            guy.clear_worn();

            item aspirin( itype_aspirin );
            item backpack( itype_backpack );
            item body_bag( itype_test_waterproof_bag );

            backpack.put_in( aspirin, pocket_type::CONTAINER );
            body_bag.put_in( backpack, pocket_type::CONTAINER );

            REQUIRE( guy.wield( body_bag ) );

            THEN( "should not dissolve in water" ) {
                g->water_affect_items( guy );
                CHECK( guy.has_item_with_flag( flag_WATER_DISSOLVE ) );
            }
        }
    }

    GIVEN( "an item with flag WATER_BREAK" ) {

        REQUIRE( itype_test_smart_phone->has_flag( flag_WATER_BREAK ) );

        WHEN( "item in hand" ) {
            guy.unwield();
            guy.clear_worn();

            item smart_phone( itype_test_smart_phone );

            REQUIRE( guy.wield( smart_phone ) );
            item *test_item = guy.get_wielded_item().get_item();

            THEN( "should be broken by water" ) {
                g->water_affect_items( guy );

                CHECK_FALSE( test_item->faults.empty() );
                CHECK( test_item->is_broken() );
            }
        }

        WHEN( "item in backpack" ) {
            guy.unwield();
            guy.clear_worn();

            item smart_phone( itype_test_smart_phone );
            item backpack( itype_test_backpack );

            backpack.put_in( smart_phone, pocket_type::CONTAINER );

            REQUIRE( guy.wield( backpack ) );
            item *test_item = &guy.get_wielded_item()->only_item();

            THEN( "should be broken by water" ) {
                g->water_affect_items( guy );
                CHECK_FALSE( test_item->faults.empty() );
                CHECK( test_item->is_broken() );
            }
        }

        WHEN( "item in body bag" ) {
            guy.unwield();
            guy.clear_worn();

            item smart_phone( itype_test_smart_phone );
            item body_bag( itype_test_waterproof_bag );

            body_bag.put_in( smart_phone, pocket_type::CONTAINER );

            REQUIRE( guy.wield( body_bag ) );
            item *test_item = &guy.get_wielded_item()->only_item();

            THEN( "should not be broken by water" ) {
                g->water_affect_items( guy );
                CHECK( test_item->faults.empty() );
                CHECK_FALSE( test_item->is_broken() );
            }
        }

        WHEN( "item in backpack inside duffel bag" ) {
            guy.unwield();
            guy.clear_worn();

            item smart_phone( itype_test_smart_phone );
            item backpack( itype_test_backpack );
            item duffelbag( itype_test_duffelbag );

            backpack.put_in( smart_phone, pocket_type::CONTAINER );
            duffelbag.put_in( backpack, pocket_type::CONTAINER );

            REQUIRE( guy.wield( duffelbag ) );
            item *test_item = &guy.get_wielded_item()->only_item().only_item();

            THEN( "should be broken by water" ) {
                g->water_affect_items( guy );
                CHECK_FALSE( test_item->faults.empty() );
                CHECK( test_item->is_broken() );
            }
        }

        WHEN( "item in backpack inside body bag" ) {
            guy.unwield();
            guy.clear_worn();

            item smart_phone( itype_test_smart_phone );
            item backpack( itype_test_backpack );
            item body_bag( itype_test_waterproof_bag );

            backpack.put_in( smart_phone, pocket_type::CONTAINER );
            body_bag.put_in( backpack, pocket_type::CONTAINER );

            REQUIRE( guy.wield( body_bag ) );
            item *test_item = &guy.get_wielded_item()->only_item().only_item();

            THEN( "should not be broken by water" ) {
                g->water_affect_items( guy );
                CHECK( test_item->faults.empty() );
                CHECK_FALSE( test_item->is_broken() );
            }
        }
    }

    GIVEN( "an item with flag WATER_BREAK_ACTIVE" ) {

        REQUIRE( itype_test_mp3->has_flag( flag_WATER_BREAK_ACTIVE ) );

        WHEN( "item in hand" ) {
            guy.unwield();
            guy.clear_worn();

            item mp3( itype_test_mp3 );

            REQUIRE( guy.wield( mp3 ) );

            THEN( "should get wet from water" ) {
                g->water_affect_items( guy );
                CHECK( guy.get_wielded_item()->wetness > 0 );
            }
        }

        WHEN( "item in backpack" ) {
            guy.unwield();
            guy.clear_worn();

            item mp3( itype_test_mp3 );
            item backpack( itype_test_backpack );

            backpack.put_in( mp3, pocket_type::CONTAINER );

            REQUIRE( guy.wield( backpack ) );

            THEN( "should get wet from water" ) {
                g->water_affect_items( guy );
                const item *test_item = guy.get_wielded_item()->all_items_top().front();
                REQUIRE( test_item->typeId() == itype_test_mp3 );
                CHECK( test_item->wetness > 0 );
            }
        }

        WHEN( "item in body bag" ) {
            guy.unwield();
            guy.clear_worn();

            item mp3( itype_test_mp3 );
            item body_bag( itype_test_waterproof_bag );

            body_bag.put_in( mp3, pocket_type::CONTAINER );

            REQUIRE( guy.wield( body_bag ) );

            THEN( "should not be broken by water" ) {
                g->water_affect_items( guy );
                const item *test_item = guy.get_wielded_item()->all_items_top().front();
                REQUIRE( test_item->typeId() == itype_test_mp3 );
                CHECK( test_item->wetness == 0 );
            }
        }

        WHEN( "item in backpack inside duffel bag" ) {
            guy.unwield();
            guy.clear_worn();

            item mp3( itype_test_mp3 );
            item backpack( itype_test_backpack );
            item duffelbag( itype_test_duffelbag );

            backpack.put_in( mp3, pocket_type::CONTAINER );
            duffelbag.put_in( backpack, pocket_type::CONTAINER );

            REQUIRE( guy.wield( duffelbag ) );

            THEN( "should get wet from water" ) {
                g->water_affect_items( guy );
                const item *test_item = guy.get_wielded_item()->all_items_top().front()->all_items_top().front();
                REQUIRE( test_item->typeId() == itype_test_mp3 );
                CHECK( test_item->wetness > 0 );
            }
        }

        WHEN( "item in backpack inside body bag" ) {
            guy.unwield();
            guy.clear_worn();

            item mp3( itype_test_mp3 );
            item backpack( itype_test_backpack );
            item body_bag( itype_test_waterproof_bag );

            backpack.put_in( mp3, pocket_type::CONTAINER );
            body_bag.put_in( backpack, pocket_type::CONTAINER );

            REQUIRE( guy.wield( body_bag ) );

            THEN( "should not be broken by water" ) {
                g->water_affect_items( guy );
                const item *test_item = guy.get_wielded_item()->all_items_top().front()->all_items_top().front();
                REQUIRE( test_item->typeId() == itype_test_mp3 );
                CHECK( test_item->wetness == 0 );
            }
        }

        WHEN( "item in hand" ) {
            guy.unwield();
            guy.clear_worn();

            item mp3( itype_test_mp3 );

            REQUIRE( guy.wield( mp3 ) );

            THEN( "should be wet for around 9562 seconds" ) {
                g->water_affect_items( guy );
                CHECK( guy.get_wielded_item()->wetness == Approx( 9562 ).margin( 20 ) );
            }
        }

        WHEN( "item in hand" ) {
            guy.unwield();
            guy.clear_worn();

            item mp3( itype_test_mp3 );

            REQUIRE( guy.wield( mp3 ) );

            THEN( "gets wet five times in a row " ) {
                g->water_affect_items( guy );
                g->water_affect_items( guy );
                g->water_affect_items( guy );
                g->water_affect_items( guy );
                g->water_affect_items( guy );
                AND_THEN( "should be wet for around 47810 seconds" ) {
                    CHECK( guy.get_wielded_item()->wetness == Approx( 47810 ).margin( 100 ) );
                }
            }
        }
    }

    GIVEN( "a towel" ) {

        WHEN( "item in hand" ) {
            guy.unwield();
            guy.clear_worn();

            item towel( itype_towel );

            REQUIRE( guy.wield( towel ) );

            THEN( "should get wet in water" ) {
                g->water_affect_items( guy );
                CHECK( guy.has_item_with_flag( flag_WET ) );
            }
        }

        WHEN( "wearing item" ) {
            guy.unwield();
            guy.clear_worn();

            item towel( itype_towel );

            REQUIRE( guy.wear_item( towel ) );

            THEN( "should get wet in water" ) {
                g->water_affect_items( guy );
                CHECK( guy.has_item_with_flag( flag_WET ) );
            }
        }

        WHEN( "inside a backpack" ) {
            guy.unwield();
            guy.clear_worn();

            item towel( itype_towel );
            item backpack( itype_backpack );

            backpack.put_in( towel, pocket_type::CONTAINER );

            REQUIRE( guy.wield( backpack ) );

            THEN( "should get wet in water" ) {
                g->water_affect_items( guy );
                CHECK( guy.has_item_with_flag( flag_WET ) );
            }
        }

        WHEN( "inside a body bag" ) {
            guy.unwield();
            guy.clear_worn();

            item towel( itype_towel );
            item body_bag( itype_test_waterproof_bag );

            body_bag.put_in( towel, pocket_type::CONTAINER );

            REQUIRE( guy.wield( body_bag ) );

            THEN( "should not get wet in water" ) {
                g->water_affect_items( guy );
                CHECK_FALSE( guy.has_item_with_flag( flag_WET ) );
            }
        }
    }
}


TEST_CASE( "item_new_to_hit_enforcement", "[item]" )
{
    std::vector<const itype *> all_items = item_controller->all();
    const std::set<itype_id> &blacklist = test_data::legacy_to_hit;
    std::string msg_enforce;
    std::string msg_prune;
    for( const itype *type : all_items ) {
        const bool on_blacklist = blacklist.find( type->get_id() ) != blacklist.end();
        if( type->using_legacy_to_hit ) {
            if( !on_blacklist ) {
                msg_enforce += msg_enforce.empty() ? string_format( "\n[\n  \"%s\"", type->get_id().str() ) :
                               string_format( ",\n  \"%s\"", type->get_id().str() );
            }
        } else if( on_blacklist ) {
            msg_prune += msg_prune.empty() ? string_format( "\n[\n  \"%s\"", type->get_id().str() ) :
                         string_format( ",\n  \"%s\"", type->get_id().str() );
        }
    }
    if( !msg_enforce.empty() ) {
        msg_enforce +=
            "\n]\nThe item(s) above use legacy to_hit, please change them to the newer object method (see /docs/design-balance-lore/GAME_BALANCE.md#to-hit-value) or remove the to_hit field if the item(s) aren't intended to be used as weapons.";
    }
    if( !msg_prune.empty() ) {
        msg_prune +=
            "\n]\nThe item(s) above should be removed from the blacklist at /data/mods/TEST_DATA/legacy_to_hit.json.";
    }
    CAPTURE( msg_enforce );
    REQUIRE( msg_enforce.empty() );
    CAPTURE( msg_prune );
    REQUIRE( msg_prune.empty() );
}

static float max_density_for_mats( const std::map<material_id, int> &mats, float total_size )
{
    REQUIRE( !mats.empty() );

    float max_density = 0.f;

    for( const auto &m : mats ) {
        max_density += m.first->density() * ( m.second  / total_size );
    }

    return max_density;
}

static float item_density( const item &target )
{
    return static_cast<float>( to_milligram( target.weight() ) / 1000.0f ) / static_cast<float>
           ( to_milliliter( target.volume() ) );
}

static void assert_maximum_density_for_material( const item &target )
{
    if( to_milliliter( target.volume() ) == 0 ) {
        return;
    }
    const std::map<material_id, int> &mats = target.made_of();
    if( !mats.empty() && test_data::known_bad.count( target.typeId() ) == 0 ) {
        const float max_density = max_density_for_mats( mats, target.type->mat_portion_total );
        CAPTURE( target.typeId(), target.weight(), target.volume() );
        CHECK( item_density( target ) <= max_density );
    }
}

TEST_CASE( "item_material_density_sanity_check", "[item]" )
{
    std::vector<const itype *> all_items = item_controller->all();

    for( const itype *type : all_items ) {
        const item sample( type, calendar::turn_zero, item::solitary_tag{} );
        assert_maximum_density_for_material( sample );
    }
}

TEST_CASE( "item_material_density_blacklist_is_pruned", "[item]" )
{
    for( const itype_id &bad : test_data::known_bad ) {
        if( !bad.is_valid() ) {
            continue;
        }
        const item target( bad, calendar::turn_zero, item::solitary_tag{} );
        if( to_milliliter( target.volume() ) == 0 ) {
            continue;
        }
        const std::map<material_id, int> &mats = target.made_of();
        if( !mats.empty() ) {
            INFO( string_format( "%s had its density fixed, remove it from the list in data/mods/TEST_DATA/known_bad_density.json",
                                 bad.str() ) );
            const float max_density = max_density_for_mats( mats, bad->mat_portion_total );
            CHECK( item_density( target ) > max_density );
        }
    }
}

TEST_CASE( "armor_entry_consolidate_check", "[item][armor]" )
{
    item test_consolidate( itype_test_consolidate );

    //check this item has a single armor entry, not 3 like is written in the json explicitly

    CHECK( test_consolidate.find_armor_data()->sub_data.size() == 1 );
}

TEST_CASE( "module_inheritance", "[item][armor]" )
{
    avatar &guy = get_avatar();
    clear_avatar();
    guy.set_body();
    guy.clear_mutations();
    guy.clear_worn();

    item test_exo( itype_test_modular_exosuit );
    item test_module( itype_test_exo_lense_module );

    test_exo.force_insert_item( test_module, pocket_type::CONTAINER );

    guy.worn.wear_item( guy, test_exo, false, false, false );

    CHECK( guy.worn.worn_with_flag( json_flag_FIX_NEARSIGHT ) );

    clear_avatar();
    item hat_hard( itype_hat_hard );
    item ear_muffs( itype_attachable_ear_muffs );
    REQUIRE( hat_hard.put_in( ear_muffs, pocket_type::CONTAINER ).success() );
    REQUIRE( !hat_hard.has_flag( json_flag_DEAF ) );
    guy.wear_item( hat_hard );
    item_location worn_hat = guy.worn.top_items_loc( guy ).front();
    item_location worn_muffs( worn_hat, &worn_hat->only_item() );
    avatar_action::use_item( guy, worn_muffs, "transform" );
    CHECK( worn_hat->has_flag( json_flag_DEAF ) );
}

TEST_CASE( "rigid_armor_compliance", "[item][armor]" )
{
    avatar &guy = get_avatar();
    clear_avatar();
    // check if you can swap a rigid armor
    item test_armguard( itype_test_armguard );
    REQUIRE( guy.wield( test_armguard ) );

    REQUIRE( guy.wear( guy.used_weapon(), false ) );

    CHECK( guy.worn.top_items_loc( guy ).front().get_item()->get_side() == side::LEFT );

    guy.change_side( *guy.worn.top_items_loc( guy ).front().get_item() );

    CHECK( guy.worn.top_items_loc( guy ).front().get_item()->get_side() == side::RIGHT );

    // check if you can't wear 3 rigid armors
    clear_avatar();

    item first_test_armguard( itype_test_armguard );
    REQUIRE( guy.wield( first_test_armguard ) );
    REQUIRE( guy.wear( guy.used_weapon(), false ) );

    item second_test_armguard( itype_test_armguard );
    REQUIRE( guy.wield( second_test_armguard ) );
    REQUIRE( guy.wear( guy.used_weapon(), false ) );

    item third_test_armguard( itype_test_armguard );
    REQUIRE( guy.wield( third_test_armguard ) );
    REQUIRE( !guy.wear( guy.used_weapon(), false ) );
}

TEST_CASE( "rigid_splint_compliance", "[item][armor]" )
{
    avatar &guy = get_avatar();
    clear_avatar();

    item test_armguard( itype_test_armguard );
    item second_test_armguard( itype_test_armguard );
    item splint( itype_arm_splint );
    item second_splint( itype_arm_splint );
    item third_splint( itype_arm_splint );

    // check if you can wear a splint
    clear_avatar();

    guy.set_part_hp_cur( bodypart_id( "arm_r" ), 0 );
    REQUIRE( guy.wield( splint ) );
    REQUIRE( guy.wear( guy.used_weapon(), false ) );

    // check if you cannot wear a splint if something rigid is on that arm
    clear_avatar();

    REQUIRE( guy.wield( test_armguard ) );
    guy.set_part_hp_cur( bodypart_id( "arm_r" ), 0 );
    REQUIRE( guy.wear( guy.used_weapon(), false ) );

    CHECK( guy.worn.top_items_loc( guy ).front().get_item()->get_side() == side::LEFT );
    // swap side to the broken arm side
    guy.change_side( *guy.worn.top_items_loc( guy ).front().get_item() );
    // should fail to wear
    REQUIRE( guy.wield( second_splint ) );
    REQUIRE( !guy.wear( guy.used_weapon(), false ) );

    // check if you can wear a splint if nothing rigid is on that arm
    clear_avatar();

    REQUIRE( guy.wield( second_test_armguard ) );
    guy.set_part_hp_cur( bodypart_id( "arm_r" ), 0 );
    REQUIRE( guy.wear( guy.used_weapon(), false ) );

    CHECK( guy.worn.top_items_loc( guy ).front().get_item()->get_side() == side::LEFT );

    // should be able to wear the arm is open
    REQUIRE( guy.wield( third_splint ) );
    REQUIRE( guy.wear( guy.used_weapon(), false ) );
}

TEST_CASE( "item_single_type_contents", "[item]" )
{
    item rock( itype_test_rock );
    std::array<std::string, 2> const variants = { "test_rock_blue", "test_rock_green" };
    item walnut( itype_walnut );
    item bag( itype_bag_plastic );
    REQUIRE( bag.get_category_of_contents().id == item_category_container );
    int const num = GENERATE( 1, 2 );
    bool ret = true;
    for( int i = 0; i < num; i++ ) {
        rock.set_itype_variant( variants[i] );
        ret &= bag.put_in( rock, pocket_type::CONTAINER ).success();
    }
    REQUIRE( ret );
    CAPTURE( num, bag.display_name() );
    CHECK( bag.get_category_of_contents() == *item_category_spare_parts );
    REQUIRE( walnut.get_category_of_contents().id != rock.get_category_of_contents().id );
    REQUIRE( bag.put_in( walnut, pocket_type::CONTAINER ).success() );
    if( num == 1 ) {
        // 1 rock and 1 walnut - nothing dominates
        CHECK( bag.get_category_of_contents().id == item_category_container );
    } else {
        // 2 rock and 1 walnuts - rocks dominate
        CHECK( bag.get_category_of_contents().id == item_category_spare_parts );
        REQUIRE( bag.put_in( walnut, pocket_type::CONTAINER ).success() );
        item hammer( itype_hammer );
        REQUIRE( hammer.get_category_of_contents().id != rock.get_category_of_contents().id );
        REQUIRE( hammer.get_category_of_contents().id != walnut.get_category_of_contents().id );
        REQUIRE( bag.put_in( hammer, pocket_type::CONTAINER ).success() );
        // no dominant category anymore - revert to container
        CHECK( bag.get_category_of_contents().id == item_category_container );
    }

    SECTION( "clothing" ) {
        item jeans( itype_jeans );
        REQUIRE( jeans.get_category_of_contents().id == item_category_clothing );
        REQUIRE( walnut.get_category_of_contents().id == item_category_food );
        REQUIRE( jeans.put_in( walnut, pocket_type::CONTAINER ).success() );
        CHECK( jeans.get_category_of_contents().id == item_category_clothing );
    }

    SECTION( "software" ) {
        item usb_drive( itype_usb_drive );
        item software_hacking( itype_software_hacking );
        REQUIRE( usb_drive.get_category_of_contents().id == item_category_tools );
        REQUIRE( usb_drive.put_in( software_hacking, pocket_type::E_FILE_STORAGE ).success() );
        CHECK( usb_drive.get_category_of_contents().id == item_category_tools );
    }
}

TEST_CASE( "item_nested_contents", "[item]" )
{
    item walnut( itype_walnut );
    item outer_bag( itype_bag_plastic );
    item inner_bag1( itype_bag_plastic );
    item inner_bag2( itype_bag_plastic );

    REQUIRE( inner_bag1.put_in( walnut, pocket_type::CONTAINER ).success() );
    REQUIRE( inner_bag1.put_in( walnut, pocket_type::CONTAINER ).success() );
    CHECK( inner_bag1.get_category_of_contents().id == item_category_food );

    REQUIRE( inner_bag2.put_in( walnut, pocket_type::CONTAINER ).success() );
    CHECK( inner_bag2.get_category_of_contents().id == item_category_food );

    REQUIRE( outer_bag.put_in( inner_bag1, pocket_type::CONTAINER ).success() );
    REQUIRE( outer_bag.put_in( inner_bag2, pocket_type::CONTAINER ).success() );
    CAPTURE( outer_bag.display_name() );
    // outer_bag
    //   inner_bag1
    //     walnut
    //     walnut
    //   inner_bag2
    //     walnut
    CHECK( outer_bag.get_category_of_contents().id == item_category_food );
}

TEST_CASE( "item_rotten_contents", "[item]" )
{
    item wrapper( itype_wrapper );
    REQUIRE( wrapper.get_category_of_contents().id == item_category_container );

    item butter_rotten( itype_butter );
    butter_rotten.set_relative_rot( 1.01 );
    REQUIRE( wrapper.put_in( butter_rotten, pocket_type::CONTAINER ).success() );
    REQUIRE( wrapper.put_in( butter_rotten, pocket_type::CONTAINER ).success() );
    CAPTURE( wrapper.display_name() );
    CHECK( wrapper.get_category_of_contents().id == item_category_food );

    item butter( itype_butter );
    butter.set_relative_rot( 0.5 );
    REQUIRE( wrapper.put_in( butter, pocket_type::CONTAINER ).success() );
    CAPTURE( wrapper.display_name() );
    // wrapper
    //   butter (rotten)
    //   butter (rotten)
    //   butter
    CHECK( wrapper.get_category_of_contents().id == item_category_food );
}

static bool uncraft_need_to_be_checked( const item &it )
{
    // skip items that do not have uncraft
    if( it.get_uncraft_components().empty() ) {
        return false;
    }

    // skip specifically these mod stuff
    if( it.type->src.back().second == MOD_INFORMATION_test_data ) {
        return false;
    }

    // ethereal, technical, mutation related item etc stuff are skipped even if have uncraft
    if( it.type->weight == 0_gram ||
        it.type->volume == 0_ml ||
        it.type->has_flag( flag_INTEGRATED ) ||
        it.type->has_flag( flag_NO_TAKEOFF ) ||
        it.type->has_flag( flag_AURA ) ||
        it.type->has_flag( flag_PERSONAL ) ) {
        return false;
    }
    return true;
}

// checks only items that have some uncraft, either manually defined or reversible craft
TEST_CASE( "uncraft_sanity_check", "[item]" )
{
    std::vector<const itype *> all_items = item_controller->all();

    // this specifies the precision of a crafting test
    // 0.1 means 1000 g item can drop from 900 g to 1100 g of resources
    const float tolerance = 0.1f;

    for( const itype *type : all_items ) {

        // skip if already know it's bad
        const bool in_list = test_data::known_bad_uncraft.count( type->id ) != 0;
        if( in_list ) {
            continue;
        }

        const item target( type, calendar::turn_zero, item::solitary_tag{} );

        if( !uncraft_need_to_be_checked( target ) ) {
            continue;
        }

        units::mass sum_of_components_weight;
        for( const item_comp &c : target.get_uncraft_components() ) {
            sum_of_components_weight += c.type->weight * c.count;
        }

        const units::mass item_weight = target.type->weight;
        const units::mass weight_difference = units::abs( sum_of_components_weight - item_weight );
        const units::mass weight_tolerance = tolerance * item_weight;
        const bool is_within_tolerance = weight_difference <= weight_tolerance;

        if( !is_within_tolerance ) {
            INFO( string_format( "Item %s weight %d gram, but it's uncrafting recipe (including crafting with 'reversible: true') has components with total weight of %d gram.  It should be within %.0f%%.",
                                 target.typeId().str(), to_gram( item_weight ),
                                 to_gram( sum_of_components_weight ), tolerance * 100.f ) );
            CHECK( is_within_tolerance );
        }
    }
}

TEST_CASE( "uncraft_blacklist_is_pruned", "[item]" )
{
    std::string list;
    // this specifies the precision of a crafting test
    // 0.1 means 1000 g item can drop from 900 g to 1100 g of resources
    const float tolerance = 0.1f;

    for( const itype_id &bad : test_data::known_bad_uncraft ) {

        if( !bad.is_valid() ) {
            continue;
        }

        const units::mass item_weight = bad->weight;
        const item target( bad, calendar::turn_zero, item::solitary_tag{} );

        if( target.get_uncraft_components().empty() ) {
            INFO( string_format( "Item '%s' do not have uncraft anymore, remove it from data/mods/TEST_DATA/known_bad_uncrafts.json",
                                 bad.str() ) );
            CHECK( false );
            continue;
        }

        units::mass sum_of_components_weight;
        for( const item_comp &c : target.get_uncraft_components() ) {
            sum_of_components_weight += c.type->weight * c.count;
        }
        const units::mass weight_difference = units::abs( sum_of_components_weight - item_weight );
        const units::mass weight_tolerance = tolerance * item_weight;
        const bool is_within_tolerance = weight_difference <= weight_tolerance;

        if( is_within_tolerance ) {
            INFO( string_format( "%s had its uncrafting recipe fixed, remove it from the list in data/mods/TEST_DATA/known_bad_uncrafts.json",
                                 bad.str() ) );
            CHECK_FALSE( is_within_tolerance );
        }
    }
}

// NEW fixtures use native constructor identities, never rewrite copied tokens.
static monster &new_capture_predator( const mtype_id &type, const tripoint_bub_ms &position )
{
    for( int n = 0; n < 256; ++n ) {
        auto body = make_shared_fast<monster>( type );
        body->ensure_predator_state();
        const auto &id = body->predator_state().actor_id;
        bool occupied = overmap_buffer.global_state.predator_activity.find( id ) != nullptr;
        for( const monster &other : g->all_monsters() ) {
            occupied = occupied || ( other.is_caol_predator() && other.predator_state().actor_id == id );
        }
        if( occupied ) {
            continue;
        }
        body->ammo = body->type->starting_ammo;
        monster *placed = g->place_critter_at( body, position );
        REQUIRE( placed );
        return *placed;
    }
    FAIL( "no unused native predator constructor identity" );
    std::abort();
}

static std::string capture_body_bytes( const monster &body )
{
    std::ostringstream out;
    JsonOut json( out );
    body.serialize( json );
    return out.str();
}

static std::string capture_ledger_bytes()
{
    std::ostringstream out;
    JsonOut json( out );
    overmap_buffer.global_state.predator_activity.serialize( json );
    return out.str();
}

TEST_CASE( "native_capture_predator_item_owner_freezes_reload_and_resumes_once",
           "[item][predator_activation][capture][native]" )
{
    restore_on_out_of_scope restore_time( calendar::turn );
    restore_on_out_of_scope restore_activity( overmap_buffer.global_state.predator_activity );
    on_out_of_scope cleanup( [] { clear_map_without_vision(); } );
    clear_avatar();
    clear_map_without_vision();
    set_time( calendar::turn_zero + 9000_turns );
    map &here = get_map();
    const tripoint_bub_ms from( 65, 65, 0 );
    const tripoint_bub_ms to = from + point::east * 3;
    here.ter_set( from, ter_id( "t_floor" ) );
    here.ter_set( to, ter_id( "t_floor" ) );
    get_avatar().setpos( here, tripoint_bub_ms( from.xy(), -2 ) );
    const std::string type = GENERATE( "mon_zombie_rider", "mon_writhing_stalker" );
    monster &body = new_capture_predator( mtype_id( type ), from );
    REQUIRE( overmap_buffer.predator_activity_for( body, predator_activation::representation::local, body.pos_abs() ) );
    const auto id = body.predator_state().actor_id;
    const auto epoch = body.predator_state().handoff_epoch;
    body.set_hp( body.get_hp() - 5 );
    body.set_moves( 173 );
    body.add_item( item( itype_id( "rock" ), calendar::turn ) );
    const auto inventory_uid = body.inv.front().uid().get_value();
    const auto hp = body.get_hp();
    const auto ammo = body.ammo;
    const auto active_before = overmap_buffer.predator_active_time( body );
    item carrier( itype_id( "pet_carrier" ), calendar::turn );
    const auto carrier_uid = carrier.uid().get_value();
    carrier.contain_monster( from );
    REQUIRE( carrier.has_var( "contained_json" ) );
    CHECK_FALSE( get_creature_tracker().creature_at<monster>( from ) );
    const auto *record = overmap_buffer.global_state.predator_activity.find( id );
    REQUIRE( record );
    CHECK( record->owner == predator_activation::representation::captured );
    CHECK( record->epoch == epoch + 1 );
    CHECK( record->capture_item_uid == carrier_uid );
    CHECK_FALSE( record->position );
    CHECK_FALSE( record->activity.active );
    const auto contained = carrier.get_var( "contained_json", "" );
    calendar::turn += 600_turns;
    // Actual native item and ledger serializers, without an item/UID copy.
    const std::string item_json = serialize( carrier );
    const std::string ledger_json = capture_ledger_bytes();
    item loaded;
    deserialize_from_string( loaded, item_json );
    { JsonObject json = json_loader::from_string( ledger_json ).get_object();
      overmap_buffer.global_state.predator_activity.deserialize( json ); }
    CHECK( loaded.uid().get_value() == carrier_uid );
    CHECK( loaded.get_var( "contained_json", "" ) == contained );
    const auto refused_before = capture_ledger_bytes();
    here.ter_set( to, ter_id( "t_wall" ) );
    CHECK_FALSE( loaded.release_monster( to, 0 ) );
    CHECK( loaded.get_var( "contained_json", "" ) == contained );
    CHECK( capture_ledger_bytes() == refused_before );
    here.ter_set( to, ter_id( "t_floor" ) );
    // A copied item is not the native captured owner.
    item clone( loaded );
    REQUIRE( clone.uid().get_value() != carrier_uid );
    CHECK_FALSE( clone.release_monster( to, 0 ) );
    CHECK( capture_ledger_bytes() == refused_before );
    REQUIRE( loaded.release_monster( to, 0 ) );
    monster *released = get_creature_tracker().creature_at<monster>( to );
    REQUIRE( released );
    CHECK( released->predator_state().actor_id == id );
    CHECK( released->predator_state().handoff_epoch == epoch + 2 );
    CHECK( released->get_hp() == hp );
    CHECK( released->ammo == ammo );
    CHECK( released->get_moves() == 173 );
    REQUIRE( released->inv.size() == 1 );
    CHECK( released->inv.front().uid().get_value() == inventory_uid );
    CHECK( overmap_buffer.predator_active_time( *released ) == active_before );
    CHECK( released->predator_state().activity_suspension == 600 );
    CHECK_FALSE( loaded.has_var( "contained_json" ) );
    CHECK_FALSE( clone.release_monster( from, 0 ) );
    CHECK( overmap_buffer.global_state.predator_activity.owns( id, epoch + 2,
           predator_activation::representation::local, released->pos_abs() ) );
    const auto once = capture_body_bytes( *released );
    REQUIRE( overmap_buffer.predator_activity_for( *released, predator_activation::representation::local, released->pos_abs() ) );
    CHECK( capture_body_bytes( *released ) == once );
    calendar::turn += 1_turns;
    int earned = -1;
    REQUIRE( overmap_buffer.predator_activity_for( *released, predator_activation::representation::local, released->pos_abs(), &earned ) );
    CHECK( earned == 1 );
    CHECK( overmap_buffer.predator_active_time( *released ) == active_before + 1 );
}

TEST_CASE( "native_capture_refuses_foreign_local_and_missing_owner_without_rebinding",
           "[item][predator_activation][capture][native]" )
{
    restore_on_out_of_scope restore_activity( overmap_buffer.global_state.predator_activity );
    on_out_of_scope cleanup( [] { clear_map_without_vision(); } );
    clear_avatar();
    clear_map_without_vision();
    map &here = get_map();
    const tripoint_bub_ms p( 65, 65, 0 );
    get_avatar().setpos( here, p + point::west * 3 );
    monster &body = new_capture_predator( mtype_id( "mon_writhing_stalker" ), p );
    REQUIRE( overmap_buffer.predator_activity_for( body, predator_activation::representation::local, body.pos_abs() ) );
    item carrier( itype_id( "pet_carrier" ), calendar::turn );
    carrier.set_var( "contained_json", capture_body_bytes( body ) );
    const auto before_body = capture_body_bytes( body );
    const auto before_record = capture_ledger_bytes();
    CHECK_FALSE( carrier.release_monster( p + point::east, 0 ) );
    CHECK( capture_body_bytes( body ) == before_body );
    CHECK( capture_ledger_bytes() == before_record );
    g->remove_zombie( body );
    CHECK_FALSE( carrier.release_monster( p + point::east, 0 ) );
    CHECK( capture_ledger_bytes() == before_record );
    CHECK( carrier.get_var( "contained_json", "" ) == before_body );
}

TEST_CASE( "native_placement_acceptance_is_once_before_gravity_and_absent_on_refusal",
           "[item][predator_activation][capture][native]" )
{
    restore_on_out_of_scope restore_activity( overmap_buffer.global_state.predator_activity );
    on_out_of_scope cleanup( [] { clear_map_without_vision(); } );
    clear_avatar();
    clear_map_without_vision();
    map &here = get_map();
    const tripoint_bub_ms p( 65, 65, 0 );
    get_avatar().setpos( here, p + point::west * 3 );
    auto candidate = make_shared_fast<monster>( mtype_id( "mon_zombie" ) );
    int callbacks = 0;
    here.ter_set( p, ter_id( "t_wall" ) );
    CHECK_FALSE( g->place_critter_around( candidate, p, 0, false, [&]( monster & ) { ++callbacks; } ) );
    CHECK( callbacks == 0 );
    here.ter_set( p, ter_id( "t_floor" ) );
    REQUIRE( g->place_critter_around( candidate, p, 0, false, [&]( monster &accepted ) {
        ++callbacks;
        CHECK( &accepted == candidate.get() );
        CHECK( accepted.pos_bub() == p );
        CHECK( get_creature_tracker().creature_at<monster>( p ) == &accepted );
    } ) );
    CHECK( callbacks == 1 );
    CHECK_FALSE( candidate->is_caol_predator() );
}

TEST_CASE( "native_capture_acceptance_before_forced_gravity_keeps_local_or_death_owner",
           "[item][predator_activation][capture][native][falling]" )
{
    restore_on_out_of_scope restore_activity( overmap_buffer.global_state.predator_activity );
    on_out_of_scope cleanup( [] { clear_map_without_vision(); } );
    clear_avatar();
    clear_map_without_vision();
    map &here = get_map();
    const tripoint_bub_ms from( 65, 65, 0 );
    const tripoint_bub_ms air = from + point::east * 3;
    const tripoint_bub_ms landing = air + tripoint( 0, 0, -3 );
    here.ter_set( from, ter_id( "t_floor" ) );
    for( int z = 0; z > -3; --z ) {
        here.ter_set( tripoint_bub_ms( air.xy(), z ), ter_id( "t_open_air" ) );
    }
    here.ter_set( landing, ter_id( "t_floor" ) );
    get_avatar().setpos( here, from + point::west * 3 );
    const std::string type = GENERATE( "mon_zombie_rider", "mon_writhing_stalker" );
    const bool fatal = GENERATE( false, true );
    CAPTURE( type, fatal );
    monster &body = new_capture_predator( mtype_id( type ), from );
    REQUIRE( overmap_buffer.predator_activity_for( body, predator_activation::representation::local, body.pos_abs() ) );
    const auto id = body.predator_state().actor_id;
    const auto epoch = body.predator_state().handoff_epoch;
    if( fatal ) {
        body.set_hp( 1 );
    }
    item carrier( itype_id( "pet_carrier" ), calendar::turn );
    carrier.contain_monster( from );
    REQUIRE( carrier.has_var( "contained_json" ) );
    // These real species avoid unsupported air at the ordinary item route.
    // Retain that refusal. Exercise the separate existing forced-placement
    // route to test acceptance ordering against genuine native gravity.
    const auto ledger_before = capture_ledger_bytes();
    const auto item_before = serialize( carrier );
    CHECK_FALSE( carrier.release_monster( air, 0 ) );
    CHECK( serialize( carrier ) == item_before );
    CHECK( capture_ledger_bytes() == ledger_before );
    auto candidate = make_shared_fast<monster>();
    deserialize_from_string( *candidate, carrier.get_var( "contained_json", "" ) );
    REQUIRE( overmap_buffer.can_release_captured_predator( *candidate, carrier.uid().get_value() ) );
    int callbacks = 0;
    REQUIRE( g->place_critter_around( candidate, air, 0, true, [&]( monster &accepted ) {
        ++callbacks;
        CHECK( accepted.pos_bub() == air );
        CHECK( get_creature_tracker().creature_at<monster>( air ) == &accepted );
        overmap_buffer.release_captured_predator( accepted, carrier.uid().get_value(), epoch );
        carrier.erase_var( "contained_name" );
        carrier.erase_var( "contained_json" );
        carrier.erase_var( "name" );
        carrier.erase_var( "weight" );
    } ) );
    CHECK( callbacks == 1 );
    CHECK_FALSE( carrier.has_var( "contained_json" ) );
    // creature_at intentionally excludes dead bodies before cleanup. Retain
    // the actual accepted native shared_ptr, not an absence-as-death premise.
    monster *fallen = candidate.get();
    REQUIRE( fallen->pos_bub() == landing );
    CHECK( ( get_creature_tracker().creature_at<monster>( landing ) == fallen ) == !fatal );
    CHECK( fallen->predator_state().handoff_epoch == epoch + 2 );
    CHECK( overmap_buffer.global_state.predator_activity.owns( id, epoch + 2,
           predator_activation::representation::local, fallen->pos_abs() ) );
    CHECK( fallen->is_dead() == fatal );
    g->cleanup_dead(); // ordinary death owns the accepted post-gravity body
    const auto *record = overmap_buffer.global_state.predator_activity.find( id );
    REQUIRE( record );
    CHECK( record->epoch == epoch + 2 );
    CHECK( record->owner == ( fatal ? predator_activation::representation::dead :
                             predator_activation::representation::local ) );
    CHECK_FALSE( carrier.has_var( "contained_json" ) );
}

TEST_CASE( "native_capture_legacy_item_adopts_zero_only_without_genuine_owner",
           "[item][predator_activation][capture][native][legacy]" )
{
    restore_on_out_of_scope restore_time( calendar::turn );
    restore_on_out_of_scope restore_activity( overmap_buffer.global_state.predator_activity );
    on_out_of_scope cleanup( [] { clear_map_without_vision(); } );
    clear_avatar();
    clear_map_without_vision();
    set_time( calendar::turn_zero + 12000_turns );
    map &here = get_map();
    const tripoint_bub_ms from( 65, 65, 0 );
    const tripoint_bub_ms to = from + point::east * 3;
    get_avatar().setpos( here, from + point::west * 3 );
    here.ter_set( from, ter_id( "t_floor" ) );
    here.ter_set( to, ter_id( "t_floor" ) );
    overmap_buffer.bootstrap_predator_positions();
    monster &body = new_capture_predator( mtype_id( "mon_writhing_stalker" ), from );
    const auto id = body.predator_state().actor_id;
    const auto epoch = body.predator_state().handoff_epoch;
    REQUIRE_FALSE( overmap_buffer.global_state.predator_activity.find( id ) );
    item carrier( itype_id( "pet_carrier" ), calendar::turn );
    // Actual old native item format, prior to introduction of CAPTURED.
    carrier.set_var( "contained_json", capture_body_bytes( body ) );
    carrier.set_var( "contained_name", body.type->nname() );
    const auto before = capture_ledger_bytes();
    CHECK_FALSE( carrier.release_monster( to, 0 ) ); // genuine unqueried LOCAL owner
    CHECK( capture_ledger_bytes() == before );
    g->remove_zombie( body );
    calendar::turn += 777_turns;
    REQUIRE( carrier.release_monster( to, 0 ) );
    monster *released = get_creature_tracker().creature_at<monster>( to );
    REQUIRE( released );
    CHECK( released->predator_state().actor_id == id );
    CHECK( released->predator_state().handoff_epoch == epoch + 2 );
    CHECK( overmap_buffer.predator_active_time( *released ) == 0 );
    CHECK( released->predator_state().activity_adopted_world == to_turn<int>( calendar::turn ) );
    CHECK( released->predator_state().activity_suspension == 0 );
    const auto once = capture_body_bytes( *released );
    released->process_turn();
    CHECK( capture_body_bytes( *released ) == once );
}

TEST_CASE( "native_capture_settles_prior_frozen_body_pause_before_snapshot_once",
           "[item][predator_activation][capture][native][biology]" )
{
    restore_on_out_of_scope restore_time( calendar::turn );
    restore_on_out_of_scope restore_activity( overmap_buffer.global_state.predator_activity );
    on_out_of_scope cleanup( [] { clear_map_without_vision(); } );
    clear_avatar();
    clear_map_without_vision();
    set_time( calendar::turn_zero + 15013_turns );
    map &here = get_map();
    const tripoint_bub_ms from( 65, 65, 0 );
    const tripoint_bub_ms to = from + point::east * 3;
    here.ter_set( from, ter_id( "t_floor" ) );
    here.ter_set( to, ter_id( "t_floor" ) );
    get_avatar().setpos( here, from + point::west * 3 );
    const std::string type = GENERATE( "mon_zombie_rider", "mon_writhing_stalker" );
    monster &body = new_capture_predator( mtype_id( type ), from );
    REQUIRE( overmap_buffer.predator_activity_for( body, predator_activation::representation::local, body.pos_abs() ) );
    body.set_baby_timer( calendar::turn + 61_minutes );
    const auto anchor = []( const monster &value, const std::string &key ) {
        const auto json = json_loader::from_string( capture_body_bytes( value ) ).get_object();
        json.allow_omitted_members();
        return json.get_int64( key );
    };
    const auto baby = anchor( body, "baby_timer" );
    const auto update = anchor( body, "last_updated" );
    const auto evolution = anchor( body, "suspended_evolution_time" );
    const auto active = overmap_buffer.predator_active_time( body );
    const auto where = body.pos_abs_omt();
    get_avatar().setpos( project_to<coords::ms>( where - point( 41, 0 ) ), false );
    CHECK_FALSE( overmap_buffer.predator_activity_for( body, predator_activation::representation::local, body.pos_abs() ) );
    calendar::turn += 77_turns;
    item carrier( itype_id( "pet_carrier" ), calendar::turn );
    carrier.contain_monster( from );
    REQUIRE( carrier.has_var( "contained_json" ) );
    monster snapshot;
    deserialize_from_string( snapshot, carrier.get_var( "contained_json", "" ) );
    CHECK( snapshot.predator_state().activity_suspension == 77 );
    CHECK( anchor( snapshot, "baby_timer" ) == baby + 77 );
    calendar::turn += 53_turns;
    get_avatar().setpos( here, from + point::west * 3 );
    REQUIRE( carrier.release_monster( to, 0 ) );
    monster *released = get_creature_tracker().creature_at<monster>( to );
    REQUIRE( released );
    CHECK( released->predator_state().activity_suspension == 130 );
    CHECK( anchor( *released, "baby_timer" ) == baby + 130 );
    CHECK( anchor( *released, "last_updated" ) == update + 130 );
    CHECK( anchor( *released, "suspended_evolution_time" ) == evolution + 130 );
    CHECK( overmap_buffer.predator_active_time( *released ) == active );
    const auto once = capture_body_bytes( *released );
    REQUIRE( overmap_buffer.predator_activity_for( *released, predator_activation::representation::local, released->pos_abs() ) );
    CHECK( capture_body_bytes( *released ) == once );
}


TEST_CASE( "native_capture_refuses_malformed_epoch_or_future_body_without_mutation",
           "[item][predator_activation][capture][native]" )
{
    restore_on_out_of_scope restore_time( calendar::turn );
    restore_on_out_of_scope restore_activity( overmap_buffer.global_state.predator_activity );
    on_out_of_scope cleanup( [] { clear_map_without_vision(); } );
    clear_avatar();
    clear_map_without_vision();
    set_time( calendar::turn_zero + 19000_turns );
    map &here = get_map();
    const tripoint_bub_ms p( 65, 65, 0 );
    here.ter_set( p, ter_id( "t_floor" ) );
    get_avatar().setpos( here, p + point::west * 3 );
    monster &body = new_capture_predator( mtype_id( "mon_writhing_stalker" ), p );
    const bool future = GENERATE( false, true );
    if( future ) {
        REQUIRE( overmap_buffer.predator_activity_for( body, predator_activation::representation::local, body.pos_abs() ) );
        calendar::turn -= 1_turns;
    } else {
        // Deliberately invalid input, not an authenticated positive identity.
        body.predator_state().handoff_epoch = std::numeric_limits<std::uint64_t>::max();
    }
    item carrier( itype_id( "pet_carrier" ), calendar::turn );
    const auto body_before = capture_body_bytes( body );
    const auto item_before = serialize( carrier );
    const auto ledger_before = capture_ledger_bytes();
    carrier.contain_monster( p );
    CHECK( capture_body_bytes( body ) == body_before );
    CHECK( serialize( carrier ) == item_before );
    CHECK( capture_ledger_bytes() == ledger_before );
    CHECK( get_creature_tracker().creature_at<monster>( p ) == &body );
}

TEST_CASE( "native_capture_ordinary_nonpredator_preserves_native_item_and_body_route",
           "[item][predator_activation][capture][native]" )
{
    restore_on_out_of_scope restore_activity( overmap_buffer.global_state.predator_activity );
    on_out_of_scope cleanup( [] { clear_map_without_vision(); } );
    clear_avatar();
    clear_map_without_vision();
    map &here = get_map();
    const tripoint_bub_ms from( 65, 65, 0 );
    const tripoint_bub_ms to = from + point::east * 3;
    here.ter_set( from, ter_id( "t_floor" ) );
    here.ter_set( to, ter_id( "t_floor" ) );
    get_avatar().setpos( here, from + point::west * 3 );
    monster *body = g->place_critter_at( mtype_id( "mon_zombie" ), from );
    REQUIRE( body );
    body->set_hp( body->get_hp() - 3 );
    body->add_item( item( itype_id( "rock" ), calendar::turn ) );
    const auto hp = body->get_hp();
    const auto rock = body->inv.front().uid().get_value();
    const auto ledger = capture_ledger_bytes();
    item carrier( itype_id( "pet_carrier" ), calendar::turn );
    const auto carrier_uid = carrier.uid().get_value();
    carrier.contain_monster( from );
    REQUIRE( carrier.has_var( "contained_json" ) );
    REQUIRE( carrier.release_monster( to, 0 ) );
    body = get_creature_tracker().creature_at<monster>( to );
    REQUIRE( body );
    CHECK_FALSE( body->is_caol_predator() );
    CHECK( body->type->id == mtype_id( "mon_zombie" ) );
    CHECK( body->get_hp() == hp );
    REQUIRE( body->inv.size() == 1 );
    CHECK( body->inv.front().uid().get_value() == rock );
    CHECK( carrier.uid().get_value() == carrier_uid );
    CHECK_FALSE( carrier.has_var( "contained_json" ) );
    CHECK( capture_ledger_bytes() == ledger );
}
