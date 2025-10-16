//===-- RISCVExpandPseudoInsts.cpp - Expand pseudo instructions -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a pass that expands pseudo instructions into target
// instructions. This pass should be run after register allocation but before
// the post-regalloc scheduling pass.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include <iterator>
#include "RISCVInstrInfo.h"
#include "RISCVTargetMachine.h"

#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/MC/MCContext.h"

using namespace llvm;

#define RISCV_EXPAND_PSEUDO_NAME "RISC-V pseudo instruction expansion pass"
#define RISCV_PRERA_EXPAND_PSEUDO_NAME "RISC-V Pre-RA pseudo instruction expansion pass"

namespace {

class RISCVExpandPseudo : public MachineFunctionPass {
public:
  const RISCVSubtarget *STI;
  const RISCVInstrInfo *TII;
  static char ID;

  RISCVExpandPseudo() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return RISCV_EXPAND_PSEUDO_NAME; }

private:
  bool expandYUVRGB(MachineBasicBlock &MBB,
                    MachineBasicBlock::iterator MBBI,
                    MachineBasicBlock::iterator &NextMBBI);
  bool expandDot(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                 MachineBasicBlock::iterator &NextMBBI);
  bool expandMBB(MachineBasicBlock &MBB);
  bool expandMI(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                MachineBasicBlock::iterator &NextMBBI);
  bool expandCCOp(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                  MachineBasicBlock::iterator &NextMBBI);
  bool expandVSetVL(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI);
  bool expandVMSET_VMCLR(MachineBasicBlock &MBB,
                         MachineBasicBlock::iterator MBBI, unsigned Opcode);
  bool expandRV32ZdinxStore(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator MBBI);
  bool expandRV32ZdinxLoad(MachineBasicBlock &MBB,
                           MachineBasicBlock::iterator MBBI);
#ifndef NDEBUG
  unsigned getInstSizeInBytes(const MachineFunction &MF) const {
    unsigned Size = 0;
    for (auto &MBB : MF)
      for (auto &MI : MBB)
        Size += TII->getInstSizeInBytes(MI);
    return Size;
  }
#endif
};

char RISCVExpandPseudo::ID = 0;

bool RISCVExpandPseudo::runOnMachineFunction(MachineFunction &MF) {
  errs() << "*** RISCVExpandPseudo running on " << MF.getName() << "\n";
  STI = &MF.getSubtarget<RISCVSubtarget>();
  TII = STI->getInstrInfo();

#ifndef NDEBUG
  const unsigned OldSize = getInstSizeInBytes(MF);
#endif

  bool Modified = false;
  for (auto &MBB : MF)
    Modified |= expandMBB(MBB);

#ifndef NDEBUG
  const unsigned NewSize = getInstSizeInBytes(MF);
  assert(OldSize >= NewSize);
#endif
  return Modified;
}

bool RISCVExpandPseudo::expandMBB(MachineBasicBlock &MBB) {
  bool Modified = false;

  MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
  while (MBBI != E) {
    MachineBasicBlock::iterator NMBBI = std::next(MBBI);
    Modified |= expandMI(MBB, MBBI, NMBBI);
    MBBI = NMBBI;
  }

  return Modified;
}


/// Expand pseudo-instrucţiunea DOT într-un loop 2× un-rolled
///   ACC = 0
///   for (i = 0; i < n; i += 2) {
///       ACC += a[i]   * b[i];
///       ACC += a[i+1] * b[i+1];
///   }
///   Rd = ACC;
///
/// • Rulează post-RA ⇒ folosim doar registre fizice caller-saved
/// • Nu introducem vreg-uri noi, deci SSA rămâne intactă
/// • *n* trebuie să fie par (altfel ultimul element nu e procesat;
///   TODO: tail-loop dacă se doreşte suport complet)
/*
bool RISCVExpandPseudo::expandDot(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MBBI,
                                 MachineBasicBlock::iterator &NextMBBI) {
 errs() << ">>> expandDot() called for DOT pseudo\n";

 MachineInstr &MI = *MBBI;
 const DebugLoc DL = MI.getDebugLoc();

 // --- 1. Validare rapidă ------------------------------------------------
if (MI.getNumOperands() != 4 || !MI.getOperand(0).isReg() ||
    !MI.getOperand(1).isReg() || !MI.getOperand(2).isReg() ||
    !MI.getOperand(3).isReg())
  return false;

Register Rd = MI.getOperand(0).getReg();   // dest
Register SrcA = MI.getOperand(1).getReg(); // ptr a
Register SrcB = MI.getOperand(2).getReg(); // ptr b
Register SrcN = MI.getOperand(3).getReg(); // n

// --- 2. Registre fizice temporare -------------------------------------- 
const Register ACC = RISCV::X13;   // s3
const Register PTR_A = RISCV::X5;  // t0
const Register PTR_B = RISCV::X6;  // t1
const Register END_A = RISCV::X16; // t5 : capăt vector a
const Register VA0 = RISCV::X28;   // t3
const Register VA1 = RISCV::X14;   // t1' (suficient liber)
const Register VB0 = RISCV::X29;   // t4
const Register VB1 = RISCV::X15;   // t2'

MachineFunction &MF = *MBB.getParent();
const auto *TII = MF.getSubtarget<RISCVSubtarget>().getInstrInfo();

// --- 3. Creăm blocurile loop / exit ------------------------------------ 
auto *LoopMBB = MF.CreateMachineBasicBlock();
auto *ExitMBB = MF.CreateMachineBasicBlock();
MF.insert(std::next(MachineFunction::iterator(&MBB)), LoopMBB);
MF.insert(std::next(MachineFunction::iterator(LoopMBB)), ExitMBB);

// mutăm instr. după DOT în Exit 
ExitMBB->splice(ExitMBB->begin(), &MBB, std::next(MBBI), MBB.end());
ExitMBB->transferSuccessorsAndUpdatePHIs(&MBB);

// re-legăm CFG 
while (!MBB.succ_empty())
  MBB.removeSuccessor(MBB.succ_begin());
MBB.addSuccessor(LoopMBB);
LoopMBB->addSuccessor(LoopMBB); // back-edge
LoopMBB->addSuccessor(ExitMBB); // fall-through

// --- 4. Pre-header ----------------------------------------------------- 
// ACC = 0
BuildMI(MBB, MBBI, DL, TII->get(RISCV::ADDI), ACC).addReg(RISCV::X0).addImm(0);

// PTR_A = a , PTR_B = b
BuildMI(MBB, MBBI, DL, TII->get(RISCV::ADDI), PTR_A).addReg(SrcA).addImm(0);
BuildMI(MBB, MBBI, DL, TII->get(RISCV::ADDI), PTR_B).addReg(SrcB).addImm(0);

// END_A = a + n*8   (<<3)
BuildMI(MBB, MBBI, DL, TII->get(RISCV::SLLI), END_A).addReg(SrcN).addImm(3);
BuildMI(MBB, MBBI, DL, TII->get(RISCV::ADD), END_A).addReg(END_A).addReg(SrcA);

// JAL x0, LoopMBB
BuildMI(MBB, std::next(MBBI), DL, TII->get(RISCV::JAL))
    .addReg(RISCV::X0, RegState::Define)
    .addMBB(LoopMBB);

// --- 5. Loop body 2× un-rolled ---------------------------------------- 
auto End = LoopMBB->end();

// load a[i] / a[i+1]
BuildMI(*LoopMBB, End, DL, TII->get(RISCV::LD), VA0).addReg(PTR_A).addImm(0);
BuildMI(*LoopMBB, End, DL, TII->get(RISCV::LD), VA1).addReg(PTR_A).addImm(8);

// load b[i] / b[i+1]
BuildMI(*LoopMBB, End, DL, TII->get(RISCV::LD), VB0).addReg(PTR_B).addImm(0);
BuildMI(*LoopMBB, End, DL, TII->get(RISCV::LD), VB1).addReg(PTR_B).addImm(8);

// MUL-uri
BuildMI(*LoopMBB, End, DL, TII->get(RISCV::MUL), VB0).addReg(VA0).addReg(VB0);
BuildMI(*LoopMBB, End, DL, TII->get(RISCV::MUL), VB1).addReg(VA1).addReg(VB1);

// ACC += prod0 + prod1
BuildMI(*LoopMBB, End, DL, TII->get(RISCV::ADD), ACC).addReg(ACC).addReg(VB0);
BuildMI(*LoopMBB, End, DL, TII->get(RISCV::ADD), ACC).addReg(ACC).addReg(VB1);

// avansăm ptr-urile cu 16 B
BuildMI(*LoopMBB, End, DL, TII->get(RISCV::ADDI), PTR_A)
    .addReg(PTR_A)
    .addImm(16);
BuildMI(*LoopMBB, End, DL, TII->get(RISCV::ADDI), PTR_B)
    .addReg(PTR_B)
    .addImm(16);

// BLT PTR_A, END_A, LoopMBB
BuildMI(*LoopMBB, End, DL, TII->get(RISCV::BLT))
    .addReg(PTR_A)
    .addReg(END_A)
    .addMBB(LoopMBB);

// --- 6. Exit: copiem ACC în Rd ---------------------------------------- 
BuildMI(*ExitMBB, ExitMBB->begin(), DL, TII->get(RISCV::ADDI), Rd)
    .addReg(ACC)
    .addImm(0);

// --- 7. Live-ins ------------------------------------------------------- 
for (Register R : {ACC, PTR_A, PTR_B, END_A})
  if (!LoopMBB->isLiveIn(R))
    LoopMBB->addLiveIn(R);
if (!ExitMBB->isLiveIn(ACC))
  ExitMBB->addLiveIn(ACC);

// --- 8. Curăţenie ------------------------------------------------------ 
MF.RenumberBlocks();
NextMBBI = MBB.erase(MBBI); // ştergem pseudo-instr. şi continuăm
return true;
}*/

bool RISCVExpandPseudo::expandDot(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator MBBI,
                                  MachineBasicBlock::iterator &NextMBBI) {
  errs() << ">>> expandDot() called for DOT pseudo\n";

  MachineInstr &MI = *MBBI;
  const DebugLoc DL = MI.getDebugLoc();

  // 1. Quick sanity check.
  if (MI.getNumOperands() != 4 || !MI.getOperand(0).isReg() ||
      !MI.getOperand(1).isReg() || !MI.getOperand(2).isReg() ||
      !MI.getOperand(3).isReg())
    return false;

  Register Rd = MI.getOperand(0).getReg();   // dest
  Register SrcA = MI.getOperand(1).getReg(); // ptr a
  Register SrcB = MI.getOperand(2).getReg(); // ptr b
  Register SrcN = MI.getOperand(3).getReg(); // n

  // Temporary physical registers:
  const Register ACC = RISCV::X13;   // s3
  const Register PTR_A = RISCV::X5;  // t0
  const Register PTR_B = RISCV::X6;  // t1
  const Register END_A = RISCV::X16; // t5
  const Register VA0 = RISCV::X28;   // t3
  const Register VA1 = RISCV::X14;   // s2
  const Register VB0 = RISCV::X29;   // t4
  const Register VB1 = RISCV::X15;   // s3

  MachineFunction &MF = *MBB.getParent();
  const auto *TII = MF.getSubtarget<RISCVSubtarget>().getInstrInfo();

  // Create the loop and exit blocks right after the current one.
  auto *LoopMBB = MF.CreateMachineBasicBlock();
  auto *ExitMBB = MF.CreateMachineBasicBlock();
  MF.insert(std::next(MachineFunction::iterator(&MBB)), LoopMBB);
  MF.insert(std::next(MachineFunction::iterator(LoopMBB)), ExitMBB);

  // Move instructions after the DOT into ExitMBB:
  ExitMBB->splice(ExitMBB->begin(), &MBB, std::next(MBBI), MBB.end());
  ExitMBB->transferSuccessorsAndUpdatePHIs(&MBB);

  // Re-wire CFG: MBB -> Loop, Loop->Loop(back-edge) and Loop->Exit
  while (!MBB.succ_empty())
    MBB.removeSuccessor(MBB.succ_begin());
  MBB.addSuccessor(LoopMBB);
  LoopMBB->addSuccessor(LoopMBB);
  LoopMBB->addSuccessor(ExitMBB);

  // --- Pre-header in MBB ------------------------------------------------

  // ACC = 0
  BuildMI(MBB, MBBI, DL, TII->get(RISCV::ADDI), ACC)
      .addReg(RISCV::X0)
      .addImm(0);

  // PTR_A = a, PTR_B = b
  BuildMI(MBB, MBBI, DL, TII->get(RISCV::ADDI), PTR_A).addReg(SrcA).addImm(0);
  BuildMI(MBB, MBBI, DL, TII->get(RISCV::ADDI), PTR_B).addReg(SrcB).addImm(0);

  // END_A = a + n*8   (n<<3)
  BuildMI(MBB, MBBI, DL, TII->get(RISCV::SLLI), END_A).addReg(SrcN).addImm(3);
  BuildMI(MBB, MBBI, DL, TII->get(RISCV::ADD), END_A)
      .addReg(END_A)
      .addReg(SrcA);

  // Unconditional jump into the loop:
  //  <- aici s-a înlocuit pseudoinstr J cu JAL x0, LoopMBB ->
  BuildMI(MBB, std::next(MBBI), DL, TII->get(RISCV::JAL))
      .addReg(RISCV::X0, RegState::Define) // link discard
      .addMBB(LoopMBB);

  // --- 2× unrolled loop body in LoopMBB -------------------------------

  {
    auto End = LoopMBB->end();

    // load a[i], a[i+1]
    BuildMI(*LoopMBB, End, DL, TII->get(RISCV::LD), VA0)
        .addReg(PTR_A)
        .addImm(0);
    BuildMI(*LoopMBB, End, DL, TII->get(RISCV::LD), VA1)
        .addReg(PTR_A)
        .addImm(8);

    // load b[i], b[i+1]
    BuildMI(*LoopMBB, End, DL, TII->get(RISCV::LD), VB0)
        .addReg(PTR_B)
        .addImm(0);
    BuildMI(*LoopMBB, End, DL, TII->get(RISCV::LD), VB1)
        .addReg(PTR_B)
        .addImm(8);

    // MULs
    BuildMI(*LoopMBB, End, DL, TII->get(RISCV::MUL), VB0)
        .addReg(VA0)
        .addReg(VB0);
    BuildMI(*LoopMBB, End, DL, TII->get(RISCV::MUL), VB1)
        .addReg(VA1)
        .addReg(VB1);

    // ACC += prod0 + prod1
    BuildMI(*LoopMBB, End, DL, TII->get(RISCV::ADD), ACC)
        .addReg(ACC)
        .addReg(VB0);
    BuildMI(*LoopMBB, End, DL, TII->get(RISCV::ADD), ACC)
        .addReg(ACC)
        .addReg(VB1);

    // advance pointers by 16 bytes
    BuildMI(*LoopMBB, End, DL, TII->get(RISCV::ADDI), PTR_A)
        .addReg(PTR_A)
        .addImm(16);
    BuildMI(*LoopMBB, End, DL, TII->get(RISCV::ADDI), PTR_B)
        .addReg(PTR_B)
        .addImm(16);

    // if (PTR_A < END_A) goto LoopMBB
    BuildMI(*LoopMBB, End, DL, TII->get(RISCV::BLT))
        .addReg(PTR_A)
        .addReg(END_A)
        .addMBB(LoopMBB);
  }

  // --- ExitMBB: move ACC into Rd ---------------------------------------

  BuildMI(*ExitMBB, ExitMBB->begin(), DL, TII->get(RISCV::ADDI), Rd)
      .addReg(ACC)
      .addImm(0);

  // --- Live-ins --------------------------------------------------------

  for (Register R : {ACC, PTR_A, PTR_B, END_A})
    if (!LoopMBB->isLiveIn(R))
      LoopMBB->addLiveIn(R);
  if (!ExitMBB->isLiveIn(ACC))
    ExitMBB->addLiveIn(ACC);

  // --- Cleanup ---------------------------------------------------------

  MF.RenumberBlocks();
  NextMBBI = MBB.erase(MBBI);
  return true;
}

bool RISCVExpandPseudo::expandYUVRGB(MachineBasicBlock &MBB,
                                     MachineBasicBlock::iterator MBBI,
                                     MachineBasicBlock::iterator &NextMBBI) {
  MachineInstr &MI = *MBBI;
  const DebugLoc DL = MI.getDebugLoc();
  MachineFunction &MF = *MBB.getParent();
  const RISCVInstrInfo *TII = MF.getSubtarget<RISCVSubtarget>().getInstrInfo();

  // PSEUDO operands: (yuv_ptr, rgb_ptr, height, width)
  if (MI.getNumOperands() < 4 || !MI.getOperand(0).isReg() ||
      !MI.getOperand(1).isReg() || !MI.getOperand(2).isReg() ||
      !MI.getOperand(3).isReg())
    return false;

  Register Y = MI.getOperand(0).getReg();
  Register R = MI.getOperand(1).getReg();
  Register H = MI.getOperand(2).getReg();
  Register W = MI.getOperand(3).getReg();

  // Physical regs (must match .td Defs)
  const Register BASE = RISCV::X5;
  const Register Yp = RISCV::X6;
  const Register Rp = RISCV::X7;
  const Register EndY = RISCV::X8;
  const Register Cnt = RISCV::X9;
  const Register Tmp0 = RISCV::X10;
  const Register Tmp1 = RISCV::X11;
  const Register Word0 = RISCV::X12;
  const Register Word1 = RISCV::X13;
  const Register Stat = RISCV::X14;
  const Register RGB0 = RISCV::X15;
  const Register RGB1 = RISCV::X16;
  const Register RGB2 = RISCV::X28;
  const Register RGB3 = RISCV::X29;
  const Register Byte = RISCV::X30;
  const Register Neg1 = RISCV::X31;

  constexpr int OFF_Y0U0Y1V0 = 0x10;
  constexpr int OFF_Y2U1Y3V1 = 0x14;
  constexpr int OFF_STATUS = 0x20;
  constexpr int OFF_RGB0 = 0x30;
  constexpr int OFF_RGB1 = 0x34;
  constexpr int OFF_RGB2 = 0x38;
  constexpr int OFF_RGB3 = 0x3C;

  // Create blocks
  auto *CheckMBB = MF.CreateMachineBasicBlock(MBB.getBasicBlock());
  auto *IssueMBB = MF.CreateMachineBasicBlock(MBB.getBasicBlock());
  auto *PollMBB = MF.CreateMachineBasicBlock(MBB.getBasicBlock());
  auto *ContMBB = MF.CreateMachineBasicBlock(MBB.getBasicBlock());
  auto *ExitMBB = MF.CreateMachineBasicBlock(MBB.getBasicBlock());

  MF.insert(std::next(MachineFunction::iterator(&MBB)), CheckMBB);
  MF.insert(std::next(MachineFunction::iterator(CheckMBB)), IssueMBB);
  MF.insert(std::next(MachineFunction::iterator(IssueMBB)), PollMBB);
  MF.insert(std::next(MachineFunction::iterator(PollMBB)), ContMBB);
  MF.insert(std::next(MachineFunction::iterator(ContMBB)), ExitMBB);

  // Move tail after pseudo into ExitMBB
  ExitMBB->splice(ExitMBB->begin(), &MBB, std::next(MBBI), MBB.end());
  ExitMBB->transferSuccessorsAndUpdatePHIs(&MBB);
  while (!MBB.succ_empty())
    MBB.removeSuccessor(MBB.succ_begin());

  // Prolog (no terminators except final branch)
  auto It = std::next(MBBI);
  BuildMI(MBB, It, DL, TII->get(RISCV::LUI), BASE).addImm(0x41100);
  BuildMI(MBB, It, DL, TII->get(RISCV::ADDI), Yp).addReg(Y).addImm(0);
  BuildMI(MBB, It, DL, TII->get(RISCV::ADDI), Rp).addReg(R).addImm(0);
  BuildMI(MBB, It, DL, TII->get(RISCV::MUL), Cnt).addReg(W).addReg(H);
  BuildMI(MBB, It, DL, TII->get(RISCV::SLLI), Cnt).addReg(Cnt).addImm(1);
  BuildMI(MBB, It, DL, TII->get(RISCV::ADD), EndY).addReg(Y).addReg(Cnt);
  BuildMI(MBB, It, DL, TII->get(RISCV::ADDI), Neg1)
      .addReg(RISCV::X0)
      .addImm(-1);
  // Uncond -> Check (proper terminator)
  BuildMI(MBB, It, DL, TII->get(RISCV::PseudoBR)).addMBB(CheckMBB);
  MBB.addSuccessor(CheckMBB);

  // CHECK: if (Yp==EndY) -> Exit; else -> Issue
  {
    auto E = CheckMBB->end();
    BuildMI(*CheckMBB, E, DL, TII->get(RISCV::BEQ))
        .addReg(Yp)
        .addReg(EndY)
        .addMBB(ExitMBB);
    BuildMI(*CheckMBB, E, DL, TII->get(RISCV::PseudoBR)).addMBB(IssueMBB);
    CheckMBB->addSuccessor(ExitMBB);
    CheckMBB->addSuccessor(IssueMBB);
  }

  // ISSUE: pack 8B -> Word0/Word1; write MMIO; -> Poll
  {
    auto E = IssueMBB->end();

    // Word0 b0..b3
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::LBU), Byte).addReg(Yp).addImm(0);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::ADDI), Word0)
        .addReg(Byte)
        .addImm(0);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::LBU), Byte).addReg(Yp).addImm(1);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::SLLI), Tmp0)
        .addReg(Byte)
        .addImm(8);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::OR), Word0)
        .addReg(Word0)
        .addReg(Tmp0);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::LBU), Byte).addReg(Yp).addImm(2);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::SLLI), Tmp0)
        .addReg(Byte)
        .addImm(16);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::OR), Word0)
        .addReg(Word0)
        .addReg(Tmp0);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::LBU), Byte).addReg(Yp).addImm(3);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::SLLI), Tmp0)
        .addReg(Byte)
        .addImm(24);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::OR), Word0)
        .addReg(Word0)
        .addReg(Tmp0);

    // Word1 b4..b7
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::LBU), Byte).addReg(Yp).addImm(4);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::ADDI), Word1)
        .addReg(Byte)
        .addImm(0);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::LBU), Byte).addReg(Yp).addImm(5);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::SLLI), Tmp0)
        .addReg(Byte)
        .addImm(8);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::OR), Word1)
        .addReg(Word1)
        .addReg(Tmp0);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::LBU), Byte).addReg(Yp).addImm(6);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::SLLI), Tmp0)
        .addReg(Byte)
        .addImm(16);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::OR), Word1)
        .addReg(Word1)
        .addReg(Tmp0);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::LBU), Byte).addReg(Yp).addImm(7);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::SLLI), Tmp0)
        .addReg(Byte)
        .addImm(24);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::OR), Word1)
        .addReg(Word1)
        .addReg(Tmp0);

    // MMIO writes
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::SW))
        .addReg(Word0)
        .addReg(BASE)
        .addImm(OFF_Y0U0Y1V0);
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::SW))
        .addReg(Word1)
        .addReg(BASE)
        .addImm(OFF_Y2U1Y3V1);

    // -> Poll
    BuildMI(*IssueMBB, E, DL, TII->get(RISCV::PseudoBR)).addMBB(PollMBB);
    IssueMBB->addSuccessor(PollMBB);
  }

  // POLL: while (STATUS != -1) loop; then -> Cont
  {
    auto E = PollMBB->end();
    BuildMI(*PollMBB, E, DL, TII->get(RISCV::LW), Stat)
        .addReg(BASE)
        .addImm(OFF_STATUS);
    BuildMI(*PollMBB, E, DL, TII->get(RISCV::BNE))
        .addReg(Stat)
        .addReg(Neg1)
        .addMBB(PollMBB);
    BuildMI(*PollMBB, E, DL, TII->get(RISCV::PseudoBR)).addMBB(ContMBB);
    PollMBB->addSuccessor(PollMBB);
    PollMBB->addSuccessor(ContMBB);
  }

  // CONT: read RGBx, write 12B, advance Yp/Rp; -> Check
  {
    auto E = ContMBB->end();

    BuildMI(*ContMBB, E, DL, TII->get(RISCV::LW), RGB0)
        .addReg(BASE)
        .addImm(OFF_RGB0);
    BuildMI(*ContMBB, E, DL, TII->get(RISCV::LW), RGB1)
        .addReg(BASE)
        .addImm(OFF_RGB1);
    BuildMI(*ContMBB, E, DL, TII->get(RISCV::LW), RGB2)
        .addReg(BASE)
        .addImm(OFF_RGB2);
    BuildMI(*ContMBB, E, DL, TII->get(RISCV::LW), RGB3)
        .addReg(BASE)
        .addImm(OFF_RGB3);

    auto emit3 = [&](Register Reg, int off) {
      BuildMI(*ContMBB, E, DL, TII->get(RISCV::SRLI), Byte)
          .addReg(Reg)
          .addImm(24);
      BuildMI(*ContMBB, E, DL, TII->get(RISCV::ANDI), Byte)
          .addReg(Byte)
          .addImm(0xFF);
      BuildMI(*ContMBB, E, DL, TII->get(RISCV::SB))
          .addReg(Byte)
          .addReg(Rp)
          .addImm(off + 0);

      BuildMI(*ContMBB, E, DL, TII->get(RISCV::SRLI), Byte)
          .addReg(Reg)
          .addImm(16);
      BuildMI(*ContMBB, E, DL, TII->get(RISCV::ANDI), Byte)
          .addReg(Byte)
          .addImm(0xFF);
      BuildMI(*ContMBB, E, DL, TII->get(RISCV::SB))
          .addReg(Byte)
          .addReg(Rp)
          .addImm(off + 1);

      BuildMI(*ContMBB, E, DL, TII->get(RISCV::SRLI), Byte)
          .addReg(Reg)
          .addImm(8);
      BuildMI(*ContMBB, E, DL, TII->get(RISCV::ANDI), Byte)
          .addReg(Byte)
          .addImm(0xFF);
      BuildMI(*ContMBB, E, DL, TII->get(RISCV::SB))
          .addReg(Byte)
          .addReg(Rp)
          .addImm(off + 2);
    };
    emit3(RGB0, 0);
    emit3(RGB1, 3);
    emit3(RGB2, 6);
    emit3(RGB3, 9);

    BuildMI(*ContMBB, E, DL, TII->get(RISCV::ADDI), Yp).addReg(Yp).addImm(8);
    BuildMI(*ContMBB, E, DL, TII->get(RISCV::ADDI), Rp).addReg(Rp).addImm(12);

    BuildMI(*ContMBB, E, DL, TII->get(RISCV::PseudoBR)).addMBB(CheckMBB);
    ContMBB->addSuccessor(CheckMBB);
  }

  // Live-ins
  auto addLiveIns = [&](MachineBasicBlock *B,
                        std::initializer_list<Register> Rs) {
    for (Register Reg : Rs)
      if (!B->isLiveIn(Reg))
        B->addLiveIn(Reg);
  };
  addLiveIns(CheckMBB, {Yp, EndY});
  addLiveIns(IssueMBB, {BASE, Yp});
  addLiveIns(PollMBB, {BASE, Neg1});
  addLiveIns(ContMBB, {BASE, Rp, Yp});

  // Erase pseudo
  NextMBBI = MBB.erase(MBBI);

  // Explicit succ alignment
  while (!CheckMBB->succ_empty())
    CheckMBB->removeSuccessor(CheckMBB->succ_begin());
  CheckMBB->addSuccessor(ExitMBB);
  CheckMBB->addSuccessor(IssueMBB);

  while (!IssueMBB->succ_empty())
    IssueMBB->removeSuccessor(IssueMBB->succ_begin());
  IssueMBB->addSuccessor(PollMBB);

  while (!PollMBB->succ_empty())
    PollMBB->removeSuccessor(PollMBB->succ_begin());
  PollMBB->addSuccessor(PollMBB);
  PollMBB->addSuccessor(ContMBB);

  while (!ContMBB->succ_empty())
    ContMBB->removeSuccessor(ContMBB->succ_begin());
  ContMBB->addSuccessor(CheckMBB);

  while (!MBB.succ_empty())
    MBB.removeSuccessor(MBB.succ_begin());
  MBB.addSuccessor(CheckMBB);

  // Reconcile CFG successors with actual branch targets + fallthrough
  auto CollectTermTargets = [&](MachineBasicBlock *BB) {
    llvm::SmallPtrSet<MachineBasicBlock *, 8> Targets;
    for (MachineInstr &I : *BB) {
      if (!I.isTerminator())
        continue;
      for (const MachineOperand &Op : I.operands())
        if (Op.isMBB())
          Targets.insert(Op.getMBB());
    }
    bool EndsWithTerm = (!BB->empty() && BB->back().isTerminator());
    if (!EndsWithTerm) {
      auto It2 = MachineFunction::iterator(BB);
      ++It2;
      if (It2 != BB->getParent()->end())
        Targets.insert(&*It2);
    }
    return Targets;
  };
  auto SyncSuccs =
      [&](MachineBasicBlock *BB,
          const llvm::SmallPtrSetImpl<MachineBasicBlock *> &Wanted) {
        llvm::SmallVector<MachineBasicBlock *, 4> ToRemove;
        for (auto SI = BB->succ_begin(), SE = BB->succ_end(); SI != SE; ++SI)
          if (!Wanted.count(*SI))
            ToRemove.push_back(*SI);
        for (auto *S : ToRemove)
          BB->removeSuccessor(S);
        for (auto *S : Wanted)
          if (!BB->isSuccessor(S))
            BB->addSuccessor(S);
      };
  {
    llvm::SmallVector<MachineBasicBlock *, 8> Blocks{
        &MBB, CheckMBB, IssueMBB, PollMBB, ContMBB, ExitMBB};
    for (auto *B : Blocks) {
      if (!B)
        continue;
      auto Wanted = CollectTermTargets(B);
      SyncSuccs(B, Wanted);
    }
  }

  MF.RenumberBlocks();
  return true;
}

bool RISCVExpandPseudo::expandMI(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MBBI,
                                 MachineBasicBlock::iterator &NextMBBI) {
  // RISCVInstrInfo::getInstSizeInBytes expects that the total size of the
  // expanded instructions for each pseudo is correct in the Size field of the
  // tablegen definition for the pseudo.
  errs() << ">>> expandMI: opcode = " << MBBI->getOpcode() << "\n";
  switch (MBBI->getOpcode()) {
  case RISCV::DOT:
    errs() << ">>> Detected DOT pseudo-instruction\n";
    return expandDot(MBB, MBBI, NextMBBI);
  case RISCV::YUVRGB:
    errs() << ">>> Detected YUVRGB_PSEUDO pseudo-instruction\n";
    return expandYUVRGB(MBB, MBBI, NextMBBI);
  case RISCV::PseudoRV32ZdinxSD:
    return expandRV32ZdinxStore(MBB, MBBI);
  case RISCV::PseudoRV32ZdinxLD:
    return expandRV32ZdinxLoad(MBB, MBBI);
  case RISCV::PseudoCCMOVGPRNoX0:
  case RISCV::PseudoCCMOVGPR:
  case RISCV::PseudoCCADD:
  case RISCV::PseudoCCSUB:
  case RISCV::PseudoCCAND:
  case RISCV::PseudoCCOR:
  case RISCV::PseudoCCXOR:
  case RISCV::PseudoCCADDW:
  case RISCV::PseudoCCSUBW:
  case RISCV::PseudoCCSLL:
  case RISCV::PseudoCCSRL:
  case RISCV::PseudoCCSRA:
  case RISCV::PseudoCCADDI:
  case RISCV::PseudoCCSLLI:
  case RISCV::PseudoCCSRLI:
  case RISCV::PseudoCCSRAI:
  case RISCV::PseudoCCANDI:
  case RISCV::PseudoCCORI:
  case RISCV::PseudoCCXORI:
  case RISCV::PseudoCCSLLW:
  case RISCV::PseudoCCSRLW:
  case RISCV::PseudoCCSRAW:
  case RISCV::PseudoCCADDIW:
  case RISCV::PseudoCCSLLIW:
  case RISCV::PseudoCCSRLIW:
  case RISCV::PseudoCCSRAIW:
  case RISCV::PseudoCCANDN:
  case RISCV::PseudoCCORN:
  case RISCV::PseudoCCXNOR:
    return expandCCOp(MBB, MBBI, NextMBBI);
  case RISCV::PseudoVSETVLI:
  case RISCV::PseudoVSETVLIX0:
  case RISCV::PseudoVSETIVLI:
    return expandVSetVL(MBB, MBBI);
  case RISCV::PseudoVMCLR_M_B1:
  case RISCV::PseudoVMCLR_M_B2:
  case RISCV::PseudoVMCLR_M_B4:
  case RISCV::PseudoVMCLR_M_B8:
  case RISCV::PseudoVMCLR_M_B16:
  case RISCV::PseudoVMCLR_M_B32:
  case RISCV::PseudoVMCLR_M_B64:
    // vmclr.m vd => vmxor.mm vd, vd, vd
    return expandVMSET_VMCLR(MBB, MBBI, RISCV::VMXOR_MM);
  case RISCV::PseudoVMSET_M_B1:
  case RISCV::PseudoVMSET_M_B2:
  case RISCV::PseudoVMSET_M_B4:
  case RISCV::PseudoVMSET_M_B8:
  case RISCV::PseudoVMSET_M_B16:
  case RISCV::PseudoVMSET_M_B32:
  case RISCV::PseudoVMSET_M_B64:
    // vmset.m vd => vmxnor.mm vd, vd, vd
    return expandVMSET_VMCLR(MBB, MBBI, RISCV::VMXNOR_MM);
  }

  return false;
}

bool RISCVExpandPseudo::expandCCOp(MachineBasicBlock &MBB,
                                   MachineBasicBlock::iterator MBBI,
                                   MachineBasicBlock::iterator &NextMBBI) {

  MachineFunction *MF = MBB.getParent();
  MachineInstr &MI = *MBBI;
  DebugLoc DL = MI.getDebugLoc();

  MachineBasicBlock *TrueBB = MF->CreateMachineBasicBlock(MBB.getBasicBlock());
  MachineBasicBlock *MergeBB = MF->CreateMachineBasicBlock(MBB.getBasicBlock());

  MF->insert(++MBB.getIterator(), TrueBB);
  MF->insert(++TrueBB->getIterator(), MergeBB);

  // We want to copy the "true" value when the condition is true which means
  // we need to invert the branch condition to jump over TrueBB when the
  // condition is false.
  auto CC = static_cast<RISCVCC::CondCode>(MI.getOperand(3).getImm());
  CC = RISCVCC::getOppositeBranchCondition(CC);

  // Insert branch instruction.
  BuildMI(MBB, MBBI, DL, TII->getBrCond(CC))
      .addReg(MI.getOperand(1).getReg())
      .addReg(MI.getOperand(2).getReg())
      .addMBB(MergeBB);

  Register DestReg = MI.getOperand(0).getReg();
  assert(MI.getOperand(4).getReg() == DestReg);

  if (MI.getOpcode() == RISCV::PseudoCCMOVGPR ||
      MI.getOpcode() == RISCV::PseudoCCMOVGPRNoX0) {
    // Add MV.
    BuildMI(TrueBB, DL, TII->get(RISCV::ADDI), DestReg)
        .add(MI.getOperand(5))
        .addImm(0);
  } else {
    unsigned NewOpc;
    switch (MI.getOpcode()) {
    default:
      llvm_unreachable("Unexpected opcode!");
    case RISCV::PseudoCCADD:   NewOpc = RISCV::ADD;   break;
    case RISCV::PseudoCCSUB:   NewOpc = RISCV::SUB;   break;
    case RISCV::PseudoCCSLL:   NewOpc = RISCV::SLL;   break;
    case RISCV::PseudoCCSRL:   NewOpc = RISCV::SRL;   break;
    case RISCV::PseudoCCSRA:   NewOpc = RISCV::SRA;   break;
    case RISCV::PseudoCCAND:   NewOpc = RISCV::AND;   break;
    case RISCV::PseudoCCOR:    NewOpc = RISCV::OR;    break;
    case RISCV::PseudoCCXOR:   NewOpc = RISCV::XOR;   break;
    case RISCV::PseudoCCADDI:  NewOpc = RISCV::ADDI;  break;
    case RISCV::PseudoCCSLLI:  NewOpc = RISCV::SLLI;  break;
    case RISCV::PseudoCCSRLI:  NewOpc = RISCV::SRLI;  break;
    case RISCV::PseudoCCSRAI:  NewOpc = RISCV::SRAI;  break;
    case RISCV::PseudoCCANDI:  NewOpc = RISCV::ANDI;  break;
    case RISCV::PseudoCCORI:   NewOpc = RISCV::ORI;   break;
    case RISCV::PseudoCCXORI:  NewOpc = RISCV::XORI;  break;
    case RISCV::PseudoCCADDW:  NewOpc = RISCV::ADDW;  break;
    case RISCV::PseudoCCSUBW:  NewOpc = RISCV::SUBW;  break;
    case RISCV::PseudoCCSLLW:  NewOpc = RISCV::SLLW;  break;
    case RISCV::PseudoCCSRLW:  NewOpc = RISCV::SRLW;  break;
    case RISCV::PseudoCCSRAW:  NewOpc = RISCV::SRAW;  break;
    case RISCV::PseudoCCADDIW: NewOpc = RISCV::ADDIW; break;
    case RISCV::PseudoCCSLLIW: NewOpc = RISCV::SLLIW; break;
    case RISCV::PseudoCCSRLIW: NewOpc = RISCV::SRLIW; break;
    case RISCV::PseudoCCSRAIW: NewOpc = RISCV::SRAIW; break;
    case RISCV::PseudoCCANDN:  NewOpc = RISCV::ANDN;  break;
    case RISCV::PseudoCCORN:   NewOpc = RISCV::ORN;   break;
    case RISCV::PseudoCCXNOR:  NewOpc = RISCV::XNOR;  break;
    }
    BuildMI(TrueBB, DL, TII->get(NewOpc), DestReg)
        .add(MI.getOperand(5))
        .add(MI.getOperand(6));
  }

  TrueBB->addSuccessor(MergeBB);

  MergeBB->splice(MergeBB->end(), &MBB, MI, MBB.end());
  MergeBB->transferSuccessors(&MBB);

  MBB.addSuccessor(TrueBB);
  MBB.addSuccessor(MergeBB);

  NextMBBI = MBB.end();
  MI.eraseFromParent();

  // Make sure live-ins are correctly attached to this new basic block.
  LivePhysRegs LiveRegs;
  computeAndAddLiveIns(LiveRegs, *TrueBB);
  computeAndAddLiveIns(LiveRegs, *MergeBB);

  return true;
}

bool RISCVExpandPseudo::expandVSetVL(MachineBasicBlock &MBB,
                                     MachineBasicBlock::iterator MBBI) {
  assert(MBBI->getNumExplicitOperands() == 3 && MBBI->getNumOperands() >= 5 &&
         "Unexpected instruction format");

  DebugLoc DL = MBBI->getDebugLoc();

  assert((MBBI->getOpcode() == RISCV::PseudoVSETVLI ||
          MBBI->getOpcode() == RISCV::PseudoVSETVLIX0 ||
          MBBI->getOpcode() == RISCV::PseudoVSETIVLI) &&
         "Unexpected pseudo instruction");
  unsigned Opcode;
  if (MBBI->getOpcode() == RISCV::PseudoVSETIVLI)
    Opcode = RISCV::VSETIVLI;
  else
    Opcode = RISCV::VSETVLI;
  const MCInstrDesc &Desc = TII->get(Opcode);
  assert(Desc.getNumOperands() == 3 && "Unexpected instruction format");

  Register DstReg = MBBI->getOperand(0).getReg();
  bool DstIsDead = MBBI->getOperand(0).isDead();
  BuildMI(MBB, MBBI, DL, Desc)
      .addReg(DstReg, RegState::Define | getDeadRegState(DstIsDead))
      .add(MBBI->getOperand(1))  // VL
      .add(MBBI->getOperand(2)); // VType

  MBBI->eraseFromParent(); // The pseudo instruction is gone now.
  return true;
}

bool RISCVExpandPseudo::expandVMSET_VMCLR(MachineBasicBlock &MBB,
                                          MachineBasicBlock::iterator MBBI,
                                          unsigned Opcode) {
  DebugLoc DL = MBBI->getDebugLoc();
  Register DstReg = MBBI->getOperand(0).getReg();
  const MCInstrDesc &Desc = TII->get(Opcode);
  BuildMI(MBB, MBBI, DL, Desc, DstReg)
      .addReg(DstReg, RegState::Undef)
      .addReg(DstReg, RegState::Undef);
  MBBI->eraseFromParent(); // The pseudo instruction is gone now.
  return true;
}

// This function expands the PseudoRV32ZdinxSD for storing a double-precision
// floating-point value into memory by generating an equivalent instruction
// sequence for RV32.
bool RISCVExpandPseudo::expandRV32ZdinxStore(MachineBasicBlock &MBB,
                                             MachineBasicBlock::iterator MBBI) {
  DebugLoc DL = MBBI->getDebugLoc();
  const TargetRegisterInfo *TRI = STI->getRegisterInfo();
  Register Lo =
      TRI->getSubReg(MBBI->getOperand(0).getReg(), RISCV::sub_gpr_even);
  Register Hi =
      TRI->getSubReg(MBBI->getOperand(0).getReg(), RISCV::sub_gpr_odd);

  assert(MBBI->hasOneMemOperand() && "Expected mem operand");
  MachineMemOperand *OldMMO = MBBI->memoperands().front();
  MachineFunction *MF = MBB.getParent();
  MachineMemOperand *MMOLo = MF->getMachineMemOperand(OldMMO, 0, 4);
  MachineMemOperand *MMOHi = MF->getMachineMemOperand(OldMMO, 4, 4);

  BuildMI(MBB, MBBI, DL, TII->get(RISCV::SW))
      .addReg(Lo, getKillRegState(MBBI->getOperand(0).isKill()))
      .addReg(MBBI->getOperand(1).getReg())
      .add(MBBI->getOperand(2))
      .setMemRefs(MMOLo);

  if (MBBI->getOperand(2).isGlobal() || MBBI->getOperand(2).isCPI()) {
    // FIXME: Zdinx RV32 can not work on unaligned scalar memory.
    assert(!STI->enableUnalignedScalarMem());

    assert(MBBI->getOperand(2).getOffset() % 8 == 0);
    MBBI->getOperand(2).setOffset(MBBI->getOperand(2).getOffset() + 4);
    BuildMI(MBB, MBBI, DL, TII->get(RISCV::SW))
        .addReg(Hi, getKillRegState(MBBI->getOperand(0).isKill()))
        .add(MBBI->getOperand(1))
        .add(MBBI->getOperand(2))
        .setMemRefs(MMOHi);
  } else {
    assert(isInt<12>(MBBI->getOperand(2).getImm() + 4));
    BuildMI(MBB, MBBI, DL, TII->get(RISCV::SW))
        .addReg(Hi, getKillRegState(MBBI->getOperand(0).isKill()))
        .add(MBBI->getOperand(1))
        .addImm(MBBI->getOperand(2).getImm() + 4)
        .setMemRefs(MMOHi);
  }
  MBBI->eraseFromParent();
  return true;
}

// This function expands PseudoRV32ZdinxLoad for loading a double-precision
// floating-point value from memory into an equivalent instruction sequence for
// RV32.
bool RISCVExpandPseudo::expandRV32ZdinxLoad(MachineBasicBlock &MBB,
                                            MachineBasicBlock::iterator MBBI) {
  DebugLoc DL = MBBI->getDebugLoc();
  const TargetRegisterInfo *TRI = STI->getRegisterInfo();
  Register Lo =
      TRI->getSubReg(MBBI->getOperand(0).getReg(), RISCV::sub_gpr_even);
  Register Hi =
      TRI->getSubReg(MBBI->getOperand(0).getReg(), RISCV::sub_gpr_odd);

  assert(MBBI->hasOneMemOperand() && "Expected mem operand");
  MachineMemOperand *OldMMO = MBBI->memoperands().front();
  MachineFunction *MF = MBB.getParent();
  MachineMemOperand *MMOLo = MF->getMachineMemOperand(OldMMO, 0, 4);
  MachineMemOperand *MMOHi = MF->getMachineMemOperand(OldMMO, 4, 4);

  // If the register of operand 1 is equal to the Lo register, then swap the
  // order of loading the Lo and Hi statements.
  bool IsOp1EqualToLo = Lo == MBBI->getOperand(1).getReg();
  // Order: Lo, Hi
  if (!IsOp1EqualToLo) {
    BuildMI(MBB, MBBI, DL, TII->get(RISCV::LW), Lo)
        .addReg(MBBI->getOperand(1).getReg())
        .add(MBBI->getOperand(2))
        .setMemRefs(MMOLo);
  }

  if (MBBI->getOperand(2).isGlobal() || MBBI->getOperand(2).isCPI()) {
    auto Offset = MBBI->getOperand(2).getOffset();
    assert(MBBI->getOperand(2).getOffset() % 8 == 0);
    MBBI->getOperand(2).setOffset(Offset + 4);
    BuildMI(MBB, MBBI, DL, TII->get(RISCV::LW), Hi)
        .addReg(MBBI->getOperand(1).getReg())
        .add(MBBI->getOperand(2))
        .setMemRefs(MMOHi);
    MBBI->getOperand(2).setOffset(Offset);
  } else {
    assert(isInt<12>(MBBI->getOperand(2).getImm() + 4));
    BuildMI(MBB, MBBI, DL, TII->get(RISCV::LW), Hi)
        .addReg(MBBI->getOperand(1).getReg())
        .addImm(MBBI->getOperand(2).getImm() + 4)
        .setMemRefs(MMOHi);
  }

  // Order: Hi, Lo
  if (IsOp1EqualToLo) {
    BuildMI(MBB, MBBI, DL, TII->get(RISCV::LW), Lo)
        .addReg(MBBI->getOperand(1).getReg())
        .add(MBBI->getOperand(2))
        .setMemRefs(MMOLo);
  }

  MBBI->eraseFromParent();
  return true;
}

class RISCVPreRAExpandPseudo : public MachineFunctionPass {
public:
  const RISCVSubtarget *STI;
  const RISCVInstrInfo *TII;
  static char ID;

  RISCVPreRAExpandPseudo() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  StringRef getPassName() const override {
    return RISCV_PRERA_EXPAND_PSEUDO_NAME;
  }

private:
  bool expandDot(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                 MachineBasicBlock::iterator &NextMBBI);
  bool expandMBB(MachineBasicBlock &MBB);
  bool expandMI(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                MachineBasicBlock::iterator &NextMBBI);
  bool expandAuipcInstPair(MachineBasicBlock &MBB,
                           MachineBasicBlock::iterator MBBI,
                           MachineBasicBlock::iterator &NextMBBI,
                           unsigned FlagsHi, unsigned SecondOpcode);
  bool expandLoadLocalAddress(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator MBBI,
                              MachineBasicBlock::iterator &NextMBBI);
  bool expandLoadGlobalAddress(MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator MBBI,
                               MachineBasicBlock::iterator &NextMBBI);
  bool expandLoadTLSIEAddress(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator MBBI,
                              MachineBasicBlock::iterator &NextMBBI);
  bool expandLoadTLSGDAddress(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator MBBI,
                              MachineBasicBlock::iterator &NextMBBI);
  bool expandLoadTLSDescAddress(MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator MBBI,
                                MachineBasicBlock::iterator &NextMBBI);

#ifndef NDEBUG
  unsigned getInstSizeInBytes(const MachineFunction &MF) const {
    unsigned Size = 0;
    for (auto &MBB : MF)
      for (auto &MI : MBB)
        Size += TII->getInstSizeInBytes(MI);
    return Size;
  }
#endif
};

char RISCVPreRAExpandPseudo::ID = 0;

bool RISCVPreRAExpandPseudo::runOnMachineFunction(MachineFunction &MF) {
  errs() << "*** RISCVPreRAExpandPseudo running on " << MF.getName() << "\n";
  STI = &MF.getSubtarget<RISCVSubtarget>();
  TII = STI->getInstrInfo();

#ifndef NDEBUG
  const unsigned OldSize = getInstSizeInBytes(MF);
#endif

  bool Modified = false;
  for (auto &MBB : MF)
    Modified |= expandMBB(MBB);

#ifndef NDEBUG
  const unsigned NewSize = getInstSizeInBytes(MF);
  assert(OldSize >= NewSize);
#endif
  return Modified;
}

bool RISCVPreRAExpandPseudo::expandMBB(MachineBasicBlock &MBB) {
  bool Modified = false;

  MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
  while (MBBI != E) {
    MachineBasicBlock::iterator NMBBI = std::next(MBBI);
    Modified |= expandMI(MBB, MBBI, NMBBI);
    MBBI = NMBBI;
  }

  return Modified;
}

bool RISCVPreRAExpandPseudo::expandMI(MachineBasicBlock &MBB,
                                      MachineBasicBlock::iterator MBBI,
                                      MachineBasicBlock::iterator &NextMBBI) {

  errs() << ">>> expandMI: opcode = " << MBBI->getOpcode() << "\n";
  switch (MBBI->getOpcode()) {
  /* case RISCV::DOT:
    errs() << ">>> Detected DOT pseudo-instruction\n";
    return expandDot(MBB, MBBI, NextMBBI);*/
  case RISCV::PseudoLLA:
    return expandLoadLocalAddress(MBB, MBBI, NextMBBI);
  case RISCV::PseudoLGA:
    return expandLoadGlobalAddress(MBB, MBBI, NextMBBI);
  case RISCV::PseudoLA_TLS_IE:
    return expandLoadTLSIEAddress(MBB, MBBI, NextMBBI);
  case RISCV::PseudoLA_TLS_GD:
    return expandLoadTLSGDAddress(MBB, MBBI, NextMBBI);
  case RISCV::PseudoLA_TLSDESC:
    return expandLoadTLSDescAddress(MBB, MBBI, NextMBBI);
  }
  return false;
}

bool RISCVPreRAExpandPseudo::expandAuipcInstPair(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    MachineBasicBlock::iterator &NextMBBI, unsigned FlagsHi,
    unsigned SecondOpcode) {
  MachineFunction *MF = MBB.getParent();
  MachineInstr &MI = *MBBI;
  DebugLoc DL = MI.getDebugLoc();

  Register DestReg = MI.getOperand(0).getReg();
  Register ScratchReg =
      MF->getRegInfo().createVirtualRegister(&RISCV::GPRRegClass);

  MachineOperand &Symbol = MI.getOperand(1);
  Symbol.setTargetFlags(FlagsHi);
  MCSymbol *AUIPCSymbol = MF->getContext().createNamedTempSymbol("pcrel_hi");

  MachineInstr *MIAUIPC =
      BuildMI(MBB, MBBI, DL, TII->get(RISCV::AUIPC), ScratchReg).add(Symbol);
  MIAUIPC->setPreInstrSymbol(*MF, AUIPCSymbol);

  MachineInstr *SecondMI =
      BuildMI(MBB, MBBI, DL, TII->get(SecondOpcode), DestReg)
          .addReg(ScratchReg)
          .addSym(AUIPCSymbol, RISCVII::MO_PCREL_LO);

  if (MI.hasOneMemOperand())
    SecondMI->addMemOperand(*MF, *MI.memoperands_begin());

  MI.eraseFromParent();
  return true;
}

bool RISCVPreRAExpandPseudo::expandLoadLocalAddress(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    MachineBasicBlock::iterator &NextMBBI) {
  return expandAuipcInstPair(MBB, MBBI, NextMBBI, RISCVII::MO_PCREL_HI,
                             RISCV::ADDI);
}

bool RISCVPreRAExpandPseudo::expandLoadGlobalAddress(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    MachineBasicBlock::iterator &NextMBBI) {
  unsigned SecondOpcode = STI->is64Bit() ? RISCV::LD : RISCV::LW;
  return expandAuipcInstPair(MBB, MBBI, NextMBBI, RISCVII::MO_GOT_HI,
                             SecondOpcode);
}

bool RISCVPreRAExpandPseudo::expandLoadTLSIEAddress(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    MachineBasicBlock::iterator &NextMBBI) {
  unsigned SecondOpcode = STI->is64Bit() ? RISCV::LD : RISCV::LW;
  return expandAuipcInstPair(MBB, MBBI, NextMBBI, RISCVII::MO_TLS_GOT_HI,
                             SecondOpcode);
}

bool RISCVPreRAExpandPseudo::expandLoadTLSGDAddress(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    MachineBasicBlock::iterator &NextMBBI) {
  return expandAuipcInstPair(MBB, MBBI, NextMBBI, RISCVII::MO_TLS_GD_HI,
                             RISCV::ADDI);
}

bool RISCVPreRAExpandPseudo::expandLoadTLSDescAddress(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    MachineBasicBlock::iterator &NextMBBI) {
  MachineFunction *MF = MBB.getParent();
  MachineInstr &MI = *MBBI;
  DebugLoc DL = MI.getDebugLoc();

  const auto &STI = MF->getSubtarget<RISCVSubtarget>();
  unsigned SecondOpcode = STI.is64Bit() ? RISCV::LD : RISCV::LW;

  Register FinalReg = MI.getOperand(0).getReg();
  Register DestReg =
      MF->getRegInfo().createVirtualRegister(&RISCV::GPRRegClass);
  Register ScratchReg =
      MF->getRegInfo().createVirtualRegister(&RISCV::GPRRegClass);

  MachineOperand &Symbol = MI.getOperand(1);
  Symbol.setTargetFlags(RISCVII::MO_TLSDESC_HI);
  MCSymbol *AUIPCSymbol = MF->getContext().createNamedTempSymbol("tlsdesc_hi");

  MachineInstr *MIAUIPC =
      BuildMI(MBB, MBBI, DL, TII->get(RISCV::AUIPC), ScratchReg).add(Symbol);
  MIAUIPC->setPreInstrSymbol(*MF, AUIPCSymbol);

  BuildMI(MBB, MBBI, DL, TII->get(SecondOpcode), DestReg)
      .addReg(ScratchReg)
      .addSym(AUIPCSymbol, RISCVII::MO_TLSDESC_LOAD_LO);

  BuildMI(MBB, MBBI, DL, TII->get(RISCV::ADDI), RISCV::X10)
      .addReg(ScratchReg)
      .addSym(AUIPCSymbol, RISCVII::MO_TLSDESC_ADD_LO);

  BuildMI(MBB, MBBI, DL, TII->get(RISCV::PseudoTLSDESCCall), RISCV::X5)
      .addReg(DestReg)
      .addImm(0)
      .addSym(AUIPCSymbol, RISCVII::MO_TLSDESC_CALL);

  BuildMI(MBB, MBBI, DL, TII->get(RISCV::ADD), FinalReg)
      .addReg(RISCV::X10)
      .addReg(RISCV::X4);

  MI.eraseFromParent();
  return true;
}

} // end of anonymous namespace

INITIALIZE_PASS(RISCVExpandPseudo, "riscv-expand-pseudo",
                RISCV_EXPAND_PSEUDO_NAME, false, false)

INITIALIZE_PASS(RISCVPreRAExpandPseudo, "riscv-prera-expand-pseudo",
                RISCV_PRERA_EXPAND_PSEUDO_NAME, false, false)

namespace llvm {

FunctionPass *createRISCVExpandPseudoPass() { return new RISCVExpandPseudo(); }
FunctionPass *createRISCVPreRAExpandPseudoPass() { return new RISCVPreRAExpandPseudo(); }

} // end of namespace llvm
