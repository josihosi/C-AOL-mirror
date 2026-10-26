#pragma once
#ifndef CATA_SRC_PHYSICAL_NAVIGATION_H
#define CATA_SRC_PHYSICAL_NAVIGATION_H

#include <string>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include "coordinates.h"
#include "pathfinding.h"
#include "type_id.h"

class monster;

// Native short paths over existing physical geometry. No map generation,
// actor discovery, movement or terrain mutation occurs during a route read.
namespace physical_navigation
{
enum class action : int { none, walk, open_door };
struct step_read {
    action next = action::none;
    tripoint_abs_ms position;
    int move_cost = 0;
    bool known = false;
    std::string reason;
    std::uint64_t hazard_revision = 0;
    // A request for facts at the actor's immediate frontier, never a route
    // certificate. Only an authenticated movement owner may acquire it.
    std::optional<tripoint_abs_ms> missing_geometry;
};
class preparation;
// Disposable positive path value. No map/actor pointers, failures or history.
// The caller owns identity/lifetime; reads consume only actual position changes.
class segment
{
        tripoint_abs_ms origin;
        tripoint_abs_ms goal;
        mtype_id type;
        pathfinding_settings settings;
        std::vector<tripoint_abs_ms> points;
        std::vector<int> costs;
        friend step_read read_step( const monster &, const tripoint_abs_ms &,
                                    const std::function<bool( const tripoint_abs_ms & )> &, preparation *, segment *, bool );
    public:
        void clear() { *this = segment(); }
        std::size_t size() const { return points.size(); }
};
// One operation's synchronous, read-only service span. No native action,
// terrain/field edit, world/file mutation, save, unload or transport may occur.
// Owns one compatible view, never a route/result or borrowed actor pointer.
class preparation
{
        struct implementation;
        std::unique_ptr<implementation> impl;
        friend step_read read_step( const monster &, const tripoint_abs_ms &,
                                    const std::function<bool( const tripoint_abs_ms & )> &, preparation *, segment *, bool );
    public:
        preparation();
        ~preparation();
        preparation( const preparation & ) = delete;
        preparation &operator=( const preparation & ) = delete;
};
// A transport owner may supply its actual occupied cells. They affect this
// short physical route only, never perception or the destination's identity.
step_read read_step( const monster &body, const tripoint_abs_ms &goal,
                     const std::function<bool( const tripoint_abs_ms & )> &occupied = {},
                     preparation *prepared = nullptr, segment *retained = nullptr,
                     bool approach_unknown_goal = false );
// Actual movement owners only, after authenticating the canonical source and
// active opportunity. Loads at most one current/adjacent OMT through the normal
// native loader. All read/preparation views must have ended before this call.
// Loading has native world effects, but confers no actor/action/time credit.
bool acquire_movement_geometry( const tripoint_abs_ms &origin, const step_read &request );
// Caller must first authenticate the current physical owner and spend this
// action once. This opens one adjacent native door, and never moves the body.
bool commit_door( const monster &body, const tripoint_abs_ms &door );
}
#endif
