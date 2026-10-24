#pragma once
#ifndef CATA_SRC_PREDATOR_STATE_H
#define CATA_SRC_PREDATOR_STATE_H

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "coordinates.h"

class JsonObject;
class JsonOut;
namespace zombie_rider_overmap_ai
{
class rider_band_registry;
}

// This is deliberately small and lives beside the native monster state.  It
// identifies a predator across the copies used by the horde/local handoff.
// Native containers own bodies; the separate compact ledger below owns time.
struct predator_lifecycle_state {
    int schema_version = 2;
    std::string actor_id;
    std::uint64_t handoff_epoch = 0;
    int last_advanced_turn = -1;
    // These are inert continuity facts.  Evolution owns their meaning and
    // writes; rider bands own membership and union semantics.
    int ammo_initialization_version = 0;
    std::string band_reference;
    std::uint64_t band_revision_cache = 0;
    // The registry owns elapsed time. A native payload only remembers which
    // suspended interval has already been applied to its biological anchors.
    bool activity_bound = false;
    int activity_adopted_world = -1;
    std::int64_t activity_suspension = 0;
    std::int64_t activity_active_turns = 0;
    int last_native_process_world = -1;
    // Movement funding and local physiology are separate native events.
    // This consumption fact is never shifted by a suspended interval.
    int last_energy_world = -1;
    // Accepted abstract light work is distinct from world-time source expiry.
    // The carrier projects this one deadline; it never owns another countdown.
    std::int64_t light_work_until_active = -1;
    std::string light_work_sample;
};

namespace predator_activation
{
enum class representation : int { local, abstract, group, dead, captured };

struct activity_interval {
    int adopted_world = -1;
    int last_world = -1;
    bool active = false;
    std::int64_t suspended_turns = 0;
    int active_delta = 0; // transient, never renewed by repeated callbacks
    void evaluate( int now, bool eligible );
    std::int64_t suspension_at( int now ) const;
    std::int64_t active_at( int now ) const;
    void serialize( JsonOut &json ) const;
    void deserialize( const JsonObject &json );
};

struct actor_record {
    std::string actor_id;
    std::string type;
    std::uint64_t epoch = 0;
    representation owner = representation::dead;
    std::optional<tripoint_abs_ms> position;
    activity_interval activity;
    bool confirmed = false; // reconstructed from native owners after loading
    // CAPTURED alone is authenticated by this native item identity on reload.
    std::int64_t capture_item_uid = 0;
    void serialize( JsonOut &json ) const;
    void deserialize( const JsonObject &json );
};

// Native containers remain the only body owners. This ledger stores compact
// clocks and authenticated poses, with a disposable spatial lookup for range.
class world_state
{
    public:
        const actor_record *find( const std::string &id ) const;
        actor_record *find( const std::string &id );
        bool confirm( const std::string &id, const std::string &type,
                      std::uint64_t epoch, representation owner,
                      const tripoint_abs_ms &position, int now );
        bool owns( const std::string &id, std::uint64_t epoch,
                   representation owner, const tripoint_abs_ms &position ) const;
        bool move( const std::string &id, std::uint64_t epoch, representation owner,
                   const tripoint_abs_ms &from, const tripoint_abs_ms &to );
        bool transfer( const std::string &id, std::uint64_t old_epoch,
                       representation from, representation to,
                       const tripoint_abs_ms &position, int now );
        bool capture( const std::string &id, std::uint64_t epoch,
                      const tripoint_abs_ms &position, std::int64_t item_uid, int now );
        bool release_capture( const std::string &id, std::uint64_t captured_epoch,
                              std::int64_t item_uid, const tripoint_abs_ms &position, int now );
        void record_death( const std::string &id, std::uint64_t epoch, int now );
        std::string legacy_identity( const std::string &entry, const std::string &proposed );
        void refresh( const point_abs_omt &center, int now,
                      const zombie_rider_overmap_ai::rider_band_registry &bands );
        void invalidate_activation();
        const std::set<std::string> &active_actors() const;
        void clear();
        void serialize( JsonOut &json ) const;
        void deserialize( const JsonObject &json );
        bool bootstrapped = false;
        bool bootstrapping = false;
    private:
        std::map<std::string, actor_record> actors;
        std::map<std::string, std::string> legacy_entries;
        std::map<std::string, std::string> legacy_actor_entries;
        std::map<int, std::map<int, std::set<std::string>>> positions;
        std::set<std::string> active_ids;
        std::optional<point_abs_omt> evaluated_center;
        int evaluated_world = -1;
        void index_position( const actor_record &actor, bool add );
        void forget_legacy_entry( const std::string &id );
};
} // namespace predator_activation

#endif // CATA_SRC_PREDATOR_STATE_H
