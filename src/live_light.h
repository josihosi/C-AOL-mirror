#pragma once
#ifndef CATA_SRC_LIVE_LIGHT_H
#define CATA_SRC_LIVE_LIGHT_H

#include "bandit_live_world.h"
#include "bandit_mark_generation.h"
#include "calendar.h"
#include "coordinates.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

struct live_bandit_signal_observation {
    bandit_live_world::live_signal_mark mark;
    bandit_mark_generation::signal_input signal;
    tripoint_abs_omt source_omt;
    std::optional<tripoint_abs_ms> source_ms;
    int range_cap_omt = 0;
    int horde_signal_power = 0;
    std::string weather_summary;
    // A smoke source may be seen across levels only after local geometry
    // established that its plume escaped into open air.
    bool smoke_source_exposed_to_sky = false;
    // Derived once from current source geometry, never a second emitter or
    // independent observation. Source identity/site/time remain above.
    std::optional<tripoint_abs_ms> smoke_observable_ms;
    std::string smoke_support_reason;
    bool has_light_projection = false;
    bandit_mark_generation::light_projection light_projection;
    std::string sample_id;
    int observed_turn = -1;
    int observed_minutes = -1;
};

enum class live_light_delivery_stage {
    rider_reconciliation, rider_memory_aging, fresh_sampling, horde_delivery,
    stalker_delivery, rider_delivery, cannibal_delivery, migo_delivery, staffed_observer_recording
};

namespace physical_light
{
struct loaded_z_source_index;
}
namespace live_light
{
struct callbacks {
    std::function<void()> reconcile_riders;
    std::function<void()> age_rider_memory;
    std::function<std::vector<live_bandit_signal_observation>()> discover;
    std::function<void( const std::vector<live_bandit_signal_observation> & )> deliver_hordes;
    std::function<void( const std::vector<live_bandit_signal_observation> & )> deliver_stalkers;
    std::function<void( const std::vector<live_bandit_signal_observation> & )> deliver_riders;
    std::function<void( const std::vector<live_bandit_signal_observation> & )> deliver_cannibals;
    std::function<void( const std::vector<live_bandit_signal_observation> & )> deliver_migo;
};

void reset();
// Rider state, discovery and immediate recipients advance once per real turn.
// The staffed observer remains owned by its existing overmap cadence and reads
// the immutable packet through record_staffed_observer().
void run_advancing_turn( time_point turn, const callbacks &callbacks );
const std::vector<live_bandit_signal_observation> &samples_for_turn( time_point turn );
void record_staffed_observer( time_point turn,
                              const std::function<void( const std::vector<live_bandit_signal_observation> & )> &record );
bool sample_is_current( time_point turn );
// The advancing-turn discovery producer records raw loaded emitter presence
// before packet strength/weather thresholds. Consumers may check only a
// previously received source and must independently establish loaded geometry
// and actual observer visibility; absence here is not itself perception.
void record_loaded_sources( time_point turn, const physical_light::loaded_z_source_index &sources );
std::optional<bool> source_present_for_turn( time_point turn, const tripoint_abs_ms &source,
        const std::string &kind );
std::vector<live_light_delivery_stage> delivery_order();
std::vector<live_light_delivery_stage> delivery_trace();
}

#endif // CATA_SRC_LIVE_LIGHT_H
