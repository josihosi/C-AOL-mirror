#include "physical_navigation.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <vector>

#include "do_turn.h"
#include "field_type.h"
#include "line.h"
#include "map.h"
#include "mapbuffer.h"
#include "mapdata.h"
#include "monster.h"
#include "mtype.h"
#include "pathfinding.h"
#include "raid_sweep.h"
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
            // One preparation has no native actions or geometry/file writes.
            // Destroy the archive reader here, before any route or movement.
            mapbuffer::existing_submap_reader existing( MAPBUFFER );
            const int low = std::max( -OVERMAP_DEPTH, std::min( from.z(), to.z() ) - 1 );
            const int high = std::min( OVERMAP_HEIGHT, std::max( from.z(), to.z() ) + 1 );
            for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; ++z ) {
                for( int x = 0; x < getmapsize(); ++x ) {
                    for( int y = 0; y < getmapsize(); ++y ) {
                        const tripoint_abs_sm p( first.x() + x, first.y() + y, z );
                        submap *sm = z >= low && z <= high ? existing.lookup( p ) : nullptr;
                        if( sm && sm->vehicles.empty() && MAPBUFFER.vehicle_parts_at( p ).empty() ) {
                            available.insert( p );
                        } else {
                            sm = &unknown;
                        }
                        const tripoint_rel_sm grid( x, y, z );
                        setsubmap( get_nonant( grid ), sm );
                        if( sm != &unknown ) {
                            cache_submap_fields( grid );
                        }
                    }
                }
            }
        }
        bool known( const tripoint_abs_ms &p ) const {
            return inbounds( p ) && available.count( project_to<coords::sm>( p ) ) != 0;
        }
};

} // namespace

step_read read_step( const monster &body, const tripoint_abs_ms &goal,
                     const std::function<bool( const tripoint_abs_ms & )> &occupied )
{
    // Declare before the borrowed view so the total includes its destruction
    // and every refusal. The existing row owns all timing/count state.
    raid_perf_scope read_scope( raid_perf_stage::physical_navigation_read );
    const auto origin = body.pos_abs();
    step_read out;
    out.position = origin;
    const auto &native = body.get_pathfinding_settings();
    if( native.max_dist <= 0 ) {
        out.reason = "no_native_pathfinding";
        return out;
    }
    const auto delta = goal - origin;
    // Match the native route's distance bound. A diagonal can exceed it even
    // when each axis fits; slow terrain also disables its straight-line shortcut.
    const int length = rl_dist( origin.xy(), goal.xy() );
    const int segment = std::max( 1, std::min( 60, native.max_dist - 2 ) );
    tripoint_abs_ms aim( goal.xy(), origin.z() );
    if( length > segment ) {
        aim = origin + tripoint_rel_ms( delta.x() * segment / length,
                                        delta.y() * segment / length, 0 );
    }
    // Include the bounded horizontal approach when looking for a real floor
    // connection. A distant upper-floor home need not have stairs beside the
    // returning actor; a safe current-floor segment can bring it closer.
    const tripoint_abs_ms extent( aim.xy(), goal.z() );
    std::unique_ptr<navigation_view> remote;
    map *here = &get_map();
    if( !here->inbounds( origin ) || !here->inbounds( extent ) ) {
        raid_perf_scope build_scope( raid_perf_stage::physical_navigation_view_build );
        remote = std::make_unique<navigation_view>( origin, extent );
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
    if( goal == origin && raid_sweep::fire_approach( *here, from ) ) {
        out.known = true;
        out.reason = "approach_fire_hazard";
        out.hazard_revision = raid_sweep::fire_revision( *here, {from} );
        return out;
    }
    const bool can_open = body.has_flag( mon_flag_CAN_OPEN_DOORS );
    const auto door = [&]( const tripoint_bub_ms & p ) {
        return can_open && here->open_door( body, p, !here->is_outside( from ), true );
    };
    const auto avoid = [&]( const tripoint_bub_ms & p ) {
        return !known( p ) || raid_sweep::fire_approach( *here, p ) ||
               ( occupied && occupied( here->get_abs( p ) ) ) ||
               ( !body.will_move_to( here, p ) && !door( p ) ) ||
               ( !body.flies() && !here->has_floor_or_water( p ) ) || !body.know_danger_at( here, p );
    };
    const auto geometry_only = [&]( const tripoint_bub_ms &p ) {
        return !known( p ) || ( occupied && occupied( here->get_abs( p ) ) ) ||
               ( !body.will_move_to( here, p ) && !door( p ) ) ||
               ( !body.flies() && !here->has_floor_or_water( p ) );
    };
    pathfinding_settings settings = native;
    settings.allow_climb_stairs = false;
    settings.allow_open_doors = can_open;
    // The callback above enforces this creature's actual field safety.
    // The pathfinder's generic danger penalty ignores immunity and can
    // exhaust the native route budget on harmless home-world atmosphere.
    settings.avoid_dangerous_fields = false;
    settings.bash_strength.clear(); // An unseen wall is not an invented doorway.
    const auto native_route = [&]( const tripoint_bub_ms &to, const auto &exclude ) {
        raid_perf_scope route_scope( raid_perf_stage::physical_navigation_native_route );
        return here->route( from, pathfinding_target::point( to ), settings, exclude );
    };
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
            auto candidate = native_route( stair, avoid );
            if( !candidate.empty() && ( route.empty() || candidate.size() < route.size() ) ) {
                route = std::move( candidate );
            }
        }
    }
    if( route.empty() ) {
        if( aim == origin ) {
            out.known = true;
            out.reason = origin == goal ? "goal_reached" : "no_existing_safe_native_route";
            return out;
        }
        // A shortened endpoint is an approach preference, not a required
        // physical destination. Choose the farthest native-reachable point
        // on this bounded segment. In particular, a projection inside a
        // building wall must not prevent approaching its real entrance.
        // An unshortened goal still requires its actual square; neither an
        // unknown tile nor a nearby substitute can count as arrival.
        const int reach = std::min( length, segment );
        const int last = length > segment ? 1 : reach;
        for( int distance = reach; distance >= last; --distance ) {
            const auto approach = origin + tripoint_rel_ms( delta.x() * distance / length,
                                  delta.y() * distance / length, 0 );
            const auto to = here->get_bub( approach );
            if( !known( to ) ) {
                continue;
            }
            out.known = true;
            if( avoid( to ) ) {
                continue;
            }
            route = native_route( to, avoid );
            if( !route.empty() ) {
                break;
            }
        }
        if( !out.known ) {
            out.reason = "segment_geometry_unknown";
            return out;
        }
    }
    if( route.empty() ) {
        out.reason = "no_existing_safe_native_route";
        if( origin.z() == aim.z() && known( here->get_bub( aim ) ) ) {
            // Diagnose the same short approach; this second route is never
            // executed. A fire-excluded route is a hazard, not a searched
            // candidate or an invented impassable wall. Other geometry and
            // native door/floor constraints remain in force.
            const auto approach = native_route( here->get_bub( aim ), geometry_only );
            const bool fire_blocked = std::any_of( approach.begin(), approach.end(),
            [&]( const auto & cell ) {
                return raid_sweep::fire_approach( *here, cell );
            } );
            if( fire_blocked ) {
                out.reason = "approach_fire_hazard";
                out.hazard_revision = raid_sweep::fire_revision( *here, approach );
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
