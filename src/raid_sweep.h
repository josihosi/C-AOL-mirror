#pragma once
#ifndef CATA_SRC_RAID_SWEEP_H
#define CATA_SRC_RAID_SWEEP_H

#include <cstdint>
#include <optional>
#include <vector>

#include "coordinates.h"

class JsonObject;
class JsonOut;

// Physical search progress shared by species-specific callers. This owns no
// actors, perception, hostility, dispatch permission or transport.
namespace raid_sweep
{
enum class disposition : int { pending, searched, hazardous, unreachable };

struct candidate {
    tripoint_abs_ms position;
    disposition state = disposition::pending;
    std::uint64_t geometry_revision = 0;
    bool reconsidered = false;
    void serialize( JsonOut &json ) const;
    void deserialize( const JsonObject &json );
};

struct progress {
    tripoint_abs_omt source;
    std::uint64_t generation = 0;
    std::vector<candidate> candidates;
    int cursor = 0;
    std::int64_t active_elapsed = 0;
    std::int64_t last_progress = 0;
    std::int64_t candidate_started = 0;
    int last_world = -1;
    // Callers supply legitimate active elapsed; a world timestamp is never
    // catch-up credit. An old/absent clock initializes without backlog.
    void advance( int world_turn, int active_turns );
    void made_progress();
    bool obstructed() const;
    std::optional<tripoint_abs_ms> pending() const;
    bool arrive( const tripoint_abs_ms &actual_position, bool eligible_observation );
    bool exclude( disposition reason, std::uint64_t geometry_revision );
    bool reconsider( const tripoint_abs_ms &position, std::uint64_t geometry_revision );
    void serialize( JsonOut &json ) const;
    void deserialize( const JsonObject &json );
};
} // namespace raid_sweep
#endif
