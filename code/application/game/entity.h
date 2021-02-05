#pragma once
//------------------------------------------------------------------------------
/**
    @struct Game::Entity

    An entity is essentially just an Id with some utility functions attached.
    What actually makes up the entities are their properties.

    The id is split into three parts: the 2 upper bits are used to identify which
    world the entity exists in; the next 8 bits are used as a generation
    counter, so that we can easily reuse the lower 22 bits as an index.
    
    @see    Game::IsValid
    @see    api.h
    @see    propertyid.h
    @see    memdb/typeregistry.h

    @copyright
    (C) 2018-2020 Individual contributors, see AUTHORS file
*/
//------------------------------------------------------------------------------
#include "ids/id.h"

namespace Game
{
    /// category id
    ID_32_TYPE(CategoryId);

    /// instance id point into a category table. Entities are mapped to instanceids
    ID_32_TYPE(InstanceId);

    //ID_32_TYPE(Entity);
    /// 
    struct Entity
    {
        uint32_t index     : 22;
        uint32_t generation : 8;
        uint32_t worldId    : 2;
    
        static Entity FromId(Ids::Id32 id)
        {
            Entity ret;
            ret.index = id & 0x003FFFFF;
            ret.generation = (id & 0x3FC0000) >> 22;
            ret.worldId = (id & 0xC000000) >> 30;
            return ret;
        }
        explicit constexpr operator Ids::Id32() const
        {
            return ((worldId << 30) & 0xC0000000) + ((generation << 22) & 0x3FC0000) + (index & 0x003FFFFF);
        }
        static constexpr Entity Invalid()
        {
            return { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
        }
        constexpr uint32_t HashCode() const
        {
            return index;
        }
        const bool operator==(const Entity& rhs) const { return Ids::Id32(*this) == Ids::Id32(rhs); }
        const bool operator!=(const Entity& rhs) const { return Ids::Id32(*this) != Ids::Id32(rhs); }
        const bool operator<(const Entity& rhs) const { return index < rhs.index; }
        const bool operator>(const Entity& rhs) const { return index > rhs.index; }
    };

} // namespace Game



