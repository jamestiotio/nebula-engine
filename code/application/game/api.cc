//------------------------------------------------------------------------------
//  api.cc
//  (C) 2020 Individual contributors, see AUTHORS file
//------------------------------------------------------------------------------
#include "application/stdneb.h"
#include "api.h"
#include "gameserver.h"
#include "ids/idallocator.h"
#include "memdb/tablesignature.h"
#include "memdb/database.h"
#include "memory/arenaallocator.h"
#include "basegamefeature/managers/blueprintmanager.h"
#include "profiling/profiling.h"

namespace Game
{

//------------------------------------------------------------------------------
using InclusiveTableMask = MemDb::TableSignature;
using ExclusiveTableMask = MemDb::TableSignature;
using PropertyArray = Util::FixedArray<PropertyId>;
using AccessModeArray = Util::FixedArray<AccessMode>;

Ids::IdAllocator<InclusiveTableMask, ExclusiveTableMask, PropertyArray, AccessModeArray>  filterAllocator;
static Memory::ArenaAllocator<sizeof(Dataset::CategoryTableView) * 256> viewAllocator;
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
using RegPidQueue = Util::Queue<Op::RegisterProperty>;
using DeregPidQueue = Util::Queue<Op::DeregisterProperty>;

static Ids::IdAllocator<RegPidQueue, DeregPidQueue> opBufferAllocator;
static Memory::ArenaAllocator<1024> opAllocator;
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
/**
*/
Ptr<MemDb::Database>
GetWorldDatabase()
{
    n_assert(GameServer::HasInstance());
    return GameServer::Singleton->state.world.db;
}

//------------------------------------------------------------------------------
/**
*/
Game::Entity
CreateEntity(EntityCreateInfo const& info)
{
    n_assert(GameServer::HasInstance());
    GameServer::State* const state = &GameServer::Singleton->state;

    World& world = state->world;

    Entity entity;
    world.pool.Allocate(entity);
    world.numEntities++;

    // Make sure the entitymap can contain this entity
    if (world.entityMap.Size() <= entity.index)
    {
        world.entityMap.Grow();
        world.entityMap.Resize(world.entityMap.Capacity());
    }

    World::AllocateInstanceCommand cmd;
    cmd.entity = entity;
    if (info.templateId != TemplateId::Invalid())
    {
        cmd.tid = info.templateId;
    }
    else
    {
        cmd.tid.blueprintId = info.blueprint.id;
        cmd.tid.templateId = Ids::InvalidId16;
    }

    if (!info.immediate)
    {
        world.allocQueue.Enqueue(std::move(cmd));
    }
    else
    {
        if (cmd.tid.templateId != Ids::InvalidId16)
        {
            world.AllocateInstance(cmd.entity, cmd.tid);
        }
        else
        {
            world.AllocateInstance(cmd.entity, (BlueprintId)cmd.tid.blueprintId);
        }
    }

    return entity;
}

//------------------------------------------------------------------------------
/**
*/
void
DeleteEntity(Game::Entity entity)
{
    n_assert(IsValid(entity));
    n_assert(GameServer::HasInstance());
    GameServer::State* const state = &GameServer::Singleton->state;

    // make sure we're not trying to dealloc an instance that does not exist
    n_assert2(IsActive(entity), "Cannot delete and entity before it has been instantiated!\n");

    World& world = state->world;

    World::DeallocInstanceCommand cmd;
    cmd.category = world.entityMap[entity.index].category;
    cmd.instance = world.entityMap[entity.index].instance;

    world.deallocQueue.Enqueue(std::move(cmd));

    world.entityMap[entity.index].category = CategoryId::Invalid();
    world.entityMap[entity.index].instance = InstanceId::Invalid();

    world.pool.Deallocate(entity);

    world.numEntities--;
}

//------------------------------------------------------------------------------
/**
*/
OpBuffer
CreateOpBuffer()
{
    return opBufferAllocator.Alloc();
}

//------------------------------------------------------------------------------
/**
    @todo   We can bundle all add and remove property for each entity into one
            migration.
            We can also batch them based on their new category, so we won't
            need to do as many column id lookups.
    @todo   This is not thread safe. Either, we keep it like it is and make sure
            this function is always called synchronously, or add a critical section?
*/
void
Dispatch(OpBuffer& buffer)
{
    RegPidQueue& registerPropertyQueue = opBufferAllocator.Get<0>(buffer);
    DeregPidQueue& deregisterPropertyQueue = opBufferAllocator.Get<1>(buffer);

    while (!registerPropertyQueue.IsEmpty())
    {
        auto op = registerPropertyQueue.Dequeue();
        Game::Execute(op);
    }

    while (!deregisterPropertyQueue.IsEmpty())
    {
        auto op = deregisterPropertyQueue.Dequeue();
        Game::Execute(op);
    }

    opBufferAllocator.Dealloc(buffer);
    buffer = InvalidIndex;
}

//------------------------------------------------------------------------------
/**
    @todo   optimize
*/
void
AddOp(OpBuffer buffer, Op::RegisterProperty op)
{
    if (op.value != nullptr)
    {
        SizeT const typeSize = MemDb::TypeRegistry::TypeSize(op.pid);
        void* value = opAllocator.Alloc(typeSize);
        Memory::Copy(op.value, value, typeSize);
        op.value = value;
    }
    opBufferAllocator.Get<0>(buffer).Enqueue(op);
}

//------------------------------------------------------------------------------
/**
*/
void
AddOp(OpBuffer buffer, Op::DeregisterProperty const& op)
{
    opBufferAllocator.Get<1>(buffer).Enqueue(op);
}

//------------------------------------------------------------------------------
/**
    @todo   Optimize me
*/
void
Execute(Op::RegisterProperty const& op)
{
    GameServer::State* const state = &GameServer::Singleton->state;
    EntityMapping mapping = GetEntityMapping(op.entity);
    World& world = GameServer::Singleton->state.world;
    
    MemDb::TableSignature signature = world.db->GetTableSignature(mapping.category);
    if (signature.IsSet(op.pid))
        return;

    signature.FlipBit(op.pid);

    CategoryId newCategoryId = world.db->FindTable(signature);
    if (newCategoryId == CategoryId::Invalid())
    {
        // Category with this hash does not exist. Create a new category.
        CategoryCreateInfo info;
        auto const& cols = world.db->GetTable(mapping.category).properties;
        info.properties.SetSize(cols.Size() + 1);
        IndexT i;
        for (i = 0; i < cols.Size(); ++i)
        {
            info.properties[i] = cols[i];
        }
        info.properties[i] = op.pid;

        newCategoryId = world.CreateCategory(info);
    }

    InstanceId newInstance = world.Migrate(op.entity, newCategoryId);

    if (op.value == nullptr)
        return; // default value should already be set

    auto cid = world.db->GetColumnId(newCategoryId, op.pid);
    void* ptr = world.db->GetValuePointer(newCategoryId, cid, newInstance.id);
    Memory::Copy(op.value, ptr, MemDb::TypeRegistry::TypeSize(op.pid));
}

//------------------------------------------------------------------------------
/**
    @bug   If you deregister a managed property, the property will just disappear
           without letting the manager clean up any resources, leading to memleaks.
*/
void
Execute(Op::DeregisterProperty const& op)
{
#if NEBULA_DEBUG
    n_assert(Game::HasProperty(op.entity, op.pid));
#endif
    GameServer::State* const state = &GameServer::Singleton->state;
    EntityMapping mapping = GetEntityMapping(op.entity);
    World& world = GameServer::Singleton->state.world;
    MemDb::TableSignature signature = world.db->GetTableSignature(mapping.category);
    if (!signature.IsSet(op.pid))
        return;

    signature.FlipBit(op.pid);

    CategoryId newCategoryId = world.db->FindTable(signature);
    if (newCategoryId == CategoryId::Invalid())
    {
        CategoryCreateInfo info;
        auto const& cols = world.db->GetTable(mapping.category).properties;
        SizeT const num = cols.Size();
        info.properties.SetSize(num - 1);
        int col = 0;
        for (int i = 0; i < num; ++i)
        {
            if (cols[i] == op.pid)
                continue;

            info.properties[col++] = cols[i];
        }
        
        newCategoryId = world.CreateCategory(info);
    }

    world.Migrate(op.entity, newCategoryId);
}

//------------------------------------------------------------------------------
/**
*/
void
ReleaseAllOps()
{
    opAllocator.Release();
}

//------------------------------------------------------------------------------
/**
*/
Filter
CreateFilter(FilterCreateInfo const& info)
{
    n_assert(info.numInclusive > 0);
    uint32_t filter = filterAllocator.Alloc();

    PropertyArray inclusiveArray;
    inclusiveArray.Resize(info.numInclusive);
    for (uint8_t i = 0; i < info.numInclusive; i++)
    {
        inclusiveArray[i] = info.inclusive[i];
    }

    PropertyArray exclusiveArray;
    exclusiveArray.Resize(info.numExclusive);
    for (uint8_t i = 0; i < info.numExclusive; i++)
    {
        exclusiveArray[i] = info.exclusive[i];
    }

    AccessModeArray accessArray;
    accessArray.Resize(info.numInclusive);
    for (uint8_t i = 0; i < info.numInclusive; i++)
    {
        accessArray[i] = info.access[i];
    }

    filterAllocator.Set(filter,
        InclusiveTableMask(inclusiveArray),
        ExclusiveTableMask(exclusiveArray),
        inclusiveArray,
        accessArray
    );

    return filter;
}

//------------------------------------------------------------------------------
/**
*/
void
DestroyFilter(Filter filter)
{
    filterAllocator.Dealloc(filter);
}

//------------------------------------------------------------------------------
/**
*/
ProcessorHandle
CreateProcessor(ProcessorCreateInfo const& info)
{
    return Game::GameServer::Instance()->CreateProcessor(info);
}

//------------------------------------------------------------------------------
/**
*/
void
ReleaseDatasets()
{
    viewAllocator.Release();
}

//------------------------------------------------------------------------------
/**
    @returns    Dataset with category table views.

    @note       The category table view buffer can be NULL if the filter contains
                a non-typed/flag property.
*/
Dataset Query(Filter filter)
{
#if NEBULA_ENABLE_PROFILING
    //N_COUNTER_INCR("Calls to Game::Query", 1);
    N_SCOPE_ACCUM(QueryTime, EntitySystem);
#endif
    Ptr<MemDb::Database> db = Game::GetWorldDatabase();

    Util::Array<MemDb::TableId> tids = db->Query(filterAllocator.Get<0>(filter), filterAllocator.Get<1>(filter));

    return Query(tids, filter);
}

//------------------------------------------------------------------------------
/**
*/
Dataset
Query(Util::Array<MemDb::TableId>& tids, Filter filter)
{
    Ptr<MemDb::Database> db = Game::GetWorldDatabase();
    return Query(db, tids, filter);
}

//------------------------------------------------------------------------------
/**
*/
Dataset
Query(Ptr<MemDb::Database> const& db, Util::Array<MemDb::TableId>& tids, Filter filter)
{
    Dataset data;
    data.numViews = 0;

    if (tids.Size() == 0)
    {
        data.views = nullptr;
        return data;
    }

    data.views = (Dataset::CategoryTableView*)viewAllocator.Alloc(sizeof(Dataset::CategoryTableView) * tids.Size());

    PropertyArray const& properties = filterAllocator.Get<2>(filter);

    for (IndexT tableIndex = 0; tableIndex < tids.Size(); tableIndex++)
    {
        if (db->IsValid(tids[tableIndex]))
        {
            SizeT const numRows = db->GetNumRows(tids[tableIndex]);
            if (numRows > 0)
            {
                Dataset::CategoryTableView* view = data.views + data.numViews;
                view->cid = tids[tableIndex];

                MemDb::Table const& tbl = db->GetTable(tids[tableIndex]);

                IndexT i = 0;
                for (auto pid : properties)
                {
                    MemDb::ColumnIndex colId = db->GetColumnId(tbl.tid, pid);
                    // Check if the property is a flag, and return a nullptr in that case.
                    if (colId != InvalidIndex)
                        view->buffers[i] = db->GetBuffer(tbl.tid, colId);
                    else
                        view->buffers[i] = nullptr;
                    i++;
                }

                view->numInstances = numRows;
                data.numViews++;
            }
        }
        else
        {
            tids.EraseIndexSwap(tableIndex);
            // re-run the same index
            tableIndex--;
        }
    }

    return data;
}

//------------------------------------------------------------------------------
/**
*/
bool
IsValid(Entity e)
{
    n_assert(GameServer::HasInstance());
    return GameServer::Singleton->state.world.pool.IsValid(e);
}

//------------------------------------------------------------------------------
/**
*/
bool
IsActive(Entity e)
{
    n_assert(GameServer::HasInstance());
    n_assert(IsValid(e));
    return GameServer::Singleton->state.world.entityMap[e.index].instance != InstanceId::Invalid();
}

//------------------------------------------------------------------------------
/**
*/
uint
GetNumEntities()
{
    n_assert(GameServer::HasInstance());
    return GameServer::Singleton->state.world.numEntities;
}

//------------------------------------------------------------------------------
/**
*/
EntityMapping
GetEntityMapping(Game::Entity entity)
{
    n_assert(GameServer::HasInstance());
    n_assert(IsActive(entity));
    return GameServer::Singleton->state.world.entityMap[entity.index];
}

//------------------------------------------------------------------------------
/**
*/
PropertyId
CreateProperty(PropertyCreateInfo const& info)
{
    return MemDb::TypeRegistry::Register(info.name, info.byteSize, info.defaultValue, info.flags);
}

//------------------------------------------------------------------------------
/**
*/
PropertyId
GetPropertyId(Util::StringAtom name)
{
    return MemDb::TypeRegistry::GetPropertyId(name);
}

//------------------------------------------------------------------------------
/**
   TODO: This is not thread safe!
*/
bool
HasProperty(Game::Entity const entity, PropertyId const pid)
{
    GameServer::State& state = GameServer::Singleton->state;
    EntityMapping mapping = GetEntityMapping(entity);
    return GameServer::Singleton->state.world.db->HasProperty(mapping.category, pid);
}

//------------------------------------------------------------------------------
/**
*/
BlueprintId
GetBlueprintId(Util::StringAtom name)
{
    return BlueprintManager::GetBlueprintId(name);
}

//------------------------------------------------------------------------------
/**
*/
TemplateId
GetTemplateId(Util::StringAtom name)
{
    return BlueprintManager::GetTemplateId(name);
}

//------------------------------------------------------------------------------
/**
*/
SizeT
GetNumInstances(CategoryId category)
{
    Ptr<MemDb::Database> db = GetWorldDatabase();
    return db->GetNumRows(category);
}

//------------------------------------------------------------------------------
/**
*/
void*
GetInstanceBuffer(CategoryId const category, PropertyId const pid)
{
    Ptr<MemDb::Database> db = Game::GameServer::Singleton->state.world.db;
    auto cid = db->GetColumnId(category, pid);
#if NEBULA_DEBUG
    n_assert_fmt(cid != MemDb::ColumnIndex::Invalid(), "GetInstanceBuffer: Category does not have property with id '%i'!\n", pid.id);
#endif
    return db->GetBuffer(category, cid);
}

//------------------------------------------------------------------------------
/**
*/
InclusiveTableMask const&
GetInclusiveTableMask(Filter filter)
{
    return filterAllocator.Get<0>(filter);
}

//------------------------------------------------------------------------------
/**
*/
ExclusiveTableMask const&
GetExclusiveTableMask(Filter filter)
{
    return filterAllocator.Get<1>(filter);
}

//------------------------------------------------------------------------------
/**
*/
InstanceId
GetInstanceId(Entity entity)
{
    return GetEntityMapping(entity).instance;
}

} // namespace Game
