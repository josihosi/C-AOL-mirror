#pragma once
#ifndef CATA_SRC_ACTIVE_TIME_H
#define CATA_SRC_ACTIVE_TIME_H

#include <algorithm>
#include <cstdint>
#include <limits>

// Value arithmetic only. Callers retain their own eligibility, clock anchors,
// once-only credit, action costs and blocked/unspent-credit settlement policy.
namespace active_time
{
inline int clipped_turns( const int now, const int previous, const int supplied )
{
    if( previous < 0 || now <= previous || supplied <= 0 ) {
        return 0; // Adoption and suspension never manufacture catch-up.
    }
    return static_cast<int>( std::min<std::int64_t>( supplied,
                             static_cast<std::int64_t>( now ) - previous ) );
}

inline int funded_moves( const int previous, const int speed, const int elapsed_turns )
{
    // Eligibility/refusal belongs to the adapter. Preserve the existing
    // signed funding arithmetic rather than concealing an invalid interval.
    const std::int64_t credit = static_cast<std::int64_t>( speed ) * elapsed_turns;
    return static_cast<int>( std::clamp<std::int64_t>( previous + credit,
                             std::numeric_limits<int>::min(), std::numeric_limits<int>::max() ) );
}
} // namespace active_time

#endif
