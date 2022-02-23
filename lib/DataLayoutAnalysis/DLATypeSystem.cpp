//
// Copyright (c) rev.ng Srls. See LICENSE.md for details.
//

#include <algorithm>
#include <string>

#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/raw_ostream.h"

#include "revng/ADT/FilteredGraphTraits.h"
#include "revng/Support/Assert.h"
#include "revng/Support/Debug.h"
#include "revng/Support/IRHelpers.h"

#include "revng-c/DataLayoutAnalysis/DLATypeSystem.h"

using namespace llvm;

using NodeAllocatorT = SpecificBumpPtrAllocator<dla::LayoutTypeSystemNode>;

static Logger<> CollapsedNodePrinter("dla-print-collapsed-in-dot");

void *operator new(size_t, NodeAllocatorT &NodeAllocator) {
  return NodeAllocator.Allocate();
}

namespace dla {

void OffsetExpression::print(llvm::raw_ostream &OS) const {
  OS << "Off: " << Offset;
  auto NStrides = Strides.size();
  revng_assert(NStrides == TripCounts.size());
  if (not Strides.empty()) {
    for (decltype(NStrides) N = 0; N < NStrides; ++N) {
      OS << ", {S:" << Strides[N] << ",TC:";
      if (TripCounts[N].has_value())
        OS << TripCounts[N].value();
      else
        OS << "none";
      OS << '}';
    }
  }
}

void LayoutTypePtr::print(raw_ostream &Out) const {
  Out << '{';
  Out << "0x";
  Out.write_hex(reinterpret_cast<const unsigned long long>(V));
  Out << " [";
  if (isa<Function>(V)) {
    Out << "fname: " << V->getName();
  } else {
    if (auto *I = dyn_cast<Instruction>(V))
      Out << "In Func: " << I->getFunction()->getName() << " Instr: ";
    else if (auto *A = dyn_cast<Argument>(V))
      Out << "In Func: " << A->getParent()->getName() << " Arg: ";

    Out.write_escaped(getName(V));
  }
  Out << "], 0x";
  Out.write_hex(FieldIdx);
  Out << '}';
}

std::string LayoutTypePtr::toString() const {
  std::string S;
  llvm::raw_string_ostream OS(S);
  print(OS);
  return S;
}

void LayoutTypeSystemNode::print(llvm::raw_ostream &OS) const {
  OS << "LTSN ID: " << ID;
}

namespace {

static constexpr size_t str_len(const char *S) {
  return S ? (*S ? (1 + str_len(S + 1)) : 0UL) : 0UL;
}

// We use \l here instead of \n, because graphviz has this sick way of saying
// that the text in the node labels should be left-justified
static constexpr const char DoRet[] = "\\l";
static constexpr const char NoRet[] = "";
static_assert(sizeof(DoRet) == (str_len(DoRet) + 1));
static_assert(sizeof(NoRet) == (str_len(NoRet) + 1));

static constexpr const char Equal[] = "Equal";
static constexpr const char Inherits[] = "Inherits from";
static constexpr const char Instance[] = "Has Instance of: ";
static constexpr const char Pointer[] = "Points to ";
static constexpr const char Unexpected[] = "Unexpected!";
static_assert(sizeof(Equal) == (str_len(Equal) + 1));
static_assert(sizeof(Inherits) == (str_len(Inherits) + 1));
static_assert(sizeof(Instance) == (str_len(Instance) + 1));
static_assert(sizeof(Unexpected) == (str_len(Unexpected) + 1));
} // end unnamed namespace

void debug_function LayoutTypeSystem::dumpDotOnFile(const char *FName,
                                                    bool ShowCollapsed) const {
  std::error_code EC;
  raw_fd_ostream DotFile(FName, EC);
  revng_check(not EC, (EC.message() + ": " + FName).c_str());

  DotFile << "digraph LayoutTypeSystem {\n";
  DotFile << "  // List of nodes\n";

  for (const LayoutTypeSystemNode *L : getLayoutsRange()) {

    DotFile << "  node_" << L->ID << " [shape=rect,label=\"NODE ID: " << L->ID
            << " Size: " << L->Size << " InterferingChild: ";

    llvm::SmallVector<const llvm::Use *, 8> PtrUses;
    switch (L->InterferingInfo) {
    case Unknown:
      DotFile << 'U';
      break;
    case AllChildrenAreInterfering:
      DotFile << 'A';
      break;
    case AllChildrenAreNonInterfering:
      DotFile << 'N';
      break;
    default:
      revng_unreachable();
    }

    if (CollapsedNodePrinter.isEnabled() or ShowCollapsed)
      DebugPrinter->printNodeContent(*this, L, DotFile);

    DotFile << "\"];\n";
  }

  DotFile << "  // List of edges\n";

  for (LayoutTypeSystemNode *L : getLayoutsRange()) {

    uint64_t SrcNodeId = L->ID;

    for (const auto &PredP : L->Predecessors) {
      const TypeLinkTag *PredTag = PredP.second;
      const auto SameLink = [&](auto &OtherPair) {
        return SrcNodeId == OtherPair.first->ID and PredTag == OtherPair.second;
      };
      revng_assert(std::any_of(PredP.first->Successors.begin(),
                               PredP.first->Successors.end(),
                               SameLink));
    }

    std::string Extra;
    std::string Color;
    std::string Style;
    for (const auto &SuccP : L->Successors) {
      const TypeLinkTag *EdgeTag = SuccP.second;
      const auto SameLink = [&](auto &OtherPair) {
        return SrcNodeId == OtherPair.first->ID and EdgeTag == OtherPair.second;
      };
      revng_assert(std::any_of(SuccP.first->Predecessors.begin(),
                               SuccP.first->Predecessors.end(),
                               SameLink));
      const auto *TgtNode = SuccP.first;
      const char *EdgeLabel = nullptr;
      size_t LabelSize = 0;
      Extra.clear();
      Style.clear();
      switch (EdgeTag->getKind()) {
      case TypeLinkTag::LK_Equality: {
        EdgeLabel = Equal;
        LabelSize = sizeof(Equal) - 1;
        Color = ",color=green";
      } break;
      case TypeLinkTag::LK_Instance: {
        EdgeLabel = Instance;
        LabelSize = sizeof(Instance) - 1;
        Extra = dumpToString(EdgeTag->getOffsetExpr());
        Color = ",color=blue";
      } break;
      case TypeLinkTag::LK_Inheritance: {
        EdgeLabel = Inherits;
        LabelSize = sizeof(Inherits) - 1;
        Color = ",color=orange";
      } break;
      case TypeLinkTag::LK_Pointer: {
        EdgeLabel = Pointer;
        LabelSize = sizeof(Pointer) - 1;
        Color = ",color=purple";
        Style = ",style=dashed";
      } break;
      default: {
        EdgeLabel = Unexpected;
        LabelSize = sizeof(Unexpected) - 1;
        Color = ",color=red";
      } break;
      }
      DotFile << "  node_" << SrcNodeId << " -> node_" << TgtNode->ID
              << " [label=\"" << StringRef(EdgeLabel, LabelSize) << Extra
              << "\"" << Color << Style << "];\n";
    }
  }

  DotFile << "}\n";
}

LayoutTypeSystemNode *LayoutTypeSystem::createArtificialLayoutType() {
  using LTSN = LayoutTypeSystemNode;
  LTSN *New = new (NodeAllocator) LayoutTypeSystemNode(NID);
  revng_assert(New);
  ++NID;
  EqClasses.growBy1();
  bool Success = Layouts.insert(New).second;
  revng_assert(Success);
  return New;
}

static void
fixPredSucc(LayoutTypeSystemNode *From, LayoutTypeSystemNode *Into) {

  revng_assert(From != Into);

  // All the predecessors of all the successors of From are updated so that they
  // point to Into
  for (auto &[Neighbor, Tag] : From->Successors) {
    auto It = Neighbor->Predecessors.lower_bound({ From, nullptr });
    auto End = Neighbor->Predecessors.upper_bound({ std::next(From), nullptr });
    while (It != End) {
      auto Next = std::next(It);
      auto Extracted = Neighbor->Predecessors.extract(It);
      revng_assert(Extracted);
      Neighbor->Predecessors.insert({ Into, Extracted.value().second });
      It = Next;
    }
  }

  // All the successors of all the predecessors of From are updated so that they
  // point to Into
  for (auto &[Neighbor, Tag] : From->Predecessors) {
    auto It = Neighbor->Successors.lower_bound({ From, nullptr });
    auto End = Neighbor->Successors.upper_bound({ std::next(From), nullptr });
    while (It != End) {
      auto Next = std::next(It);
      auto Extracted = Neighbor->Successors.extract(It);
      revng_assert(Extracted);
      Neighbor->Successors.insert({ Into, Extracted.value().second });
      It = Next;
    }
  }

  // Merge all the predecessors and successors.
  {
    Into->Predecessors.insert(From->Predecessors.begin(),
                              From->Predecessors.end());
    Into->Successors.insert(From->Successors.begin(), From->Successors.end());
  }

  // Remove self-references from predecessors and successors.
  {
    const auto RemoveSelfEdges = [From, Into](auto &NeighborsSet) {
      auto FromIt = NeighborsSet.lower_bound({ From, nullptr });
      auto FromEnd = NeighborsSet.upper_bound({ std::next(From), nullptr });
      NeighborsSet.erase(FromIt, FromEnd);
      auto IntoIt = NeighborsSet.lower_bound({ Into, nullptr });
      auto IntoEnd = NeighborsSet.upper_bound({ std::next(Into), nullptr });
      NeighborsSet.erase(IntoIt, IntoEnd);
    };
    RemoveSelfEdges(Into->Predecessors);
    RemoveSelfEdges(Into->Successors);
  }
}

static Logger<> MergeLog("dla-merge-nodes");

using LayoutTypeSystemNodePtrVec = std::vector<LayoutTypeSystemNode *>;

void LayoutTypeSystem::mergeNodes(const LayoutTypeSystemNodePtrVec &ToMerge) {
  revng_assert(ToMerge.size() > 1ULL);
  LayoutTypeSystemNode *Into = ToMerge[0];
  const unsigned IntoID = Into->ID;

  for (LayoutTypeSystemNode *From : llvm::drop_begin(ToMerge, 1)) {
    revng_assert(From != Into);
    revng_log(MergeLog, "Merging: " << From->ID << " Into: " << Into->ID);

    EqClasses.join(IntoID, From->ID);

    fixPredSucc(From, Into);
    Into->InterferingInfo = Unknown;
    revng_assert(not Into->Size or From->Size <= Into->Size);
    Into->Size = std::max(Into->Size, From->Size);

    // Remove From from Layouts
    bool Erased = Layouts.erase(From);
    revng_assert(Erased);
    From->~LayoutTypeSystemNode();
    NodeAllocator.Deallocate(From);
  }
}

void LayoutTypeSystem::removeNode(LayoutTypeSystemNode *ToRemove) {
  // Join the node's eq class with the removed class
  EqClasses.remove(ToRemove->ID);
  revng_log(MergeLog, "Removing " << ToRemove->ID << "\n");

  for (auto &[Neighbor, Tag] : ToRemove->Successors) {
    auto &PredOfSucc = Neighbor->Predecessors;
    auto It = PredOfSucc.lower_bound({ ToRemove, nullptr });
    auto End = PredOfSucc.upper_bound({ std::next(ToRemove), nullptr });
    PredOfSucc.erase(It, End);
  }

  for (auto &[Neighbor, Tag] : ToRemove->Predecessors) {
    auto &SuccOfPred = Neighbor->Successors;
    auto It = SuccOfPred.lower_bound({ ToRemove, nullptr });
    auto End = SuccOfPred.upper_bound({ std::next(ToRemove), nullptr });
    SuccOfPred.erase(It, End);
  }

  bool Erased = Layouts.erase(ToRemove);
  revng_assert(Erased);
  ToRemove->~LayoutTypeSystemNode();
  NodeAllocator.Deallocate(ToRemove);
}

using NeighborIterator = LayoutTypeSystemNode::NeighborsSet::iterator;

static void moveEdgeWithoutSumming(LayoutTypeSystemNode *OldSrc,
                                   LayoutTypeSystemNode *NewSrc,
                                   NeighborIterator EdgeIt) {

  // First, move successor edge from OldSrc to NewSrc
  auto SuccHandle = OldSrc->Successors.extract(EdgeIt);
  revng_assert(not SuccHandle.empty());
  NewSrc->Successors.insert(std::move(SuccHandle));

  // Then, move predecessor edge from OldSrc to NewSrc
  LayoutTypeSystemNode *Tgt = EdgeIt->first;
  auto PredHandle = Tgt->Predecessors.extract({ OldSrc, EdgeIt->second });
  revng_assert(not PredHandle.empty());
  PredHandle.value().first = NewSrc;
  Tgt->Predecessors.insert(std::move(PredHandle));
}

void LayoutTypeSystem::moveEdge(LayoutTypeSystemNode *OldSrc,
                                LayoutTypeSystemNode *NewSrc,
                                NeighborIterator EdgeIt,
                                int64_t OffsetToSum) {

  if (not OldSrc or not NewSrc)
    return;

  if (not OffsetToSum)
    return moveEdgeWithoutSumming(OldSrc, NewSrc, EdgeIt);

  LayoutTypeSystemNode *Tgt = EdgeIt->first;

  // First, move successor edges from OldSrc to NewSrc
  auto OldSuccHandle = OldSrc->Successors.extract(EdgeIt);
  revng_assert(not OldSuccHandle.empty());

  // Add new instance links with adjusted offsets from NewSrc to Tgt.
  // Using the addInstanceLink methods already marks injects NewSrc among the
  // predecessors of Tgt, so after this we only need to remove OldSrc from
  // Tgt's predecessors and we're done.

  const TypeLinkTag *EdgeTag = OldSuccHandle.value().second;
  switch (EdgeTag->getKind()) {

  case TypeLinkTag::LK_Inheritance: {
    if (OffsetToSum > 0LL)
      addInstanceLink(NewSrc, Tgt, OffsetExpression(OffsetToSum));
    else
      addInheritanceLink(NewSrc, Tgt);
  } break;

  case TypeLinkTag::LK_Instance: {
    OffsetExpression NewOE = EdgeTag->getOffsetExpr();
    NewOE.Offset += OffsetToSum;
    revng_assert(NewOE.Offset >= 0LL);
    addInstanceLink(NewSrc, Tgt, std::move(NewOE));
  } break;

  case TypeLinkTag::LK_Equality:
  case TypeLinkTag::LK_Pointer:
  default:
    revng_unreachable("unexpected edge kind");
  }

  // Then, remove all the remaining info in Tgt that represent the fact that
  // OldSrc was a predecessor.
  auto PredHandle = Tgt->Predecessors.extract({ OldSrc, EdgeIt->second });
}

static Logger<> VerifyDLALog("dla-verify-strict");

bool LayoutTypeSystem::verifyConsistency() const {
  for (LayoutTypeSystemNode *NodePtr : Layouts) {
    if (not NodePtr) {
      if (VerifyDLALog.isEnabled())
        revng_check(false);
      return false;
    }
    // Check that predecessors and successors are consistent
    for (auto &P : NodePtr->Predecessors) {
      if (P.first == nullptr) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }

      // same edge with same tag
      auto It = P.first->Successors.find({ NodePtr, P.second });
      if (It == P.first->Successors.end()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
    for (auto &P : NodePtr->Successors) {
      if (P.first == nullptr) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }

      // same edge with same tag
      auto It = P.first->Predecessors.find({ NodePtr, P.second });
      if (It == P.first->Predecessors.end()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }

    // Check that there are no self-edges
    for (auto &P : NodePtr->Predecessors) {
      LayoutTypeSystemNode *Pred = P.first;
      if (Pred == NodePtr) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }

    for (auto &P : NodePtr->Successors) {
      LayoutTypeSystemNode *Succ = P.first;
      if (Succ == NodePtr) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }

    // Verify that pointers are not also structs or unions
    unsigned NonPtrChildren = 0U;
    bool IsPointer = false;
    for (const auto &Edge : NodePtr->Successors) {
      if (isPointerEdge(Edge))
        IsPointer = true;
      else if (isInheritanceEdge(Edge) or isInstanceEdge(Edge))
        NonPtrChildren++;

      if (IsPointer and NonPtrChildren > 0) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }
  return true;
}

bool LayoutTypeSystem::verifyDAG() const {
  if (not verifyConsistency())
    return false;

  if (not verifyInheritanceDAG())
    return false;

  if (not verifyInstanceDAG())
    return false;

  std::set<const LayoutTypeSystemNode *> SCCHeads;

  // A graph is a DAG if and only if all its strongly connected components have
  // size 1
  std::set<const LayoutTypeSystemNode *> Visited;
  for (const auto &Node : llvm::nodes(this)) {
    revng_assert(Node != nullptr);
    if (Visited.count(Node))
      continue;

    using NonPointerFilterT = EdgeFilteredGraph<LayoutTypeSystemNode *,
                                                isNotPointerEdge>;

    auto I = scc_begin(NonPointerFilterT(Node));
    auto E = scc_end(NonPointerFilterT(Node));
    for (; I != E; ++I) {
      Visited.insert(I->begin(), I->end());
      if (I.hasCycle()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyInheritanceDAG() const {
  if (not verifyConsistency())
    return false;

  // A graph is a DAG if and only if all its strongly connected components have
  // size 1
  std::set<const LayoutTypeSystemNode *> Visited;
  for (const auto &Node : llvm::nodes(this)) {
    revng_assert(Node != nullptr);
    if (Visited.count(Node))
      continue;

    using GraphNodeT = const LayoutTypeSystemNode *;
    using InheritanceNodeT = EdgeFilteredGraph<GraphNodeT, isInheritanceEdge>;
    auto I = scc_begin(InheritanceNodeT(Node));
    auto E = scc_end(InheritanceNodeT(Node));
    for (; I != E; ++I) {
      Visited.insert(I->begin(), I->end());
      if (I.hasCycle()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyInstanceDAG() const {
  if (not verifyConsistency())
    return false;

  // A graph is a DAG if and only if all its strongly connected components have
  // size 1
  std::set<const LayoutTypeSystemNode *> Visited;
  for (const auto &Node : llvm::nodes(this)) {
    revng_assert(Node != nullptr);
    if (Visited.count(Node))
      continue;

    using GraphNodeT = const LayoutTypeSystemNode *;
    using InstanceNodeT = EdgeFilteredGraph<GraphNodeT, isInstanceEdge>;
    auto I = scc_begin(InstanceNodeT(Node));
    auto E = scc_end(InstanceNodeT(Node));
    for (; I != E; ++I) {
      Visited.insert(I->begin(), I->end());
      if (I.hasCycle()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyPointerDAG() const {
  if (not verifyConsistency())
    return false;

  // A graph is a DAG if and only if all its strongly connected components have
  // size 1
  std::set<const LayoutTypeSystemNode *> Visited;
  for (const auto &Node : llvm::nodes(this)) {
    revng_assert(Node != nullptr);
    if (Visited.count(Node))
      continue;

    using GraphNodeT = const LayoutTypeSystemNode *;
    using PointerNodeT = EdgeFilteredGraph<GraphNodeT, isPointerEdge>;
    auto I = scc_begin(PointerNodeT(Node));
    auto E = scc_end(PointerNodeT(Node));
    for (; I != E; ++I) {
      Visited.insert(I->begin(), I->end());
      if (I.hasCycle()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyNoEquality() const {
  if (not verifyConsistency())
    return false;
  for (const auto &Node : llvm::nodes(this)) {
    using LTSN = LayoutTypeSystemNode;
    for (const auto &Edge : llvm::children_edges<const LTSN *>(Node)) {
      if (isEqualityEdge(Edge)) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }
  return true;
}

bool LayoutTypeSystem::verifyInstanceAtOffset0DAG() const {
  if (not verifyConsistency())
    return false;

  std::set<const LayoutTypeSystemNode *> Visited;
  for (const auto &Node : llvm::nodes(this)) {
    revng_assert(Node != nullptr);
    if (Visited.count(Node))
      continue;

    using GraphNodeT = const LayoutTypeSystemNode *;
    using InstanceNodeT = EdgeFilteredGraph<GraphNodeT, isInstanceOff0>;
    auto I = scc_begin(InstanceNodeT(Node));
    auto E = scc_end(InstanceNodeT(Node));
    for (; I != E; ++I) {
      Visited.insert(I->begin(), I->end());
      if (I.hasCycle()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyLeafs() const {
  for (const auto &Node : llvm::nodes(this)) {
    if (isLeaf(Node) and Node->Size == 0) {
      if (VerifyDLALog.isEnabled())
        revng_check(false);
      return false;
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyInheritanceTree() const {
  using GraphNodeT = const LayoutTypeSystemNode *;
  using InheritanceNodeT = EdgeFilteredGraph<GraphNodeT, isInheritanceEdge>;
  using GT = GraphTraits<InheritanceNodeT>;
  for (GraphNodeT Node : llvm::nodes(this)) {
    auto Beg = GT::child_begin(Node);
    auto End = GT::child_end(Node);
    if ((Beg != End) and (std::next(Beg) != End)) {
      if (VerifyDLALog.isEnabled())
        revng_check(false);
      return false;
    }
  }
  return true;
}

bool LayoutTypeSystem::verifyUnions() const {
  using GraphNodeT = const LayoutTypeSystemNode *;
  for (GraphNodeT Node : llvm::nodes(this)) {
    if (Node->InterferingInfo == AllChildrenAreInterfering
        and Node->Successors.size() <= 1) {
      if (VerifyDLALog.isEnabled())
        revng_check(false);
      return false;
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyConflicts() const {
  using GraphNodeT = const LayoutTypeSystemNode *;
  using LinkT = const LayoutTypeSystemNode::Link;

  for (GraphNodeT Node : llvm::nodes(this)) {
    for (auto &Succ : Node->Successors) {

      auto HasSameSuccAtOffset0 = [&Succ](const LinkT &L2) {
        return isInstanceOff0(L2) and (Succ.first == L2.first);
      };

      if (isInheritanceEdge(Succ)
          and llvm::any_of(Node->Successors, HasSameSuccAtOffset0)) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }

  return true;
}

unsigned VectEqClasses::growBy1() {
  ++NElems;
  grow(NElems);
  return NElems;
}

void VectEqClasses::remove(const unsigned A) {
  if (RemovedID)
    join(A, *RemovedID);
  else
    RemovedID = A;
}

bool VectEqClasses::isRemoved(const unsigned ID) const {
  // No removed nodes
  if (not RemovedID)
    return false;

  // Uncompressed map
  if (getNumClasses() == 0)
    return (findLeader(ID) == findLeader(*RemovedID));

  // Compressed map
  unsigned ElementEqClass = lookupEqClass(ID);
  unsigned RemovedEqClass = lookupEqClass(*RemovedID);
  return (ElementEqClass == RemovedEqClass);
}

std::optional<unsigned> VectEqClasses::getEqClassID(const unsigned ID) const {
  unsigned EqID = lookupEqClass(ID);
  bool IsRemoved = (RemovedID) ? lookupEqClass(*RemovedID) == EqID : false;

  if (IsRemoved)
    return {};
  return EqID;
}

std::vector<unsigned>
VectEqClasses::computeEqClass(const unsigned ElemID) const {
  std::vector<unsigned> EqClass;

  for (unsigned OtherID = 0; OtherID < NElems; OtherID++)
    if (haveSameEqClass(ElemID, OtherID))
      EqClass.push_back(OtherID);

  return EqClass;
}

bool VectEqClasses::haveSameEqClass(unsigned ID1, unsigned ID2) const {
  // Uncompressed map
  if (getNumClasses() == 0)
    return findLeader(ID1) == findLeader(ID2);

  // Compressed map
  return lookupEqClass(ID1) == lookupEqClass(ID2);
}

void TSDebugPrinter::printNodeContent(const LayoutTypeSystem &TS,
                                      const LayoutTypeSystemNode *N,
                                      llvm::raw_fd_ostream &File) const {
  auto EqClasses = TS.getEqClasses();

  File << DoRet;
  if (EqClasses.isRemoved(N->ID))
    File << "Removed" << DoRet;

  File << "Equivalence Class: [";
  for (auto ID : EqClasses.computeEqClass(N->ID))
    File << ID << ", ";
  File << "]" << DoRet;
}

} // end namespace dla
