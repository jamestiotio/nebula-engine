//------------------------------------------------------------------------------
//  nvx2streamreader.cc
//  (C) 2007 Radon Labs GmbH
//  (C) 2013-2020 Individual contributors, see AUTHORS file
//------------------------------------------------------------------------------

#include "coregraphics/legacy/nvx2streamreader.h"
#include "coregraphics/legacy/nvx2fileformatstructs.h"
#include "coregraphics/config.h"
#include "coregraphics/indextype.h"
#include "coregraphics/graphicsdevice.h"

#if NEBULA_LEGACY_SUPPORT
namespace Legacy
{
__ImplementClass(Legacy::Nvx2StreamReader, 'N2SR', IO::StreamReader);

using namespace CoreGraphics;
using namespace Util;
using namespace Math;
using namespace Resources;

//------------------------------------------------------------------------------
/**
*/
Nvx2StreamReader::Nvx2StreamReader() :
    usage(CoreGraphics::GpuBufferTypes::UsageImmutable),
    access(CoreGraphics::GpuBufferTypes::AccessNone),
    rawMode(false),
    vbo(InvalidBufferId),
    ibo(InvalidBufferId),
    layout(InvalidVertexLayoutId),
    copySourceFlag(false),
    mapPtr(0),
    groupDataPtr(nullptr),
    vertexDataPtr(nullptr),
    indexDataPtr(nullptr),
    groupDataSize(0),
    vertexDataSize(0),
    indexDataSize(0),
    numGroups(0),
    numVertices(0),
    vertexWidth(0),
    numIndices(0),
    numEdges(0),
    vertexComponentMask(0)
{
    // empty
}

//------------------------------------------------------------------------------
/**
*/
Nvx2StreamReader::~Nvx2StreamReader()
{
    if (this->IsOpen())
    {
        this->Close();
    }
}

//------------------------------------------------------------------------------
/**
*/
bool
Nvx2StreamReader::Open(const Resources::ResourceName& name)
{
    n_assert(0 == this->primGroups.Size());
    n_assert(this->stream->CanBeMapped());
    n_assert(this->vertexComponents.IsEmpty());
    n_assert(0 == this->mapPtr);
    if (StreamReader::Open())
    {
        // map the stream to memory
        if (!this->rawMode)
        {
            this->mapPtr = this->stream->MemoryMap();
        }
        else
        {
            this->mapPtr = this->stream->Map();
        }
        
        n_assert(0 != this->mapPtr);

        // read data
        this->ReadHeaderData();
        this->ReadPrimitiveGroups();
        this->SetupVertexComponents();
        if (!this->rawMode)
        {
            this->SetupVertexBuffer(name);
            this->SetupIndexBuffer(name);
            this->UpdateGroupBoundingBoxes();
            stream->MemoryUnmap();
        }
        return true;
    }
    return false;
}

//------------------------------------------------------------------------------
/**
*/
void
Nvx2StreamReader::Close()
{
    this->mapPtr = 0;
    this->groupDataPtr = nullptr;
    this->vertexDataPtr = nullptr;
    this->indexDataPtr = nullptr;
    this->ibo = InvalidBufferId;
    this->vbo = InvalidBufferId;
    this->primGroups.Clear();
    this->vertexComponents.Clear();
    StreamReader::Close();
}

//------------------------------------------------------------------------------
/**
    This reads the nvx2 header data and checks whether the file is 
    actually an nvx2 file through the magic number. All header data
    will be read into member variables, and pointers to the
    start of the group-, vertex- and index-data will be setup.

    NOTE: we assume that the file is in the correct byte order!
*/
void
Nvx2StreamReader::ReadHeaderData()
{
    n_assert(0 != this->mapPtr);
    
    // endian-convert header
    struct Nvx2Header* header = (struct Nvx2Header*) this->mapPtr;

    // check magic number
    if (FourCC(header->magic) != FourCC('NVX2'))
    {
        // not a nvx2 file, break hard
        n_error("Nvx2StreamReader: '%s' is not a nvx2 file!", this->stream->GetURI().AsString().AsCharPtr());
    }    
    this->numGroups = header->numGroups;
    this->numVertices = header->numVertices;
    this->vertexWidth = header->vertexWidth;
    this->numIndices = header->numIndices * 3;
    this->numEdges = header->numEdges;
    this->vertexComponentMask = header->vertexComponentMask;
    this->groupDataSize = 6 * sizeof(uint) * this->numGroups;
    this->vertexDataSize = this->numVertices * this->vertexWidth * sizeof(float);
    this->indexDataSize = this->numIndices * sizeof(int);

    this->groupDataPtr = header + 1;
    this->vertexDataPtr = ((uchar*)this->groupDataPtr) + this->groupDataSize;
    this->indexDataPtr = ((uchar*)this->vertexDataPtr) + this->vertexDataSize;
}

//------------------------------------------------------------------------------
/**
    Question here, basevertex is supposed to be a vertex offset into the vertex buffer.
    However, the indices describe where to fetch the vertex data, so why would we need it
    if we are using static buffers?
*/
void
Nvx2StreamReader::ReadPrimitiveGroups()
{
    n_assert(this->primGroups.IsEmpty());
    n_assert(this->numGroups > 0);
    n_assert(0 != this->groupDataPtr);
    Nvx2Group* group = (Nvx2Group*) this->groupDataPtr;
    IndexT i;
    for (i = 0; i < (SizeT)this->numGroups; i++)
    {
        // setup a primitive group object
        PrimitiveGroup primGroup;
        //primGroup.SetBaseVertex(group->firstVertex);
        primGroup.SetNumVertices(group->numVertices);
        primGroup.SetBaseIndex(group->firstTriangle * 3);
        primGroup.SetNumIndices(group->numTriangles * 3);
        this->primGroups.Append(primGroup);

        // set top next group
        group++;
    }
}

//------------------------------------------------------------------------------
/**
*/
void
Nvx2StreamReader::SetupVertexComponents()
{
    n_assert(this->vertexComponents.IsEmpty());

    IndexT i;
    for (i = 0; i < N2NumVertexComponents; i++)
    {
        VertexComponent::Format fmt;
        IndexT index = 0;
        if (this->vertexComponentMask & (1<<i))
        {
            switch (1<<i)
            {
                case N2Coord:        index = VertexComponent::Position;     fmt = VertexComponent::Float3; break;
                case N2Normal:       index = VertexComponent::Normal;       fmt = VertexComponent::Float3; break;
                case N2NormalB4N:    index = VertexComponent::Normal;       fmt = VertexComponent::Byte4N; break;
                case N2Uv0:          index = VertexComponent::TexCoord1;    fmt = VertexComponent::Float2; break;
                case N2Uv0S2:        index = VertexComponent::TexCoord1;    fmt = VertexComponent::Short2; break;
                case N2Uv1:          index = VertexComponent::TexCoord2;    fmt = VertexComponent::Float2; break;
                case N2Uv1S2:        index = VertexComponent::TexCoord2;    fmt = VertexComponent::Short2; break;
                case N2Uv2:          index = VertexComponent::TexCoord3;    fmt = VertexComponent::Float2; break;
                case N2Uv2S2:        index = VertexComponent::TexCoord3;    fmt = VertexComponent::Short2; break;
                case N2Uv3:          index = VertexComponent::TexCoord4;    fmt = VertexComponent::Float2; break;
                case N2Uv3S2:        index = VertexComponent::TexCoord4;    fmt = VertexComponent::Short2; break;
                case N2Tangent:      index = VertexComponent::Tangent;      fmt = VertexComponent::Float3; break;
                case N2TangentB4N:   index = VertexComponent::Tangent;      fmt = VertexComponent::Byte4N; break;
                case N2Binormal:     index = VertexComponent::Binormal;     fmt = VertexComponent::Float3; break;
                case N2BinormalB4N:  index = VertexComponent::Binormal;     fmt = VertexComponent::Byte4N; break;
                case N2Color:        index = VertexComponent::Color;        fmt = VertexComponent::Float4; break;
                case N2ColorUB4N:    index = VertexComponent::Color;        fmt = VertexComponent::UByte4N; break;
                case N2Weights:      index = VertexComponent::SkinWeights;  fmt = VertexComponent::Float4; break;
                case N2WeightsUB4N:  index = VertexComponent::SkinWeights;  fmt = VertexComponent::UByte4N; break;
                case N2JIndices:     index = VertexComponent::SkinJIndices; fmt = VertexComponent::Float4; break;
                case N2JIndicesUB4:  index = VertexComponent::SkinJIndices; fmt = VertexComponent::UByte4; break;
                default:
                    n_error("Invalid Nebula VertexComponent in Nvx2StreamReader::SetupVertexComponents");
                    index = VertexComponent::Position;
                    fmt = VertexComponent::Float3;
                    break;
            }
            this->vertexComponents.Append(VertexComponent(index, fmt));
        }
    }

    this->layout = CoreGraphics::CreateVertexLayout({ .name = "Legacy NVX2 Dynamic Layout"_atm, .comps = this->vertexComponents });
}

//------------------------------------------------------------------------------
/**
    Since nvx2 files don't contain any bounding box information
    we need to compute per-primitive-group bounding boxes
    manually by walking the triangle indices. This may be inefficient
    with large meshes.
*/
void
Nvx2StreamReader::UpdateGroupBoundingBoxes()
{
    n_assert(0 != this->vertexDataPtr);
    n_assert(0 != this->indexDataPtr);
    n_assert(this->primGroups.Size() > 0);

    float* vertexPtr = (float*) this->vertexDataPtr;
    ushort* indexPtr = (ushort*) this->indexDataPtr;
    IndexT groupIndex;
    for (groupIndex = 0; groupIndex < this->primGroups.Size(); groupIndex++)
    {
        PrimitiveGroup& group = this->primGroups[groupIndex];        
        bbox box;
        box.begin_extend();
        vec3 p;
        IndexT ii;
        for (ii = 0; ii < group.GetNumIndices(); ii++)
        {
            float* curVertexPtr = vertexPtr + (indexPtr[ii + group.GetBaseIndex()] * this->vertexWidth);
            p.set(curVertexPtr[0], curVertexPtr[1], curVertexPtr[2]);
            box.extend(p);
        }
    }
}

//------------------------------------------------------------------------------
/**
*/
void
Nvx2StreamReader::SetupVertexBuffer(const Resources::ResourceName& name)
{
    n_assert(this->vbo == InvalidBufferId);
    n_assert(!this->rawMode);
    n_assert(0 != this->vertexDataPtr);
    n_assert(this->vertexDataSize > 0);
    n_assert(this->numVertices > 0);    
    n_assert(this->vertexComponents.Size() > 0);

    // Get upload buffer
    auto [offset, buffer] = CoreGraphics::Upload(this->vertexDataPtr, this->vertexDataSize);

    // Get main vertex buffer
    this->vbo = CoreGraphics::GetVertexBuffer();

    // Allocate vertices from global repository 
    uint elementSize = VertexLayoutGetSize(this->layout);
    this->vertexAllocation = CoreGraphics::AllocateVertices(this->numVertices, elementSize);

    // Copy from host mappable buffer to device local buffer
    CoreGraphics::BufferCopy from, to;
    from.offset = offset;
    to.offset = this->vertexAllocation.offset;
    CoreGraphics::CmdBufferId cmdBuf = CoreGraphics::LockGraphicsSetupCommandBuffer();
    CoreGraphics::CmdCopy(cmdBuf, buffer, {from}, this->vbo, {to}, this->vertexDataSize);
    CoreGraphics::UnlockGraphicsSetupCommandBuffer();
}

//------------------------------------------------------------------------------
/**
*/
void
Nvx2StreamReader::SetupIndexBuffer(const Resources::ResourceName& name)
{
    n_assert(this->ibo == InvalidBufferId);
    n_assert(!this->rawMode);
    n_assert(0 != this->indexDataPtr);
    n_assert(this->indexDataSize > 0);
    n_assert(this->numIndices > 0);

    // Get upload buffer
    auto [offset, buffer] = CoreGraphics::Upload(this->indexDataPtr, this->indexDataSize);

    // Get main index buffer
    this->ibo = CoreGraphics::GetIndexBuffer();

    // Allocate vertices from global repository 
    this->indexAllocation = CoreGraphics::AllocateIndices(this->numIndices, CoreGraphics::IndexType::Index32);

    // Copy from host mappable buffer to device local buffer
    CoreGraphics::BufferCopy from, to;
    from.offset = offset;
    to.offset = this->indexAllocation.offset;
    CoreGraphics::CmdBufferId cmdBuf = CoreGraphics::LockGraphicsSetupCommandBuffer();
    CoreGraphics::CmdCopy(cmdBuf, buffer, {from}, this->ibo, {to}, this->indexDataSize);
    CoreGraphics::UnlockGraphicsSetupCommandBuffer();
}

} // namespace Legacy

#endif // NEBULA_LEGACY_SUPPORT
