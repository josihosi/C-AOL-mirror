#pragma once
#ifndef CATA_SRC_MIGO_POPULATION_IDENTITY_H
#define CATA_SRC_MIGO_POPULATION_IDENTITY_H

#include <cstdint>
#include <string>
#include "coordinates.h"

class JsonObject;
class JsonOut;

namespace migo_population
{
struct identity {
    std::string site_id;
    std::string member_id;
    std::uint64_t epoch = 0;
    tripoint_abs_omt site_anchor = tripoint_abs_omt::zero;
    bool empty() const { return site_id.empty() || member_id.empty(); }
    void serialize( JsonOut &json ) const;
    void deserialize( const JsonObject &json );
};
} // namespace migo_population
#endif
