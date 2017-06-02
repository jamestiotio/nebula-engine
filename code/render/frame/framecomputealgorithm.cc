//------------------------------------------------------------------------------
// framecomputealgorithm.cc
// (C) 2016 Individual contributors, see AUTHORS file
//------------------------------------------------------------------------------
#include "stdneb.h"
#include "framecomputealgorithm.h"

namespace Frame
{

__ImplementClass(Frame::FrameComputeAlgorithm, 'FRCA', Frame::FrameOp);
//------------------------------------------------------------------------------
/**
*/
FrameComputeAlgorithm::FrameComputeAlgorithm()
{
	// empty
}

//------------------------------------------------------------------------------
/**
*/
FrameComputeAlgorithm::~FrameComputeAlgorithm()
{
	// empty
}


//------------------------------------------------------------------------------
/**
*/
void
FrameComputeAlgorithm::Setup()
{
	this->func = this->alg->GetFunction(this->funcName);
}

//------------------------------------------------------------------------------
/**
*/
void
FrameComputeAlgorithm::Discard()
{
	FrameOp::Discard();

	this->func = nullptr;
	this->alg = 0;
}

//------------------------------------------------------------------------------
/**
*/
void
FrameComputeAlgorithm::Run(const IndexT frameIndex)
{
	this->func(frameIndex);
}

} // namespace Frame2