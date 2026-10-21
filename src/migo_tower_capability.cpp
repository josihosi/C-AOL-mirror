#include "migo_tower_capability.h"

#include <algorithm>
#include <cmath>

#include "anatomy.h"
#include "bodypart.h"
#include "character.h"
#include "item.h"
#include "item_location.h"
#include "itype.h"
#include "mattack_actors.h"
#include "monster.h"
#include "mtype.h"
#include "scout_observation.h"
#include "skill.h"

namespace migo_tower
{
namespace
{
combat_profile monster_profile( const monster &body, bool own )
{
    combat_profile out;
    out.known = true;
    out.hp = own ? body.get_hp() : body.type->hp;
    out.speed = own ? body.get_speed() : body.type->speed;
    out.hit = body.type->melee_skill;
    out.dodge = body.type->sk_dodge;
    out.melee_cost = body.type->attack_cost;
    out.melee = body.type->melee_damage;
    out.melee.add_damage( damage_type_id( "bash" ),
                          body.type->melee_dice * ( body.type->melee_sides + 1 ) / 2.0,
                          body.type->melee_dice_ap );
    out.armor = body.type->armor;
    if( own ) {
        for( const auto &type : damage_type::get_all() ) {
            out.armor.set_resist( type.id, body.get_armor_type( type.id, bodypart_id( "torso" ) ) );
        }
    }
    for( const auto &attack : body.type->special_attacks ) {
        if( own && !body.has_special( attack.first ) ) {
            continue;
        }
        if( const auto *melee = dynamic_cast<const melee_actor *>( attack.second.get() ) ) {
            if( !melee->cooldown.is_constant() || melee->has_condition || melee->is_grab ||
                melee->throw_strength != 0 || ( !own && ( !melee->effects.empty() || !melee->eoc.empty() ) ) ) {
                out.known = false;
                continue;
            }
            damage_instance damage = melee->damage_max_instance;
            damage.mult_damage( own ? ( melee->min_mul + melee->max_mul ) / 2.0 : melee->max_mul );
            const int cost = std::max( melee->move_cost,
                                       static_cast<int>( std::ceil( melee->cooldown.constant() * out.speed ) ) );
            out.melee_options.push_back( {damage, std::max( 1, cost ),
                                          melee->accuracy == INT_MIN ? out.hit : melee->accuracy, melee->dodgeable,
                                          melee->range, melee->no_adjacent} );
            continue;
        }
        const auto *gun = dynamic_cast<const gun_actor *>( attack.second.get() );
        if( !gun ) {
            // Unmodelled special abilities are not converted into zero damage.
            // Mi-go ordinary PARROT speech has no offensive capability.
            if( attack.first != "PARROT" ) {
                out.known = false;
            }
            continue;
        }
        if( !gun->cooldown.is_constant() || gun->has_condition ) {
            out.known = false;
            continue;
        }
        item weapon( gun->gun_type );
        const itype_id ammo = gun->ammo_type.is_null() ? weapon.ammo_default() : gun->ammo_type;
        if( own && weapon.ammo_required() > 0 ) {
            const auto supply = body.ammo.find( gun->ammo_type );
            if( supply == body.ammo.end() || supply->second < weapon.ammo_required() ) {
                continue;
            }
        }
        const int cost = std::max( gun->move_cost,
                                   static_cast<int>( std::ceil( gun->cooldown.constant() * out.speed ) ) );
        const auto damage = ammo.is_null() ? weapon.gun_damage( false ) : weapon.gun_damage( ammo );
        if( damage.total_damage() * out.ranged_cost > out.ranged.total_damage() * cost ) {
            out.ranged = damage;
            out.ranged_cost = std::max( 1, cost );
            out.ranged_range = gun->get_max_range();
        }
    }
    return out;
}

combat_profile visible_human_profile( const Character &enemy )
{
    combat_profile out;
    out.known = true;
    // Healthy human physiology, including a conservative strength allowance.
    // Do not inspect this actor's current HP, attributes, skills or mutations.
    for( const bodypart_id &part : anatomy_id( "human_anatomy" )->get_bodyparts() ) {
        if( part->main_part == part.id() ) {
            out.hp += 2 * part->base_hp;
        }
    }
    out.speed = 100;
    out.hit = 8;
    out.dodge = 4;
    out.melee_cost = 65;
    out.melee.add_damage( damage_type_id( "bash" ), 12 );
    // Only visible outer equipment types enter the estimate. Constructing the
    // native definition avoids reading concealed modifications or contents.
    for( const item_location &visible : enemy.get_visible_worn_items() ) {
        const item armor( visible->typeId() );
        out.armor += resistances( armor, false, 0, bodypart_id( "torso" ) );
    }
    const item_location held = enemy.get_wielded_item();
    if( held ) {
        item weapon( held->typeId() );
        out.melee.add( weapon.base_damage_melee() );
        // Native item::attack_time without the target's hidden enchantments.
        out.melee_cost = std::max( 1, static_cast<int>( 65 +
            ( weapon.volume() / 62.5_ml + weapon.weight() / 60_gram ) / weapon.count() ) );
        if( weapon.is_gun() ) {
            // Assume usable ammunition and the native fastest trained firing
            // interval. Never inspect the observed actor's magazine/inventory.
            weapon.ammo_set( weapon.ammo_default() );
            out.ranged = weapon.gun_damage();
            out.ranged_range = weapon.gun_range();
            out.ranged_cost = weapon.type->gun->skill_used->time_to_attack().min_time;
        }
    }
    return out;
}
} // namespace

combat_profile own_capability( const monster &member )
{
    return monster_profile( member, true );
}

std::optional<combat_profile> observe_enemy( const monster &observer, const Creature &enemy,
        scout_observation::site_reader &reader )
{
    if( enemy.is_dead_state() || observer.attitude_to( enemy ) != Creature::Attitude::HOSTILE ||
        !reader.read_actor( observer, enemy, {MAX_VIEW_DISTANCE * 2, 2.0f} ).visible ) {
        return std::nullopt;
    }
    if( const monster *body = enemy.as_monster() ) {
        return monster_profile( *body, false );
    }
    if( const Character *human = enemy.as_character() ) {
        return visible_human_profile( *human );
    }
    return combat_profile();
}
} // namespace migo_tower
