#include "raid_sweep.h"

#include <algorithm>

#include "json.h"

namespace raid_sweep
{
namespace
{
void next( progress &state )
{
    const int previous = state.cursor;
    while( state.cursor < static_cast<int>( state.candidates.size() ) &&
           state.candidates[state.cursor].state != disposition::pending ) {
        ++state.cursor;
    }
    if( previous != state.cursor ) {
        state.candidate_started = state.active_elapsed;
    }
}
} // namespace

void progress::advance( int world_turn, int active_turns )
{
    if( world_turn <= last_world ) {
        return;
    }
    if( last_world >= 0 ) {
        active_elapsed += std::min( std::max( 0, active_turns ), world_turn - last_world );
    }
    last_world = world_turn;
}

void progress::made_progress()
{
    last_progress = active_elapsed;
}

bool progress::obstructed() const
{
    return active_elapsed - std::max( last_progress, candidate_started ) >= 30 * 60;
}

std::optional<tripoint_abs_ms> progress::pending() const
{
    if( cursor < 0 || cursor >= static_cast<int>( candidates.size() ) ) {
        return std::nullopt;
    }
    return candidates[cursor].position;
}

bool progress::arrive( const tripoint_abs_ms &position, bool eligible_observation )
{
    if( !eligible_observation || pending() != position ) {
        return false;
    }
    candidates[cursor].state = disposition::searched;
    made_progress();
    next( *this );
    return true;
}

bool progress::exclude( disposition reason, std::uint64_t geometry_revision )
{
    if( !pending() || ( reason != disposition::hazardous && reason != disposition::unreachable ) ) {
        return false;
    }
    candidates[cursor].state = reason;
    candidates[cursor].geometry_revision = geometry_revision;
    // Exclusion is knowledge about a candidate, not physical search progress.
    next( *this );
    return true;
}

bool progress::reconsider( const tripoint_abs_ms &position, std::uint64_t revision )
{
    for( size_t i = 0; i < candidates.size(); ++i ) {
        auto &candidate = candidates[i];
        if( candidate.position == position && candidate.state == disposition::hazardous &&
            !candidate.reconsidered && revision != candidate.geometry_revision ) {
            candidate.state = disposition::pending;
            candidate.reconsidered = true;
            candidate.geometry_revision = revision;
            cursor = std::min( cursor, static_cast<int>( i ) );
            candidate_started = active_elapsed;
            return true;
        }
    }
    return false;
}

void candidate::serialize( JsonOut &json ) const
{
    json.start_object();
    json.member( "position", position );
    json.member( "state", static_cast<int>( state ) );
    json.member( "geometry_revision", geometry_revision );
    json.member( "reconsidered", reconsidered );
    json.end_object();
}

void candidate::deserialize( const JsonObject &json )
{
    *this = candidate();
    json.read( "position", position );
    state = static_cast<disposition>( std::clamp( json.get_int( "state", 0 ), 0, 3 ) );
    json.read( "geometry_revision", geometry_revision );
    json.read( "reconsidered", reconsidered );
}

void progress::serialize( JsonOut &json ) const
{
    json.start_object();
    json.member( "source", source );
    json.member( "generation", generation );
    json.member( "candidates", candidates );
    json.member( "cursor", cursor );
    json.member( "active_elapsed", active_elapsed );
    json.member( "last_progress", last_progress );
    json.member( "candidate_started", candidate_started );
    json.member( "last_world", last_world );
    json.end_object();
}

void progress::deserialize( const JsonObject &json )
{
    *this = progress();
    json.read( "source", source );
    json.read( "generation", generation );
    json.read( "candidates", candidates );
    json.read( "cursor", cursor );
    cursor = std::clamp( cursor, 0, static_cast<int>( candidates.size() ) );
    json.read( "active_elapsed", active_elapsed );
    json.read( "last_progress", last_progress );
    json.read( "candidate_started", candidate_started );
    json.read( "last_world", last_world );
    next( *this );
}
} // namespace raid_sweep
