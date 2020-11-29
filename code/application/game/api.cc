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
#include "basegamefeature/managers/entitymanager.h"
#include "basegamefeature/managers/blueprintmanager.h"

namespace Game
{

//------------------------------------------------------------------------------
using InclusiveTableMask = MemDb::TableSignature;
using ExclusiveTableMask = MemDb::TableSignature;
using PropertyArray = Util::FixedArray<PropertyId>;
using AccessModeArray = Util::FixedArray<AccessMode>;

static Ids::IdAllocator<InclusiveTableMask, ExclusiveTableMask, PropertyArray, AccessModeArray> filterAllocator;
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
    @todo	optimize
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
*/
void
Execute(Op::RegisterProperty const& op)
{
    EntityManager::State& state = EntityManager::Singleton->state;
    EntityMapping mapping = GetEntityMapping(op.entity);
    Category const& cat = EntityManager::Singleton->GetCategory(mapping.category);
    CategoryHash newHash = cat.hash;
    newHash.AddToHash(op.pid.id);
    CategoryId newCategoryId;
    if (state.catIndexMap.Contains(newHash))
    {
        newCategoryId = state.catIndexMap[newHash];
    }
    else
    {
        CategoryCreateInfo info;
        auto const& cols = state.worldDatabase->GetTable(cat.instanceTable).columns.GetArray<0>();
        info.columns.SetSize(cols.Size());
        IndexT i;
        // Note: Skips owner column
        for (i = 0; i < cols.Size() - 1; ++i)
        {
            info.columns[i] = cols[i + 1];
        }
        info.columns[i] = op.pid;

#ifdef NEBULA_DEBUG
        info.name = cat.name + " + ";
        info.name += MemDb::TypeRegistry::GetDescription(op.pid)->name.AsString();
#endif

        newCategoryId = EntityManager::Singleton->CreateCategory(info);
    }

    EntityManager::Singleton->Migrate(op.entity, newCategoryId);

    if (op.value == nullptr)
        return; // default value should already be set

    Ptr<MemDb::Database> db = Game::GetWorldDatabase();
    auto cid = db->GetColumnId(cat.instanceTable, op.pid);
    void* ptr = db->GetValuePointer(cat.instanceTable, cid, mapping.instance.id);
    Memory::Copy(op.value, ptr, MemDb::TypeRegistry::TypeSize(op.pid));
}

//------------------------------------------------------------------------------
/**
*/
void
Execute(Op::DeregisterProperty const& op)
{
    EntityManager::State& state = EntityManager::Singleton->state;
    EntityMapping mapping = GetEntityMapping(op.entity);
    Category const& cat = EntityManager::Singleton->GetCategory(mapping.category);
    CategoryHash newHash = cat.hash;
    newHash.RemoveFromHash(op.pid.id);
    CategoryId newCategoryId;
    if (state.catIndexMap.Contains(newHash))
    {
        newCategoryId = state.catIndexMap[newHash];
    }
    else
    {
        CategoryCreateInfo info;
        auto const& cols = state.worldDatabase->GetTable(cat.instanceTable).columns.GetArray<0>();
        info.columns.SetSize(cols.Size() - 1);
        // Note: Skips owner column
        int col = 0;
        for (int i = 0; i < cols.Size(); ++i)
        {
            if (cols[i] == op.pid)
                continue;

            info.columns[col++] = cols[i];
        }

#ifdef NEBULA_DEBUG
        info.name = cat.name + " - ";
        info.name += MemDb::TypeRegistry::GetDescription(op.pid)->name.AsString();
#endif

        newCategoryId = EntityManager::Singleton->CreateCategory(info);
    }

    EntityManager::Singleton->Migrate(op.entity, newCategoryId);
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
*/
Dataset Query(Filter filter)
{
    Ptr<MemDb::Database> db = Game::GetWorldDatabase();

    Util::Array<MemDb::TableId> tids = db->Query(filterAllocator.Get<0>(filter), filterAllocator.Get<1>(filter));

    Dataset data;
    data.numViews = tids.Size();

    if (tids.Size() == 0)
    {
        data.views = nullptr;
        return data;
    }

    data.views = (Dataset::CategoryTableView*)viewAllocator.Alloc(sizeof(Dataset::CategoryTableView) * data.numViews);

    PropertyArray const& properties = filterAllocator.Get<2>(filter);

    for (IndexT viewIndex = 0; viewIndex < data.numViews; viewIndex++)
    {
        Dataset::CategoryTableView* view = data.views + viewIndex;
        // FIXME
        view->cid = CategoryId::Invalid();

        MemDb::Table const& tbl = db->GetTable(tids[viewIndex]);

        IndexT i = 0;
        for (auto pid : properties)
        {
            MemDb::ColumnIndex colId = db->GetColumnId(tbl.tid, pid);
            view->buffers[i] = db->GetBuffer(tbl.tid, colId);
            i++;
        }

        view->numInstances = db->GetNumRows(tbl.tid);
    }

    return data;
}

//------------------------------------------------------------------------------
/**
*/
bool
IsValid(Entity e)
{
    n_assert(EntityManager::HasInstance());
    return EntityManager::Singleton->state.pool.IsValid(e.id);
}

//------------------------------------------------------------------------------
/**
*/
bool
IsActive(Entity e)
{
    n_assert(EntityManager::HasInstance());
    n_assert(IsValid(e));
    return EntityManager::Singleton->state.entityMap[Ids::Index(e.id)].instance != InstanceId::Invalid();
}

//------------------------------------------------------------------------------
/**
*/
uint
GetNumEntities()
{
    n_assert(EntityManager::HasInstance());
    return EntityManager::Singleton->state.numEntities;
}


//------------------------------------------------------------------------------
/**
*/
bool
CategoryExists(CategoryHash hash)
{
    n_assert(EntityManager::HasInstance());
    return EntityManager::Singleton->state.catIndexMap.Contains(hash);
}

//------------------------------------------------------------------------------
/**
*/
CategoryId const
GetCategoryId(CategoryHash hash)
{
    n_assert(EntityManager::HasInstance());
    n_assert(EntityManager::Singleton->state.catIndexMap.Contains(hash));
    return EntityManager::Singleton->state.catIndexMap[hash];
}

//------------------------------------------------------------------------------
/**
*/
EntityMapping
GetEntityMapping(Game::Entity entity)
{
    n_assert(EntityManager::HasInstance());
    return EntityManager::Singleton->state.entityMap[Ids::Index(entity.id)];
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
*/
bool
HasProperty(Game::Entity const entity, PropertyId const pid)
{
    EntityManager::State& state = EntityManager::Singleton->state;
    EntityMapping mapping = GetEntityMapping(entity);
    Category const& cat = EntityManager::Singleton->GetCategory(mapping.category);
    return EntityManager::Singleton->state.worldDatabase->HasProperty(cat.instanceTable, pid);
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
    MemDb::TableId tid = EntityManager::Instance()->GetCategory(category).instanceTable;
    return db->GetNumRows(tid);
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