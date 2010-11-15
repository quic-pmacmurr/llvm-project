//===-- PPCMCInstLower.cpp - Convert PPC MachineInstr to an MCInst --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains code to lower PPC MachineInstrs to their corresponding
// MCInst records.
//
//===----------------------------------------------------------------------===//

#include "PPC.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfoImpls.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Target/Mangler.h"
#include "llvm/ADT/SmallString.h"
using namespace llvm;

static MachineModuleInfoMachO &getMachOMMI(AsmPrinter &AP) {
  return AP.MMI->getObjFileInfo<MachineModuleInfoMachO>();
}


static MCSymbol *GetSymbolFromOperand(const MachineOperand &MO, AsmPrinter &AP){
  MCContext &Ctx = AP.OutContext;

  SmallString<128> Name;
  if (!MO.isGlobal()) {
    assert(MO.isSymbol() && "Isn't a symbol reference");
    Name += AP.MAI->getGlobalPrefix();
    Name += MO.getSymbolName();
  } else {    
    const GlobalValue *GV = MO.getGlobal();
    bool isImplicitlyPrivate = false;
    if (MO.getTargetFlags() == PPCII::MO_DARWIN_STUB ||
        //MO.getTargetFlags() == PPCII::MO_DARWIN_NONLAZY ||
        //MO.getTargetFlags() == PPCII::MO_DARWIN_NONLAZY_PIC_BASE ||
        //MO.getTargetFlags() == PPCII::MO_DARWIN_HIDDEN_NONLAZY_PIC_BASE
        0)
      isImplicitlyPrivate = true;
    
    AP.Mang->getNameWithPrefix(Name, GV, isImplicitlyPrivate);
  }
  
  // If the target flags on the operand changes the name of the symbol, do that
  // before we return the symbol.
  switch (MO.getTargetFlags()) {
  default: break;
#if 0
  case X86II::MO_DARWIN_NONLAZY:
  case X86II::MO_DARWIN_NONLAZY_PIC_BASE: {
    Name += "$non_lazy_ptr";
    MCSymbol *Sym = Ctx.GetOrCreateSymbol(Name.str());
    
    MachineModuleInfoImpl::StubValueTy &StubSym =
      getMachOMMI(AP).getGVStubEntry(Sym);
    if (StubSym.getPointer() == 0) {
      assert(MO.isGlobal() && "Extern symbol not handled yet");
      StubSym =
      MachineModuleInfoImpl::
      StubValueTy(Mang->getSymbol(MO.getGlobal()),
                  !MO.getGlobal()->hasInternalLinkage());
    }
    return Sym;
  }
  case X86II::MO_DARWIN_HIDDEN_NONLAZY_PIC_BASE: {
    Name += "$non_lazy_ptr";
    MCSymbol *Sym = Ctx.GetOrCreateSymbol(Name.str());
    MachineModuleInfoImpl::StubValueTy &StubSym =
      getMachOMMI(AP).getHiddenGVStubEntry(Sym);
    if (StubSym.getPointer() == 0) {
      assert(MO.isGlobal() && "Extern symbol not handled yet");
      StubSym =
      MachineModuleInfoImpl::
      StubValueTy(Mang->getSymbol(MO.getGlobal()),
                  !MO.getGlobal()->hasInternalLinkage());
    }
    return Sym;
  }
#endif
  case PPCII::MO_DARWIN_STUB: {
    Name += "$stub";
    MCSymbol *Sym = Ctx.GetOrCreateSymbol(Name.str());
    MachineModuleInfoImpl::StubValueTy &StubSym =
      getMachOMMI(AP).getFnStubEntry(Sym);
    if (StubSym.getPointer())
      return Sym;
    
    if (MO.isGlobal()) {
      StubSym =
      MachineModuleInfoImpl::
      StubValueTy(AP.Mang->getSymbol(MO.getGlobal()),
                  !MO.getGlobal()->hasInternalLinkage());
    } else {
      Name.erase(Name.end()-5, Name.end());
      StubSym =
      MachineModuleInfoImpl::
      StubValueTy(Ctx.GetOrCreateSymbol(Name.str()), false);
    }
    return Sym;
  }
  }
  
  return Ctx.GetOrCreateSymbol(Name.str());
}

static MCOperand GetSymbolRef(const MachineOperand &MO, const MCSymbol *Symbol,
                              AsmPrinter &Printer) {
  MCContext &Ctx = Printer.OutContext;
  MCSymbolRefExpr::VariantKind RefKind = MCSymbolRefExpr::VK_None;

  const MCExpr *Expr = 0;
  switch (MO.getTargetFlags()) {
  default: assert(0 && "Unknown target flag on symbol operand");
  case PPCII::MO_NO_FLAG:
  // These affect the name of the symbol, not any suffix.
  case PPCII::MO_DARWIN_STUB:
    break;
      
  case PPCII::MO_LO16: RefKind = MCSymbolRefExpr::VK_PPC_LO16; break;
  case PPCII::MO_HA16: RefKind = MCSymbolRefExpr::VK_PPC_HA16; break;
  case PPCII::MO_LO16_PIC: break;
  case PPCII::MO_HA16_PIC: break;
  }

  if (Expr == 0)
    Expr = MCSymbolRefExpr::Create(Symbol, RefKind, Ctx);

  if (!MO.isJTI() && MO.getOffset())
    Expr = MCBinaryExpr::CreateAdd(Expr,
                                   MCConstantExpr::Create(MO.getOffset(), Ctx),
                                   Ctx);

  // Subtract off the PIC base.
  if (MO.getTargetFlags() == PPCII::MO_LO16_PIC ||
      MO.getTargetFlags() == PPCII::MO_HA16_PIC) {
    const MachineFunction *MF = MO.getParent()->getParent()->getParent();
    
    const MCExpr *PB = MCSymbolRefExpr::Create(MF->getPICBaseSymbol(), Ctx);
    Expr = MCBinaryExpr::CreateSub(Expr, PB, Ctx);
    // FIXME: We have no way to make the result be VK_PPC_LO16/VK_PPC_HA16,
    // since it is not a symbol!
  }
  
  return MCOperand::CreateExpr(Expr);
}

void llvm::LowerPPCMachineInstrToMCInst(const MachineInstr *MI, MCInst &OutMI,
                                        AsmPrinter &AP) {
  OutMI.setOpcode(MI->getOpcode());
  
  for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i) {
    const MachineOperand &MO = MI->getOperand(i);
    
    MCOperand MCOp;
    switch (MO.getType()) {
    default:
      MI->dump();
      assert(0 && "unknown operand type");
    case MachineOperand::MO_Register:
      assert(!MO.getSubReg() && "Subregs should be eliminated!");
      MCOp = MCOperand::CreateReg(MO.getReg());
      break;
    case MachineOperand::MO_Immediate:
      MCOp = MCOperand::CreateImm(MO.getImm());
      break;
    case MachineOperand::MO_MachineBasicBlock:
      MCOp = MCOperand::CreateExpr(MCSymbolRefExpr::Create(
                                      MO.getMBB()->getSymbol(), AP.OutContext));
      break;
    case MachineOperand::MO_GlobalAddress:
    case MachineOperand::MO_ExternalSymbol:
      MCOp = GetSymbolRef(MO, GetSymbolFromOperand(MO, AP), AP);
      break;
    case MachineOperand::MO_JumpTableIndex:
      MCOp = GetSymbolRef(MO, AP.GetJTISymbol(MO.getIndex()), AP);
      break;
    case MachineOperand::MO_ConstantPoolIndex:
      MCOp = GetSymbolRef(MO, AP.GetCPISymbol(MO.getIndex()), AP);
      break;
    case MachineOperand::MO_BlockAddress:
      MCOp = GetSymbolRef(MO,AP.GetBlockAddressSymbol(MO.getBlockAddress()),AP);
      break;
    }
    
    OutMI.addOperand(MCOp);
  }
}
