#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "calendar.h"
#include "cata_catch.h"
#include "json.h"
#include "json_loader.h"
#include "migo_population.h"

namespace
{
migo_population::site_record tower()
{
    using namespace migo_population;
    site_record site;
    site.id = "test-tower";
    site.anchor = tripoint_abs_omt( 2, 3, 0 );
    site.tower = true;
    std::vector<member_record> slots;
    for( int i = 0; i < 8; ++i ) {
        member_record m;
        m.token = { site.id, "original-" + std::to_string( i ), 0 };
        m.type = mtype_id( i < 6 ? "mon_mi_go" : "mon_mi_go_scout" );
        m.home = tripoint_abs_ms( 50 + i, 75, 0 );
        m.hp = 100;
        slots.push_back( m );
    }
    REQUIRE( site.commit_seed( slots, tripoint_abs_ms( 50, 74, 0 ),
                              "f_migo_brood_pod", 0, true, true ) );
    return site;
}

migo_population::site_record reload( const migo_population::site_record &site )
{
    std::ostringstream out;
    { JsonOut json( out ); site.serialize( json ); }
    migo_population::site_record loaded;
    loaded.deserialize( json_loader::from_string( out.str() ) );
    return loaded;
}
} // namespace

TEST_CASE( "migo_population_seed_and_native_leases_do_not_refill_or_revive",
           "[migo_population]" )
{
    using namespace migo_population;
    site_record site = tower();
    const std::vector<member_record> original = site.members;
    CHECK_FALSE( site.commit_seed( original, tripoint_abs_ms( 99, 99, 0 ),
                                  "f_alien_pod", 0, true, true ) );
    CHECK( site.source == tripoint_abs_ms( 50, 74, 0 ) );
    const identity slot = site.members[0].token;
    REQUIRE( site.transfer( slot, representation::slot, representation::local ) );
    CHECK_FALSE( site.transfer( slot, representation::slot, representation::local ) );
    const identity local = site.members[0].token;
    REQUIRE( site.transfer( local, representation::local, representation::abstract ) );
    const identity abstract = site.members[0].token;
    CHECK( abstract.member_id == slot.member_id );
    CHECK( abstract.epoch == slot.epoch + 2 );
    CHECK_FALSE( site.record_death( local ) ); // Old snapshot cannot kill the new owner.
    REQUIRE( site.transfer( abstract, representation::abstract, representation::local ) );
    REQUIRE( site.record_death( site.members[0].token ) );
    site = reload( site );
    CHECK_FALSE( site.transfer( abstract, representation::abstract, representation::local ) );
    CHECK_FALSE( site.observe_health( site.members[0].token, 100 ) );
    CHECK_FALSE( site.record_death( site.members[0].token ) );
    CHECK( site.living_count() == 7 );
    CHECK( site.members.size() == 8 );
    CHECK_FALSE( site.commit_seed( original, site.source, "f_migo_brood_pod", 0, true, true ) );
}

TEST_CASE( "migo_population_reservations_count_every_living_member_and_return_once",
           "[migo_population]" )
{
    using namespace migo_population;
    site_record site = tower();
    const auto healthy = []( const member_record &m ) { return m.hp >= 100; };
    CHECK( site.reserve( 1, healthy ).empty() ); // A blocked/unmaterialized slot is unavailable.
    for( member_record &m : site.members ) {
        REQUIRE( site.transfer( m.token, representation::slot, representation::abstract ) );
    }
    site.members[0].hp = 20;
    CHECK( site.reserve( 6, healthy ).empty() ); // Seven healthy, keep two.
    const auto party = site.reserve( 5, healthy );
    REQUIRE( party.size() == 5 );
    CHECK( site.living_count() == 8 );
    CHECK( site.reserve( 1, healthy ).empty() );
    REQUIRE( site.returning( party[0] ) );
    CHECK_FALSE( site.returned( party[0], tripoint_abs_ms( 1000, 1000, 0 ) ) );
    REQUIRE( site.returned( party[0], site.find( party[0].member_id )->home ) );
    CHECK_FALSE( site.returned( party[0], site.find( party[0].member_id )->home ) );
    CHECK( site.living_count() == 8 );
}

TEST_CASE( "migo_population_production_has_one_due_event_and_no_discovery_backlog",
           "[migo_population]" )
{
    using namespace migo_population;
    site_record site = tower();
    REQUIRE( site.record_death( site.members[6].token ) ); // Missing scout is replaced toward 3:1.
    std::vector<member_record> births;
    const auto place = [&]( const member_record &m ) { births.push_back( m ); return true; };
    const auto blocked = []( const member_record & ) { return false; };
    const int week = to_turns<int>( 7_days );
    CHECK_FALSE( site.advance( week, week, true, place ) ); // First service starts now.
    CHECK( site.production_elapsed == 0 );
    CHECK_FALSE( site.advance( week + 1, 0, true, place ) ); // Frozen interval is not credit.
    CHECK_FALSE( site.advance( 2 * week, week - 1, true, place ) );
    CHECK_FALSE( site.advance( 2 * week + 1, 1, true, blocked ) );
    CHECK( site.production_elapsed == week );
    site = reload( site );
    CHECK_FALSE( site.advance( 2 * week + 1, week, true, place ) ); // Repeat/reload cannot spend twice.
    CHECK_FALSE( site.advance( 20 * week, 18 * week - 1, true, blocked ) );
    CHECK( site.production_elapsed == week );
    REQUIRE( site.advance( 20 * week + 1, 1, true, place ) );
    REQUIRE( births.size() == 1 );
    CHECK( births[0].type == mtype_id( "mon_mi_go_scout" ) );
    CHECK( births[0].token.member_id != "original-6" );
    CHECK( births[0].birth_sequence == 1 );
    CHECK( site.living_count() == 8 );
    CHECK_FALSE( site.advance( 40 * week, 20 * week - 1, true, place ) ); // Ceiling stores no credit.
    REQUIRE( site.record_death( site.members[0].token ) );
    CHECK_FALSE( site.advance( 40 * week + 1, 1, true, place ) );
    CHECK( site.production_elapsed == 1 );
    CHECK( births.size() == 1 );
}

TEST_CASE( "migo_population_legacy_capacity_and_source_facts_are_conservative",
           "[migo_population]" )
{
    using namespace migo_population;
    site_record site = tower();
    REQUIRE( site.record_death( site.members[0].token ) );
    const int week = to_turns<int>( 7_days );
    int births = 0;
    const auto place = [&]( const member_record & ) { ++births; return true; };
    site.occupancy_verified = false;
    CHECK_FALSE( site.advance( 0, week, true, place ) );
    CHECK_FALSE( site.advance( week, week, true, place ) );
    CHECK( site.production_elapsed == 0 );
    site.occupancy_verified = true;
    site.ambient_occupancy = 1;
    CHECK_FALSE( site.advance( 2 * week, week, true, place ) );
    CHECK( site.production_elapsed == 0 );
    site.ambient_occupancy = 0;
    CHECK_FALSE( site.advance( 3 * week, week, std::nullopt, place ) );
    CHECK( site.production_elapsed == 0 );
    CHECK_FALSE( site.advance( 3 * week + 1, 1, false, place ) );
    CHECK( site.source_destroyed );
    site = reload( site );
    CHECK_FALSE( site.advance( 10 * week, 7 * week - 1, true, place ) );
    CHECK( site.source_destroyed );
    CHECK( births == 0 );

    site_record camp;
    camp.id = "legacy-camp";
    std::vector<member_record> residents = tower().members;
    residents.push_back( residents[0] );
    residents.back().token.member_id = "capable-guard";
    residents.back().type = mtype_id( "mon_mi_go_guard" );
    for( member_record &m : residents ) {
        m.token.site_id = camp.id;
    }
    REQUIRE( camp.commit_seed( residents, std::nullopt, "", 2, true, false ) );
    CHECK( camp.ceiling == 9 );
    CHECK( camp.living_count() == 9 );
    CHECK( camp.members.back().type == mtype_id( "mon_mi_go_guard" ) );
    CHECK_FALSE( camp.source );
    CHECK_FALSE( camp.advance( week, week, true, place ) );
}

TEST_CASE( "migo_due_birth_waits_for_admitted_active_time_after_freeze_or_reload",
           "[migo_population][production][regression]" )
{
    using namespace migo_population;
    site_record site = tower();
    REQUIRE( site.record_death( site.members.front().token ) );
    const int week = to_turns<int>( 7_days );
    const auto blocked = []( const member_record & ) { return false; };
    CHECK_FALSE( site.advance( 0, 0, true, blocked ) );
    CHECK_FALSE( site.advance( week, week, true, blocked ) );
    REQUIRE( site.production_elapsed == week );
    int births = 0;
    const auto place = [&]( const member_record & ) { ++births; return true; };
    CHECK_FALSE( site.advance( week + 1, 0, true, place ) );
    CHECK( births == 0 );
    site = reload( site );
    CHECK_FALSE( site.advance( 10 * week, 0, true, place ) );
    CHECK( births == 0 );
    REQUIRE( site.advance( 10 * week + 1, 1, true, place ) );
    CHECK( births == 1 );
    CHECK( site.production_elapsed == 0 );
}

TEST_CASE( "migo_exact_party_reservation_authenticates_all_members_and_keeps_defenders",
           "[migo_population][migo_tower]" )
{
    using namespace migo_population;
    site_record site = tower();
    for( const auto &m : site.members ) {
        REQUIRE( site.transfer( m.token, representation::slot, representation::abstract ) );
    }
    const auto healthy = []( const member_record &m ) { return m.hp >= 50; };
    const std::vector<identity> preferred = {site.members[0].token, site.members[1].token,
                                           site.members[6].token};
    auto bad = preferred;
    ++bad.back().epoch;
    CHECK( site.reserve_exact( bad, healthy ).empty() );
    CHECK( site.reserve_exact( {preferred[0], preferred[0], preferred[2]}, healthy ).empty() );
    CHECK( std::all_of( site.members.begin(), site.members.end(), []( const auto &m ) {
        return m.assignment == duty::available;
    } ) );
    REQUIRE( site.reserve_exact( preferred, healthy ).size() == 3 );
    CHECK( site.find( preferred[2].member_id )->assignment == duty::dispatched );
    CHECK( site.members[2].assignment == duty::available );
    CHECK( site.reserve_exact( preferred, healthy ).empty() );
    site = reload( site );
    site.members[2].hp = 1; // Five unleased bodies, only four capable.
    CHECK( site.reserve_exact( {site.members[3].token, site.members[4].token,
                               site.members[5].token}, healthy ).empty() );
    REQUIRE( site.reserve_exact( {site.members[3].token, site.members[7].token}, healthy ).size() == 2 );
    CHECK( site.members[4].assignment == duty::available );
    CHECK( site.members[5].assignment == duty::available );
}
