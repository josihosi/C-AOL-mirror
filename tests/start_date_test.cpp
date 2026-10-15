#include <functional>
#include <memory>
#include <string>

#include "calendar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "game.h"
#include "scenario.h"
#include "string_id.h"

static const string_id<scenario> scenario_test_custom_both( "test_custom_both" );
static const string_id<scenario> scenario_test_custom_cataclysm( "test_custom_cataclysm" );
static const string_id<scenario> scenario_test_custom_game( "test_custom_game" );
static const string_id<scenario> scenario_test_custom_game_invalid( "test_custom_game_invalid" );

TEST_CASE( "Test_start_dates" )
{
    int default_season_length = 91;
    int default_year_length = default_season_length * 4;
    const scenario *previous_scenario = get_scenario();
    const time_point previous_turn = calendar::turn;
    const time_point previous_start_of_cataclysm = calendar::start_of_cataclysm;
    const time_point previous_start_of_game = calendar::start_of_game;
    const season_type previous_initial_season = calendar::initial_season;

    on_out_of_scope guard{ [previous_scenario, previous_turn, previous_start_of_cataclysm,
                                               previous_start_of_game, previous_initial_season]()
    {
        set_scenario( previous_scenario );
        calendar::turn = previous_turn;
        calendar::start_of_cataclysm = previous_start_of_cataclysm;
        calendar::start_of_game = previous_start_of_game;
        calendar::initial_season = previous_initial_season;
    } };

    SECTION( "Scenario with custom game start date" ) {
        const scenario &scen = scenario_test_custom_game.obj();
        set_scenario( &scen );
        g->start_calendar();

        CHECK( calendar::start_of_game == calendar::turn_zero +
               1_hours * 18 +
               1_days * 6 +
               1_days * default_season_length * season_type::SUMMER +
               1_days * default_year_length * 3
             );
    }

    SECTION( "Scenario has game start date before cataclysm start date" ) {
        const scenario &scen = scenario_test_custom_game_invalid.obj();
        set_scenario( &scen );
        g->start_calendar();

        CHECK( calendar::start_of_game == calendar::start_of_cataclysm );
    }

    SECTION( "Scenario with custom cataclysm start date" ) {
        const scenario &scen = scenario_test_custom_cataclysm.obj();
        set_scenario( &scen );
        g->start_calendar();

        CHECK( calendar::start_of_cataclysm == calendar::turn_zero +
               1_hours * 1 +
               1_days * 8 +
               1_days * default_season_length * season_type::AUTUMN +
               1_days * default_year_length * 6
             );
    }

    SECTION( "Scenario with custom cataclysm start date and game start date" ) {
        const scenario &scen = scenario_test_custom_both.obj();
        set_scenario( &scen );
        g->start_calendar();

        CHECK( calendar::start_of_cataclysm == calendar::turn_zero +
               1_hours * 6 +
               1_days * 3 +
               1_days * default_season_length * season_type::WINTER +
               1_days * default_year_length * 5
             );
        CHECK( calendar::start_of_game == calendar::turn_zero +
               1_hours * 13 +
               1_days * 18 +
               1_days * default_season_length * season_type::AUTUMN +
               1_days * default_year_length * 8
             );
    }
}
