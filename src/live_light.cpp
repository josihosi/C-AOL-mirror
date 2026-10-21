#include "live_light.h"

#include "physical_light.h"
#include <map>

namespace live_light
{
namespace
{
std::vector<live_bandit_signal_observation> sample_cache;
time_point sample_turn = calendar::turn_zero;
bool sample_valid = false;
physical_light::turn_sample_gate sample_gate;
std::vector<live_light_delivery_stage> trace;
std::map<tripoint_abs_ms, unsigned> source_presence;
time_point source_presence_turn = calendar::turn_zero;
bool source_presence_valid = false;

void note( const live_light_delivery_stage stage )
{
    trace.push_back( stage );
}
}

void reset()
{
    sample_cache.clear();
    sample_turn = calendar::turn_zero;
    sample_valid = false;
    sample_gate = physical_light::turn_sample_gate{};
    trace.clear();
    source_presence.clear();
    source_presence_valid = false;
}

void record_loaded_sources( time_point turn, const physical_light::loaded_z_source_index &sources )
{
    source_presence.clear();
    source_presence_turn = turn;
    source_presence_valid = true;
    for( const auto &field : sources.field_sources ) {
        if( field.fire_intensity > 0 || field.smoke_intensity > 0 ) {
            source_presence[field.position] |= 1;
        }
        if( field.fire_intensity > 0 ) {
            source_presence[field.position] |= 2;
        }
    }
    for( const auto &emitter : sources.item_emitters ) {
        if( emitter.luminance > 0 ) {
            source_presence[emitter.position] |= 2;
        }
    }
    for( const auto &emitter : sources.stationary_emitters ) {
        if( emitter.luminance > 0 ) {
            source_presence[emitter.position] |= 2;
        }
    }
}

std::optional<bool> source_present_for_turn( time_point turn, const tripoint_abs_ms &source,
        const std::string &kind )
{
    if( !source_presence_valid || source_presence_turn != turn || !sample_is_current( turn ) ) {
        return std::nullopt;
    }
    const unsigned flag = kind == "smoke" ? 1 :
                          ( kind == "light" || kind == "searchlight" ? 2 : 0 );
    if( flag == 0 ) {
        return std::nullopt;
    }
    const auto found = source_presence.find( source );
    return found != source_presence.end() && ( found->second & flag ) != 0;
}

void run_advancing_turn( const time_point turn, const callbacks &callbacks )
{
    trace.clear();
    const int turn_number = to_turns<int>( turn - calendar::turn_zero );
    // The advancing-turn owner is the only caller allowed to reconcile,
    // age, sample or deliver for a turn.  Same-turn re-entry retains the
    // immutable packet for later staffed access but cannot replay work.
    if( !sample_gate.begin_advancing_turn( turn_number, true ) ) {
        return;
    }
    source_presence.clear();
    source_presence_valid = false;
    note( live_light_delivery_stage::rider_reconciliation );
    callbacks.reconcile_riders();
    note( live_light_delivery_stage::rider_memory_aging );
    callbacks.age_rider_memory();
    note( live_light_delivery_stage::fresh_sampling );
    sample_cache = callbacks.discover();
    sample_turn = turn;
    sample_valid = true;
    if( !sample_valid || sample_turn != turn ) {
        return;
    }
    std::vector<live_bandit_signal_observation> light_samples;
    std::vector<live_bandit_signal_observation> cannibal_samples;
    for( const live_bandit_signal_observation &sample : sample_cache ) {
        if( sample.has_light_projection && !sample.sample_id.empty() ) {
            light_samples.push_back( sample );
        }
        // Cannibal contact can be initiated by physical smoke as well as
        // light.  Keep the discovered packet intact so downstream range and
        // geometry admission sees its original source, mark and sample age.
        if( !sample.sample_id.empty() &&
            ( sample.mark.kind == "smoke" || sample.has_light_projection ) ) {
            cannibal_samples.push_back( sample );
        }
    }
    note( live_light_delivery_stage::horde_delivery );
    if( !light_samples.empty() ) {
        callbacks.deliver_hordes( light_samples );
    }
    note( live_light_delivery_stage::stalker_delivery );
    if( !light_samples.empty() ) {
        callbacks.deliver_stalkers( light_samples );
    }
    note( live_light_delivery_stage::rider_delivery );
    if( !light_samples.empty() ) {
        callbacks.deliver_riders( light_samples );
    }
    note( live_light_delivery_stage::cannibal_delivery );
    if( !cannibal_samples.empty() ) {
        callbacks.deliver_cannibals( cannibal_samples );
    }
    note( live_light_delivery_stage::migo_delivery );
    if( callbacks.deliver_migo ) {
        callbacks.deliver_migo( cannibal_samples );
    }
}

const std::vector<live_bandit_signal_observation> &samples_for_turn( const time_point turn )
{
    static const std::vector<live_bandit_signal_observation> empty;
    return sample_valid && sample_turn == turn ? sample_cache : empty;
}

void record_staffed_observer( const time_point turn,
                              const std::function<void( const std::vector<live_bandit_signal_observation> & )> &record )
{
    note( live_light_delivery_stage::staffed_observer_recording );
    record( samples_for_turn( turn ) );
}

bool sample_is_current( const time_point turn )
{
    return sample_valid && sample_turn == turn;
}

std::vector<live_light_delivery_stage> delivery_order()
{
    return { live_light_delivery_stage::rider_reconciliation,
             live_light_delivery_stage::rider_memory_aging,
             live_light_delivery_stage::fresh_sampling,
             live_light_delivery_stage::horde_delivery,
             live_light_delivery_stage::stalker_delivery,
             live_light_delivery_stage::rider_delivery,
             live_light_delivery_stage::cannibal_delivery,
             live_light_delivery_stage::migo_delivery,
             live_light_delivery_stage::staffed_observer_recording };
}

std::vector<live_light_delivery_stage> delivery_trace()
{
    return trace;
}
}
