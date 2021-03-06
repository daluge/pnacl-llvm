//===-- llvm/CodeGen/AsmPrinter/DbgValueHistoryCalculator.cpp -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "DbgValueHistoryCalculator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/Debug.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include <algorithm>
#include <map>
#include <set>

#define DEBUG_TYPE "dwarfdebug"

namespace llvm {

// \brief If @MI is a DBG_VALUE with debug value described by a
// defined register, returns the number of this register.
// In the other case, returns 0.
static unsigned isDescribedByReg(const MachineInstr &MI) {
  assert(MI.isDebugValue());
  assert(MI.getNumOperands() == 3);
  // If location of variable is described using a register (directly or
  // indirecltly), this register is always a first operand.
  return MI.getOperand(0).isReg() ? MI.getOperand(0).getReg() : 0;
}

void DbgValueHistoryMap::startInstrRange(const MDNode *Var,
                                         const MachineInstr &MI) {
  // Instruction range should start with a DBG_VALUE instruction for the
  // variable.
  assert(MI.isDebugValue() && MI.getDebugVariable() == Var);
  auto &Ranges = VarInstrRanges[Var];
  if (!Ranges.empty() && Ranges.back().second == nullptr &&
      Ranges.back().first->isIdenticalTo(&MI)) {
    DEBUG(dbgs() << "Coalescing identical DBG_VALUE entries:\n"
                 << "\t" << Ranges.back().first << "\t" << MI << "\n");
    return;
  }
  Ranges.push_back(std::make_pair(&MI, nullptr));
}

void DbgValueHistoryMap::endInstrRange(const MDNode *Var,
                                       const MachineInstr &MI) {
  auto &Ranges = VarInstrRanges[Var];
  // Verify that the current instruction range is not yet closed.
  assert(!Ranges.empty() && Ranges.back().second == nullptr);
  // For now, instruction ranges are not allowed to cross basic block
  // boundaries.
  // @LOCALMOD-START
  // This assertion seems to fire for DBG_VALUE insts which are not present
  // in the LLVM IR but are inserted by a previous MachineFunction pass run in
  // -O0 mode.
  // TODO(dschuff): investigate more after we merge closer to LLVM 3.6
  // assert(Ranges.back().first->getParent() == MI.getParent());
  // @LOCALMOD-END
  Ranges.back().second = &MI;
}

unsigned DbgValueHistoryMap::getRegisterForVar(const MDNode *Var) const {
  const auto &I = VarInstrRanges.find(Var);
  if (I == VarInstrRanges.end())
    return 0;
  const auto &Ranges = I->second;
  if (Ranges.empty() || Ranges.back().second != nullptr)
    return 0;
  return isDescribedByReg(*Ranges.back().first);
}

namespace {
// Maps physreg numbers to the variables they describe.
typedef std::map<unsigned, SmallVector<const MDNode *, 1>> RegDescribedVarsMap;
}

// \brief Claim that @Var is not described by @RegNo anymore.
static void dropRegDescribedVar(RegDescribedVarsMap &RegVars,
                                unsigned RegNo, const MDNode *Var) {
  const auto &I = RegVars.find(RegNo);
  assert(RegNo != 0U && I != RegVars.end());
  auto &VarSet = I->second;
  const auto &VarPos = std::find(VarSet.begin(), VarSet.end(), Var);
  assert(VarPos != VarSet.end());
  VarSet.erase(VarPos);
  // Don't keep empty sets in a map to keep it as small as possible.
  if (VarSet.empty())
    RegVars.erase(I);
}

// \brief Claim that @Var is now described by @RegNo.
static void addRegDescribedVar(RegDescribedVarsMap &RegVars,
                               unsigned RegNo, const MDNode *Var) {
  assert(RegNo != 0U);
  auto &VarSet = RegVars[RegNo];
  assert(std::find(VarSet.begin(), VarSet.end(), Var) == VarSet.end());
  VarSet.push_back(Var);
}

// \brief Terminate the location range for variables described by register
// @RegNo by inserting @ClobberingInstr to their history.
static void clobberRegisterUses(RegDescribedVarsMap &RegVars, unsigned RegNo,
                                DbgValueHistoryMap &HistMap,
                                const MachineInstr &ClobberingInstr) {
  const auto &I = RegVars.find(RegNo);
  if (I == RegVars.end())
    return;
  // Iterate over all variables described by this register and add this
  // instruction to their history, clobbering it.
  for (const auto &Var : I->second)
    HistMap.endInstrRange(Var, ClobberingInstr);
  RegVars.erase(I);
}

// \brief Collect all registers clobbered by @MI and insert them to @Regs.
static void collectClobberedRegisters(const MachineInstr &MI,
                                      const TargetRegisterInfo *TRI,
                                      std::set<unsigned> &Regs) {
  for (const MachineOperand &MO : MI.operands()) {
    if (!MO.isReg() || !MO.isDef() || !MO.getReg())
      continue;
    for (MCRegAliasIterator AI(MO.getReg(), TRI, true); AI.isValid(); ++AI)
      Regs.insert(*AI);
  }
}

// \brief Returns the first instruction in @MBB which corresponds to
// the function epilogue, or nullptr if @MBB doesn't contain an epilogue.
static const MachineInstr *getFirstEpilogueInst(const MachineBasicBlock &MBB) {
  auto LastMI = MBB.getLastNonDebugInstr();
  if (LastMI == MBB.end() || !LastMI->isReturn())
    return nullptr;
  // Assume that epilogue starts with instruction having the same debug location
  // as the return instruction.
  DebugLoc LastLoc = LastMI->getDebugLoc();
  auto Res = LastMI;
  for (MachineBasicBlock::const_reverse_iterator I(std::next(LastMI)); I != MBB.rend();
       ++I) {
    if (I->getDebugLoc() != LastLoc)
      return Res;
    Res = std::prev(I.base());
  }
  // If all instructions have the same debug location, assume whole MBB is
  // an epilogue.
  return MBB.begin();
}

// \brief Collect registers that are modified in the function body (their
// contents is changed only in the prologue and epilogue).
static void collectChangingRegs(const MachineFunction *MF,
                                const TargetRegisterInfo *TRI,
                                std::set<unsigned> &Regs) {
  for (const auto &MBB : *MF) {
    auto FirstEpilogueInst = getFirstEpilogueInst(MBB);
    bool IsInEpilogue = false;
    for (const auto &MI : MBB) {
      IsInEpilogue |= &MI == FirstEpilogueInst;
      if (!MI.getFlag(MachineInstr::FrameSetup) && !IsInEpilogue)
        collectClobberedRegisters(MI, TRI, Regs);
    }
  }
}

void calculateDbgValueHistory(const MachineFunction *MF,
                              const TargetRegisterInfo *TRI,
                              DbgValueHistoryMap &Result) {
  std::set<unsigned> ChangingRegs;
  collectChangingRegs(MF, TRI, ChangingRegs);

  RegDescribedVarsMap RegVars;
  for (const auto &MBB : *MF) {
    for (const auto &MI : MBB) {
      if (!MI.isDebugValue()) {
        // Not a DBG_VALUE instruction. It may clobber registers which describe
        // some variables.
        std::set<unsigned> MIClobberedRegs;
        collectClobberedRegisters(MI, TRI, MIClobberedRegs);
        for (unsigned RegNo : MIClobberedRegs) {
          if (ChangingRegs.count(RegNo))
            clobberRegisterUses(RegVars, RegNo, Result, MI);
        }
        continue;
      }

      assert(MI.getNumOperands() > 1 && "Invalid DBG_VALUE instruction!");
      const MDNode *Var = MI.getDebugVariable();

      if (unsigned PrevReg = Result.getRegisterForVar(Var))
        dropRegDescribedVar(RegVars, PrevReg, Var);

      Result.startInstrRange(Var, MI);

      if (unsigned NewReg = isDescribedByReg(MI))
        addRegDescribedVar(RegVars, NewReg, Var);
    }

    // Make sure locations for register-described variables are valid only
    // until the end of the basic block (unless it's the last basic block, in
    // which case let their liveness run off to the end of the function).
    if (!MBB.empty() &&  &MBB != &MF->back()) {
      for (unsigned RegNo : ChangingRegs)
        clobberRegisterUses(RegVars, RegNo, Result, MBB.back());
    }
  }
}

}
