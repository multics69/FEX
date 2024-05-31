// SPDX-License-Identifier: MIT
/*
$info$
tags: ir|opts
desc: ConstProp, ZExt elim, addressgen coalesce, const pooling, fcmp reduction, const inlining
$end_info$
*/


// aarch64 heuristics
#include "aarch64/assembler-aarch64.h"
#include "aarch64/cpu-aarch64.h"
#include "aarch64/disasm-aarch64.h"
#include "aarch64/assembler-aarch64.h"

#include "Interface/IR/IREmitter.h"
#include "Interface/IR/PassManager.h"

#include <FEXCore/IR/IR.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/Profiler.h>
#include <FEXCore/fextl/map.h>
#include <FEXCore/fextl/robin_map.h>
#include <FEXCore/fextl/unordered_map.h>

#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
#include <string.h>
#include <tuple>
#include <utility>

namespace FEXCore::IR {

uint64_t getMask(IROp_Header* Op) {
  uint64_t NumBits = Op->Size * 8;
  return (~0ULL) >> (64 - NumBits);
}

// Returns true if the number bits from [0:width) contain the same bit.
// Ensuring that the consecutive bits in the range are entirely 0 or 1.
static bool HasConsecutiveBits(uint64_t imm, unsigned width) {
  if (width == 0) {
    return true;
  }

  // Credit to https://github.com/dougallj for this implementation.
  return ((imm ^ (imm >> 1)) & ((1ULL << (width - 1)) - 1)) == 0;
}

// aarch64 heuristics
static bool IsImmLogical(uint64_t imm, unsigned width) {
  if (width < 32) {
    width = 32;
  }
  return vixl::aarch64::Assembler::IsImmLogical(imm, width);
}
static bool IsImmAddSub(uint64_t imm) {
  return vixl::aarch64::Assembler::IsImmAddSub(imm);
}

static bool IsSIMM9Range(uint64_t imm) {
  // AArch64 signed immediate unscaled 9-bit range.
  // Used for both regular unscaled loadstore instructions
  // and LRPCPC2 unscaled loadstore instructions.
  return ((int64_t)imm >= -256) && ((int64_t)imm <= 255);
}

static bool IsImmMemory(uint64_t imm, uint8_t AccessSize) {
  if (IsSIMM9Range(imm)) {
    return true;
  } else if ((imm & (AccessSize - 1)) == 0 && imm / AccessSize <= 4095) {
    return true;
  } else {
    return false;
  }
}

static bool IsTSOImm9(uint64_t imm) {
  // RCPC2 only has a 9-bit signed offset
  if (IsSIMM9Range(imm)) {
    return true;
  } else {
    return false;
  }
}

static bool IsBfeAlreadyDone(IREmitter* IREmit, OrderedNodeWrapper src, uint64_t Width) {
  auto IROp = IREmit->GetOpHeader(src);
  if (IROp->Op == OP_BFE) {
    auto Op = IROp->C<IR::IROp_Bfe>();
    if (Width >= Op->Width) {
      return true;
    }
  }
  return false;
}

class ConstProp final : public FEXCore::IR::Pass {
public:
  explicit ConstProp(bool DoInlineConstants, bool SupportsTSOImm9)
    : InlineConstants(DoInlineConstants)
    , SupportsTSOImm9 {SupportsTSOImm9} {}

  void Run(IREmitter* IREmit) override;

  bool InlineConstants;

private:
  void HandleConstantPools(IREmitter* IREmit, const IRListView& CurrentIR);
  void ConstantPropagation(IREmitter* IREmit, const IRListView& CurrentIR, OrderedNode* CodeNode, IROp_Header* IROp);
  void ConstantInlining(IREmitter* IREmit, const IRListView& CurrentIR);

  struct ConstPoolData {
    OrderedNode* Node;
    IR::NodeID NodeID;
  };
  fextl::unordered_map<uint64_t, ConstPoolData> ConstPool;
  fextl::map<OrderedNode*, uint64_t> AddressgenConsts;

  // Pool inline constant generation. These are typically very small and pool efficiently.
  fextl::robin_map<uint64_t, OrderedNode*> InlineConstantGen;
  OrderedNode* CreateInlineConstant(IREmitter* IREmit, uint64_t Constant) {
    const auto it = InlineConstantGen.find(Constant);
    if (it != InlineConstantGen.end()) {
      return it->second;
    }
    auto Result = InlineConstantGen.insert_or_assign(Constant, IREmit->_InlineConstant(Constant));
    return Result.first->second;
  }
  bool SupportsTSOImm9 {};
  // This is a heuristic to limit constant pool live ranges to reduce RA interference pressure.
  // If the range is unbounded then RA interference pressure seems to increase to the point
  // that long blocks of constant usage can slow to a crawl.
  // See https://github.com/FEX-Emu/FEX/issues/2688 for more information.
  constexpr static uint32_t CONSTANT_POOL_RANGE_LIMIT = 500;
};

// Constants are pooled per block. Similarly for LoadMem / StoreMem, if imms are
// close by, use address gen to generate the values instead of using a new imm.
void ConstProp::HandleConstantPools(IREmitter* IREmit, const IRListView& CurrentIR) {
  for (auto [BlockNode, BlockIROp] : CurrentIR.GetBlocks()) {
    for (auto [CodeNode, IROp] : CurrentIR.GetCode(BlockNode)) {
      if (IROp->Op == OP_LOADMEM || IROp->Op == OP_STOREMEM) {
        size_t AddrIndex = 0;
        size_t OffsetIndex = 0;

        if (IROp->Op == OP_LOADMEM) {
          AddrIndex = IR::IROp_LoadMem::Addr_Index;
          OffsetIndex = IR::IROp_LoadMem::Offset_Index;
        } else {
          AddrIndex = IR::IROp_StoreMem::Addr_Index;
          OffsetIndex = IR::IROp_StoreMem::Offset_Index;
        }
        uint64_t Addr;

        if (IREmit->IsValueConstant(IROp->Args[AddrIndex], &Addr) && IROp->Args[OffsetIndex].IsInvalid()) {
          for (auto& Const : AddressgenConsts) {
            if ((Addr - Const.second) < 65536) {
              IREmit->ReplaceNodeArgument(CodeNode, AddrIndex, Const.first);
              IREmit->ReplaceNodeArgument(CodeNode, OffsetIndex, IREmit->_Constant(Addr - Const.second));
              goto doneOp;
            }
          }

          AddressgenConsts[IREmit->UnwrapNode(IROp->Args[AddrIndex])] = Addr;
        }
doneOp:;
      } else if (IROp->Op == OP_CONSTANT) {
        auto Op = IROp->C<IR::IROp_Constant>();
        const auto NewNodeID = CurrentIR.GetID(CodeNode);

        auto it = ConstPool.find(Op->Constant);
        if (it != ConstPool.end()) {
          const auto OldNodeID = it->second.NodeID;

          if ((NewNodeID.Value - OldNodeID.Value) > CONSTANT_POOL_RANGE_LIMIT) {
            // Don't reuse if the live range is beyond the heurstic range.
            // Update the tracked value to this new constant.
            it->second.Node = CodeNode;
            it->second.NodeID = NewNodeID;
            continue;
          }

          auto CodeIter = CurrentIR.at(CodeNode);
          IREmit->ReplaceUsesWithAfter(CodeNode, it->second.Node, CodeIter);
        } else {
          ConstPool[Op->Constant] = ConstPoolData {
            .Node = CodeNode,
            .NodeID = NewNodeID,
          };
        }
      }

      IREmit->SetWriteCursor(CodeNode);
    }
    AddressgenConsts.clear();
    ConstPool.clear();
  }
}

// constprop + some more per instruction logic
void ConstProp::ConstantPropagation(IREmitter* IREmit, const IRListView& CurrentIR, OrderedNode* CodeNode, IROp_Header* IROp) {
  switch (IROp->Op) {
  case OP_ADD:
  case OP_SUB:
  case OP_ADDWITHFLAGS:
  case OP_SUBWITHFLAGS: {
    auto Op = IROp->C<IR::IROp_Add>();
    uint64_t Constant1 {};
    uint64_t Constant2 {};
    bool IsConstant1 = IREmit->IsValueConstant(IROp->Args[0], &Constant1);
    bool IsConstant2 = IREmit->IsValueConstant(IROp->Args[1], &Constant2);

    if (IsConstant1 && IsConstant2 && IROp->Op == OP_ADD) {
      uint64_t NewConstant = (Constant1 + Constant2) & getMask(IROp);
      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    } else if (IsConstant1 && IsConstant2 && IROp->Op == OP_SUB) {
      uint64_t NewConstant = (Constant1 - Constant2) & getMask(IROp);
      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    } else if (IsConstant2 && !IsImmAddSub(Constant2) && IsImmAddSub(-Constant2)) {
      // If the second argument is constant, the immediate is not ImmAddSub, but when negated is.
      // So, negate the operation to negate (and inline) the constant.
      if (IROp->Op == OP_ADD) {
        IROp->Op = OP_SUB;
      } else if (IROp->Op == OP_SUB) {
        IROp->Op = OP_ADD;
      } else if (IROp->Op == OP_ADDWITHFLAGS) {
        IROp->Op = OP_SUBWITHFLAGS;
      } else if (IROp->Op == OP_SUBWITHFLAGS) {
        IROp->Op = OP_ADDWITHFLAGS;
      }

      IREmit->SetWriteCursorBefore(CodeNode);

      // Negate the constant.
      auto NegConstant = IREmit->_Constant(-Constant2);

      // Replace the second source with the negated constant.
      IREmit->ReplaceNodeArgument(CodeNode, Op->Src2_Index, NegConstant);
    }
    break;
  }
  case OP_SUBSHIFT: {
    auto Op = IROp->C<IR::IROp_SubShift>();

    uint64_t Constant1, Constant2;
    if (IREmit->IsValueConstant(IROp->Args[0], &Constant1) && IREmit->IsValueConstant(IROp->Args[1], &Constant2) &&
        Op->Shift == IR::ShiftType::LSL) {
      // Optimize the LSL case when we know both sources are constant.
      // This is a pattern that shows up with direction flag calculations if DF was set just before the operation.
      uint64_t NewConstant = (Constant1 - (Constant2 << Op->ShiftAmount)) & getMask(IROp);
      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    }
    break;
  }
  case OP_AND: {
    uint64_t Constant1 {};
    uint64_t Constant2 {};

    if (IREmit->IsValueConstant(IROp->Args[0], &Constant1) && IREmit->IsValueConstant(IROp->Args[1], &Constant2)) {
      uint64_t NewConstant = (Constant1 & Constant2) & getMask(IROp);
      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    } else if (Constant2 == 1) {
      // happens from flag calcs
      auto val = IREmit->GetOpHeader(IROp->Args[0]);

      uint64_t Constant3;
      if (val->Op == OP_SELECT && IREmit->IsValueConstant(val->Args[2], &Constant2) && IREmit->IsValueConstant(val->Args[3], &Constant3) &&
          Constant2 == 1 && Constant3 == 0) {
        IREmit->ReplaceAllUsesWith(CodeNode, CurrentIR.GetNode(IROp->Args[0]));
      }
    } else if (IROp->Args[0].ID() == IROp->Args[1].ID()) {
      // AND with same value results in original value
      IREmit->ReplaceAllUsesWith(CodeNode, CurrentIR.GetNode(IROp->Args[0]));
    }
    break;
  }
  case OP_OR: {
    uint64_t Constant1 {};
    uint64_t Constant2 {};

    if (IREmit->IsValueConstant(IROp->Args[0], &Constant1) && IREmit->IsValueConstant(IROp->Args[1], &Constant2)) {
      uint64_t NewConstant = Constant1 | Constant2;
      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    } else if (IROp->Args[0].ID() == IROp->Args[1].ID()) {
      // OR with same value results in original value
      IREmit->ReplaceAllUsesWith(CodeNode, CurrentIR.GetNode(IROp->Args[0]));
    }
    break;
  }
  case OP_ORLSHL: {
    auto Op = IROp->CW<IR::IROp_Orlshl>();
    uint64_t Constant1 {};
    uint64_t Constant2 {};

    if (IREmit->IsValueConstant(IROp->Args[0], &Constant1) && IREmit->IsValueConstant(IROp->Args[1], &Constant2)) {
      uint64_t NewConstant = Constant1 | (Constant2 << Op->BitShift);
      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    }
    break;
  }
  case OP_ORLSHR: {
    auto Op = IROp->CW<IR::IROp_Orlshr>();
    uint64_t Constant1 {};
    uint64_t Constant2 {};

    if (IREmit->IsValueConstant(IROp->Args[0], &Constant1) && IREmit->IsValueConstant(IROp->Args[1], &Constant2)) {
      uint64_t NewConstant = Constant1 | (Constant2 >> Op->BitShift);
      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    }
    break;
  }
  case OP_XOR: {
    uint64_t Constant1 {};
    uint64_t Constant2 {};

    if (IREmit->IsValueConstant(IROp->Args[0], &Constant1) && IREmit->IsValueConstant(IROp->Args[1], &Constant2)) {
      uint64_t NewConstant = Constant1 ^ Constant2;
      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    } else if (IROp->Args[0].ID() == IROp->Args[1].ID()) {
      // XOR with same value results to zero
      IREmit->SetWriteCursor(CodeNode);
      IREmit->ReplaceAllUsesWith(CodeNode, IREmit->_Constant(0));
    } else {
      // XOR with zero results in the nonzero source
      for (unsigned i = 0; i < 2; ++i) {
        if (!IREmit->IsValueConstant(IROp->Args[i], &Constant1)) {
          continue;
        }

        if (Constant1 != 0) {
          continue;
        }

        IREmit->SetWriteCursor(CodeNode);
        OrderedNode* Arg = CurrentIR.GetNode(IROp->Args[1 - i]);
        IREmit->ReplaceAllUsesWith(CodeNode, Arg);
        break;
      }
    }
    break;
  }
  case OP_NEG: {
    uint64_t Constant {};

    if (IREmit->IsValueConstant(IROp->Args[0], &Constant)) {
      uint64_t NewConstant = -Constant;
      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    }
    break;
  }
  case OP_LSHL: {
    uint64_t Constant1 {};
    uint64_t Constant2 {};

    if (IREmit->IsValueConstant(IROp->Args[0], &Constant1) && IREmit->IsValueConstant(IROp->Args[1], &Constant2)) {
      // Shifts mask the shift amount by 63 or 31 depending on operating size;
      uint64_t ShiftMask = IROp->Size == 8 ? 63 : 31;
      uint64_t NewConstant = (Constant1 << (Constant2 & ShiftMask)) & getMask(IROp);
      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    } else if (IREmit->IsValueConstant(IROp->Args[1], &Constant2) && Constant2 == 0) {
      IREmit->SetWriteCursor(CodeNode);
      OrderedNode* Arg = CurrentIR.GetNode(IROp->Args[0]);
      IREmit->ReplaceAllUsesWith(CodeNode, Arg);
    }
    break;
  }
  case OP_LSHR: {
    uint64_t Constant1 {};
    uint64_t Constant2 {};

    if (IREmit->IsValueConstant(IROp->Args[0], &Constant1) && IREmit->IsValueConstant(IROp->Args[1], &Constant2)) {
      // Shifts mask the shift amount by 63 or 31 depending on operating size;
      uint64_t ShiftMask = IROp->Size == 8 ? 63 : 31;
      uint64_t NewConstant = (Constant1 >> (Constant2 & ShiftMask)) & getMask(IROp);
      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    } else if (IREmit->IsValueConstant(IROp->Args[1], &Constant2) && Constant2 == 0) {
      IREmit->SetWriteCursor(CodeNode);
      OrderedNode* Arg = CurrentIR.GetNode(IROp->Args[0]);
      IREmit->ReplaceAllUsesWith(CodeNode, Arg);
    }
    break;
  }
  case OP_BFE: {
    auto Op = IROp->C<IR::IROp_Bfe>();
    uint64_t Constant;

    // Is this value already BFE'd?
    if (IsBfeAlreadyDone(IREmit, Op->Src, Op->Width)) {
      IREmit->ReplaceAllUsesWith(CodeNode, CurrentIR.GetNode(Op->Src));
      break;
    }

    // Is this value already ZEXT'd?
    if (Op->lsb == 0) {
      // LoadMem, LoadMemTSO & LoadContext ZExt
      auto source = Op->Src;
      auto sourceHeader = IREmit->GetOpHeader(source);

      if (Op->Width >= (sourceHeader->Size * 8) &&
          (sourceHeader->Op == OP_LOADMEM || sourceHeader->Op == OP_LOADMEMTSO || sourceHeader->Op == OP_LOADCONTEXT)) {
        //  Load mem / load ctx zexts, no need to vmem
        IREmit->ReplaceAllUsesWith(CodeNode, CurrentIR.GetNode(source));
        break;
      }
    }

    if (IROp->Size <= 8 && IREmit->IsValueConstant(Op->Src, &Constant)) {
      uint64_t SourceMask = Op->Width == 64 ? ~0ULL : ((1ULL << Op->Width) - 1);
      SourceMask <<= Op->lsb;

      uint64_t NewConstant = (Constant & SourceMask) >> Op->lsb;
      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    } else if (IROp->Size == CurrentIR.GetOp<IROp_Header>(IROp->Args[0])->Size && Op->Width == (IROp->Size * 8) && Op->lsb == 0) {
      // A BFE that extracts all bits results in original value
      // XXX - This is broken for now - see https://github.com/FEX-Emu/FEX/issues/351
      // IREmit->ReplaceAllUsesWith(CodeNode, CurrentIR.GetNode(IROp->Args[0]));
    } else if (Op->Width == 1 && Op->lsb == 0) {
      // common from flag codegen
      auto val = IREmit->GetOpHeader(IROp->Args[0]);

      uint64_t Constant2 {};
      uint64_t Constant3 {};
      if (val->Op == OP_SELECT && IREmit->IsValueConstant(val->Args[2], &Constant2) && IREmit->IsValueConstant(val->Args[3], &Constant3) &&
          Constant2 == 1 && Constant3 == 0) {
        IREmit->ReplaceAllUsesWith(CodeNode, CurrentIR.GetNode(IROp->Args[0]));
      }
    }

    break;
  }
  case OP_SBFE: {
    auto Op = IROp->C<IR::IROp_Bfe>();
    uint64_t Constant;
    if (IREmit->IsValueConstant(Op->Src, &Constant)) {
      // SBFE of a constant can be converted to a constant.
      uint64_t SourceMask = Op->Width == 64 ? ~0ULL : ((1ULL << Op->Width) - 1);
      uint64_t DestSizeInBits = IROp->Size * 8;
      uint64_t DestMask = DestSizeInBits == 64 ? ~0ULL : ((1ULL << DestSizeInBits) - 1);
      SourceMask <<= Op->lsb;

      int64_t NewConstant = (Constant & SourceMask) >> Op->lsb;
      NewConstant <<= 64 - Op->Width;
      NewConstant >>= 64 - Op->Width;
      NewConstant &= DestMask;
      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    }
    break;
  }
  case OP_BFI: {
    auto Op = IROp->C<IR::IROp_Bfi>();
    uint64_t ConstantDest {};
    uint64_t ConstantSrc {};
    bool DestIsConstant = IREmit->IsValueConstant(IROp->Args[0], &ConstantDest);
    bool SrcIsConstant = IREmit->IsValueConstant(IROp->Args[1], &ConstantSrc);

    if (DestIsConstant && SrcIsConstant) {
      uint64_t SourceMask = Op->Width == 64 ? ~0ULL : ((1ULL << Op->Width) - 1);
      uint64_t NewConstant = ConstantDest & ~(SourceMask << Op->lsb);
      NewConstant |= (ConstantSrc & SourceMask) << Op->lsb;

      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    } else if (SrcIsConstant && HasConsecutiveBits(ConstantSrc, Op->Width)) {
      // We are trying to insert constant, if it is a bitfield of only set bits then we can orr or and it.
      IREmit->SetWriteCursor(CodeNode);
      uint64_t SourceMask = Op->Width == 64 ? ~0ULL : ((1ULL << Op->Width) - 1);
      uint64_t NewConstant = SourceMask << Op->lsb;

      if (ConstantSrc & 1) {
        auto orr = IREmit->_Or(IR::SizeToOpSize(IROp->Size), CurrentIR.GetNode(IROp->Args[0]), IREmit->_Constant(NewConstant));
        IREmit->ReplaceAllUsesWith(CodeNode, orr);
      } else {
        // We are wanting to clear the bitfield.
        auto andn = IREmit->_Andn(IR::SizeToOpSize(IROp->Size), CurrentIR.GetNode(IROp->Args[0]), IREmit->_Constant(NewConstant));
        IREmit->ReplaceAllUsesWith(CodeNode, andn);
      }
    }
    break;
  }
  case OP_MUL: {
    uint64_t Constant1 {};
    uint64_t Constant2 {};

    if (IREmit->IsValueConstant(IROp->Args[0], &Constant1) && IREmit->IsValueConstant(IROp->Args[1], &Constant2)) {
      uint64_t NewConstant = (Constant1 * Constant2) & getMask(IROp);
      IREmit->ReplaceWithConstant(CodeNode, NewConstant);
    } else if (IREmit->IsValueConstant(IROp->Args[1], &Constant2) && std::popcount(Constant2) == 1) {
      if (IROp->Size == 4 || IROp->Size == 8) {
        uint64_t amt = std::countr_zero(Constant2);
        IREmit->SetWriteCursor(CodeNode);
        auto shift = IREmit->_Lshl(IR::SizeToOpSize(IROp->Size), CurrentIR.GetNode(IROp->Args[0]), IREmit->_Constant(amt));
        IREmit->ReplaceAllUsesWith(CodeNode, shift);
      }
    }
    break;
  }

  case OP_VMOV: {
    // elim from load mem
    auto source = IROp->Args[0];
    auto sourceHeader = IREmit->GetOpHeader(source);

    if (IROp->Size >= sourceHeader->Size &&
        (sourceHeader->Op == OP_LOADMEM || sourceHeader->Op == OP_LOADMEMTSO || sourceHeader->Op == OP_LOADCONTEXT)) {
      //  Load mem / load ctx zexts, no need to vmem
      IREmit->ReplaceAllUsesWith(CodeNode, CurrentIR.GetNode(source));
    }
    break;
  }
  default: break;
  }
}

void ConstProp::ConstantInlining(IREmitter* IREmit, const IRListView& CurrentIR) {
  InlineConstantGen.clear();

  for (auto [CodeNode, IROp] : CurrentIR.GetAllCode()) {
    switch (IROp->Op) {
    case OP_LSHR:
    case OP_ASHR:
    case OP_ROR:
    case OP_LSHL: {
      uint64_t Constant2 {};
      if (IREmit->IsValueConstant(IROp->Args[1], &Constant2)) {
        IREmit->SetWriteCursor(CurrentIR.GetNode(IROp->Args[1]));

        // this shouldn't be here, but rather on the emitter themselves or the constprop transformation?
        if (IROp->Size <= 4) {
          Constant2 &= 31;
        } else {
          Constant2 &= 63;
        }

        IREmit->ReplaceNodeArgument(CodeNode, 1, CreateInlineConstant(IREmit, Constant2));
      }
      break;
    }
    case OP_ADD:
    case OP_SUB:
    case OP_ADDNZCV:
    case OP_SUBNZCV:
    case OP_ADDWITHFLAGS:
    case OP_SUBWITHFLAGS: {
      uint64_t Constant2 {};
      if (IREmit->IsValueConstant(IROp->Args[1], &Constant2)) {
        // We don't allow 8/16-bit operations to have constants, since no
        // constant would be in bounds after the JIT's 24/16 shift.
        if (IsImmAddSub(Constant2) && IROp->Size >= 4) {
          IREmit->SetWriteCursor(CurrentIR.GetNode(IROp->Args[1]));
          IREmit->ReplaceNodeArgument(CodeNode, 1, CreateInlineConstant(IREmit, Constant2));
        }
      } else if (IROp->Op == OP_SUBNZCV || IROp->Op == OP_SUBWITHFLAGS || IROp->Op == OP_SUB) {
        // TODO: Generalize this
        uint64_t Constant1 {};
        if (IREmit->IsValueConstant(IROp->Args[0], &Constant1)) {
          if (Constant1 == 0) {
            IREmit->SetWriteCursor(CurrentIR.GetNode(IROp->Args[0]));
            IREmit->ReplaceNodeArgument(CodeNode, 0, CreateInlineConstant(IREmit, 0));
          }
        }
      }

      break;
    }
    case OP_ADC:
    case OP_ADCWITHFLAGS: {
      uint64_t Constant1 {};
      if (IREmit->IsValueConstant(IROp->Args[0], &Constant1)) {
        if (Constant1 == 0) {
          IREmit->SetWriteCursor(CurrentIR.GetNode(IROp->Args[0]));
          IREmit->ReplaceNodeArgument(CodeNode, 0, CreateInlineConstant(IREmit, 0));
        }
      }

      break;
    }
    case OP_RMIFNZCV: {
      uint64_t Constant1 {};
      if (IREmit->IsValueConstant(IROp->Args[0], &Constant1)) {
        if (Constant1 == 0) {
          IREmit->SetWriteCursor(CurrentIR.GetNode(IROp->Args[0]));
          IREmit->ReplaceNodeArgument(CodeNode, 0, CreateInlineConstant(IREmit, 0));
        }
      }

      break;
    }
    case OP_CONDADDNZCV:
    case OP_CONDSUBNZCV: {
      uint64_t Constant2 {};
      if (IREmit->IsValueConstant(IROp->Args[1], &Constant2)) {
        if (IsImmAddSub(Constant2)) {
          IREmit->SetWriteCursor(CurrentIR.GetNode(IROp->Args[1]));
          IREmit->ReplaceNodeArgument(CodeNode, 1, CreateInlineConstant(IREmit, Constant2));
        }
      }

      uint64_t Constant1 {};
      if (IREmit->IsValueConstant(IROp->Args[0], &Constant1)) {
        if (Constant1 == 0) {
          IREmit->SetWriteCursor(CurrentIR.GetNode(IROp->Args[0]));
          IREmit->ReplaceNodeArgument(CodeNode, 0, CreateInlineConstant(IREmit, 0));
        }
      }
      break;
    }
    case OP_TESTNZ: {
      uint64_t Constant1 {};
      if (IREmit->IsValueConstant(IROp->Args[1], &Constant1)) {
        if (IsImmLogical(Constant1, IROp->Size * 8)) {
          IREmit->SetWriteCursor(CurrentIR.GetNode(IROp->Args[1]));
          IREmit->ReplaceNodeArgument(CodeNode, 1, CreateInlineConstant(IREmit, Constant1));
        }
      }
      break;
    }
    case OP_SELECT: {
      uint64_t Constant1 {};
      if (IREmit->IsValueConstant(IROp->Args[1], &Constant1)) {
        if (IsImmAddSub(Constant1)) {
          IREmit->SetWriteCursor(CurrentIR.GetNode(IROp->Args[1]));
          IREmit->ReplaceNodeArgument(CodeNode, 1, CreateInlineConstant(IREmit, Constant1));
        }
      }

      uint64_t AllOnes = IROp->Size == 8 ? 0xffff'ffff'ffff'ffffull : 0xffff'ffffull;

      uint64_t Constant2 {};
      uint64_t Constant3 {};
      if (IREmit->IsValueConstant(IROp->Args[2], &Constant2) && IREmit->IsValueConstant(IROp->Args[3], &Constant3) &&
          (Constant2 == 1 || Constant2 == AllOnes) && Constant3 == 0) {
        IREmit->SetWriteCursor(CurrentIR.GetNode(IROp->Args[2]));

        IREmit->ReplaceNodeArgument(CodeNode, 2, CreateInlineConstant(IREmit, Constant2));
        IREmit->ReplaceNodeArgument(CodeNode, 3, CreateInlineConstant(IREmit, Constant3));
      }

      break;
    }
    case OP_NZCVSELECT: {
      uint64_t AllOnes = IROp->Size == 8 ? 0xffff'ffff'ffff'ffffull : 0xffff'ffffull;

      // We always allow source 1 to be zero, but source 0 can only be a
      // special 1/~0 constant if source 1 is 0.
      uint64_t Constant0 {};
      uint64_t Constant1 {};
      if (IREmit->IsValueConstant(IROp->Args[1], &Constant1) && Constant1 == 0) {
        IREmit->SetWriteCursor(CurrentIR.GetNode(IROp->Args[1]));
        IREmit->ReplaceNodeArgument(CodeNode, 1, CreateInlineConstant(IREmit, Constant1));

        if (IREmit->IsValueConstant(IROp->Args[0], &Constant0) && (Constant0 == 1 || Constant0 == AllOnes)) {
          IREmit->SetWriteCursor(CurrentIR.GetNode(IROp->Args[0]));
          IREmit->ReplaceNodeArgument(CodeNode, 0, CreateInlineConstant(IREmit, Constant0));
        }
      }

      break;
    }
    case OP_CONDJUMP: {
      uint64_t Constant2 {};
      if (IREmit->IsValueConstant(IROp->Args[1], &Constant2)) {
        if (IsImmAddSub(Constant2)) {
          IREmit->SetWriteCursor(CurrentIR.GetNode(IROp->Args[1]));
          IREmit->ReplaceNodeArgument(CodeNode, 1, CreateInlineConstant(IREmit, Constant2));
        }
      }
      break;
    }
    case OP_EXITFUNCTION: {
      auto Op = IROp->C<IR::IROp_ExitFunction>();

      uint64_t Constant {};
      if (IREmit->IsValueConstant(Op->NewRIP, &Constant)) {
        IREmit->SetWriteCursor(CurrentIR.GetNode(Op->NewRIP));
        IREmit->ReplaceNodeArgument(CodeNode, 0, CreateInlineConstant(IREmit, Constant));
      } else {
        auto NewRIP = IREmit->GetOpHeader(Op->NewRIP);
        if (NewRIP->Op == OP_ENTRYPOINTOFFSET) {
          auto EO = NewRIP->C<IR::IROp_EntrypointOffset>();
          IREmit->SetWriteCursor(CurrentIR.GetNode(Op->NewRIP));

          IREmit->ReplaceNodeArgument(CodeNode, 0, IREmit->_InlineEntrypointOffset(IR::SizeToOpSize(EO->Header.Size), EO->Offset));
        }
      }
      break;
    }
    case OP_OR:
    case OP_XOR:
    case OP_AND:
    case OP_ANDWITHFLAGS:
    case OP_ANDN: {
      uint64_t Constant2 {};
      if (IREmit->IsValueConstant(IROp->Args[1], &Constant2)) {
        if (IsImmLogical(Constant2, IROp->Size * 8)) {
          IREmit->SetWriteCursor(CurrentIR.GetNode(IROp->Args[1]));
          IREmit->ReplaceNodeArgument(CodeNode, 1, CreateInlineConstant(IREmit, Constant2));
        }
      }
      break;
    }
    case OP_LOADMEM: {
      auto Op = IROp->CW<IR::IROp_LoadMem>();

      uint64_t Constant2 {};
      if (Op->OffsetType == MEM_OFFSET_SXTX && IREmit->IsValueConstant(Op->Offset, &Constant2)) {
        if (IsImmMemory(Constant2, IROp->Size)) {
          IREmit->SetWriteCursor(CurrentIR.GetNode(Op->Offset));
          IREmit->ReplaceNodeArgument(CodeNode, Op->Offset_Index, CreateInlineConstant(IREmit, Constant2));
        }
      }
      break;
    }
    case OP_STOREMEM: {
      auto Op = IROp->CW<IR::IROp_StoreMem>();

      uint64_t Constant2 {};
      if (Op->OffsetType == MEM_OFFSET_SXTX && IREmit->IsValueConstant(Op->Offset, &Constant2)) {
        if (IsImmMemory(Constant2, IROp->Size)) {
          IREmit->SetWriteCursor(CurrentIR.GetNode(Op->Offset));
          IREmit->ReplaceNodeArgument(CodeNode, Op->Offset_Index, CreateInlineConstant(IREmit, Constant2));
        }
      }
      break;
    }
    case OP_LOADMEMTSO: {
      auto Op = IROp->CW<IR::IROp_LoadMemTSO>();

      uint64_t Constant2 {};
      if (SupportsTSOImm9) {
        if (Op->OffsetType == MEM_OFFSET_SXTX && IREmit->IsValueConstant(Op->Offset, &Constant2)) {
          if (IsTSOImm9(Constant2)) {
            IREmit->SetWriteCursor(CurrentIR.GetNode(Op->Offset));
            IREmit->ReplaceNodeArgument(CodeNode, Op->Offset_Index, CreateInlineConstant(IREmit, Constant2));
          }
        }
      }
      break;
    }
    case OP_STOREMEMTSO: {
      auto Op = IROp->CW<IR::IROp_StoreMemTSO>();

      uint64_t Constant2 {};
      if (SupportsTSOImm9) {
        if (Op->OffsetType == MEM_OFFSET_SXTX && IREmit->IsValueConstant(Op->Offset, &Constant2)) {
          if (IsTSOImm9(Constant2)) {
            IREmit->SetWriteCursor(CurrentIR.GetNode(Op->Offset));
            IREmit->ReplaceNodeArgument(CodeNode, Op->Offset_Index, CreateInlineConstant(IREmit, Constant2));
          }
        }
      }
      break;
    }
    case OP_MEMCPY: {
      auto Op = IROp->CW<IR::IROp_MemCpy>();

      uint64_t Constant {};
      if (IREmit->IsValueConstant(Op->Direction, &Constant)) {
        IREmit->SetWriteCursor(CurrentIR.GetNode(Op->Direction));
        IREmit->ReplaceNodeArgument(CodeNode, Op->Direction_Index, CreateInlineConstant(IREmit, Constant));
      }
      break;
    }
    case OP_MEMSET: {
      auto Op = IROp->CW<IR::IROp_MemSet>();

      uint64_t Constant {};
      if (IREmit->IsValueConstant(Op->Direction, &Constant)) {
        IREmit->SetWriteCursor(CurrentIR.GetNode(Op->Direction));
        IREmit->ReplaceNodeArgument(CodeNode, Op->Direction_Index, CreateInlineConstant(IREmit, Constant));
      }
      break;
    }

    case OP_PREFETCH: {
      auto Op = IROp->CW<IR::IROp_Prefetch>();

      uint64_t Constant2 {};
      if (Op->OffsetType == MEM_OFFSET_SXTX && IREmit->IsValueConstant(Op->Offset, &Constant2)) {
        if (IsImmMemory(Constant2, IROp->Size)) {
          IREmit->SetWriteCursor(CurrentIR.GetNode(Op->Offset));
          IREmit->ReplaceNodeArgument(CodeNode, Op->Offset_Index, CreateInlineConstant(IREmit, Constant2));
        }
      }
      break;
    }
    default: break;
    }
  }
}

void ConstProp::Run(IREmitter* IREmit) {
  FEXCORE_PROFILE_SCOPED("PassManager::ConstProp");

  auto CurrentIR = IREmit->ViewIR();

  HandleConstantPools(IREmit, CurrentIR);

  for (auto [CodeNode, IROp] : CurrentIR.GetAllCode()) {
    ConstantPropagation(IREmit, CurrentIR, CodeNode, IROp);
  }

  if (InlineConstants) {
    ConstantInlining(IREmit, CurrentIR);
  }
}

fextl::unique_ptr<FEXCore::IR::Pass> CreateConstProp(bool InlineConstants, bool SupportsTSOImm9) {
  return fextl::make_unique<ConstProp>(InlineConstants, SupportsTSOImm9);
}
} // namespace FEXCore::IR
