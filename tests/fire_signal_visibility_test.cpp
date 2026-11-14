#include <string>
#include <vector>

#include "cata_catch.h"
#include "fire_signal_visibility.h"
#include "weather_type.h"

TEST_CASE( "fire_signal_stock_weather_calibration", "[fire_signal_visibility]" )
{
    struct expected_weather {
        const char *id;
        int smoke;
        int light;
        int smoke_cap;
        int light_cap;
    };
    const std::vector<expected_weather> cases = {
        { "clear", 100, 100, 30, 2147483647 }, { "sunny", 100, 100, 30, 2147483647 },
        { "cloudy", 90, 100, 27, 2147483647 }, { "light_drizzle", 80, 90, 20, 20 },
        { "drizzle", 65, 80, 15, 15 }, { "rain", 40, 60, 8, 10 },
        { "rainstorm", 20, 35, 3, 5 }, { "thunder", 20, 35, 3, 5 },
        { "lightning", 20, 35, 3, 5 }, { "flurries", 65, 80, 15, 15 },
        { "snowing", 35, 50, 6, 8 }, { "snowstorm", 10, 20, 1, 2 },
        { "mist", 25, 40, 5, 8 }, { "fog", 5, 10, 1, 2 },
        { "early_portal_storm", 75, 90, 20, 20 },
        { "distant_portal_storm", 50, 75, 10, 15 },
        { "close_portal_storm", 25, 50, 5, 8 },
        { "portal_storm", 10, 20, 1, 2 },
        { "rainstorm_reverberation", 10, 20, 1, 2 },
        { "scarlet_day", 0, 20, 0, 2 }
    };
    for( const expected_weather &test : cases ) {
        INFO( test.id );
        const auto actual = fire_signal_visibility::classify_weather(
                                test.id, precip_class::none, false, 1.0f, 0 );
        CHECK( actual.smoke_percent == test.smoke );
        CHECK( actual.firelight_percent == test.light );
        CHECK( actual.smoke_cap_omt == test.smoke_cap );
        CHECK( actual.firelight_cap_omt == test.light_cap );
        if( std::string( test.id ) != "clear" ) {
            const weather_type_id id( test.id );
            REQUIRE( id.is_valid() );
            const weather_type &stock = id.obj();
            const auto live_properties = fire_signal_visibility::classify_weather(
                                             id.str(), stock.precip, stock.rains, stock.sight_penalty, 0 );
            CHECK( live_properties.smoke_percent == test.smoke );
            CHECK( live_properties.firelight_percent == test.light );
            CHECK( live_properties.smoke_cap_omt == test.smoke_cap );
            CHECK( live_properties.firelight_cap_omt == test.light_cap );
        }
    }
}

TEST_CASE( "fire_signal_range_keeps_fraction_and_combines_weather", "[fire_signal_visibility]" )
{
    using namespace fire_signal_visibility;
    const auto fog = classify_weather( "fog", precip_class::none, false, 1.7f, 20 );
    CHECK( effective_range_ms( 10, channel::smoke, time_band::daylight, fog ) == 9 );
    CHECK( effective_range_ms( 10, channel::smoke, time_band::night, fog ) == 0 );
    CHECK( effective_range_ms( 10, channel::firelight, time_band::daylight, fog ) == 0 );
    CHECK( effective_range_ms( 10, channel::firelight, time_band::night, fog ) == 24 );
    CHECK( effective_range_ms( 10, channel::firelight, time_band::twilight, fog ) == 12 );
    CHECK( effective_range_ms( 30, channel::smoke, time_band::daylight, fog ) == 24 );

    // A positive range below one square still permits the source square,
    // while an actually disabled channel stays disabled.
    CHECK( effective_range_ms( 1, channel::smoke, time_band::twilight, fog ) == 0 );
    CHECK( channel_enabled( channel::smoke, time_band::twilight, fog ) );
    CHECK_FALSE( channel_enabled( channel::smoke, time_band::night, fog ) );

    const auto unknown = classify_weather( "modded", precip_class::heavy, true, 1.7f, 0 );
    CHECK( unknown.smoke_percent == 5 );
    CHECK( unknown.firelight_cap_omt == 2 );
    CHECK( unknown.band.find( "fog" ) != std::string::npos );
}
