#include "fire_signal_visibility.h"

#include <algorithm>
#include <limits>

#include "coordinates.h"
#include "weather_type.h"

namespace fire_signal_visibility
{
namespace
{
constexpr int unlimited = std::numeric_limits<int>::max();

weather_profile profile( const char *name, int smoke, int light, int smoke_cap, int light_cap )
{
    return { name, smoke, light, smoke_cap, light_cap, false };
}

void restrict_with( weather_profile &result, const weather_profile &restriction )
{
    result.smoke_percent = std::min( result.smoke_percent, restriction.smoke_percent );
    result.firelight_percent = std::min( result.firelight_percent, restriction.firelight_percent );
    result.smoke_cap_omt = std::min( result.smoke_cap_omt, restriction.smoke_cap_omt );
    result.firelight_cap_omt = std::min( result.firelight_cap_omt, restriction.firelight_cap_omt );
    if( restriction.band != "clear" ) {
        result.band += "+" + restriction.band;
    }
}
} // namespace

weather_profile classify_weather( const std::string &id, precip_class precipitation,
                                  bool rains, float sight_penalty, int windspeed )
{
    weather_profile result = profile( "clear", 100, 100, 30, unlimited );
    if( id == "cloudy" ) {
        result = profile( "cloudy", 90, 100, 27, unlimited );
    } else if( id == "light_drizzle" ) {
        result = profile( "light_drizzle", 80, 90, 20, 20 );
    } else if( id == "drizzle" || id == "flurries" ) {
        result = profile( id.c_str(), 65, 80, 15, 15 );
    } else if( id == "rain" ) {
        result = profile( "rain", 40, 60, 8, 10 );
    } else if( id == "rainstorm" || id == "thunder" || id == "lightning" ) {
        result = profile( id.c_str(), 20, 35, 3, 5 );
    } else if( id == "snowing" ) {
        result = profile( "snowing", 35, 50, 6, 8 );
    } else if( id == "snowstorm" ) {
        result = profile( "snowstorm", 10, 20, 1, 2 );
    } else if( id == "mist" ) {
        result = profile( "mist", 25, 40, 5, 8 );
    } else if( id == "fog" ) {
        result = profile( "fog", 5, 10, 1, 2 );
    } else if( id == "early_portal_storm" ) {
        result = profile( "early_portal_storm", 75, 90, 20, 20 );
    } else if( id == "distant_portal_storm" ) {
        result = profile( "distant_portal_storm", 50, 75, 10, 15 );
    } else if( id == "close_portal_storm" ) {
        result = profile( "close_portal_storm", 25, 50, 5, 8 );
    } else if( id == "portal_storm" || id == "rainstorm_reverberation" ) {
        result = profile( id.c_str(), 10, 20, 1, 2 );
    } else if( id == "scarlet_day" ) {
        result = profile( "scarlet_day", 0, 20, 0, 2 );
    } else if( id != "sunny" && id != "clear" && id != "null" ) {
        // Unknown weather can combine precipitation and reduced visibility.
        // Retain the stricter restriction from either property.
        result.band = "unknown";
        if( precipitation == precip_class::heavy ) {
            restrict_with( result, rains ? profile( "rain", 40, 60, 8, 10 ) :
                           profile( "snowing", 35, 50, 6, 8 ) );
        } else if( precipitation == precip_class::light ) {
            restrict_with( result, profile( "drizzle", 65, 80, 15, 15 ) );
        } else if( precipitation == precip_class::very_light || rains ) {
            restrict_with( result, profile( "light_drizzle", 80, 90, 20, 20 ) );
        }
        if( sight_penalty >= 1.7f ) {
            restrict_with( result, profile( "fog", 5, 10, 1, 2 ) );
        } else if( sight_penalty >= 1.3f ) {
            restrict_with( result, profile( "mist", 25, 40, 5, 8 ) );
        } else if( sight_penalty > 1.0f ) {
            restrict_with( result, profile( "light_drizzle", 80, 90, 20, 20 ) );
        }
    }
    result.windy = windspeed >= 20;
    return result;
}

bool channel_enabled( channel signal, time_band time, const weather_profile &weather )
{
    const bool smoke = signal == channel::smoke;
    return ( smoke ? time != time_band::night : time != time_band::daylight ) &&
           ( smoke ? weather.smoke_percent > 0 && weather.smoke_cap_omt > 0 :
             weather.firelight_percent > 0 && weather.firelight_cap_omt > 0 );
}

int effective_range_ms( int baseline_omt, channel signal, time_band time,
                        const weather_profile &weather )
{
    const bool smoke = signal == channel::smoke;
    const int time_percent = time == time_band::twilight ? 50 :
                             ( smoke == ( time == time_band::daylight ) ? 100 : 0 );
    const int weather_percent = smoke ? weather.smoke_percent : weather.firelight_percent;
    const int wind_percent = smoke && weather.windy ? 75 : 100;
    const int cap = smoke ? weather.smoke_cap_omt : weather.firelight_cap_omt;
    constexpr int squares_per_omt = SEEX * 2;
    const long long scaled = static_cast<long long>( std::max( 0, baseline_omt ) ) * squares_per_omt *
                             time_percent * weather_percent * wind_percent / 1000000;
    return static_cast<int>( std::min( scaled, static_cast<long long>( cap ) * squares_per_omt ) );
}
} // namespace fire_signal_visibility
