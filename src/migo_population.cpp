#include "migo_population.h"

#include <algorithm>
#include <limits>
#include <set>

#include "calendar.h"
#include "json.h"
#include "mtype.h"
#include "rng.h"

namespace migo_population
{
void identity::serialize( JsonOut &json ) const
{
    json.start_object();
    json.member( "site", site_id );
    json.member( "member", member_id );
    json.member( "epoch", epoch );
    json.member( "anchor", site_anchor );
    json.end_object();
}

void identity::deserialize( const JsonObject &json )
{
    json.read( "site", site_id );
    json.read( "member", member_id );
    json.read( "epoch", epoch );
    json.read( "anchor", site_anchor );
}

void member_record::serialize( JsonOut &json ) const
{
    json.start_object();
    json.member( "identity", token );
    json.member( "type", type );
    json.member( "home", home );
    json.member( "owner", static_cast<int>( owner ) );
    json.member( "duty", static_cast<int>( assignment ) );
    json.member( "hp", hp );
    json.member( "birth_sequence", birth_sequence );
    json.end_object();
}

void member_record::deserialize( const JsonObject &json )
{
    json.read( "identity", token );
    json.read( "type", type );
    json.read( "home", home );
    owner = static_cast<representation>( json.get_int( "owner", 0 ) );
    assignment = static_cast<duty>( json.get_int( "duty", 0 ) );
    json.read( "hp", hp );
    json.read( "birth_sequence", birth_sequence );
}

bool site_record::commit_seed( const std::vector<member_record> &slots,
                              std::optional<tripoint_abs_ms> source_position,
                              const std::string &furniture, int unproven_occupancy,
                              bool occupancy_known, bool ordinary_new_tower )
{
    if( seed_committed || id.empty() || unproven_occupancy < 0 ||
        ( source_position && furniture.empty() ) ) {
        return false;
    }
    std::set<std::string> ids;
    int ordinary = 0;
    int scouts = 0;
    for( const member_record &m : slots ) {
        if( m.token.empty() || m.token.site_id != id ||
            !ids.insert( m.token.member_id ).second ) {
            return false;
        }
        ordinary += m.type == mtype_id( "mon_mi_go" );
        scouts += m.type == mtype_id( "mon_mi_go_scout" );
    }
    if( ordinary_new_tower && ( slots.size() != 8 || ordinary != 6 || scouts != 2 ) ) {
        return false;
    }
    members = slots;
    source = source_position;
    source_furniture = furniture;
    occupancy_verified = occupancy_known;
    ambient_occupancy = unproven_occupancy;
    ceiling = tower ? 8 : std::max<int>( 8, members.size() );
    seed_committed = true;
    return true;
}

member_record *site_record::find( const std::string &member_id )
{
    const auto it = std::find_if( members.begin(), members.end(), [&]( const member_record & m ) {
        return m.token.member_id == member_id;
    } );
    return it == members.end() ? nullptr : &*it;
}

const member_record *site_record::find( const std::string &member_id ) const
{
    return const_cast<site_record *>( this )->find( member_id );
}

int site_record::living_count() const
{
    return std::count_if( members.begin(), members.end(), []( const member_record & m ) {
        return m.owner != representation::dead;
    } );
}

bool site_record::owns( const identity &token, representation owner ) const
{
    const member_record *m = find( token.member_id );
    return seed_committed && token.site_id == id && m && m->token.epoch == token.epoch &&
           m->owner == owner;
}

bool site_record::transfer( const identity &token, representation from, representation to )
{
    if( from == representation::dead || to == representation::dead || from == to ||
        !owns( token, from ) || token.epoch == std::numeric_limits<std::uint64_t>::max() ) {
        return false;
    }
    member_record &m = *find( token.member_id );
    ++m.token.epoch;
    m.owner = to;
    return true;
}

bool site_record::record_death( const identity &token )
{
    member_record *m = find( token.member_id );
    if( token.site_id != id || !m || m->owner == representation::dead ||
        token.epoch != m->token.epoch ) {
        return false;
    }
    m->owner = representation::dead;
    m->hp = 0;
    return true;
}

bool site_record::observe_health( const identity &token, int hp )
{
    member_record *m = find( token.member_id );
    if( token.site_id != id || !m || m->owner == representation::dead ||
        token.epoch != m->token.epoch ) {
        return false;
    }
    m->hp = hp;
    return true;
}

std::vector<identity> site_record::reserve( int count,
        const std::function<bool( const member_record & )> &healthy )
{
    std::vector<identity> result;
    if( !seed_committed || count <= 0 ) {
        return result;
    }
    std::vector<member_record *> available;
    for( member_record &m : members ) {
        if( m.owner != representation::dead && m.owner != representation::slot &&
            m.assignment == duty::available && m.hp > 0 &&
            healthy( m ) ) {
            available.push_back( &m );
        }
    }
    if( static_cast<int>( available.size() ) - count < defensive_reserve ) {
        return result;
    }
    for( int i = 0; i < count; ++i ) {
        available[i]->assignment = duty::dispatched;
        result.push_back( available[i]->token );
    }
    return result;
}

bool site_record::returning( const identity &token )
{
    member_record *m = find( token.member_id );
    if( token.site_id != id || !m || m->owner == representation::dead ||
        m->token.epoch != token.epoch || m->assignment != duty::dispatched ) {
        return false;
    }
    m->assignment = duty::returning;
    return true;
}

bool site_record::returned( const identity &token, const tripoint_abs_ms &actual_position )
{
    member_record *m = find( token.member_id );
    if( token.site_id != id || !m || m->owner == representation::dead ||
        m->token.epoch != token.epoch || m->assignment != duty::returning ||
        project_to<coords::omt>( actual_position ) != project_to<coords::omt>( m->home ) ) {
        return false;
    }
    m->assignment = duty::available;
    return true;
}

bool site_record::advance( int world_turn, int active_elapsed, std::optional<bool> source_exists,
                           const std::function<bool( const member_record & )> &place )
{
    // Repeated/reloaded service cannot spend the same active interval twice.
    // World time identifies the observation; the caller separately supplies
    // admitted active time. This does not implement the activation system.
    if( world_turn <= last_service_world ) {
        return false;
    }
    const int admitted = last_service_world < 0 ? 0 :
                         std::min( std::max( 0, active_elapsed ), world_turn - last_service_world );
    last_service_world = world_turn;
    if( source_exists && !*source_exists ) {
        source_destroyed = true;
        production_elapsed = 0;
    }
    if( !seed_committed || !source || source_destroyed || !source_exists.value_or( false ) ||
        !occupancy_verified ) {
        return false;
    }
    if( living_count() + ambient_occupancy >= ceiling ) {
        production_elapsed = 0;
        return false;
    }
    // A retained due event is earned progress, not permission to create
    // while frozen or on the first zero-elapsed service after a load gap.
    if( admitted == 0 ) {
        return false;
    }
    constexpr std::int64_t interval = to_turns<std::int64_t>( 7_days );
    production_elapsed += std::min<std::int64_t>( admitted,
                          interval - production_elapsed );
    if( production_elapsed < interval || birth_sequence == std::numeric_limits<std::uint64_t>::max() ) {
        return false;
    }
    member_record birth;
    birth.token = { id, random_string( 32 ), 0, anchor };
    birth.birth_sequence = birth_sequence + 1;
    const int scouts = std::count_if( members.begin(), members.end(), []( const member_record & m ) {
        return m.owner != representation::dead && m.type == mtype_id( "mon_mi_go_scout" );
    } );
    const int mix = std::count_if( members.begin(), members.end(), []( const member_record & m ) {
        return m.owner != representation::dead &&
               ( m.type == mtype_id( "mon_mi_go" ) || m.type == mtype_id( "mon_mi_go_scout" ) );
    } );
    birth.type = mtype_id( scouts * 4 < mix ? "mon_mi_go_scout" : "mon_mi_go" );
    birth.hp = birth.type->hp;
    birth.home = *source;
    birth.owner = representation::abstract;
    if( !place( birth ) ) {
        return false;
    }
    members.push_back( birth );
    birth_sequence = birth.birth_sequence;
    production_elapsed = 0;
    return true;
}

void site_record::serialize( JsonOut &json ) const
{
    json.start_object();
    json.member( "id", id );
    json.member( "anchor", anchor );
    json.member( "tower", tower );
    json.member( "new_content", new_content );
    json.member( "seed_committed", seed_committed );
    json.member( "ceiling", ceiling );
    json.member( "defensive_reserve", defensive_reserve );
    json.member( "source", source );
    json.member( "source_furniture", source_furniture );
    json.member( "source_destroyed", source_destroyed );
    json.member( "occupancy_verified", occupancy_verified );
    json.member( "ambient_occupancy", ambient_occupancy );
    json.member( "production_elapsed", production_elapsed );
    json.member( "birth_sequence", birth_sequence );
    json.member( "last_service_world", last_service_world );
    json.member( "members", members );
    json.end_object();
}

void site_record::deserialize( const JsonObject &json )
{
    json.read( "id", id );
    json.read( "anchor", anchor );
    json.read( "tower", tower );
    json.read( "new_content", new_content );
    json.read( "seed_committed", seed_committed );
    json.read( "ceiling", ceiling );
    json.read( "defensive_reserve", defensive_reserve );
    json.read( "source", source );
    json.read( "source_furniture", source_furniture );
    json.read( "source_destroyed", source_destroyed );
    json.read( "occupancy_verified", occupancy_verified );
    json.read( "ambient_occupancy", ambient_occupancy );
    json.read( "production_elapsed", production_elapsed );
    json.read( "birth_sequence", birth_sequence );
    json.read( "last_service_world", last_service_world );
    json.read( "members", members );
    production_elapsed = std::clamp<std::int64_t>( production_elapsed, 0,
                         to_turns<std::int64_t>( 7_days ) );
}

site_record *world_state::find( const std::string &site_id )
{
    const auto it = std::find_if( sites.begin(), sites.end(), [&]( const site_record & s ) {
        return s.id == site_id;
    } );
    return it == sites.end() ? nullptr : &*it;
}

const site_record *world_state::find( const std::string &site_id ) const
{
    return const_cast<world_state *>( this )->find( site_id );
}

void world_state::serialize( JsonOut &json ) const
{
    json.start_object();
    json.member( "sites", sites );
    json.end_object();
}

void world_state::deserialize( const JsonObject &json )
{
    sites.clear();
    json.read( "sites", sites );
}
} // namespace migo_population
