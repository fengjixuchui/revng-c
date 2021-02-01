#pragma once

//
// Copyright rev.ng Srls. See LICENSE.md for details.
//

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/Pass.h"

#include "revng/ADT/GenericGraph.h"

namespace TypeShrinking {

struct DataFlowNodeData {
  DataFlowNodeData(llvm::Instruction *Ins) : Instruction(Ins){};
  llvm::Instruction *Instruction;
};

using DataFlowNode = BidirectionalNode<DataFlowNodeData>;

/// Builds a data flow graph with edges from uses to definitions
GenericGraph<DataFlowNode> buildDataFlowGraph(llvm::Function &F);

} // namespace TypeShrinking
