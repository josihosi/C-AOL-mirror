#pragma once
#ifndef CATA_SRC_PHYSICAL_NAVIGATION_H
#define CATA_SRC_PHYSICAL_NAVIGATION_H

#include <string>
#include <cstdint>
#include <functional>
#include "coordinates.h"

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
};
// A transport owner may supply its actual occupied cells. They affect this
// short physical route only, never perception or the destination's identity.
step_read read_step( const monster &body, const tripoint_abs_ms &goal,
                     const std::function<bool( const tripoint_abs_ms & )> &occupied = {} );
// Caller must first authenticate the current physical owner and spend this
// action once. This opens one adjacent native door, and never moves the body.
bool commit_door( const monster &body, const tripoint_abs_ms &door );
}
#endif
