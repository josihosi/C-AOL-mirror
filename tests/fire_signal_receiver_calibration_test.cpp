#include "bandit_mark_generation.h"
#include "cata_catch.h"
#include "fire_signal_visibility.h"
#include "live_light.h"
#include "physical_light.h"
#include "weather_type.h"

TEST_CASE( "weather capped fire remains eligible to nearby zombies", "[fire_signal_visibility]" )
{
    using namespace bandit_mark_generation;
    local_field_signal_reading fire;
    fire.fire_intensity = 3;
    fire.outside = true;
    fire.light_time = light_time_band::night;
    fire.fire_weather = fire_signal_visibility::classify_weather(
                            "fog", precip_class::none, false, 1.7f, 0 );
    const auto projection = adapt_local_field_signal_reading( fire );
    REQUIRE( projection.has_light_packet );
    REQUIRE( projection.light.viable );
    CHECK( projection.light.projected_range_omt < 8 );
    CHECK( horde_signal_power_from_light_projection( projection.light ) > 0 );

    light_projection legacy;
    legacy.viable = true;
    legacy.packet.time = light_time_band::night;
    legacy.packet.exposure = light_exposure_band::exposed;
    legacy.projected_range_omt = 7;
    CHECK( horde_signal_power_from_light_projection( legacy ) == 0 );
}

TEST_CASE( "fractional signal candidate bounds do not assume OMT centres",
           "[fire_signal_visibility]" )
{
    live_bandit_signal_observation signal;
    signal.source_ms = tripoint_abs_ms( 23, 0, 0 );
    signal.range_cap_ms = 2;
    CHECK( live_light::within_range( signal, tripoint_abs_ms( 24, 0, 0 ) ) );
    CHECK_FALSE( live_light::within_range( signal, tripoint_abs_ms( 26, 0, 0 ) ) );
    CHECK( live_light::within_range_of_omt( signal, tripoint_abs_omt( 1, 0, 0 ) ) );
    CHECK_FALSE( live_light::within_range_of_omt( signal, tripoint_abs_omt( 2, 0, 0 ) ) );
}

TEST_CASE( "fractional fire range pays exact terrain budget", "[fire_signal_visibility]" )
{
    physical_light::route route;
    route.source_exposed = true;
    route.vertical_sightline = true;
    route.distance_omt = 1;
    route.brightness_range_omt = 2; // Candidate envelope rounds up.
    route.distance_ms = 25;
    route.brightness_range_ms = 35;
    route.terrain_see_costs = { 1 };
    CHECK_FALSE( physical_light::detect( route ).visible );
    route.brightness_range_ms = 49;
    CHECK( physical_light::detect( route ).visible );
    route.terrain_see_costs = { -1 };
    CHECK_FALSE( physical_light::detect( route ).visible );
    CHECK_FALSE( physical_light::within_exact_budget( 23, 0, { 1 } ) );
    CHECK( physical_light::within_exact_budget( 24, 0, { 1 } ) );
}
