/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#include "hphp/hhbbc/emit.h"

#include <vector>
#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <type_traits>

#include <folly/gen/Base.h>
#include <folly/Conv.h>
#include <folly/Optional.h>
#include <folly/Memory.h>

#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/repo-auth-type-array.h"
#include "hphp/runtime/base/repo-auth-type-codec.h"
#include "hphp/runtime/vm/bytecode.h"
#include "hphp/runtime/vm/func-emitter.h"
#include "hphp/runtime/vm/preclass-emitter.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/hhbbc/representation.h"
#include "hphp/hhbbc/cfg.h"
#include "hphp/hhbbc/unit-util.h"
#include "hphp/hhbbc/class-util.h"
#include "hphp/hhbbc/index.h"

namespace HPHP { namespace HHBBC {

TRACE_SET_MOD(hhbbc_emit);

namespace {

//////////////////////////////////////////////////////////////////////

const StaticString s_empty("");
const StaticString s_invoke("__invoke");
const StaticString s_86cinit("86cinit");

//////////////////////////////////////////////////////////////////////

struct EmitUnitState {
  explicit EmitUnitState(const Index& index) : index(index) {}

  /*
   * Access to the Index for this program.
   */
  const Index& index;

  /*
   * While emitting bytecode, we keep track of where the DefCls
   * opcodes for each class are.  The PreClass runtime structures
   * require knowing these offsets.
   */
  std::vector<Offset> defClsMap;
};

//////////////////////////////////////////////////////////////////////

php::SrcLoc srcLoc(const php::Func& func, int32_t ix) {
  return ix >= 0 ? func.unit->srcLocs[ix] : php::SrcLoc{};
}

/*
 * Order the blocks for bytecode emission.
 *
 * Rules about block order:
 *
 *   - The "primary function body" must come first.  This is all blocks
 *     that aren't part of a fault funclet.
 *
 *   - Each funclet must have all of its blocks contiguous, with the
 *     entry block first.
 *
 *   - Main entry point must be the first block.
 *
 * It is not a requirement, but we attempt to locate all the DV entry
 * points after the rest of the primary function body.  The normal
 * case for DV initializers is that each one falls through to the
 * next, with the block jumping back to the main entry point.
 */
std::vector<borrowed_ptr<php::Block>> order_blocks(const php::Func& f) {
  auto sorted = rpoSortFromMain(f);

  // Get the DV blocks, without the rest of the primary function body,
  // and then add them to the end of sorted.
  auto const dvBlocks = [&] {
    auto withDVs = rpoSortAddDVs(f);
    withDVs.erase(
      std::find(begin(withDVs), end(withDVs), sorted.front()),
      end(withDVs)
    );
    return withDVs;
  }();
  sorted.insert(end(sorted), begin(dvBlocks), end(dvBlocks));

  // This stable sort will keep the blocks only reachable from DV
  // entry points after all other main code, and move fault funclets
  // after all that.
  std::stable_sort(
    begin(sorted), end(sorted),
    [&] (borrowed_ptr<php::Block> a, borrowed_ptr<php::Block> b) {
      using T = std::underlying_type<php::Block::Section>::type;
      return static_cast<T>(a->section) < static_cast<T>(b->section);
    }
  );

  // If the first block is just a Nop, this means that there is a jump to the
  // second block from somewhere in the function. We don't want this, so we
  // change this nop to an EntryNop so it doesn't get optimized away
  if (is_single_nop(*sorted.front())) {
    sorted.front()->hhbcs.clear();
    sorted.front()->hhbcs.push_back(bc::EntryNop{});
    FTRACE(2, "      changing Nop to EntryNop in block {}\n",
           sorted.front()->id);
  }

  FTRACE(2, "      block order:{}\n",
    [&] {
      std::string ret;
      for (auto& b : sorted) {
        ret += " ";
        if (b->section != php::Block::Section::Main) {
          ret += "f";
        }
        ret += folly::to<std::string>(b->id);
      }
      return ret;
    }()
  );
  return sorted;
}

// While emitting bytecode, we learn about some metadata that will
// need to be registered in the FuncEmitter.
struct EmitBcInfo {
  struct FPI {
    Offset fpushOff;
    Offset fpiEndOff;
    int32_t fpDelta;
  };

  struct JmpFixup { Offset instrOff; Offset jmpImmedOff; };

  struct BlockInfo {
    BlockInfo()
      : offset(kInvalidOffset)
      , past(kInvalidOffset)
      , regionsToPop(0)
    {}

    // The offset of the block, if we've already emitted it.
    // Otherwise kInvalidOffset.
    Offset offset;

    // The offset past the end of this block.
    Offset past;

    // How many fault regions the jump at the end of this block is leaving.
    // 0 if there is no jump or if the jump is to the same fault region or a
    // child
    int regionsToPop;

    // When we emit a forward jump to a block we haven't seen yet, we
    // write down where the jump was so we can fix it up when we get
    // to that block.
    std::vector<JmpFixup> forwardJumps;

    // When we see a forward jump to a block, we record the stack
    // depth at the jump site here.  This is needed to track
    // currentStackDepth correctly (and we also assert all the jumps
    // have the same depth).
    folly::Optional<uint32_t> expectedStackDepth;

    // Similar to expectedStackDepth, for the fpi stack. Needed to deal with
    // terminal instructions that end an fpi region.
    folly::Optional<uint32_t> expectedFPIDepth;
  };

  std::vector<borrowed_ptr<php::Block>> blockOrder;
  uint32_t maxStackDepth;
  uint32_t maxFpiDepth;
  bool containsCalls;
  std::vector<FPI> fpiRegions;
  std::vector<BlockInfo> blockInfo;
};


typedef borrowed_ptr<php::ExnNode> ExnNodePtr;

bool handleEquivalent (ExnNodePtr eh1, ExnNodePtr eh2) {
  if (!eh1 && !eh2) return true;
  if (!eh1 || !eh2 || eh1->depth != eh2->depth) return false;

  auto entry = [](ExnNodePtr eh) {
    return match<BlockId>(eh->info,
          [] (const php::CatchRegion& c) { return c.catchEntry; },
          [] (const php::FaultRegion& f) { return f.faultEntry; });
  };

  while (entry(eh1) == entry(eh2)) {
    eh1 = eh1->parent;
    eh2 = eh2->parent;
    if (!eh1 && !eh2) return true;
  }

  return false;
};

// The common parent P of eh1 and eh2 is the deepest region such that
// eh1 and eh2 are both handle-equivalent to P or a child of P
ExnNodePtr commonParent(ExnNodePtr eh1, ExnNodePtr eh2) {
  if (!eh1 || !eh2) return nullptr;
  while (eh1->depth > eh2->depth) eh1 = eh1->parent;
  while (eh2->depth > eh1->depth) eh2 = eh2->parent;
  while (!handleEquivalent(eh1, eh2)) {
    eh1 = eh1->parent;
    eh2 = eh2->parent;
  }
  return eh1;
};

EmitBcInfo emit_bytecode(EmitUnitState& euState,
                         UnitEmitter& ue,
                         const php::Func& func) {
  EmitBcInfo ret = {};
  auto& blockInfo = ret.blockInfo;
  blockInfo.resize(func.blocks.size());

  // Track the stack depth while emitting to determine maxStackDepth.
  int32_t currentStackDepth { 0 };

  // Stack of in-progress fpi regions.
  std::vector<EmitBcInfo::FPI> fpiStack;

  // Temporary buffer for vector immediates.  (Hoisted so it's not
  // allocated in the loop.)
  std::vector<uint8_t> immVec;

  // Offset of the last emitted bytecode.
  Offset lastOff { 0 };

  bool traceBc = false;

  auto map_local = [&] (LocalId id) {
    auto const loc = func.locals[id];
    assert(!loc.killed);
    assert(loc.id <= id);
    return loc.id;
  };

  auto end_fpi = [&] (Offset off) {
    auto& fpi = fpiStack.back();
    fpi.fpiEndOff = off;
    ret.fpiRegions.push_back(fpi);
    fpiStack.pop_back();
  };

  auto set_expected_depth = [&] (EmitBcInfo::BlockInfo& info) {
    if (info.expectedStackDepth) {
      assert(*info.expectedStackDepth == currentStackDepth);
    } else {
      info.expectedStackDepth = currentStackDepth;
    }

    if (info.expectedFPIDepth) {
      assert(*info.expectedFPIDepth == fpiStack.size());
    } else {
      info.expectedFPIDepth = fpiStack.size();
    }
  };

  auto make_member_key = [&] (MKey mkey) {
    switch (mkey.mcode) {
      case MEC: case MPC:
        return MemberKey{mkey.mcode, mkey.idx};
      case MEL: case MPL:
        return MemberKey{
          mkey.mcode, static_cast<int32_t>(map_local(mkey.local))
        };
      case MET: case MPT: case MQT:
        return MemberKey{mkey.mcode, mkey.litstr};
      case MEI:
        return MemberKey{mkey.mcode, mkey.int64};
      case MW:
        return MemberKey{};
    }
    not_reached();
  };

  auto emit_inst = [&] (const Bytecode& inst) {
    auto const startOffset = ue.bcPos();
    lastOff = startOffset;

    FTRACE(4, " emit: {} -- {} @ {}\n", currentStackDepth, show(&func, inst),
           show(srcLoc(func, inst.srcLoc)));

    if (options.TraceBytecodes.count(inst.op)) traceBc = true;

    auto emit_vsa = [&] (const CompactVector<LSString>& keys) {
      auto n = keys.size();
      ue.emitInt32(n);
      for (size_t i = 0; i < n; ++i) {
        ue.emitInt32(ue.mergeLitstr(keys[i]));
      }
    };

    auto emit_branch = [&] (BlockId id) {
      auto& info = blockInfo[id];

      set_expected_depth(info);

      if (info.offset != kInvalidOffset) {
        ue.emitInt32(info.offset - startOffset);
      } else {
        info.forwardJumps.push_back({ startOffset, ue.bcPos() });
        ue.emitInt32(0);
      }
    };

    auto emit_switch = [&] (const SwitchTab& targets) {
      ue.emitInt32(targets.size());
      for (auto t : targets) emit_branch(t);
    };

    auto emit_sswitch = [&] (const SSwitchTab& targets) {
      ue.emitInt32(targets.size());
      for (size_t i = 0; i < targets.size() - 1; ++i) {
        ue.emitInt32(ue.mergeLitstr(targets[i].first));
        emit_branch(targets[i].second);
      }
      ue.emitInt32(-1);
      emit_branch(targets[targets.size() - 1].second);
    };

    auto emit_itertab = [&] (const IterTab& iterTab) {
      ue.emitInt32(iterTab.size());
      for (auto& kv : iterTab) {
        ue.emitInt32(kv.first);
        ue.emitInt32(kv.second);
      }
    };

    auto emit_srcloc = [&] {
      auto const sl = srcLoc(func, inst.srcLoc);
      if (!sl.isValid()) return;
      Location::Range loc(sl.start.line, sl.start.col,
                          sl.past.line, sl.past.col);
      ue.recordSourceLocation(loc, startOffset);
    };

    auto pop = [&] (int32_t n) {
      currentStackDepth -= n;
      assert(currentStackDepth >= 0);
    };
    auto push = [&] (int32_t n) {
      currentStackDepth += n;
      if (currentStackDepth > ret.maxStackDepth) {
        ret.maxStackDepth = currentStackDepth;
      }
    };

    auto fpush = [&] {
      fpiStack.push_back({startOffset, kInvalidOffset, currentStackDepth});
      ret.maxFpiDepth = std::max<uint32_t>(ret.maxFpiDepth, fpiStack.size());
    };

    auto fcall = [&] {
      end_fpi(startOffset);
    };

    auto ret_assert = [&] { assert(currentStackDepth == 1); };

    auto defcls = [&] {
      auto const id = inst.DefCls.arg1;
      always_assert(euState.defClsMap[id] == kInvalidOffset);
      euState.defClsMap[id] = startOffset;
    };

    auto defclsnop = [&] {
      auto const id = inst.DefCls.arg1;
      always_assert(euState.defClsMap[id] == kInvalidOffset);
      euState.defClsMap[id] = startOffset;
    };

    auto emit_lar = [&](const LocalRange& range) {
      always_assert(range.first + range.restCount < func.locals.size());
      auto const first = map_local(range.first);
      DEBUG_ONLY auto const last = map_local(range.first + range.restCount);
      assert(last - first == range.restCount);
      encodeLocalRange(ue, HPHP::LocalRange{first, range.restCount});
    };

#define IMM_BLA(n)     emit_switch(data.targets);
#define IMM_SLA(n)     emit_sswitch(data.targets);
#define IMM_ILA(n)     emit_itertab(data.iterTab);
#define IMM_IVA(n)     ue.emitIVA(data.arg##n);
#define IMM_I64A(n)    ue.emitInt64(data.arg##n);
#define IMM_LA(n)      ue.emitIVA(map_local(data.loc##n));
#define IMM_IA(n)      ue.emitIVA(data.iter##n);
#define IMM_CAR(n)     ue.emitIVA(data.slot);
#define IMM_CAW(n)     ue.emitIVA(data.slot);
#define IMM_DA(n)      ue.emitDouble(data.dbl##n);
#define IMM_SA(n)      ue.emitInt32(ue.mergeLitstr(data.str##n));
#define IMM_RATA(n)    encodeRAT(ue, data.rat);
#define IMM_AA(n)      ue.emitInt32(ue.mergeArray(data.arr##n));
#define IMM_OA_IMPL(n) ue.emitByte(static_cast<uint8_t>(data.subop##n));
#define IMM_OA(type)   IMM_OA_IMPL
#define IMM_BA(n)      emit_branch(data.target);
#define IMM_VSA(n)     emit_vsa(data.keys);
#define IMM_KA(n)      encode_member_key(make_member_key(data.mkey), ue);
#define IMM_LAR(n)     emit_lar(data.locrange);

#define IMM_NA
#define IMM_ONE(x)           IMM_##x(1)
#define IMM_TWO(x, y)        IMM_##x(1);         IMM_##y(2);
#define IMM_THREE(x, y, z)   IMM_TWO(x, y);      IMM_##z(3);
#define IMM_FOUR(x, y, z, n) IMM_THREE(x, y, z); IMM_##n(4);

#define POP_NOV
#define POP_ONE(x)            pop(1);
#define POP_TWO(x, y)         pop(2);
#define POP_THREE(x, y, z)    pop(3);

#define POP_MFINAL     pop(data.arg1);
#define POP_F_MFINAL   pop(data.arg2);
#define POP_C_MFINAL   pop(1); pop(data.arg1);
#define POP_V_MFINAL   POP_C_MFINAL
#define POP_CMANY      pop(data.arg##1);
#define POP_SMANY      pop(data.keys.size());
#define POP_FMANY      pop(data.arg##1);
#define POP_CVUMANY    pop(data.arg##1);

#define PUSH_NOV
#define PUSH_ONE(x)            push(1);
#define PUSH_TWO(x, y)         push(2);
#define PUSH_THREE(x, y, z)    push(3);
#define PUSH_INS_1(x)          push(1);

#define O(opcode, imms, inputs, outputs, flags)                   \
    auto emit_##opcode = [&] (const bc::opcode& data) {           \
      if (Op::opcode == Op::DefCls)    defcls();                  \
      if (Op::opcode == Op::DefClsNop) defclsnop();               \
      if (isRet(Op::opcode))           ret_assert();              \
      ue.emitOp(Op::opcode);                                      \
      POP_##inputs                                                \
      PUSH_##outputs                                              \
      IMM_##imms                                                  \
      if (isFPush(Op::opcode))     fpush();                       \
      if (isFCallStar(Op::opcode)) fcall();                       \
      if (flags & TF) currentStackDepth = 0;                      \
      if (Op::opcode == Op::FCall || Op::opcode == Op::FCallD) {  \
        ret.containsCalls = true;                                 \
      }                                                           \
      emit_srcloc();                                              \
    };

    OPCODES

#undef O

#undef IMM_MA
#undef IMM_BLA
#undef IMM_SLA
#undef IMM_ILA
#undef IMM_IVA
#undef IMM_I64A
#undef IMM_LA
#undef IMM_IA
#undef IMM_CAR
#undef IMM_CAW
#undef IMM_DA
#undef IMM_SA
#undef IMM_RATA
#undef IMM_AA
#undef IMM_BA
#undef IMM_OA_IMPL
#undef IMM_OA
#undef IMM_VSA
#undef IMM_KA
#undef IMM_LAR

#undef IMM_NA
#undef IMM_ONE
#undef IMM_TWO
#undef IMM_THREE
#undef IMM_FOUR

#undef POP_NOV
#undef POP_ONE
#undef POP_TWO
#undef POP_THREE

#undef POP_CMANY
#undef POP_SMANY
#undef POP_FMANY
#undef POP_CVUMANY
#undef POP_MFINAL
#undef POP_F_MFINAL
#undef POP_C_MFINAL
#undef POP_V_MFINAL

#undef PUSH_NOV
#undef PUSH_ONE
#undef PUSH_TWO
#undef PUSH_THREE
#undef PUSH_INS_1

#define O(opcode, ...)                                        \
    case Op::opcode:                                          \
      if (Op::opcode != Op::Nop) emit_##opcode(inst.opcode);  \
      break;
    switch (inst.op) { OPCODES }
#undef O
  };

  ret.blockOrder        = order_blocks(func);
  auto blockIt          = begin(ret.blockOrder);
  auto const endBlockIt = end(ret.blockOrder);
  for (; blockIt != endBlockIt; ++blockIt) {
    auto& b = *blockIt;
    auto& info = blockInfo[b->id];
    info.offset = ue.bcPos();
    FTRACE(2, "      block {}: {}\n", b->id, info.offset);

    for (auto& fixup : info.forwardJumps) {
      ue.emitInt32(info.offset - fixup.instrOff, fixup.jmpImmedOff);
    }

    if (!info.expectedStackDepth) {
      // unreachable, or entry block
      info.expectedStackDepth = 0;
    }

    currentStackDepth = *info.expectedStackDepth;

    if (!info.expectedFPIDepth) {
      // unreachable, or an entry block
      info.expectedFPIDepth = 0;
    }

    // deal with fpiRegions that were ended by terminal instructions
    assert(*info.expectedFPIDepth <= fpiStack.size());
    while (*info.expectedFPIDepth < fpiStack.size()) end_fpi(lastOff);

    for (auto& inst : b->hhbcs) emit_inst(inst);

    info.past = ue.bcPos();

    if (b->fallthrough != NoBlockId) {
      set_expected_depth(blockInfo[b->fallthrough]);
      if (std::next(blockIt) == endBlockIt ||
          blockIt[1]->id != b->fallthrough) {
        if (b->fallthroughNS) {
          emit_inst(bc::JmpNS { b->fallthrough });
        } else {
          emit_inst(bc::Jmp { b->fallthrough });
        }

        auto parent = commonParent(func.blocks[b->fallthrough]->exnNode,
                                   b->exnNode);
        // If we are in an exn region we pop from the current region to the
        // common parent. If the common parent is null, we pop all regions
        info.regionsToPop = b->exnNode ?
                            b->exnNode->depth - (parent ? parent->depth : 0) :
                            0;
        assert(info.regionsToPop >= 0);
        FTRACE(4, "      popped fault regions: {}\n", info.regionsToPop);
      }
    }

    if (b->factoredExits.size()) {
      FTRACE(4, "      factored:");
      for (auto DEBUG_ONLY id : b->factoredExits) FTRACE(4, " {}", id);
      FTRACE(4, "\n");
    }
    if (b->fallthrough != NoBlockId) {
      FTRACE(4, "      fallthrough: {}\n", b->fallthrough);
    }
    FTRACE(2, "      block {} end: {}\n", b->id, info.past);
  }

  while (fpiStack.size()) end_fpi(lastOff);

  if (traceBc) {
    FTRACE(0, "TraceBytecode (emit): {}::{} in {}\n",
           func.cls ? func.cls->name->data() : "",
           func.name, func.unit->filename);
  }

  return ret;
}

void emit_locals_and_params(FuncEmitter& fe,
                            const php::Func& func,
                            const EmitBcInfo& info) {
  Id id = 0;

  for (auto& loc : func.locals) {
    if (loc.id < func.params.size()) {
      assert(!loc.killed);
      auto& param = func.params[id];
      FuncEmitter::ParamInfo pinfo;
      pinfo.defaultValue = param.defaultValue;
      pinfo.typeConstraint = param.typeConstraint;
      pinfo.userType = param.userTypeConstraint;
      pinfo.phpCode = param.phpCode;
      pinfo.userAttributes = param.userAttributes;
      pinfo.builtinType = param.builtinType;
      pinfo.byRef = param.byRef;
      pinfo.variadic = param.isVariadic;
      fe.appendParam(func.locals[id].name, pinfo);
      auto const dv = param.dvEntryPoint;
      if (dv != NoBlockId) {
        fe.params[id].funcletOff = info.blockInfo[dv].offset;
      }
      ++id;
    } else if (!loc.killed) {
      if (loc.name) {
        fe.allocVarId(loc.name);
        assert(fe.lookupVarId(loc.name) == id);
        assert(loc.id == id);
      } else {
        fe.allocUnnamedLocal();
      }
      ++id;
    }
  }
  assert(fe.numLocals() == id);
  fe.setNumIterators(func.numIters);
  fe.setNumClsRefSlots(func.numClsRefSlots);

  for (auto& sv : func.staticLocals) {
    fe.staticVars.push_back(Func::SVInfo {sv.name});
  }
}

struct EHRegion {
  borrowed_ptr<const php::ExnNode> node;
  borrowed_ptr<EHRegion> parent;
  Offset start;
  Offset past;
};

template<class BlockInfo, class ParentIndexMap>
void emit_eh_region(FuncEmitter& fe,
                    borrowed_ptr<const EHRegion> region,
                    const BlockInfo& blockInfo,
                    ParentIndexMap& parentIndexMap) {
  FTRACE(2,  "    func {}: ExnNode {}\n", fe.name, region->node->id);
  // A region on a single empty block.
  if (region->start == region->past) {
    FTRACE(2, "    Skipping\n");
    return;
  }

  FTRACE(2, "    Process @ {}-{}\n", region->start, region->past);

  auto& eh = fe.addEHEnt();
  eh.m_base = region->start;
  eh.m_past = region->past;
  assert(eh.m_past >= eh.m_base);
  assert(eh.m_base != kInvalidOffset && eh.m_past != kInvalidOffset);

  if (region->parent) {
    auto parentIt = parentIndexMap.find(region->parent);
    assert(parentIt != end(parentIndexMap));
    eh.m_parentIndex = parentIt->second;
  } else {
    eh.m_parentIndex = -1;
  }
  parentIndexMap[region] = fe.ehtab.size() - 1;

  match<void>(
    region->node->info,
    [&] (const php::CatchRegion& cr) {
      eh.m_type = EHEnt::Type::Catch;
      eh.m_handler = blockInfo[cr.catchEntry].offset;
      eh.m_end = kInvalidOffset;
      eh.m_iterId = cr.iterId;
      eh.m_itRef = cr.itRef;
    },
    [&] (const php::FaultRegion& fr) {
      eh.m_type = EHEnt::Type::Fault;
      eh.m_handler = blockInfo[fr.faultEntry].offset;
      eh.m_end = kInvalidOffset;
      eh.m_iterId = fr.iterId;
      eh.m_itRef = fr.itRef;
    }
  );
}

void exn_path(std::vector<const php::ExnNode*>& ret, const php::ExnNode* n) {
  if (!n) return;
  exn_path(ret, n->parent);
  ret.push_back(n);
}

// Return the count of shared elements in the front of two forward
// ranges.
template<class ForwardRange1, class ForwardRange2>
size_t shared_prefix(ForwardRange1& r1, ForwardRange2& r2) {
  auto r1it = begin(r1);
  auto r2it = begin(r2);
  auto const r1end = end(r1);
  auto const r2end = end(r2);
  auto ret = size_t{0};
  while (r1it != r1end && r2it != r2end && *r1it == *r2it) {
    ++ret; ++r1it; ++r2it;
  }
  return ret;
}

/*
 * Traverse the actual block layout, and find out the intervals for
 * each exception region in the tree.
 *
 * The basic idea here is that we haven't constrained block layout
 * based on the exception tree, but adjacent blocks are still
 * reasonably likely to have the same ExnNode.  Try to coalesce the EH
 * regions we create for in those cases.
 */
void emit_ehent_tree(FuncEmitter& fe, const php::Func& /*func*/,
                     const EmitBcInfo& info) {
  std::map<
    borrowed_ptr<const php::ExnNode>,
    std::vector<std::unique_ptr<EHRegion>>
  > exnMap;

  /*
   * While walking over the blocks in layout order, we track the set
   * of "active" exnNodes.  This are a list of exnNodes that inherit
   * from each other.  When a new active node is pushed, begin an
   * EHEnt, and when it's popped, it's done.
   */
  std::vector<borrowed_ptr<const php::ExnNode>> activeList;

  auto pop_active = [&] (Offset past) {
    auto p = activeList.back();
    activeList.pop_back();
    exnMap[p].back()->past = past;
  };

  auto push_active = [&] (const php::ExnNode* p, Offset start) {
    auto const parent = activeList.empty()
      ? nullptr
      : borrow(exnMap[activeList.back()].back());
    exnMap[p].push_back(
      std::make_unique<EHRegion>(
        EHRegion { p, parent, start, kInvalidOffset }
      )
    );
    activeList.push_back(p);
  };

  /*
   * Walk over the blocks, and compare the new block's exnNode path to
   * the active one.  Find the least common ancestor of the two paths,
   * then modify the active list by popping and then pushing nodes to
   * set it to the new block's path.
   */
  for (auto& b : info.blockOrder) {
    auto const offset = info.blockInfo[b->id].offset;

    if (!b->exnNode) {
      while (!activeList.empty()) pop_active(offset);
      continue;
    }

    std::vector<borrowed_ptr<const php::ExnNode>> current;
    exn_path(current, b->exnNode);

    auto const prefix = shared_prefix(current, activeList);
    for (size_t i = prefix, sz = activeList.size(); i < sz; ++i) {
      pop_active(offset);
    }
    for (size_t i = prefix, sz = current.size(); i < sz; ++i) {
      push_active(current[i], offset);
    }

    for (int i = 0; i < info.blockInfo[b->id].regionsToPop; i++) {
      // If the block ended in a jump out of the fault region, this effectively
      // ends all fault regions deeper than the one we are jumping to
      pop_active(info.blockInfo[b->id].past);
    }

    if (debug && !activeList.empty()) {
      current.clear();
      exn_path(current, activeList.back());
      assert(current == activeList);
    }
  }

  while (!activeList.empty()) {
    pop_active(info.blockInfo[info.blockOrder.back()->id].past);
  }

  /*
   * We've created all our regions, but we need to sort them instead
   * of trying to get the UnitEmitter to do it.
   *
   * The UnitEmitter expects EH regions that look a certain way
   * (basically the way emitter.cpp likes them).  There are some rules
   * about the order it needs to have at runtime, which we set up
   * here.
   *
   * Essentially, an entry a is less than an entry b iff:
   *
   *   - a starts before b
   *   - a starts at the same place, but encloses b entirely
   *   - a has the same extents as b, but is a parent of b
   */
  std::vector<borrowed_ptr<EHRegion>> regions;
  for (auto& mapEnt : exnMap) {
    for (auto& region : mapEnt.second) {
      regions.push_back(borrow(region));
    }
  }
  std::sort(
    begin(regions), end(regions),
    [&] (borrowed_ptr<const EHRegion> a, borrowed_ptr<const EHRegion> b) {
      if (a == b) return false;
      if (a->start == b->start) {
        if (a->past == b->past) {
          // When regions exactly overlap, the parent is less than the
          // child.
          for (auto p = b->parent; p != nullptr; p = p->parent) {
            if (p == a) return true;
          }
          // If a is not a parent of b, and they have the same region;
          // then b better be a parent of a.
          if (debug) {
            auto p = a->parent;
            for (; p != b && p != nullptr; p = p->parent) continue;
            assert(p == b);
          }
          return false;
        }
        return a->past > b->past;
      }
      return a->start < b->start;
    }
  );

  std::map<borrowed_ptr<const EHRegion>,uint32_t> parentIndexMap;
  for (auto& r : regions) {
    emit_eh_region(fe, r, info.blockInfo, parentIndexMap);
  }
  fe.setEHTabIsSorted();
}

void merge_repo_auth_type(UnitEmitter& ue, RepoAuthType rat) {
  using T = RepoAuthType::Tag;

  switch (rat.tag()) {
  case T::OptBool:
  case T::OptInt:
  case T::OptSStr:
  case T::OptStr:
  case T::OptDbl:
  case T::OptRes:
  case T::OptObj:
  case T::OptUncArrKey:
  case T::OptArrKey:
  case T::Null:
  case T::Cell:
  case T::Ref:
  case T::InitUnc:
  case T::Unc:
  case T::UncArrKey:
  case T::ArrKey:
  case T::InitCell:
  case T::InitGen:
  case T::Gen:
  case T::Uninit:
  case T::InitNull:
  case T::Bool:
  case T::Int:
  case T::Dbl:
  case T::Res:
  case T::SStr:
  case T::Str:
  case T::Obj:
    return;

  case T::OptSArr:
  case T::OptArr:
  case T::SArr:
  case T::Arr:
  case T::OptSVec:
  case T::OptVec:
  case T::SVec:
  case T::Vec:
  case T::OptSDict:
  case T::OptDict:
  case T::SDict:
  case T::Dict:
  case T::OptSKeyset:
  case T::OptKeyset:
  case T::SKeyset:
  case T::Keyset:
    // We don't need to merge the litstrs in the array, because rats
    // in arrays in the array type table must be using global litstr
    // ids.  (As the array type table itself is not associated with
    // any unit.)
    return;

  case T::OptSubObj:
  case T::OptExactObj:
  case T::SubObj:
  case T::ExactObj:
    ue.mergeLitstr(rat.clsName());
    return;
  }
}

void emit_finish_func(EmitUnitState& state,
                      const php::Func& func,
                      FuncEmitter& fe,
                      const EmitBcInfo& info) {
  if (info.containsCalls) fe.containsCalls = true;;

  for (auto& fpi : info.fpiRegions) {
    auto& e = fe.addFPIEnt();
    e.m_fpushOff = fpi.fpushOff;
    e.m_fpiEndOff = fpi.fpiEndOff;
    e.m_fpOff    = fpi.fpDelta;
  }

  emit_locals_and_params(fe, func, info);
  emit_ehent_tree(fe, func, info);

  fe.userAttributes = func.userAttributes;
  fe.retUserType = func.returnUserType;
  fe.originalFilename = func.originalFilename;
  fe.isClosureBody = func.isClosureBody;
  fe.isAsync = func.isAsync;
  fe.isGenerator = func.isGenerator;
  fe.isPairGenerator = func.isPairGenerator;
  fe.isNative = func.nativeInfo != nullptr;
  fe.isMemoizeWrapper = func.isMemoizeWrapper;

  auto const retTy = state.index.lookup_return_type_raw(&func);
  if (!retTy.subtypeOf(TBottom)) {
    auto const rat = make_repo_type(*state.index.array_table_builder(), retTy);
    merge_repo_auth_type(fe.ue(), rat);
    fe.repoReturnType = rat;
  }

  if (is_specialized_wait_handle(retTy)) {
    auto const awaitedTy = wait_handle_inner(retTy);
    if (!awaitedTy.subtypeOf(TBottom)) {
      auto const rat = make_repo_type(
        *state.index.array_table_builder(),
        awaitedTy
      );
      merge_repo_auth_type(fe.ue(), rat);
      fe.repoAwaitedReturnType = rat;
    }
  }

  if (func.nativeInfo) {
    fe.hniReturnType = func.nativeInfo->returnType;
    fe.dynCallWrapperId = func.nativeInfo->dynCallWrapperId;
  }
  fe.retTypeConstraint = func.retTypeConstraint;

  fe.maxStackCells = info.maxStackDepth +
                     fe.numLocals() +
                     fe.numIterators() * kNumIterCells +
                     clsRefCountToCells(fe.numClsRefSlots()) +
                     info.maxFpiDepth * kNumActRecCells;

  fe.finish(fe.ue().bcPos(), false /* load */);
  fe.ue().recordFunction(&fe);
}

void emit_init_func(FuncEmitter& fe, const php::Func& func) {
  Id id = 0;

  for (auto& loc : const_cast<php::Func&>(func).locals) {
    if (loc.killed) {
      // make sure its out of range, in case someone tries to read it.
      loc.id = INT_MAX;
    } else {
      loc.id = id++;
    }
  }

  fe.init(
    std::get<0>(func.srcInfo.loc),
    std::get<1>(func.srcInfo.loc),
    fe.ue().bcPos(),
    func.attrs,
    func.top,
    func.srcInfo.docComment
  );
}

void emit_func(EmitUnitState& state, UnitEmitter& ue, const php::Func& func) {
  FTRACE(2,  "    func {}\n", func.name->data());
  auto const fe = ue.newFuncEmitter(func.name);
  emit_init_func(*fe, func);
  auto const info = emit_bytecode(state, ue, func);
  emit_finish_func(state, func, *fe, info);
}

void emit_pseudomain(EmitUnitState& state,
                     UnitEmitter& ue,
                     const php::Unit& unit) {
  FTRACE(2,  "    pseudomain\n");
  auto& pm = *unit.pseudomain;
  ue.initMain(std::get<0>(pm.srcInfo.loc),
              std::get<1>(pm.srcInfo.loc));
  auto const fe = ue.getMain();
  auto const info = emit_bytecode(state, ue, pm);
  emit_finish_func(state, pm, *fe, info);
}

void emit_class(EmitUnitState& state,
                UnitEmitter& ue,
                const php::Class& cls) {
  FTRACE(2, "    class: {}\n", cls.name->data());
  auto const pce = ue.newPreClassEmitter(
    cls.name->toCppString(),
    cls.hoistability
  );
  pce->init(
    std::get<0>(cls.srcInfo.loc),
    std::get<1>(cls.srcInfo.loc),
    ue.bcPos(),
    cls.attrs,
    cls.parentName ? cls.parentName : s_empty.get(),
    cls.srcInfo.docComment
  );
  pce->setUserAttributes(cls.userAttributes);

  for (auto& x : cls.interfaceNames)     pce->addInterface(x);
  for (auto& x : cls.usedTraitNames)     pce->addUsedTrait(x);
  for (auto& x : cls.requirements)       pce->addClassRequirement(x);
  for (auto& x : cls.traitPrecRules)     pce->addTraitPrecRule(x);
  for (auto& x : cls.traitAliasRules)    pce->addTraitAliasRule(x);
  pce->setNumDeclMethods(cls.numDeclMethods);

  pce->setIfaceVtableSlot(state.index.lookup_iface_vtable_slot(&cls));

  bool needs86cinit = false;

  for (auto& cconst : cls.constants) {
    if (!cconst.val.hasValue()) {
      pce->addAbstractConstant(
        cconst.name,
        cconst.typeConstraint,
        cconst.isTypeconst
      );
    } else {
      needs86cinit |= cconst.val->m_type == KindOfUninit;

      pce->addConstant(
        cconst.name,
        cconst.typeConstraint,
        &cconst.val.value(),
        cconst.phpCode,
        cconst.isTypeconst
      );
    }
  }

  for (auto& m : cls.methods) {
    if (!needs86cinit && m->name == s_86cinit.get()) continue;
    FTRACE(2, "    method: {}\n", m->name->data());
    auto const fe = ue.newMethodEmitter(m->name, pce);
    emit_init_func(*fe, *m);
    pce->addMethod(fe);
    auto const info = emit_bytecode(state, ue, *m);
    emit_finish_func(state, *m, *fe, info);
  }

  std::vector<Type> useVars;
  if (is_closure(cls)) {
    auto f = find_method(&cls, s_invoke.get());
    useVars = state.index.lookup_closure_use_vars(f);
  }
  auto uvIt = useVars.begin();

  auto const privateProps   = state.index.lookup_private_props(&cls);
  auto const privateStatics = state.index.lookup_private_statics(&cls);
  for (auto& prop : cls.properties) {
    auto const makeRat = [&] (const Type& ty) -> RepoAuthType {
      if (ty.couldBe(TCls)) {
        return RepoAuthType{};
      }
      auto const rat = make_repo_type(*state.index.array_table_builder(), ty);
      merge_repo_auth_type(ue, rat);
      return rat;
    };

    auto const privPropTy = [&] (const PropState& ps) -> Type {
      if (is_closure(cls)) {
        // For closures use variables will be the first properties of the
        // closure object, in declaration order
        if (uvIt != useVars.end()) return *uvIt++;
        return Type{};
      }

      auto it = ps.find(prop.name);
      if (it == end(ps)) return Type{};
      return it->second;
    };

    Type propTy;
    auto const attrs = prop.attrs;
    if (attrs & AttrPrivate) {
      propTy = privPropTy((attrs & AttrStatic) ? privateStatics : privateProps);
    } else if ((attrs & AttrPublic) && (attrs & AttrStatic)) {
      propTy = state.index.lookup_public_static(&cls, prop.name);
    }

    pce->addProperty(
      prop.name,
      prop.attrs,
      prop.typeConstraint,
      prop.docComment,
      &prop.val,
      makeRat(propTy)
    );
  }
  assert(uvIt == useVars.end());

  pce->setEnumBaseTy(cls.enumBaseTy);
}

void emit_typealias(UnitEmitter& ue, const php::TypeAlias& alias) {
  auto const id = ue.addTypeAlias(alias);
  ue.pushMergeableTypeAlias(HPHP::Unit::MergeKind::TypeAlias, id);
}

//////////////////////////////////////////////////////////////////////

}

std::unique_ptr<UnitEmitter> emit_unit(const Index& index,
                                       const php::Unit& unit) {
  auto const is_systemlib = is_systemlib_part(unit);
  Trace::Bump bumper{Trace::hhbbc_emit, kSystemLibBump, is_systemlib};

  auto ue = std::make_unique<UnitEmitter>(unit.md5);
  FTRACE(1, "  unit {}\n", unit.filename->data());
  ue->m_filepath = unit.filename;
  ue->m_preloadPriority = unit.preloadPriority;
  ue->m_isHHFile = unit.isHHFile;
  ue->m_useStrictTypes = unit.useStrictTypes;
  ue->m_useStrictTypesForBuiltins = unit.useStrictTypesForBuiltins;

  EmitUnitState state { index };
  state.defClsMap.resize(unit.classes.size(), kInvalidOffset);

  /*
   * Unfortunate special case for Systemlib units.
   *
   * We need to ensure these units end up mergeOnly, at runtime there
   * are things that assume this (right now no other HHBBC units end
   * up being merge only, because of the returnSeen stuff below).
   *
   * (Merge-only-ness provides no measurable perf win in repo mode now
   * that we have persistent classes, so we're not too worried about
   * this.)
   */
  if (is_systemlib) {
    ue->m_mergeOnly = true;
    auto const tv = make_tv<KindOfInt64>(1);
    ue->m_mainReturn = tv;
  } else {
    /*
     * TODO(#3017265): UnitEmitter is very coupled to emitter.cpp, and
     * expects classes and things to be added in an order that isn't
     * quite clear.  If you don't set returnSeen things relating to
     * hoistability break.
     */
    ue->m_returnSeen = true;
  }

  emit_pseudomain(state, *ue, unit);
  for (auto& c : unit.classes)     emit_class(state, *ue, *c);
  for (auto& f : unit.funcs)       emit_func(state, *ue, *f);
  for (auto& t : unit.typeAliases) emit_typealias(*ue, *t);

  for (size_t id = 0; id < unit.classes.size(); ++id) {
    // We may not have a DefCls PC if we're a closure, or a
    // non-top-level class declaration is DCE'd.
    if (state.defClsMap[id] != kInvalidOffset) {
      ue->pce(id)->setOffset(state.defClsMap[id]);
    }
  }

  return ue;
}

//////////////////////////////////////////////////////////////////////

}}
