#include "physical_navigation.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <vector>

#include "field_type.h"
#include "line.h"
#include "map.h"
#include "mapbuffer.h"
#include "mapdata.h"
#include "monster.h"
#include "mtype.h"
#include "pathfinding.h"
#include "submap.h"

namespace physical_navigation
{
namespace
{
// Borrow existing submaps only. Unknown cells and remote vehicle geometry are
// walls, including physical parts overhanging from another resident submap.
class navigation_view : public map
{
        submap unknown;
        std::set<tripoint_abs_sm> available;
    public:
        navigation_view( const tripoint_abs_ms &from, const tripoint_abs_ms &to ) :
            map( std::min( MAPSIZE, std::max( std::abs( from.x() - to.x() ),
                                              std::abs( from.y() - to.y() ) ) / SEEX + 3 ), true ) {
            unknown.set_all_ter( ter_str_id( "t_wall" ) );
            const auto first = project_to<coords::sm>( tripoint_abs_ms(
                    std::min( from.x(), to.x() ) - SEEX,
                    std::min( from.y(), to.y() ) - SEEY, from.z() ) );
            set_abs_sub( first );
            const int low = std::max( -OVERMAP_DEPTH, std::min( from.z(), to.z() ) - 1 );
            const int high = std::min( OVERMAP_HEIGHT, std::max( from.z(), to.z() ) + 1 );
            for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; ++z ) {
                for( int x = 0; x < getmapsize(); ++x ) {
                    for( int y = 0; y < getmapsize(); ++y ) {
                        const tripoint_abs_sm p( first.x() + x, first.y() + y, z );
                        submap *sm = z >= low && z <= high ? MAPBUFFER.lookup_submap_existing( p ) : nullptr;
                        if( sm && sm->vehicles.empty() && MAPBUFFER.vehicle_parts_at( p ).empty() ) {
                            available.insert( p );
                        } else {
                            sm = &unknown;
                        }
                        setsubmap( get_nonant( tripoint_rel_sm( x, y, z ) ), sm );
                    }
                }
            }
        }
        bool known( const tripoint_abs_ms &p ) const {
            return inbounds( p ) && available.count( project_to<coords::sm>( p ) ) != 0;
        }
};

bool fire_approach( const map &here, const tripoint_bub_ms &p )
{
    static const field_type_id fire( "fd_fire" );
    for( const auto &cell : here.points_in_radius( p, 1 ) ) {
        if( here.get_field_intensity( cell, fire ) > 0 &&
            ( cell == p || !here.has_flag( ter_furn_flag::TFLAG_FIRE_CONTAINER, cell ) ) ) {
            return true;
        }
    }
    return false;
}
} // namespace

step_read read_step( const monster &body, const tripoint_abs_ms &goal )
{
    const auto origin = body.pos_abs();
    step_read out;
    out.position = origin;
    const auto &native = body.get_pathfinding_settings();
    if( native.max_dist <= 0 ) {
        out.reason = "no_native_pathfinding";
        return out;
    }
    tripoint_abs_ms aim = goal;
    if( origin.z() != goal.z() ) {
        // Resolve one real floor connection first. Do not call the generic
        // cross-z route's main-map stair helper with remote-view coordinates.
        aim = tripoint_abs_ms( origin.xy(), goal.z() );
    } else {
        const auto delta = goal - origin;
        const int length = std::max( std::abs( delta.x() ), std::abs( delta.y() ) );
        const int segment = std::max( 1, std::min( 60, native.max_dist - 2 ) );
        if( length > segment ) {
            aim = origin + tripoint_rel_ms( delta.x() * segment / length,
                                            delta.y() * segment / length, 0 );
        }
    }
    std::unique_ptr<navigation_view> remote;
    map *here = &get_map();
    if( !here->inbounds( origin ) || !here->inbounds( aim ) ) {
        remote = std::make_unique<navigation_view>( origin, aim );
        here = remote.get();
    }
    const auto known = [&]( const tripoint_bub_ms & p ) {
        return here->inbounds( p ) && ( !remote || remote->known( here->get_abs( p ) ) );
    };
    const auto from = here->get_bub( origin );
    if( !known( from ) ) {
        out.reason = "origin_geometry_unknown";
        return out;
    }
    const bool can_open = body.has_flag( mon_flag_CAN_OPEN_DOORS );
    const auto door = [&]( const tripoint_bub_ms & p ) {
        return can_open && here->open_door( body, p, !here->is_outside( from ), true );
    };
    const auto avoid = [&]( const tripoint_bub_ms & p ) {
        return !known( p ) || fire_approach( *here, p ) ||
               ( !body.will_move_to( here, p ) && !door( p ) ) ||
               ( !body.flies() && !here->has_floor_or_water( p ) ) || !body.know_danger_at( here, p );
    };
    const auto geometry_only = [&]( const tripoint_bub_ms &p ) {
        return !known( p ) || ( !body.will_move_to( here, p ) && !door( p ) ) ||
               ( !body.flies() && !here->has_floor_or_water( p ) );
    };
    pathfinding_settings settings = native;
    settings.allow_climb_stairs = false;
    settings.allow_open_doors = can_open;
    settings.bash_strength.clear(); // An unseen wall is not an invented doorway.
    std::vector<tripoint_bub_ms> route;
    if( origin.z() != goal.z() ) {
        const int dz = goal.z() > origin.z() ? 1 : -1;
        for( const auto &stair : here->points_in_rectangle(
                 tripoint_bub_ms( 0, 0, origin.z() ),
                 tripoint_bub_ms( here->getmapsize() * SEEX - 1,
                                  here->getmapsize() * SEEY - 1, origin.z() ) ) ) {
            const auto landing = stair + tripoint_rel_ms( 0, 0, dz );
            if( !known( stair ) || !known( landing ) ||
                !here->on_matching_stairs( stair, landing ) || avoid( stair ) || avoid( landing ) ||
                !here->valid_move( stair, landing ) ) {
                continue;
            }
            if( stair == from ) {
                route = {landing};
                break;
            }
            auto candidate = here->route( from, pathfinding_target::point( stair ), settings, avoid );
            if( !candidate.empty() && ( route.empty() || candidate.size() < route.size() ) ) {
                route = std::move( candidate );
            }
        }
    } else {
        const auto to = here->get_bub( aim );
        if( !known( to ) ) {
            out.reason = "segment_geometry_unknown";
            return out;
        }
        out.known = true;
        if( aim == origin ) {
            out.reason = "goal_reached";
            return out;
        }
        if( !avoid( to ) ) {
            route = here->route( from, pathfinding_target::point( to ), settings, avoid );
        }
    }
    if( route.empty() ) {
        out.reason = "no_existing_safe_native_route";
        if( origin.z() == aim.z() && known( here->get_bub( aim ) ) ) {
            // Diagnose the same short approach; this second route is never
            // executed. A fire-excluded route is a hazard, not a searched
            // candidate or an invented impassable wall. Other geometry and
            // native door/floor constraints remain in force.
            const auto approach = here->route( from, pathfinding_target::point( here->get_bub( aim ) ),
                                              settings, geometry_only );
            std::uint64_t revision = 14695981039346656037ULL;
            bool fire_blocked = false;
            for( const auto &cell : approach ) {
                if( !fire_approach( *here, cell ) ) {
                    continue;
                }
                fire_blocked = true;
                for( const auto &nearby : here->points_in_radius( cell, 1 ) ) {
                    const auto intensity = here->get_field_intensity( nearby, field_type_id( "fd_fire" ) );
                    if( intensity > 0 ) {
                        const auto facts = here->get_abs( nearby ).to_string_writable() + ":" +
                                           std::to_string( intensity );
                        for( unsigned char value : facts ) {
                            revision = ( revision ^ value ) * 1099511628211ULL;
                        }
                    }
                }
            }
            if( fire_blocked ) {
                out.reason = "approach_fire_hazard";
                out.hazard_revision = revision;
                out.known = true;
            }
        }
        return out;
    }
    const auto next = route.front();
    if( !known( next ) || avoid( next ) ) {
        out.reason = "next_step_refused";
        return out;
    }
    out.position = here->get_abs( next );
    out.known = true;
    if( !body.will_move_to( here, next ) && door( next ) ) {
        out.next = action::open_door;
        out.move_cost = 100;
    } else if( here->valid_move( from, next, false, body.flies() ) ) {
        out.next = action::walk;
        out.move_cost = body.calc_movecost( *here, from, next );
    }
    if( out.move_cost <= 0 ) {
        out.next = action::none;
        out.reason = "native_step_not_executable";
    }
    return out;
}

bool commit_door( const monster &body, const tripoint_abs_ms &door )
{
    const auto origin = body.pos_abs();
    if( origin.z() != door.z() || rl_dist( origin, door ) != 1 ||
        !body.has_flag( mon_flag_CAN_OPEN_DOORS ) ) {
        return false;
    }
    map &live = get_map();
    if( live.inbounds( origin ) && live.inbounds( door ) ) {
        return live.open_door( body, live.get_bub( door ), !live.is_outside( body.pos_bub( live ) ) );
    }
    navigation_view view( origin, door );
    const auto from = view.get_bub( origin );
    const auto to = view.get_bub( door );
    if( !view.known( origin ) || !view.known( door ) ||
        !view.open_door( body, to, !view.is_outside( from ), true ) ) {
        return false;
    }
    // Native terrain/furniture open transitions, without emitting a sound in
    // the unrelated main bubble's relative coordinates. Remote vehicles are
    // unknown above and cannot enter this branch.
    if( const auto &open = view.ter( to )->open ) {
        view.ter_set( to, open );
        return true;
    }
    if( const auto &open = view.furn( to )->open ) {
        view.furn_set( to, open );
        return true;
    }
    return false;
}
} // namespace physical_navigation
