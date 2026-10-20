#pragma once
#ifndef CATA_SRC_MIGO_POPULATION_H
#define CATA_SRC_MIGO_POPULATION_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "coordinates.h"
#include "migo_population_identity.h"
#include "type_id.h"

class JsonObject;
class JsonOut;

// Membership is explicit provenance, never inferred from a live monster's
// species or position. The native slot/monster/horde owns the only payload;
// this ledger owns allocation, leases and terminal facts.
namespace migo_population
{
enum class representation : int { slot, local, abstract, dead };
enum class duty : int { available, dispatched, returning };

struct member_record {
    identity token;
    mtype_id type;
    tripoint_abs_ms home;
    representation owner = representation::slot;
    duty assignment = duty::available;
    int hp = 0;
    std::uint64_t birth_sequence = 0;
    void serialize( JsonOut &json ) const;
    void deserialize( const JsonObject &json );
};

struct site_record {
    std::string id;
    tripoint_abs_omt anchor;
    bool tower = false;
    // Established at generation, before another horizontal component is
    // generated. False is the conservative old-save adoption policy.
    bool new_content = false;
    bool seed_committed = false;
    int ceiling = 8;
    int defensive_reserve = 2;
    // Selected once, including the legitimate no-source result. A missing
    // source can never be replaced by a different surviving decoration.
    std::optional<tripoint_abs_ms> source;
    std::string source_furniture;
    bool source_destroyed = false;
    bool occupancy_verified = false;
    int ambient_occupancy = 0;
    std::int64_t production_elapsed = 0;
    std::uint64_t birth_sequence = 0;
    int last_service_world = -1;
    std::vector<member_record> members;

    // The caller first persists the exact identified spawn manifest. Committing
    // it allocates no actors, and repeated discovery cannot replace its members.
    bool commit_seed( const std::vector<member_record> &slots,
                      std::optional<tripoint_abs_ms> source_position,
                      const std::string &furniture, int unproven_occupancy,
                      bool occupancy_known, bool ordinary_new_tower );
    member_record *find( const std::string &member_id );
    const member_record *find( const std::string &member_id ) const;
    int living_count() const;
    bool owns( const identity &token, representation owner ) const;
    bool transfer( const identity &token, representation from, representation to );
    bool record_death( const identity &token );
    bool observe_health( const identity &token, int hp );
    std::vector<identity> reserve( int count,
                                 const std::function<bool( const member_record & )> &healthy );
    bool returning( const identity &token );
    bool returned( const identity &token, const tripoint_abs_ms &actual_position );
    // A birth callback accepts one fresh native payload at home, or refuses
    // without mutation. False retains this single due event, not a backlog.
    bool advance( int world_turn, int active_elapsed, std::optional<bool> source_exists,
                  const std::function<bool( const member_record & )> &place );
    void serialize( JsonOut &json ) const;
    void deserialize( const JsonObject &json );
};

struct world_state {
    std::vector<site_record> sites;
    site_record *find( const std::string &site_id );
    const site_record *find( const std::string &site_id ) const;
    void clear() { sites.clear(); }
    void serialize( JsonOut &json ) const;
    void deserialize( const JsonObject &json );
};
} // namespace migo_population
#endif
