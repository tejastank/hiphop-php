/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
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
#include "dce.h"
#include "ir.h"
#include "simplifier.h"
#include <util/trace.h>


namespace HPHP {
namespace VM {
namespace JIT {


static const HPHP::Trace::Module TRACEMOD = HPHP::Trace::hhir;

static const int Essential[] = {
#define OPC(name, hasDst, canCSE, essential, effects, native, consRef,  \
            prodRef, mayModRefs, rematerializable, error)               \
  essential,
  IR_OPCODES
  #undef OPC
};


/*
 * Dead code elimination
 */
bool isEssential(IRInstruction* inst) {
  if (inst->getOpcode() == DecRefNZ) {
    // If the source of a DecRefNZ is not an IncRef, mark it as essential
    // because we won't remove its source as well as itself.
    // If the ref count optimization is turned off, mark all DecRefNZ as
    // essential.
    if (!RuntimeOption::EvalHHIREnableRefCountOpt ||
        inst->getSrc(0)->getInstruction()->getOpcode() != IncRef) {
      return true;
    }
  }
  if (inst->isControlFlowInstruction() && inst->getOpcode() != LdCls) {
    return true;
  }
  return Essential[inst->getOpcode()];
}

bool instructionIsMarkedDead(const IRInstruction* inst) {
  return inst->getId() == DEAD;
}

void removeDeadInstructions(Trace* trace) {
  trace->getInstructionList().remove_if(instructionIsMarkedDead);
}

void initInstructions(Trace* trace, IRInstruction::List& wl) {
  IRInstruction::List instructions = trace->getInstructionList();
  IRInstruction::Iterator it;
  bool unreachable = false;
  TRACE(5, "DCE:vvvvvvvvvvvvvvvvvvvv\n");
  for (it = instructions.begin(); it != instructions.end(); it++) {
    IRInstruction* inst = *it;
    ASSERT(inst->getParent() == trace);
    Simplifier::copyProp(inst);
    Opcode opc = inst->getOpcode();
    Type::Tag type = inst->getType();
    if ((opc == LdStack && (type == Type::Gen || type == Type::Cell))
        || (opc == LdLoc && type == Type::Gen)
        || (opc == LdRefNR && type == Type::Cell)) {
      // LdStack and LdLoc instructions that produce generic types
      // and LdStack instruction that produce Cell types will not
      // generate guards, so remove the label from this instruction so
      // that its no longer an essential control-flow instruction
      inst->setLabel(NULL);
    }
    // decref of anything that isn't ref counted is a nop
    if ((opc == DecRef || opc == DecRefNZ) && !isRefCounted(inst->getSrc(0))) {
      inst->setId(DEAD);
      continue;
    }
    if (!unreachable && inst->isControlFlowInstruction()) {
      // mark the destination label so that the destination trace
      // is marked reachable
      inst->getLabel()->setId(LIVE);
    }
    if (!unreachable && isEssential(inst)) {
      inst->setId(LIVE);
      wl.push_back(inst);
    } else {
      {
        std::ostringstream ss1;
        inst->printSrcs(ss1);
        TRACE(5, "DCE: %s\n", ss1.str().c_str());
        std::ostringstream ss2;
        inst->print(ss2);
        TRACE(5, "DCE: %s\n", ss2.str().c_str());
      }
      inst->setId(DEAD);
    }
    if (inst->getOpcode() == Jmp_) {
      unreachable = true;
    }
  }
  TRACE(5, "DCE:^^^^^^^^^^^^^^^^^^^^\n");
}

// Perform the following transformations:
// 1) Change all unconsumed IncRefs to Mov.
// 2) Mark a conditionally dead DecRefNZ as live if its corresponding IncRef
//    cannot be eliminated.
void optimizeRefCount(Trace* trace) {
  IRInstruction::List& instList = trace->getInstructionList();
  for (IRInstruction::Iterator it = instList.begin();
       it != instList.end();
       ++it) {
    IRInstruction* inst = *it;
    if (inst->getOpcode() == IncRef &&
        inst->getId() != REFCOUNT_CONSUMED &&
        inst->getId() != REFCOUNT_CONSUMED_OFF_TRACE) {
      inst->setOpcode(Mov);
      inst->setId(DEAD);
    }
    if (inst->getOpcode() == DecRefNZ) {
      IRInstruction* srcInst = inst->getSrc(0)->getInstruction();
      if (srcInst->getId() == REFCOUNT_CONSUMED ||
          srcInst->getId() == REFCOUNT_CONSUMED_OFF_TRACE) {
        inst->setId(LIVE);
      }
    }
    // Do copyProp at last. When processing DecRefNZs, we still need to look at
    // its source which should not be trampled over.
    Simplifier::copyProp(inst);
  }
}

// Sink IncRefs consumed off trace.
// When <trace> is an exit trace, <toSink> contains all live IncRefs in the
// main trace that are consumed off trace.
void sinkIncRefs(Trace* trace,
                 IRFactory* irFactory,
                 IRInstruction::List& toSink) {
  IRInstruction::List& instList = trace->getInstructionList();
  IRInstruction::Iterator it;

  std::map<SSATmp*, SSATmp*> sunkTmps;
  if (!trace->isMain()) {
    // Sink REFCOUNT_CONSUMED_OFF_TRACE IncRefs before the first non-label
    // instruction, and create a mapping between the original tmps to the sunk
    // tmps so that we can later replace the original ones with the sunk ones.
    for (IRInstruction::ReverseIterator j = toSink.rbegin();
         j != toSink.rend();
         ++j) {
      // prependInstruction inserts an instruction to the beginning. Therefore,
      // we iterate through toSink in the reversed order.
      IRInstruction* sunkInst = irFactory->incRef((*j)->getSrc(0));
      sunkInst->setId(LIVE);
      trace->prependInstruction(sunkInst);

      ASSERT((*j)->getDst());
      ASSERT(!sunkTmps.count((*j)->getDst()));
      sunkTmps[(*j)->getDst()] = irFactory->getSSATmp(sunkInst);
    }
  }

  // An exit trace may be entered from multiple exit points. We keep track of
  // which exit traces we already pushed sunk IncRefs to, so that we won't push
  // them multiple times.
  std::set<Trace*> pushedTo;
  for (it = instList.begin(); it != instList.end(); ++it) {
    IRInstruction* inst = *it;
    if (trace->isMain()) {
      if (inst->getOpcode() == IncRef) {
        // Must be REFCOUNT_CONSUMED or REFCOUNT_CONSUMED_OFF_TRACE;
        // otherwise, it should be already removed in optimizeRefCount.
        ASSERT(inst->getId() == REFCOUNT_CONSUMED ||
               inst->getId() == REFCOUNT_CONSUMED_OFF_TRACE);
        if (inst->getId() == REFCOUNT_CONSUMED_OFF_TRACE) {
          inst->setOpcode(Mov);
          // Mark them as dead so that they'll be removed later.
          inst->setId(DEAD);
          // Put all REFCOUNT_CONSUMED_OFF_TRACE IncRefs to the sinking list.
          toSink.push_back(inst);
        }
      }
      if (inst->getOpcode() == DecRefNZ) {
        IRInstruction* srcInst = inst->getSrc(0)->getInstruction();
        if (srcInst->getId() == DEAD) {
          inst->setId(DEAD);
          // This may take O(I) time where I is the number of IncRefs
          // in the main trace.
          toSink.remove(srcInst);
        }
      }
      if (LabelInstruction* label = inst->getLabel()) {
        Trace* exitTrace = label->getTrace();
        if (!pushedTo.count(exitTrace)) {
          pushedTo.insert(exitTrace);
          sinkIncRefs(exitTrace, irFactory, toSink);
        }
      }
    } else {
      // Replace the original tmps with the sunk tmps.
      for (uint32 i = 0; i < inst->getNumSrcs(); ++i) {
        SSATmp* src = inst->getSrc(i);
        if (SSATmp* sunkTmp = sunkTmps[src]) {
          inst->setSrc(i, sunkTmp);
        }
      }
    }
  }

  // Do copyProp at last, because we need to keep REFCOUNT_CONSUMED_OFF_TRACE
  // Movs as the prototypes for sunk instructions.
  for (it = instList.begin(); it != instList.end(); ++it) {
    Simplifier::copyProp(*it);
  }
}

void eliminateDeadCode(Trace* trace, IRFactory* irFactory) {
  IRInstruction::List wl; // worklist of live instructions
  Trace::List& exitTraces = trace->getExitTraces();
  // first mark all exit traces as unreachable my setting the id on
  // their labels to 0
  for (Trace::Iterator it = exitTraces.begin();
       it != exitTraces.end();
       it++) {
    Trace* trace = *it;
    trace->getLabel()->setId(DEAD);
  }

  // mark the essential instructions and add them to the initial
  // work list; also mark the exit traces that are reachable by
  // any control flow instruction in the main trace.
  initInstructions(trace, wl);
  for (Trace::Iterator it = exitTraces.begin();
       it != exitTraces.end();
       it++) {
    // only process those exit traces that are reachable from
    // the main trace
    Trace* trace = *it;
    if (trace->getLabel()->getId() != DEAD) {
      initInstructions(trace, wl);
    }
  }

  // process the worklist
  while (!wl.empty()) {
    IRInstruction* inst = wl.front();
    wl.pop_front();
    for (uint32 i = 0; i < inst->getNumSrcs(); i++) {
      SSATmp* src = inst->getSrc(i);
      if (src->getInstruction()->isDefConst()) {
        continue;
      }
      IRInstruction* srcInst = src->getInstruction();
      if (srcInst->getId() == DEAD) {
        srcInst->setId(LIVE);
        wl.push_back(srcInst);
      }
      // <inst> consumes <srcInst> which is an IncRef,
      // so we mark <srcInst> as REFCOUNT_CONSUMED.
      if (inst->consumesReference(i) && srcInst->getOpcode() == IncRef) {
        if (inst->getParent()->isMain() || !srcInst->getParent()->isMain()) {
          // <srcInst> is consumed from its own trace.
          srcInst->setId(REFCOUNT_CONSUMED);
        } else {
          // <srcInst> is consumed off trace.
          if (srcInst->getId() != REFCOUNT_CONSUMED) {
            // mark <srcInst> as REFCOUNT_CONSUMED_OFF_TRACE unless it is
            // also consumed from its own trace.
            srcInst->setId(REFCOUNT_CONSUMED_OFF_TRACE);
          }
        }
      }
    }
  }

  // Optimize IncRefs and DecRefs.
  optimizeRefCount(trace);
  for (Trace::Iterator it = exitTraces.begin(); it != exitTraces.end(); ++it) {
    optimizeRefCount(*it);
  }

  if (RuntimeOption::EvalHHIREnableSinking) {
    // Sink IncRefs consumed off trace.
    IRInstruction::List toSink;
    sinkIncRefs(trace, irFactory, toSink);
  }

  // now remove instructions whose id == DEAD
  removeDeadInstructions(trace);
  for (Trace::Iterator it = exitTraces.begin(); it != exitTraces.end(); it++) {
    removeDeadInstructions(*it);
  }

  // for main trace ends with an unconditional jump copy the target of
  // the jump to the end of the trace
  IRInstruction::List& instList = trace->getInstructionList();
  IRInstruction::Iterator lastInst = instList.end();
  lastInst--; // go back to the last instruction
  IRInstruction* jmpInst = *lastInst;
  if (jmpInst->getOpcode() == Jmp_) {
    Trace* targetTrace = jmpInst->getLabel()->getTrace();
    IRInstruction::List& targetInstList = targetTrace->getInstructionList();
    IRInstruction::Iterator instIter = targetInstList.begin();
    instIter++; // skip over label
    // update the parent trace of the moved instructions
    for (IRInstruction::Iterator it = instIter;
         it != targetInstList.end();
         ++it) {
      (*it)->setParent(trace);
    }
    instList.splice(lastInst, targetInstList, instIter, targetInstList.end());
    // delete the jump instruction
    instList.erase(lastInst);
  }
}

} } }
