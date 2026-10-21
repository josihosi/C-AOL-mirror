#pragma once
#ifndef CATA_SRC_MIGO_TOWER_CAPABILITY_H
#define CATA_SRC_MIGO_TOWER_CAPABILITY_H

#include <optional>

#include "migo_tower_ai.h"

class Creature;
class monster;
namespace scout_observation
{
class site_reader;
}

namespace migo_tower
{
combat_profile own_capability( const monster &member );
// No profile (or position in a saved threat record) before real recognition.
// A present unknown profile means a visible capability cannot be estimated.
std::optional<combat_profile> observe_enemy( const monster &observer, const Creature &enemy,
        scout_observation::site_reader &reader );
}
#endif
