#pragma once
#ifndef CATA_SRC_MIGO_TOWER_NATIVE_H
#define CATA_SRC_MIGO_TOWER_NATIVE_H

#include <vector>
#include <string>

#include "migo_population.h"
#include "physical_navigation.h"

struct live_bandit_signal_observation;
class monster;
class Creature;

namespace migo_tower
{
struct receiver_result {
    int actual_sites = 0;
    int committed_populations = 0;
    int eligible_observers = 0;
    int received = 0;
};

// Production recipient of the shared, current physical emitter packet. It
// discovers existing site content before selecting a real home observer. This
// is independent of every human scout/report/dispatch prerequisite.
receiver_result receive_signals( const std::vector<live_bandit_signal_observation> &samples,
                                 int now_turn );
// Revalidate the received evidence and exact current bodies before committing
// all selected population leases. A decision estimate alone cannot dispatch.
bool begin_response( const std::string &site_id, int now_turn );
bool owns_intent( const monster &body, migo_population::representation owner );
physical_navigation::step_read prepare_step( monster &body,
        migo_population::representation owner );
bool plan_local( monster &body, bool fleeing, Creature *target );
void record_combat_action( monster &body );
// Called after native movement effects and immediate special/escape handling.
bool move_local( monster &body );
void service_operations( int now_turn, int active_elapsed = 1 );
void witness_death( const monster &body );
} // namespace migo_tower
#endif
