#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "activity_tracker.h"
#include "calendar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "game_constants.h"
#include "json.h"
#include "json_loader.h"
#include "rng.h"

TEST_CASE( "activity accounting does not recount idle turns since the last action",
           "[activity_tracker][activity_elapsed]" )
{
    const time_point old_turn = calendar::turn;
    on_out_of_scope restore_time( [old_turn]() {
        calendar::turn = old_turn;
    } );
    calendar::turn = calendar::turn_zero + 50000_turns;
    activity_tracker tracker;
    tracker.new_turn();
    tracker.log_activity( 4.0f );
    for( int elapsed = 1; elapsed <= 6; ++elapsed ) {
        calendar::turn += 1_turns;
        tracker.new_turn();
        CHECK( tracker.activity() == NO_EXERCISE );
        CHECK( tracker.average_activity() == Approx( ( 4.0f + elapsed ) / ( elapsed + 1 ) ) );
    }
    std::ostringstream saved;
    { JsonOut out( saved ); tracker.serialize( out ); }
    const JsonValue value = json_loader::from_string( saved.str() );
    const JsonObject history = value.get_object();
    history.allow_omitted_members();
    CHECK( history.get_int( "num_events" ) == 7 );
    // Advancing accounting must not invent a logged action at the new time.
    CHECK( history.get_int( "current_turn" ) == 50000 );
}

TEST_CASE( "activity intervals preserve idle sleep reset and zero elapsed accounting",
           "[activity_tracker][activity_elapsed]" )
{
    const time_point old_turn = calendar::turn;
    on_out_of_scope restore_time( [old_turn]() {
        calendar::turn = old_turn;
    } );
    const bool sleeping = GENERATE( false, true );
    const float base = sleeping ? SLEEP_EXERCISE : NO_EXERCISE;
    calendar::turn = calendar::turn_zero + 50000_turns;
    activity_tracker sequential;
    sequential.new_turn( sleeping );
    activity_tracker batched = sequential;
    for( int i = 0; i < 10; ++i ) {
        calendar::turn += 1_turns;
        sequential.new_turn( sleeping );
    }
    // Same interval, evaluated at its endpoint rather than by moving global time.
    batched.new_turn( sleeping, 10_turns );
    CHECK( sequential.average_activity() == Approx( base ) );
    CHECK( batched.average_activity() == Approx( sequential.average_activity() ) );
    const auto saved = []( const activity_tracker & tracker ) {
        std::ostringstream bytes;
        JsonOut out( bytes );
        tracker.serialize( out );
        return bytes.str();
    };
    for( const activity_tracker *tracker : { &batched, &sequential } ) {
        JsonValue value = json_loader::from_string( saved( *tracker ) );
        const JsonObject history = value.get_object();
        history.allow_omitted_members();
        CHECK( history.get_int( "num_events" ) == 11 );
        CHECK( history.get_float( "accumulated_activity" ) == Approx( 10 * base ) );
        CHECK( history.get_int( "current_turn" ) == 0 );
    }
    const std::string before_no_time = saved( batched );
    batched.new_turn( !sleeping, 0_turns );
    CHECK( saved( batched ) == before_no_time );
    batched.new_turn( !sleeping, -1_turns );
    CHECK( saved( batched ) == before_no_time );

    batched.log_activity( 4.0f );
    calendar::turn += 1_turns;
    batched.new_turn( sleeping );
    const float prior_average = batched.average_activity();
    batched.reset_activity_level();
    CHECK( batched.average_activity() == Approx( prior_average - base / 12.0f ) );
    // A reset starts a new period; neither prior idle time nor the last action's
    // timestamp is a new contribution to it.
    calendar::turn += 1_turns;
    batched.new_turn( sleeping );
    CHECK( batched.average_activity() == Approx( base ) );
    calendar::turn += 5_turns;
    batched.new_turn( sleeping, 5_turns );
    CHECK( batched.average_activity() == Approx( base ) );
    activity_tracker restored;
    JsonValue value = json_loader::from_string( saved( batched ) );
    restored.deserialize( value.get_object() );
    CHECK( saved( restored ) == saved( batched ) );
}

static void test_activity_tracker( const std::vector<float> &values )
{
    activity_tracker tracker;
    for( float i : values ) {
        calendar::turn += 1_turns;
        tracker.new_turn( false );
        // If we're on a "new turn", we should have nominal activity.
        CHECK( tracker.activity( false ) == 1.0 );
        // Smaller values inserted before and after the highest value should be irrelevant.
        tracker.log_activity( rng_float( 0.0f, i - 0.01f ) );
        tracker.log_activity( i );
        tracker.log_activity( rng_float( 0.0f, i - 0.01f ) );
        // Verify the highest value inserted is the current value.
        CHECK( tracker.activity( false ) == i );
    }
    int end_value = values.back();
    // activity() still returns most recently logged activity.
    CHECK( tracker.activity( false ) == end_value );
    const float expected_activity = std::accumulate( values.begin(), values.end(), 0.0f ) /
                                    static_cast<float>( values.size() );
    // average_activity() returns average of the most recent period.
    CHECK( tracker.average_activity() == expected_activity );
    tracker.reset_activity_level();
    // activity() should be unchanged after a reset. (it's still the same turn)
    CHECK( tracker.activity( false ) == end_value );
    // average_activity() also continues to return the previous value.
    CHECK( tracker.average_activity() == expected_activity );
    calendar::turn += 1_turns;
    // activity() returns 1.0 now that it's a new turn.
    CHECK( tracker.activity( false ) == 1.0f );
    tracker.new_turn( false );
    tracker.log_activity( 5.0f );
    // After starting a new recording cycle, activity() and average_activity() return the new data.
    CHECK( tracker.activity( false ) == 5.0f );
    CHECK( tracker.average_activity() == 5.0f );
    calendar::turn += 1_turns;
    tracker.new_turn( false );
    tracker.log_activity( 7.0f );
    // And the behavior continues.
    CHECK( tracker.activity( false ) == 7.0f );
    CHECK( tracker.average_activity() == 6.0f );
    calendar::turn = calendar::turn_zero;
}

TEST_CASE( "activity_counter_from_1_to_300", "[activity_tracker]" )
{
    std::vector<float> values;
    for( int i = 1; i <= 300; ++i ) {
        values.push_back( i );
    }
    calendar::turn += 50000_turns;
    test_activity_tracker( values );
    calendar::turn = calendar::turn_zero;
}

TEST_CASE( "activity_tracker_from_300_to_1", "[activity_tracker]" )
{
    std::vector<float> values;
    for( int i = 300; i > 0; --i ) {
        values.push_back( i );
    }
    test_activity_tracker( values );
}

TEST_CASE( "activity_tracker_constant_value", "[activity_tracker]" )
{
    std::vector<float> values( 300, 1.0 );
    test_activity_tracker( values );
}

TEST_CASE( "activity_tracker_intermittent_values", "[activity_tracker]" )
{
    std::vector<float> values( 300, 1.0 );
    for( int i = 0; i < 300; i += 30 ) {
        values[ i ] = 10.0f;
    }
    test_activity_tracker( values );
}

TEST_CASE( "activity_tracker_string_representation", "[activity_tracker]" )
{
    activity_tracker tracker;
    // Start at the lowest level
    tracker.reset_activity_level();
    REQUIRE( tracker.activity_level_str() == "SLEEP_EXERCISE" );

    // Increase level a couple times
    tracker.log_activity( LIGHT_EXERCISE );
    CHECK( tracker.activity_level_str() == "LIGHT_EXERCISE" );
    tracker.log_activity( MODERATE_EXERCISE );
    CHECK( tracker.activity_level_str() == "MODERATE_EXERCISE" );
    // Cannot 'increase' to lower level
    tracker.log_activity( LIGHT_EXERCISE );
    CHECK( tracker.activity_level_str() == "MODERATE_EXERCISE" );
    tracker.log_activity( NO_EXERCISE );
    CHECK( tracker.activity_level_str() == "MODERATE_EXERCISE" );
    // Increase to highest level
    tracker.log_activity( ACTIVE_EXERCISE );
    CHECK( tracker.activity_level_str() == "ACTIVE_EXERCISE" );
    tracker.log_activity( EXTRA_EXERCISE );
    CHECK( tracker.activity_level_str() == "EXTRA_EXERCISE" );
    // Cannot increase beyond the highest
    tracker.log_activity( EXTRA_EXERCISE );
    CHECK( tracker.activity_level_str() == "EXTRA_EXERCISE" );
}
