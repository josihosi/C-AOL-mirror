#pragma once

#include <string>

enum class precip_class : int;

// The distant fire-signal calculation is independent of local rendering and
// ordinary light sources. Ranges are expressed in map squares so weather
// attenuation below one OMT is not rounded up to an OMT.
namespace fire_signal_visibility
{
enum class time_band : int { daylight, twilight, night };
enum class channel : int { smoke, firelight };

struct weather_profile {
    std::string band;
    int smoke_percent;
    int firelight_percent;
    int smoke_cap_omt;
    int firelight_cap_omt;
    bool windy;
};

weather_profile classify_weather( const std::string &id, precip_class precipitation,
                                  bool rains, float sight_penalty, int windspeed );

// baseline_omt is the unattenuated source-dependent range. The result is a
// conservative floor in map squares, never an upward-rounded OMT.
bool channel_enabled( channel signal, time_band time, const weather_profile &weather );
int effective_range_ms( int baseline_omt, channel signal, time_band time,
                        const weather_profile &weather );
} // namespace fire_signal_visibility
