#pragma once
#ifndef CATA_SRC_MIGO_POPULATION_NATIVE_H
#define CATA_SRC_MIGO_POPULATION_NATIVE_H

#include <optional>
#include <vector>

#include "coordinates.h"
#include "migo_population.h"

class map;
class monster;
class submap;

namespace migo_population
{
struct site_geometry {
    std::string id;
    tripoint_abs_omt anchor;
    bool tower = false;
    // Horizontal components; each uses only its actual Mi-go z levels.
    std::vector<tripoint_abs_omt> parts;
};

std::optional<site_geometry> geometry_at( const tripoint_abs_omt &position );
// Called only on newly generated ordinary content, after its native rotation.
void tag_generated_slots( map &here, submap &sm, const tripoint_abs_sm &position );
// Shared discovery/load-first transaction. Never spawns another resident set.
site_record *discover( const tripoint_abs_omt &position, const monster *incoming = nullptr,
                       representation incoming_owner = representation::abstract );
bool can_transfer( const monster &payload, representation from );
bool reconcile_payload( const monster &payload, representation observed_owner );
bool transfer( monster &payload, representation from, representation to );
bool superseded( const monster &payload );
void source_changed( const tripoint_abs_ms &position, const std::string &furniture );
// Existing loaded-site service only. Explicit elapsed stays available for the
// separate activation owner; this is not a new distance/index/catch-up system.
void service_loaded();
} // namespace migo_population
#endif
