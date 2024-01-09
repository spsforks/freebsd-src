//===- AMDGPUDisassembler.cpp - Disassembler for AMDGPU ISA ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//
//
/// \file
///
/// This file contains definition for AMDGPU ISA disassembler
//
//===----------------------------------------------------------------------===//

// ToDo: What to do with instruction suffixes (v_mov_b32 vs v_mov_b32_e32)?

#include "Disassembler/AMDGPUDisassembler.h"
#include "MCTargetDesc/AMDGPUMCTargetDesc.h"
#include "SIDefines.h"
#include "SIRegisterInfo.h"
#include "TargetInfo/AMDGPUTargetInfo.h"
#include "Utils/AMDGPUBaseInfo.h"
#include "llvm-c/DisassemblerTypes.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDecoderOps.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/AMDHSAKernelDescriptor.h"

using namespace llvm;

#define DEBUG_TYPE "amdgpu-disassembler"

#define SGPR_MAX                                                               \
  (isGFX10Plus() ? AMDGPU::EncValues::SGPR_MAX_GFX10                           \
                 : AMDGPU::EncValues::SGPR_MAX_SI)

using DecodeStatus = llvm::MCDisassembler::DecodeStatus;

AMDGPUDisassembler::AMDGPUDisassembler(const MCSubtargetInfo &STI,
                                       MCContext &Ctx, MCInstrInfo const *MCII)
    : MCDisassembler(STI, Ctx), MCII(MCII), MRI(*Ctx.getRegisterInfo()),
      MAI(*Ctx.getAsmInfo()), TargetMaxInstBytes(MAI.getMaxInstLength(&STI)) {
  // ToDo: AMDGPUDisassembler supports only VI ISA.
  if (!STI.hasFeature(AMDGPU::FeatureGCN3Encoding) && !isGFX10Plus())
    report_fatal_error("Disassembly not yet supported for subtarget");
}

inline static MCDisassembler::DecodeStatus
addOperand(MCInst &Inst, const MCOperand& Opnd) {
  Inst.addOperand(Opnd);
  return Opnd.isValid() ?
    MCDisassembler::Success :
    MCDisassembler::Fail;
}

static int insertNamedMCOperand(MCInst &MI, const MCOperand &Op,
                                uint16_t NameIdx) {
  int OpIdx = AMDGPU::getNamedOperandIdx(MI.getOpcode(), NameIdx);
  if (OpIdx != -1) {
    auto I = MI.begin();
    std::advance(I, OpIdx);
    MI.insert(I, Op);
  }
  return OpIdx;
}

static DecodeStatus decodeSOPPBrTarget(MCInst &Inst, unsigned Imm,
                                       uint64_t Addr,
                                       const MCDisassembler *Decoder) {
  auto DAsm = static_cast<const AMDGPUDisassembler*>(Decoder);

  // Our branches take a simm16, but we need two extra bits to account for the
  // factor of 4.
  APInt SignedOffset(18, Imm * 4, true);
  int64_t Offset = (SignedOffset.sext(64) + 4 + Addr).getSExtValue();

  if (DAsm->tryAddingSymbolicOperand(Inst, Offset, Addr, true, 2, 2, 0))
    return MCDisassembler::Success;
  return addOperand(Inst, MCOperand::createImm(Imm));
}

static DecodeStatus decodeSMEMOffset(MCInst &Inst, unsigned Imm, uint64_t Addr,
                                     const MCDisassembler *Decoder) {
  auto DAsm = static_cast<const AMDGPUDisassembler*>(Decoder);
  int64_t Offset;
  if (DAsm->isGFX12Plus()) { // GFX12 supports 24-bit signed offsets.
    Offset = SignExtend64<24>(Imm);
  } else if (DAsm->isVI()) { // VI supports 20-bit unsigned offsets.
    Offset = Imm & 0xFFFFF;
  } else { // GFX9+ supports 21-bit signed offsets.
    Offset = SignExtend64<21>(Imm);
  }
  return addOperand(Inst, MCOperand::createImm(Offset));
}

static DecodeStatus decodeBoolReg(MCInst &Inst, unsigned Val, uint64_t Addr,
                                  const MCDisassembler *Decoder) {
  auto DAsm = static_cast<const AMDGPUDisassembler*>(Decoder);
  return addOperand(Inst, DAsm->decodeBoolReg(Val));
}

static DecodeStatus decodeSplitBarrier(MCInst &Inst, unsigned Val,
                                       uint64_t Addr,
                                       const MCDisassembler *Decoder) {
  auto DAsm = static_cast<const AMDGPUDisassembler *>(Decoder);
  return addOperand(Inst, DAsm->decodeSplitBarrier(Val));
}

#define DECODE_OPERAND(StaticDecoderName, DecoderName)                         \
  static DecodeStatus StaticDecoderName(MCInst &Inst, unsigned Imm,            \
                                        uint64_t /*Addr*/,                     \
                                        const MCDisassembler *Decoder) {       \
    auto DAsm = static_cast<const AMDGPUDisassembler *>(Decoder);              \
    return addOperand(Inst, DAsm->DecoderName(Imm));                           \
  }

// Decoder for registers, decode directly using RegClassID. Imm(8-bit) is
// number of register. Used by VGPR only and AGPR only operands.
#define DECODE_OPERAND_REG_8(RegClass)                                         \
  static DecodeStatus Decode##RegClass##RegisterClass(                         \
      MCInst &Inst, unsigned Imm, uint64_t /*Addr*/,                           \
      const MCDisassembler *Decoder) {                                         \
    assert(Imm < (1 << 8) && "8-bit encoding");                                \
    auto DAsm = static_cast<const AMDGPUDisassembler *>(Decoder);              \
    return addOperand(                                                         \
        Inst, DAsm->createRegOperand(AMDGPU::RegClass##RegClassID, Imm));      \
  }

#define DECODE_SrcOp(Name, EncSize, OpWidth, EncImm, MandatoryLiteral,         \
                     ImmWidth)                                                 \
  static DecodeStatus Name(MCInst &Inst, unsigned Imm, uint64_t /*Addr*/,      \
                           const MCDisassembler *Decoder) {                    \
    assert(Imm < (1 << EncSize) && #EncSize "-bit encoding");                  \
    auto DAsm = static_cast<const AMDGPUDisassembler *>(Decoder);              \
    return addOperand(Inst,                                                    \
                      DAsm->decodeSrcOp(AMDGPUDisassembler::OpWidth, EncImm,   \
                                        MandatoryLiteral, ImmWidth));          \
  }

// Decoder for registers. Imm(7-bit) is number of register, uses decodeSrcOp to
// get register class. Used by SGPR only operands.
#define DECODE_OPERAND_REG_7(RegClass, OpWidth)                                \
  DECODE_SrcOp(Decode##RegClass##RegisterClass, 7, OpWidth, Imm, false, 0)

// Decoder for registers. Imm(10-bit): Imm{7-0} is number of register,
// Imm{9} is acc(agpr or vgpr) Imm{8} should be 0 (see VOP3Pe_SMFMAC).
// Set Imm{8} to 1 (IS_VGPR) to decode using 'enum10' from decodeSrcOp.
// Used by AV_ register classes (AGPR or VGPR only register operands).
#define DECODE_OPERAND_REG_AV10(RegClass, OpWidth)                             \
  DECODE_SrcOp(Decode##RegClass##RegisterClass, 10, OpWidth,                   \
               Imm | AMDGPU::EncValues::IS_VGPR, false, 0)

// Decoder for Src(9-bit encoding) registers only.
#define DECODE_OPERAND_SRC_REG_9(RegClass, OpWidth)                            \
  DECODE_SrcOp(decodeOperand_##RegClass, 9, OpWidth, Imm, false, 0)

// Decoder for Src(9-bit encoding) AGPR, register number encoded in 9bits, set
// Imm{9} to 1 (set acc) and decode using 'enum10' from decodeSrcOp, registers
// only.
#define DECODE_OPERAND_SRC_REG_A9(RegClass, OpWidth)                           \
  DECODE_SrcOp(decodeOperand_##RegClass, 9, OpWidth, Imm | 512, false, 0)

// Decoder for 'enum10' from decodeSrcOp, Imm{0-8} is 9-bit Src encoding
// Imm{9} is acc, registers only.
#define DECODE_SRC_OPERAND_REG_AV10(RegClass, OpWidth)                         \
  DECODE_SrcOp(decodeOperand_##RegClass, 10, OpWidth, Imm, false, 0)

// Decoder for RegisterOperands using 9-bit Src encoding. Operand can be
// register from RegClass or immediate. Registers that don't belong to RegClass
// will be decoded and InstPrinter will report warning. Immediate will be
// decoded into constant of size ImmWidth, should match width of immediate used
// by OperandType (important for floating point types).
#define DECODE_OPERAND_SRC_REG_OR_IMM_9(RegClass, OpWidth, ImmWidth)           \
  DECODE_SrcOp(decodeOperand_##RegClass##_Imm##ImmWidth, 9, OpWidth, Imm,      \
               false, ImmWidth)

#define DECODE_OPERAND_SRC_REG_OR_IMM_9_TYPED(Name, OpWidth, ImmWidth)         \
  DECODE_SrcOp(decodeOperand_##Name, 9, OpWidth, Imm, false, ImmWidth)

// Decoder for Src(9-bit encoding) AGPR or immediate. Set Imm{9} to 1 (set acc)
// and decode using 'enum10' from decodeSrcOp.
#define DECODE_OPERAND_SRC_REG_OR_IMM_A9(RegClass, OpWidth, ImmWidth)          \
  DECODE_SrcOp(decodeOperand_##RegClass##_Imm##ImmWidth, 9, OpWidth,           \
               Imm | 512, false, ImmWidth)

#define DECODE_OPERAND_SRC_REG_OR_IMM_DEFERRED_9(RegClass, OpWidth, ImmWidth)  \
  DECODE_SrcOp(decodeOperand_##RegClass##_Deferred##_Imm##ImmWidth, 9,         \
               OpWidth, Imm, true, ImmWidth)

// Default decoders generated by tablegen: 'Decode<RegClass>RegisterClass'
// when RegisterClass is used as an operand. Most often used for destination
// operands.

DECODE_OPERAND_REG_8(VGPR_32)
DECODE_OPERAND_REG_8(VGPR_32_Lo128)
DECODE_OPERAND_REG_8(VReg_64)
DECODE_OPERAND_REG_8(VReg_96)
DECODE_OPERAND_REG_8(VReg_128)
DECODE_OPERAND_REG_8(VReg_256)
DECODE_OPERAND_REG_8(VReg_288)
DECODE_OPERAND_REG_8(VReg_352)
DECODE_OPERAND_REG_8(VReg_384)
DECODE_OPERAND_REG_8(VReg_512)
DECODE_OPERAND_REG_8(VReg_1024)

DECODE_OPERAND_REG_7(SReg_32, OPW32)
DECODE_OPERAND_REG_7(SReg_32_XEXEC, OPW32)
DECODE_OPERAND_REG_7(SReg_32_XM0_XEXEC, OPW32)
DECODE_OPERAND_REG_7(SReg_32_XEXEC_HI, OPW32)
DECODE_OPERAND_REG_7(SReg_64, OPW64)
DECODE_OPERAND_REG_7(SReg_64_XEXEC, OPW64)
DECODE_OPERAND_REG_7(SReg_96, OPW96)
DECODE_OPERAND_REG_7(SReg_128, OPW128)
DECODE_OPERAND_REG_7(SReg_256, OPW256)
DECODE_OPERAND_REG_7(SReg_512, OPW512)

DECODE_OPERAND_REG_8(AGPR_32)
DECODE_OPERAND_REG_8(AReg_64)
DECODE_OPERAND_REG_8(AReg_128)
DECODE_OPERAND_REG_8(AReg_256)
DECODE_OPERAND_REG_8(AReg_512)
DECODE_OPERAND_REG_8(AReg_1024)

DECODE_OPERAND_REG_AV10(AVDst_128, OPW128)
DECODE_OPERAND_REG_AV10(AVDst_512, OPW512)

// Decoders for register only source RegisterOperands that use use 9-bit Src
// encoding: 'decodeOperand_<RegClass>'.

DECODE_OPERAND_SRC_REG_9(VGPR_32, OPW32)
DECODE_OPERAND_SRC_REG_9(VReg_64, OPW64)
DECODE_OPERAND_SRC_REG_9(VReg_128, OPW128)
DECODE_OPERAND_SRC_REG_9(VReg_256, OPW256)
DECODE_OPERAND_SRC_REG_9(VRegOrLds_32, OPW32)

DECODE_OPERAND_SRC_REG_A9(AGPR_32, OPW32)

DECODE_SRC_OPERAND_REG_AV10(AV_32, OPW32)
DECODE_SRC_OPERAND_REG_AV10(AV_64, OPW64)
DECODE_SRC_OPERAND_REG_AV10(AV_128, OPW128)

// Decoders for register or immediate RegisterOperands that use 9-bit Src
// encoding: 'decodeOperand_<RegClass>_Imm<ImmWidth>'.

DECODE_OPERAND_SRC_REG_OR_IMM_9(SReg_64, OPW64, 64)
DECODE_OPERAND_SRC_REG_OR_IMM_9(SReg_32, OPW32, 32)
DECODE_OPERAND_SRC_REG_OR_IMM_9(SReg_32, OPW32, 16)
DECODE_OPERAND_SRC_REG_OR_IMM_9(SRegOrLds_32, OPW32, 32)
DECODE_OPERAND_SRC_REG_OR_IMM_9(VS_32_Lo128, OPW16, 16)
DECODE_OPERAND_SRC_REG_OR_IMM_9(VS_32, OPW32, 16)
DECODE_OPERAND_SRC_REG_OR_IMM_9(VS_32, OPW32, 32)
DECODE_OPERAND_SRC_REG_OR_IMM_9(VS_64, OPW64, 64)
DECODE_OPERAND_SRC_REG_OR_IMM_9(VS_64, OPW64, 32)
DECODE_OPERAND_SRC_REG_OR_IMM_9(VReg_64, OPW64, 64)
DECODE_OPERAND_SRC_REG_OR_IMM_9(VReg_128, OPW128, 32)
DECODE_OPERAND_SRC_REG_OR_IMM_9(VReg_256, OPW256, 64)
DECODE_OPERAND_SRC_REG_OR_IMM_9(VReg_512, OPW512, 32)
DECODE_OPERAND_SRC_REG_OR_IMM_9(VReg_1024, OPW1024, 32)

DECODE_OPERAND_SRC_REG_OR_IMM_9_TYPED(VS_32_ImmV2I16, OPW32, 32)
DECODE_OPERAND_SRC_REG_OR_IMM_9_TYPED(VS_32_ImmV2F16, OPW32, 16)

DECODE_OPERAND_SRC_REG_OR_IMM_A9(AReg_64, OPW64, 64)
DECODE_OPERAND_SRC_REG_OR_IMM_A9(AReg_128, OPW128, 32)
DECODE_OPERAND_SRC_REG_OR_IMM_A9(AReg_256, OPW256, 64)
DECODE_OPERAND_SRC_REG_OR_IMM_A9(AReg_512, OPW512, 32)
DECODE_OPERAND_SRC_REG_OR_IMM_A9(AReg_1024, OPW1024, 32)

DECODE_OPERAND_SRC_REG_OR_IMM_DEFERRED_9(VS_32_Lo128, OPW16, 16)
DECODE_OPERAND_SRC_REG_OR_IMM_DEFERRED_9(VS_32, OPW16, 16)
DECODE_OPERAND_SRC_REG_OR_IMM_DEFERRED_9(VS_32, OPW32, 32)
DECODE_OPERAND_SRC_REG_OR_IMM_DEFERRED_9(SReg_32, OPW32, 32)

static DecodeStatus DecodeVGPR_16RegisterClass(MCInst &Inst, unsigned Imm,
                                               uint64_t /*Addr*/,
                                               const MCDisassembler *Decoder) {
  assert(isUInt<10>(Imm) && "10-bit encoding expected");
  assert((Imm & (1 << 8)) == 0 && "Imm{8} should not be used");

  bool IsHi = Imm & (1 << 9);
  unsigned RegIdx = Imm & 0xff;
  auto DAsm = static_cast<const AMDGPUDisassembler *>(Decoder);
  return addOperand(Inst, DAsm->createVGPR16Operand(RegIdx, IsHi));
}

static DecodeStatus
DecodeVGPR_16_Lo128RegisterClass(MCInst &Inst, unsigned Imm, uint64_t /*Addr*/,
                                 const MCDisassembler *Decoder) {
  assert(isUInt<8>(Imm) && "8-bit encoding expected");

  bool IsHi = Imm & (1 << 7);
  unsigned RegIdx = Imm & 0x7f;
  auto DAsm = static_cast<const AMDGPUDisassembler *>(Decoder);
  return addOperand(Inst, DAsm->createVGPR16Operand(RegIdx, IsHi));
}

static DecodeStatus decodeOperand_VSrcT16_Lo128(MCInst &Inst, unsigned Imm,
                                                uint64_t /*Addr*/,
                                                const MCDisassembler *Decoder) {
  assert(isUInt<9>(Imm) && "9-bit encoding expected");

  const auto *DAsm = static_cast<const AMDGPUDisassembler *>(Decoder);
  bool IsVGPR = Imm & (1 << 8);
  if (IsVGPR) {
    bool IsHi = Imm & (1 << 7);
    unsigned RegIdx = Imm & 0x7f;
    return addOperand(Inst, DAsm->createVGPR16Operand(RegIdx, IsHi));
  }
  return addOperand(Inst, DAsm->decodeNonVGPRSrcOp(AMDGPUDisassembler::OPW16,
                                                   Imm & 0xFF, false, 16));
}

static DecodeStatus decodeOperand_VSrcT16(MCInst &Inst, unsigned Imm,
                                          uint64_t /*Addr*/,
                                          const MCDisassembler *Decoder) {
  assert(isUInt<10>(Imm) && "10-bit encoding expected");

  const auto *DAsm = static_cast<const AMDGPUDisassembler *>(Decoder);
  bool IsVGPR = Imm & (1 << 8);
  if (IsVGPR) {
    bool IsHi = Imm & (1 << 9);
    unsigned RegIdx = Imm & 0xff;
    return addOperand(Inst, DAsm->createVGPR16Operand(RegIdx, IsHi));
  }
  return addOperand(Inst, DAsm->decodeNonVGPRSrcOp(AMDGPUDisassembler::OPW16,
                                                   Imm & 0xFF, false, 16));
}

static DecodeStatus decodeOperand_KImmFP(MCInst &Inst, unsigned Imm,
                                         uint64_t Addr,
                                         const MCDisassembler *Decoder) {
  const auto *DAsm = static_cast<const AMDGPUDisassembler *>(Decoder);
  return addOperand(Inst, DAsm->decodeMandatoryLiteralConstant(Imm));
}

static DecodeStatus decodeOperandVOPDDstY(MCInst &Inst, unsigned Val,
                                          uint64_t Addr, const void *Decoder) {
  const auto *DAsm = static_cast<const AMDGPUDisassembler *>(Decoder);
  return addOperand(Inst, DAsm->decodeVOPDDstYOp(Inst, Val));
}

static bool IsAGPROperand(const MCInst &Inst, int OpIdx,
                          const MCRegisterInfo *MRI) {
  if (OpIdx < 0)
    return false;

  const MCOperand &Op = Inst.getOperand(OpIdx);
  if (!Op.isReg())
    return false;

  unsigned Sub = MRI->getSubReg(Op.getReg(), AMDGPU::sub0);
  auto Reg = Sub ? Sub : Op.getReg();
  return Reg >= AMDGPU::AGPR0 && Reg <= AMDGPU::AGPR255;
}

static DecodeStatus decodeOperand_AVLdSt_Any(MCInst &Inst, unsigned Imm,
                                             AMDGPUDisassembler::OpWidthTy Opw,
                                             const MCDisassembler *Decoder) {
  auto DAsm = static_cast<const AMDGPUDisassembler*>(Decoder);
  if (!DAsm->isGFX90A()) {
    Imm &= 511;
  } else {
    // If atomic has both vdata and vdst their register classes are tied.
    // The bit is decoded along with the vdst, first operand. We need to
    // change register class to AGPR if vdst was AGPR.
    // If a DS instruction has both data0 and data1 their register classes
    // are also tied.
    unsigned Opc = Inst.getOpcode();
    uint64_t TSFlags = DAsm->getMCII()->get(Opc).TSFlags;
    uint16_t DataNameIdx = (TSFlags & SIInstrFlags::DS) ? AMDGPU::OpName::data0
                                                        : AMDGPU::OpName::vdata;
    const MCRegisterInfo *MRI = DAsm->getContext().getRegisterInfo();
    int DataIdx = AMDGPU::getNamedOperandIdx(Opc, DataNameIdx);
    if ((int)Inst.getNumOperands() == DataIdx) {
      int DstIdx = AMDGPU::getNamedOperandIdx(Opc, AMDGPU::OpName::vdst);
      if (IsAGPROperand(Inst, DstIdx, MRI))
        Imm |= 512;
    }

    if (TSFlags & SIInstrFlags::DS) {
      int Data2Idx = AMDGPU::getNamedOperandIdx(Opc, AMDGPU::OpName::data1);
      if ((int)Inst.getNumOperands() == Data2Idx &&
          IsAGPROperand(Inst, DataIdx, MRI))
        Imm |= 512;
    }
  }
  return addOperand(Inst, DAsm->decodeSrcOp(Opw, Imm | 256));
}

static DecodeStatus decodeOperand_VSrc_f64(MCInst &Inst, unsigned Imm,
                                           uint64_t Addr,
                                           const MCDisassembler *Decoder) {
  assert(Imm < (1 << 9) && "9-bit encoding");
  auto DAsm = static_cast<const AMDGPUDisassembler *>(Decoder);
  return addOperand(
      Inst, DAsm->decodeSrcOp(AMDGPUDisassembler::OPW64, Imm, false, 64, true));
}

static DecodeStatus
DecodeAVLdSt_32RegisterClass(MCInst &Inst, unsigned Imm, uint64_t Addr,
                             const MCDisassembler *Decoder) {
  return decodeOperand_AVLdSt_Any(Inst, Imm,
                                  AMDGPUDisassembler::OPW32, Decoder);
}

static DecodeStatus
DecodeAVLdSt_64RegisterClass(MCInst &Inst, unsigned Imm, uint64_t Addr,
                             const MCDisassembler *Decoder) {
  return decodeOperand_AVLdSt_Any(Inst, Imm,
                                  AMDGPUDisassembler::OPW64, Decoder);
}

static DecodeStatus
DecodeAVLdSt_96RegisterClass(MCInst &Inst, unsigned Imm, uint64_t Addr,
                             const MCDisassembler *Decoder) {
  return decodeOperand_AVLdSt_Any(Inst, Imm,
                                  AMDGPUDisassembler::OPW96, Decoder);
}

static DecodeStatus
DecodeAVLdSt_128RegisterClass(MCInst &Inst, unsigned Imm, uint64_t Addr,
                              const MCDisassembler *Decoder) {
  return decodeOperand_AVLdSt_Any(Inst, Imm,
                                  AMDGPUDisassembler::OPW128, Decoder);
}

static DecodeStatus
DecodeAVLdSt_160RegisterClass(MCInst &Inst, unsigned Imm, uint64_t Addr,
                              const MCDisassembler *Decoder) {
  return decodeOperand_AVLdSt_Any(Inst, Imm, AMDGPUDisassembler::OPW160,
                                  Decoder);
}

#define DECODE_SDWA(DecName) \
DECODE_OPERAND(decodeSDWA##DecName, decodeSDWA##DecName)

DECODE_SDWA(Src32)
DECODE_SDWA(Src16)
DECODE_SDWA(VopcDst)

#include "AMDGPUGenDisassemblerTables.inc"

//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

template <typename T> static inline T eatBytes(ArrayRef<uint8_t>& Bytes) {
  assert(Bytes.size() >= sizeof(T));
  const auto Res =
      support::endian::read<T, llvm::endianness::little>(Bytes.data());
  Bytes = Bytes.slice(sizeof(T));
  return Res;
}

static inline DecoderUInt128 eat12Bytes(ArrayRef<uint8_t> &Bytes) {
  assert(Bytes.size() >= 12);
  uint64_t Lo =
      support::endian::read<uint64_t, llvm::endianness::little>(Bytes.data());
  Bytes = Bytes.slice(8);
  uint64_t Hi =
      support::endian::read<uint32_t, llvm::endianness::little>(Bytes.data());
  Bytes = Bytes.slice(4);
  return DecoderUInt128(Lo, Hi);
}

// The disassembler is greedy, so we need to check FI operand value to
// not parse a dpp if the correct literal is not set. For dpp16 the
// autogenerated decoder checks the dpp literal
static bool isValidDPP8(const MCInst &MI) {
  using namespace llvm::AMDGPU::DPP;
  int FiIdx = AMDGPU::getNamedOperandIdx(MI.getOpcode(), AMDGPU::OpName::fi);
  assert(FiIdx != -1);
  if ((unsigned)FiIdx >= MI.getNumOperands())
    return false;
  unsigned Fi = MI.getOperand(FiIdx).getImm();
  return Fi == DPP8_FI_0 || Fi == DPP8_FI_1;
}

DecodeStatus AMDGPUDisassembler::getInstruction(MCInst &MI, uint64_t &Size,
                                                ArrayRef<uint8_t> Bytes_,
                                                uint64_t Address,
                                                raw_ostream &CS) const {
  bool IsSDWA = false;

  unsigned MaxInstBytesNum = std::min((size_t)TargetMaxInstBytes, Bytes_.size());
  Bytes = Bytes_.slice(0, MaxInstBytesNum);

  DecodeStatus Res = MCDisassembler::Fail;
  do {
    // ToDo: better to switch encoding length using some bit predicate
    // but it is unknown yet, so try all we can

    // Try to decode DPP and SDWA first to solve conflict with VOP1 and VOP2
    // encodings
    if (isGFX11Plus() && Bytes.size() >= 12 ) {
      DecoderUInt128 DecW = eat12Bytes(Bytes);
      Res =
          tryDecodeInst(DecoderTableDPP8GFX1196, DecoderTableDPP8GFX11_FAKE1696,
                        MI, DecW, Address, CS);
      if (Res && convertDPP8Inst(MI) == MCDisassembler::Success)
        break;
      MI = MCInst(); // clear
      Res =
          tryDecodeInst(DecoderTableDPP8GFX1296, DecoderTableDPP8GFX12_FAKE1696,
                        MI, DecW, Address, CS);
      if (Res && convertDPP8Inst(MI) == MCDisassembler::Success)
        break;
      MI = MCInst(); // clear

      const auto convertVOPDPP = [&]() {
        if (MCII->get(MI.getOpcode()).TSFlags & SIInstrFlags::VOP3P) {
          convertVOP3PDPPInst(MI);
        } else if (AMDGPU::isVOPC64DPP(MI.getOpcode())) {
          convertVOPCDPPInst(MI); // Special VOP3 case
        } else {
          assert(MCII->get(MI.getOpcode()).TSFlags & SIInstrFlags::VOP3);
          convertVOP3DPPInst(MI); // Regular VOP3 case
        }
      };
      Res = tryDecodeInst(DecoderTableDPPGFX1196, DecoderTableDPPGFX11_FAKE1696,
                          MI, DecW, Address, CS);
      if (Res) {
        convertVOPDPP();
        break;
      }
      Res = tryDecodeInst(DecoderTableDPPGFX1296, DecoderTableDPPGFX12_FAKE1696,
                          MI, DecW, Address, CS);
      if (Res) {
        convertVOPDPP();
        break;
      }
      Res = tryDecodeInst(DecoderTableGFX1196, MI, DecW, Address, CS);
      if (Res)
        break;

      Res = tryDecodeInst(DecoderTableGFX1296, MI, DecW, Address, CS);
      if (Res)
        break;
    }
    // Reinitialize Bytes
    Bytes = Bytes_.slice(0, MaxInstBytesNum);

    if (Bytes.size() >= 8) {
      const uint64_t QW = eatBytes<uint64_t>(Bytes);

      if (STI.hasFeature(AMDGPU::FeatureGFX10_BEncoding)) {
        Res = tryDecodeInst(DecoderTableGFX10_B64, MI, QW, Address, CS);
        if (Res) {
          if (AMDGPU::getNamedOperandIdx(MI.getOpcode(), AMDGPU::OpName::dpp8)
              == -1)
            break;
          if (convertDPP8Inst(MI) == MCDisassembler::Success)
            break;
          MI = MCInst(); // clear
        }
      }

      Res = tryDecodeInst(DecoderTableDPP864, MI, QW, Address, CS);
      if (Res && convertDPP8Inst(MI) == MCDisassembler::Success)
        break;
      MI = MCInst(); // clear

      Res = tryDecodeInst(DecoderTableDPP8GFX1164,
                          DecoderTableDPP8GFX11_FAKE1664, MI, QW, Address, CS);
      if (Res && convertDPP8Inst(MI) == MCDisassembler::Success)
        break;
      MI = MCInst(); // clear

      Res = tryDecodeInst(DecoderTableDPP8GFX1264,
                          DecoderTableDPP8GFX12_FAKE1664, MI, QW, Address, CS);
      if (Res && convertDPP8Inst(MI) == MCDisassembler::Success)
        break;
      MI = MCInst(); // clear

      Res = tryDecodeInst(DecoderTableDPP64, MI, QW, Address, CS);
      if (Res) break;

      Res = tryDecodeInst(DecoderTableDPPGFX1164, DecoderTableDPPGFX11_FAKE1664,
                          MI, QW, Address, CS);
      if (Res) {
        if (MCII->get(MI.getOpcode()).TSFlags & SIInstrFlags::VOPC)
          convertVOPCDPPInst(MI);
        break;
      }

      Res = tryDecodeInst(DecoderTableDPPGFX1264, DecoderTableDPPGFX12_FAKE1664,
                          MI, QW, Address, CS);
      if (Res) {
        if (MCII->get(MI.getOpcode()).TSFlags & SIInstrFlags::VOPC)
          convertVOPCDPPInst(MI);
        break;
      }

      Res = tryDecodeInst(DecoderTableSDWA64, MI, QW, Address, CS);
      if (Res) { IsSDWA = true;  break; }

      Res = tryDecodeInst(DecoderTableSDWA964, MI, QW, Address, CS);
      if (Res) { IsSDWA = true;  break; }

      Res = tryDecodeInst(DecoderTableSDWA1064, MI, QW, Address, CS);
      if (Res) { IsSDWA = true;  break; }

      if (STI.hasFeature(AMDGPU::FeatureUnpackedD16VMem)) {
        Res = tryDecodeInst(DecoderTableGFX80_UNPACKED64, MI, QW, Address, CS);
        if (Res)
          break;
      }

      // Some GFX9 subtargets repurposed the v_mad_mix_f32, v_mad_mixlo_f16 and
      // v_mad_mixhi_f16 for FMA variants. Try to decode using this special
      // table first so we print the correct name.
      if (STI.hasFeature(AMDGPU::FeatureFmaMixInsts)) {
        Res = tryDecodeInst(DecoderTableGFX9_DL64, MI, QW, Address, CS);
        if (Res)
          break;
      }
    }

    // Reinitialize Bytes as DPP64 could have eaten too much
    Bytes = Bytes_.slice(0, MaxInstBytesNum);

    // Try decode 32-bit instruction
    if (Bytes.size() < 4) break;
    const uint32_t DW = eatBytes<uint32_t>(Bytes);
    Res = tryDecodeInst(DecoderTableGFX832, MI, DW, Address, CS);
    if (Res) break;

    Res = tryDecodeInst(DecoderTableAMDGPU32, MI, DW, Address, CS);
    if (Res) break;

    Res = tryDecodeInst(DecoderTableGFX932, MI, DW, Address, CS);
    if (Res) break;

    if (STI.hasFeature(AMDGPU::FeatureGFX90AInsts)) {
      Res = tryDecodeInst(DecoderTableGFX90A32, MI, DW, Address, CS);
      if (Res)
        break;
    }

    if (STI.hasFeature(AMDGPU::FeatureGFX10_BEncoding)) {
      Res = tryDecodeInst(DecoderTableGFX10_B32, MI, DW, Address, CS);
      if (Res) break;
    }

    Res = tryDecodeInst(DecoderTableGFX1032, MI, DW, Address, CS);
    if (Res) break;

    Res = tryDecodeInst(DecoderTableGFX1132, DecoderTableGFX11_FAKE1632, MI, DW,
                        Address, CS);
    if (Res) break;

    Res = tryDecodeInst(DecoderTableGFX1232, DecoderTableGFX12_FAKE1632, MI, DW,
                        Address, CS);
    if (Res)
      break;

    if (Bytes.size() < 4) break;
    const uint64_t QW = ((uint64_t)eatBytes<uint32_t>(Bytes) << 32) | DW;

    if (STI.hasFeature(AMDGPU::FeatureGFX940Insts)) {
      Res = tryDecodeInst(DecoderTableGFX94064, MI, QW, Address, CS);
      if (Res)
        break;
    }

    if (STI.hasFeature(AMDGPU::FeatureGFX90AInsts)) {
      Res = tryDecodeInst(DecoderTableGFX90A64, MI, QW, Address, CS);
      if (Res)
        break;
    }

    Res = tryDecodeInst(DecoderTableGFX864, MI, QW, Address, CS);
    if (Res) break;

    Res = tryDecodeInst(DecoderTableAMDGPU64, MI, QW, Address, CS);
    if (Res) break;

    Res = tryDecodeInst(DecoderTableGFX964, MI, QW, Address, CS);
    if (Res) break;

    Res = tryDecodeInst(DecoderTableGFX1064, MI, QW, Address, CS);
    if (Res) break;

    Res = tryDecodeInst(DecoderTableGFX1264, DecoderTableGFX12_FAKE1664, MI, QW,
                        Address, CS);
    if (Res)
      break;

    Res = tryDecodeInst(DecoderTableGFX1164, DecoderTableGFX11_FAKE1664, MI, QW,
                        Address, CS);
    if (Res)
      break;

    Res = tryDecodeInst(DecoderTableWMMAGFX1164, MI, QW, Address, CS);
  } while (false);

  if (Res && AMDGPU::isMAC(MI.getOpcode())) {
    // Insert dummy unused src2_modifiers.
    insertNamedMCOperand(MI, MCOperand::createImm(0),
                         AMDGPU::OpName::src2_modifiers);
  }

  if (Res && (MCII->get(MI.getOpcode()).TSFlags & SIInstrFlags::DS) &&
      !AMDGPU::hasGDS(STI)) {
    insertNamedMCOperand(MI, MCOperand::createImm(0), AMDGPU::OpName::gds);
  }

  if (Res && (MCII->get(MI.getOpcode()).TSFlags &
          (SIInstrFlags::MUBUF | SIInstrFlags::FLAT | SIInstrFlags::SMRD))) {
    int CPolPos = AMDGPU::getNamedOperandIdx(MI.getOpcode(),
                                             AMDGPU::OpName::cpol);
    if (CPolPos != -1) {
      unsigned CPol =
          (MCII->get(MI.getOpcode()).TSFlags & SIInstrFlags::IsAtomicRet) ?
              AMDGPU::CPol::GLC : 0;
      if (MI.getNumOperands() <= (unsigned)CPolPos) {
        insertNamedMCOperand(MI, MCOperand::createImm(CPol),
                             AMDGPU::OpName::cpol);
      } else if (CPol) {
        MI.getOperand(CPolPos).setImm(MI.getOperand(CPolPos).getImm() | CPol);
      }
    }
  }

  if (Res && (MCII->get(MI.getOpcode()).TSFlags &
              (SIInstrFlags::MTBUF | SIInstrFlags::MUBUF)) &&
             (STI.hasFeature(AMDGPU::FeatureGFX90AInsts))) {
    // GFX90A lost TFE, its place is occupied by ACC.
    int TFEOpIdx =
        AMDGPU::getNamedOperandIdx(MI.getOpcode(), AMDGPU::OpName::tfe);
    if (TFEOpIdx != -1) {
      auto TFEIter = MI.begin();
      std::advance(TFEIter, TFEOpIdx);
      MI.insert(TFEIter, MCOperand::createImm(0));
    }
  }

  if (Res && (MCII->get(MI.getOpcode()).TSFlags &
              (SIInstrFlags::MTBUF | SIInstrFlags::MUBUF))) {
    int SWZOpIdx =
        AMDGPU::getNamedOperandIdx(MI.getOpcode(), AMDGPU::OpName::swz);
    if (SWZOpIdx != -1) {
      auto SWZIter = MI.begin();
      std::advance(SWZIter, SWZOpIdx);
      MI.insert(SWZIter, MCOperand::createImm(0));
    }
  }

  if (Res && (MCII->get(MI.getOpcode()).TSFlags & SIInstrFlags::MIMG)) {
    int VAddr0Idx =
        AMDGPU::getNamedOperandIdx(MI.getOpcode(), AMDGPU::OpName::vaddr0);
    int RsrcIdx =
        AMDGPU::getNamedOperandIdx(MI.getOpcode(), AMDGPU::OpName::srsrc);
    unsigned NSAArgs = RsrcIdx - VAddr0Idx - 1;
    if (VAddr0Idx >= 0 && NSAArgs > 0) {
      unsigned NSAWords = (NSAArgs + 3) / 4;
      if (Bytes.size() < 4 * NSAWords) {
        Res = MCDisassembler::Fail;
      } else {
        for (unsigned i = 0; i < NSAArgs; ++i) {
          const unsigned VAddrIdx = VAddr0Idx + 1 + i;
          auto VAddrRCID =
              MCII->get(MI.getOpcode()).operands()[VAddrIdx].RegClass;
          MI.insert(MI.begin() + VAddrIdx,
                    createRegOperand(VAddrRCID, Bytes[i]));
        }
        Bytes = Bytes.slice(4 * NSAWords);
      }
    }

    if (Res)
      Res = convertMIMGInst(MI);
  }

  if (Res && (MCII->get(MI.getOpcode()).TSFlags &
              (SIInstrFlags::VIMAGE | SIInstrFlags::VSAMPLE)))
    Res = convertMIMGInst(MI);

  if (Res && (MCII->get(MI.getOpcode()).TSFlags & SIInstrFlags::EXP))
    Res = convertEXPInst(MI);

  if (Res && (MCII->get(MI.getOpcode()).TSFlags & SIInstrFlags::VINTERP))
    Res = convertVINTERPInst(MI);

  if (Res && IsSDWA)
    Res = convertSDWAInst(MI);

  int VDstIn_Idx = AMDGPU::getNamedOperandIdx(MI.getOpcode(),
                                              AMDGPU::OpName::vdst_in);
  if (VDstIn_Idx != -1) {
    int Tied = MCII->get(MI.getOpcode()).getOperandConstraint(VDstIn_Idx,
                           MCOI::OperandConstraint::TIED_TO);
    if (Tied != -1 && (MI.getNumOperands() <= (unsigned)VDstIn_Idx ||
         !MI.getOperand(VDstIn_Idx).isReg() ||
         MI.getOperand(VDstIn_Idx).getReg() != MI.getOperand(Tied).getReg())) {
      if (MI.getNumOperands() > (unsigned)VDstIn_Idx)
        MI.erase(&MI.getOperand(VDstIn_Idx));
      insertNamedMCOperand(MI,
        MCOperand::createReg(MI.getOperand(Tied).getReg()),
        AMDGPU::OpName::vdst_in);
    }
  }

  int ImmLitIdx =
      AMDGPU::getNamedOperandIdx(MI.getOpcode(), AMDGPU::OpName::imm);
  bool IsSOPK = MCII->get(MI.getOpcode()).TSFlags & SIInstrFlags::SOPK;
  if (Res && ImmLitIdx != -1 && !IsSOPK)
    Res = convertFMAanyK(MI, ImmLitIdx);

  // if the opcode was not recognized we'll assume a Size of 4 bytes
  // (unless there are fewer bytes left)
  Size = Res ? (MaxInstBytesNum - Bytes.size())
             : std::min((size_t)4, Bytes_.size());
  return Res;
}

DecodeStatus AMDGPUDisassembler::convertEXPInst(MCInst &MI) const {
  if (STI.hasFeature(AMDGPU::FeatureGFX11Insts)) {
    // The MCInst still has these fields even though they are no longer encoded
    // in the GFX11 instruction.
    insertNamedMCOperand(MI, MCOperand::createImm(0), AMDGPU::OpName::vm);
    insertNamedMCOperand(MI, MCOperand::createImm(0), AMDGPU::OpName::compr);
  }
  return MCDisassembler::Success;
}

DecodeStatus AMDGPUDisassembler::convertVINTERPInst(MCInst &MI) const {
  if (MI.getOpcode() == AMDGPU::V_INTERP_P10_F16_F32_inreg_gfx11 ||
      MI.getOpcode() == AMDGPU::V_INTERP_P10_F16_F32_inreg_gfx12 ||
      MI.getOpcode() == AMDGPU::V_INTERP_P10_RTZ_F16_F32_inreg_gfx11 ||
      MI.getOpcode() == AMDGPU::V_INTERP_P10_RTZ_F16_F32_inreg_gfx12 ||
      MI.getOpcode() == AMDGPU::V_INTERP_P2_F16_F32_inreg_gfx11 ||
      MI.getOpcode() == AMDGPU::V_INTERP_P2_F16_F32_inreg_gfx12 ||
      MI.getOpcode() == AMDGPU::V_INTERP_P2_RTZ_F16_F32_inreg_gfx11 ||
      MI.getOpcode() == AMDGPU::V_INTERP_P2_RTZ_F16_F32_inreg_gfx12) {
    // The MCInst has this field that is not directly encoded in the
    // instruction.
    insertNamedMCOperand(MI, MCOperand::createImm(0), AMDGPU::OpName::op_sel);
  }
  return MCDisassembler::Success;
}

DecodeStatus AMDGPUDisassembler::convertSDWAInst(MCInst &MI) const {
  if (STI.hasFeature(AMDGPU::FeatureGFX9) ||
      STI.hasFeature(AMDGPU::FeatureGFX10)) {
    if (AMDGPU::hasNamedOperand(MI.getOpcode(), AMDGPU::OpName::sdst))
      // VOPC - insert clamp
      insertNamedMCOperand(MI, MCOperand::createImm(0), AMDGPU::OpName::clamp);
  } else if (STI.hasFeature(AMDGPU::FeatureVolcanicIslands)) {
    int SDst = AMDGPU::getNamedOperandIdx(MI.getOpcode(), AMDGPU::OpName::sdst);
    if (SDst != -1) {
      // VOPC - insert VCC register as sdst
      insertNamedMCOperand(MI, createRegOperand(AMDGPU::VCC),
                           AMDGPU::OpName::sdst);
    } else {
      // VOP1/2 - insert omod if present in instruction
      insertNamedMCOperand(MI, MCOperand::createImm(0), AMDGPU::OpName::omod);
    }
  }
  return MCDisassembler::Success;
}

struct VOPModifiers {
  unsigned OpSel = 0;
  unsigned OpSelHi = 0;
  unsigned NegLo = 0;
  unsigned NegHi = 0;
};

// Reconstruct values of VOP3/VOP3P operands such as op_sel.
// Note that these values do not affect disassembler output,
// so this is only necessary for consistency with src_modifiers.
static VOPModifiers collectVOPModifiers(const MCInst &MI,
                                        bool IsVOP3P = false) {
  VOPModifiers Modifiers;
  unsigned Opc = MI.getOpcode();
  const int ModOps[] = {AMDGPU::OpName::src0_modifiers,
                        AMDGPU::OpName::src1_modifiers,
                        AMDGPU::OpName::src2_modifiers};
  for (int J = 0; J < 3; ++J) {
    int OpIdx = AMDGPU::getNamedOperandIdx(Opc, ModOps[J]);
    if (OpIdx == -1)
      continue;

    unsigned Val = MI.getOperand(OpIdx).getImm();

    Modifiers.OpSel |= !!(Val & SISrcMods::OP_SEL_0) << J;
    if (IsVOP3P) {
      Modifiers.OpSelHi |= !!(Val & SISrcMods::OP_SEL_1) << J;
      Modifiers.NegLo |= !!(Val & SISrcMods::NEG) << J;
      Modifiers.NegHi |= !!(Val & SISrcMods::NEG_HI) << J;
    } else if (J == 0) {
      Modifiers.OpSel |= !!(Val & SISrcMods::DST_OP_SEL) << 3;
    }
  }

  return Modifiers;
}

// MAC opcodes have special old and src2 operands.
// src2 is tied to dst, while old is not tied (but assumed to be).
bool AMDGPUDisassembler::isMacDPP(MCInst &MI) const {
  constexpr int DST_IDX = 0;
  auto Opcode = MI.getOpcode();
  const auto &Desc = MCII->get(Opcode);
  auto OldIdx = AMDGPU::getNamedOperandIdx(Opcode, AMDGPU::OpName::old);

  if (OldIdx != -1 && Desc.getOperandConstraint(
                          OldIdx, MCOI::OperandConstraint::TIED_TO) == -1) {
    assert(AMDGPU::hasNamedOperand(Opcode, AMDGPU::OpName::src2));
    assert(Desc.getOperandConstraint(
               AMDGPU::getNamedOperandIdx(Opcode, AMDGPU::OpName::src2),
               MCOI::OperandConstraint::TIED_TO) == DST_IDX);
    (void)DST_IDX;
    return true;
  }

  return false;
}

// Create dummy old operand and insert dummy unused src2_modifiers
void AMDGPUDisassembler::convertMacDPPInst(MCInst &MI) const {
  assert(MI.getNumOperands() + 1 < MCII->get(MI.getOpcode()).getNumOperands());
  insertNamedMCOperand(MI, MCOperand::createReg(0), AMDGPU::OpName::old);
  insertNamedMCOperand(MI, MCOperand::createImm(0),
                       AMDGPU::OpName::src2_modifiers);
}

// We must check FI == literal to reject not genuine dpp8 insts, and we must
// first add optional MI operands to check FI
DecodeStatus AMDGPUDisassembler::convertDPP8Inst(MCInst &MI) const {
  unsigned Opc = MI.getOpcode();
  if (MCII->get(Opc).TSFlags & SIInstrFlags::VOP3P) {
    convertVOP3PDPPInst(MI);
  } else if ((MCII->get(Opc).TSFlags & SIInstrFlags::VOPC) ||
             AMDGPU::isVOPC64DPP(Opc)) {
    convertVOPCDPPInst(MI);
  } else {
    if (isMacDPP(MI))
      convertMacDPPInst(MI);

    unsigned DescNumOps = MCII->get(Opc).getNumOperands();
    if (MI.getNumOperands() < DescNumOps &&
        AMDGPU::hasNamedOperand(Opc, AMDGPU::OpName::op_sel)) {
      auto Mods = collectVOPModifiers(MI);
      insertNamedMCOperand(MI, MCOperand::createImm(Mods.OpSel),
                           AMDGPU::OpName::op_sel);
    } else {
      // Insert dummy unused src modifiers.
      if (MI.getNumOperands() < DescNumOps &&
          AMDGPU::hasNamedOperand(Opc, AMDGPU::OpName::src0_modifiers))
        insertNamedMCOperand(MI, MCOperand::createImm(0),
                             AMDGPU::OpName::src0_modifiers);

      if (MI.getNumOperands() < DescNumOps &&
          AMDGPU::hasNamedOperand(Opc, AMDGPU::OpName::src1_modifiers))
        insertNamedMCOperand(MI, MCOperand::createImm(0),
                             AMDGPU::OpName::src1_modifiers);
    }
  }
  return isValidDPP8(MI) ? MCDisassembler::Success : MCDisassembler::SoftFail;
}

DecodeStatus AMDGPUDisassembler::convertVOP3DPPInst(MCInst &MI) const {
  if (isMacDPP(MI))
    convertMacDPPInst(MI);

  unsigned Opc = MI.getOpcode();
  unsigned DescNumOps = MCII->get(Opc).getNumOperands();
  if (MI.getNumOperands() < DescNumOps &&
      AMDGPU::hasNamedOperand(Opc, AMDGPU::OpName::op_sel)) {
    auto Mods = collectVOPModifiers(MI);
    insertNamedMCOperand(MI, MCOperand::createImm(Mods.OpSel),
                         AMDGPU::OpName::op_sel);
  }
  return MCDisassembler::Success;
}

// Note that before gfx10, the MIMG encoding provided no information about
// VADDR size. Consequently, decoded instructions always show address as if it
// has 1 dword, which could be not really so.
DecodeStatus AMDGPUDisassembler::convertMIMGInst(MCInst &MI) const {
  auto TSFlags = MCII->get(MI.getOpcode()).TSFlags;

  int VDstIdx = AMDGPU::getNamedOperandIdx(MI.getOpcode(),
                                           AMDGPU::OpName::vdst);

  int VDataIdx = AMDGPU::getNamedOperandIdx(MI.getOpcode(),
                                            AMDGPU::OpName::vdata);
  int VAddr0Idx =
      AMDGPU::getNamedOperandIdx(MI.getOpcode(), AMDGPU::OpName::vaddr0);
  int RsrcOpName = TSFlags & SIInstrFlags::MIMG ? AMDGPU::OpName::srsrc
                                                : AMDGPU::OpName::rsrc;
  int RsrcIdx = AMDGPU::getNamedOperandIdx(MI.getOpcode(), RsrcOpName);
  int DMaskIdx = AMDGPU::getNamedOperandIdx(MI.getOpcode(),
                                            AMDGPU::OpName::dmask);

  int TFEIdx   = AMDGPU::getNamedOperandIdx(MI.getOpcode(),
                                            AMDGPU::OpName::tfe);
  int D16Idx   = AMDGPU::getNamedOperandIdx(MI.getOpcode(),
                                            AMDGPU::OpName::d16);

  const AMDGPU::MIMGInfo *Info = AMDGPU::getMIMGInfo(MI.getOpcode());
  const AMDGPU::MIMGBaseOpcodeInfo *BaseOpcode =
      AMDGPU::getMIMGBaseOpcodeInfo(Info->BaseOpcode);

  assert(VDataIdx != -1);
  if (BaseOpcode->BVH) {
    // Add A16 operand for intersect_ray instructions
    addOperand(MI, MCOperand::createImm(BaseOpcode->A16));
    return MCDisassembler::Success;
  }

  bool IsAtomic = (VDstIdx != -1);
  bool IsGather4 = TSFlags & SIInstrFlags::Gather4;
  bool IsVSample = TSFlags & SIInstrFlags::VSAMPLE;
  bool IsNSA = false;
  bool IsPartialNSA = false;
  unsigned AddrSize = Info->VAddrDwords;

  if (isGFX10Plus()) {
    unsigned DimIdx =
        AMDGPU::getNamedOperandIdx(MI.getOpcode(), AMDGPU::OpName::dim);
    int A16Idx =
        AMDGPU::getNamedOperandIdx(MI.getOpcode(), AMDGPU::OpName::a16);
    const AMDGPU::MIMGDimInfo *Dim =
        AMDGPU::getMIMGDimInfoByEncoding(MI.getOperand(DimIdx).getImm());
    const bool IsA16 = (A16Idx != -1 && MI.getOperand(A16Idx).getImm());

    AddrSize =
        AMDGPU::getAddrSizeMIMGOp(BaseOpcode, Dim, IsA16, AMDGPU::hasG16(STI));

    // VSAMPLE insts that do not use vaddr3 behave the same as NSA forms.
    // VIMAGE insts other than BVH never use vaddr4.
    IsNSA = Info->MIMGEncoding == AMDGPU::MIMGEncGfx10NSA ||
            Info->MIMGEncoding == AMDGPU::MIMGEncGfx11NSA ||
            Info->MIMGEncoding == AMDGPU::MIMGEncGfx12;
    if (!IsNSA) {
      if (!IsVSample && AddrSize > 12)
        AddrSize = 16;
    } else {
      if (AddrSize > Info->VAddrDwords) {
        if (!STI.hasFeature(AMDGPU::FeaturePartialNSAEncoding)) {
          // The NSA encoding does not contain enough operands for the
          // combination of base opcode / dimension. Should this be an error?
          return MCDisassembler::Success;
        }
        IsPartialNSA = true;
      }
    }
  }

  unsigned DMask = MI.getOperand(DMaskIdx).getImm() & 0xf;
  unsigned DstSize = IsGather4 ? 4 : std::max(llvm::popcount(DMask), 1);

  bool D16 = D16Idx >= 0 && MI.getOperand(D16Idx).getImm();
  if (D16 && AMDGPU::hasPackedD16(STI)) {
    DstSize = (DstSize + 1) / 2;
  }

  if (TFEIdx != -1 && MI.getOperand(TFEIdx).getImm())
    DstSize += 1;

  if (DstSize == Info->VDataDwords && AddrSize == Info->VAddrDwords)
    return MCDisassembler::Success;

  int NewOpcode =
      AMDGPU::getMIMGOpcode(Info->BaseOpcode, Info->MIMGEncoding, DstSize, AddrSize);
  if (NewOpcode == -1)
    return MCDisassembler::Success;

  // Widen the register to the correct number of enabled channels.
  unsigned NewVdata = AMDGPU::NoRegister;
  if (DstSize != Info->VDataDwords) {
    auto DataRCID = MCII->get(NewOpcode).operands()[VDataIdx].RegClass;

    // Get first subregister of VData
    unsigned Vdata0 = MI.getOperand(VDataIdx).getReg();
    unsigned VdataSub0 = MRI.getSubReg(Vdata0, AMDGPU::sub0);
    Vdata0 = (VdataSub0 != 0)? VdataSub0 : Vdata0;

    NewVdata = MRI.getMatchingSuperReg(Vdata0, AMDGPU::sub0,
                                       &MRI.getRegClass(DataRCID));
    if (NewVdata == AMDGPU::NoRegister) {
      // It's possible to encode this such that the low register + enabled
      // components exceeds the register count.
      return MCDisassembler::Success;
    }
  }

  // If not using NSA on GFX10+, widen vaddr0 address register to correct size.
  // If using partial NSA on GFX11+ widen last address register.
  int VAddrSAIdx = IsPartialNSA ? (RsrcIdx - 1) : VAddr0Idx;
  unsigned NewVAddrSA = AMDGPU::NoRegister;
  if (STI.hasFeature(AMDGPU::FeatureNSAEncoding) && (!IsNSA || IsPartialNSA) &&
      AddrSize != Info->VAddrDwords) {
    unsigned VAddrSA = MI.getOperand(VAddrSAIdx).getReg();
    unsigned VAddrSubSA = MRI.getSubReg(VAddrSA, AMDGPU::sub0);
    VAddrSA = VAddrSubSA ? VAddrSubSA : VAddrSA;

    auto AddrRCID = MCII->get(NewOpcode).operands()[VAddrSAIdx].RegClass;
    NewVAddrSA = MRI.getMatchingSuperReg(VAddrSA, AMDGPU::sub0,
                                        &MRI.getRegClass(AddrRCID));
    if (!NewVAddrSA)
      return MCDisassembler::Success;
  }

  MI.setOpcode(NewOpcode);

  if (NewVdata != AMDGPU::NoRegister) {
    MI.getOperand(VDataIdx) = MCOperand::createReg(NewVdata);

    if (IsAtomic) {
      // Atomic operations have an additional operand (a copy of data)
      MI.getOperand(VDstIdx) = MCOperand::createReg(NewVdata);
    }
  }

  if (NewVAddrSA) {
    MI.getOperand(VAddrSAIdx) = MCOperand::createReg(NewVAddrSA);
  } else if (IsNSA) {
    assert(AddrSize <= Info->VAddrDwords);
    MI.erase(MI.begin() + VAddr0Idx + AddrSize,
             MI.begin() + VAddr0Idx + Info->VAddrDwords);
  }

  return MCDisassembler::Success;
}

// Opsel and neg bits are used in src_modifiers and standalone operands. Autogen
// decoder only adds to src_modifiers, so manually add the bits to the other
// operands.
DecodeStatus AMDGPUDisassembler::convertVOP3PDPPInst(MCInst &MI) const {
  unsigned Opc = MI.getOpcode();
  unsigned DescNumOps = MCII->get(Opc).getNumOperands();
  auto Mods = collectVOPModifiers(MI, true);

  if (MI.getNumOperands() < DescNumOps &&
      AMDGPU::hasNamedOperand(Opc, AMDGPU::OpName::vdst_in))
    insertNamedMCOperand(MI, MCOperand::createImm(0), AMDGPU::OpName::vdst_in);

  if (MI.getNumOperands() < DescNumOps &&
      AMDGPU::hasNamedOperand(Opc, AMDGPU::OpName::op_sel))
    insertNamedMCOperand(MI, MCOperand::createImm(Mods.OpSel),
                         AMDGPU::OpName::op_sel);
  if (MI.getNumOperands() < DescNumOps &&
      AMDGPU::hasNamedOperand(Opc, AMDGPU::OpName::op_sel_hi))
    insertNamedMCOperand(MI, MCOperand::createImm(Mods.OpSelHi),
                         AMDGPU::OpName::op_sel_hi);
  if (MI.getNumOperands() < DescNumOps &&
      AMDGPU::hasNamedOperand(Opc, AMDGPU::OpName::neg_lo))
    insertNamedMCOperand(MI, MCOperand::createImm(Mods.NegLo),
                         AMDGPU::OpName::neg_lo);
  if (MI.getNumOperands() < DescNumOps &&
      AMDGPU::hasNamedOperand(Opc, AMDGPU::OpName::neg_hi))
    insertNamedMCOperand(MI, MCOperand::createImm(Mods.NegHi),
                         AMDGPU::OpName::neg_hi);

  return MCDisassembler::Success;
}

// Create dummy old operand and insert optional operands
DecodeStatus AMDGPUDisassembler::convertVOPCDPPInst(MCInst &MI) const {
  unsigned Opc = MI.getOpcode();
  unsigned DescNumOps = MCII->get(Opc).getNumOperands();

  if (MI.getNumOperands() < DescNumOps &&
      AMDGPU::hasNamedOperand(Opc, AMDGPU::OpName::old))
    insertNamedMCOperand(MI, MCOperand::createReg(0), AMDGPU::OpName::old);

  if (MI.getNumOperands() < DescNumOps &&
      AMDGPU::hasNamedOperand(Opc, AMDGPU::OpName::src0_modifiers))
    insertNamedMCOperand(MI, MCOperand::createImm(0),
                         AMDGPU::OpName::src0_modifiers);

  if (MI.getNumOperands() < DescNumOps &&
      AMDGPU::hasNamedOperand(Opc, AMDGPU::OpName::src1_modifiers))
    insertNamedMCOperand(MI, MCOperand::createImm(0),
                         AMDGPU::OpName::src1_modifiers);
  return MCDisassembler::Success;
}

DecodeStatus AMDGPUDisassembler::convertFMAanyK(MCInst &MI,
                                                int ImmLitIdx) const {
  assert(HasLiteral && "Should have decoded a literal");
  const MCInstrDesc &Desc = MCII->get(MI.getOpcode());
  unsigned DescNumOps = Desc.getNumOperands();
  insertNamedMCOperand(MI, MCOperand::createImm(Literal),
                       AMDGPU::OpName::immDeferred);
  assert(DescNumOps == MI.getNumOperands());
  for (unsigned I = 0; I < DescNumOps; ++I) {
    auto &Op = MI.getOperand(I);
    auto OpType = Desc.operands()[I].OperandType;
    bool IsDeferredOp = (OpType == AMDGPU::OPERAND_REG_IMM_FP32_DEFERRED ||
                         OpType == AMDGPU::OPERAND_REG_IMM_FP16_DEFERRED);
    if (Op.isImm() && Op.getImm() == AMDGPU::EncValues::LITERAL_CONST &&
        IsDeferredOp)
      Op.setImm(Literal);
  }
  return MCDisassembler::Success;
}

const char* AMDGPUDisassembler::getRegClassName(unsigned RegClassID) const {
  return getContext().getRegisterInfo()->
    getRegClassName(&AMDGPUMCRegisterClasses[RegClassID]);
}

inline
MCOperand AMDGPUDisassembler::errOperand(unsigned V,
                                         const Twine& ErrMsg) const {
  *CommentStream << "Error: " + ErrMsg;

  // ToDo: add support for error operands to MCInst.h
  // return MCOperand::createError(V);
  return MCOperand();
}

inline
MCOperand AMDGPUDisassembler::createRegOperand(unsigned int RegId) const {
  return MCOperand::createReg(AMDGPU::getMCReg(RegId, STI));
}

inline
MCOperand AMDGPUDisassembler::createRegOperand(unsigned RegClassID,
                                               unsigned Val) const {
  const auto& RegCl = AMDGPUMCRegisterClasses[RegClassID];
  if (Val >= RegCl.getNumRegs())
    return errOperand(Val, Twine(getRegClassName(RegClassID)) +
                           ": unknown register " + Twine(Val));
  return createRegOperand(RegCl.getRegister(Val));
}

inline
MCOperand AMDGPUDisassembler::createSRegOperand(unsigned SRegClassID,
                                                unsigned Val) const {
  // ToDo: SI/CI have 104 SGPRs, VI - 102
  // Valery: here we accepting as much as we can, let assembler sort it out
  int shift = 0;
  switch (SRegClassID) {
  case AMDGPU::SGPR_32RegClassID:
  case AMDGPU::TTMP_32RegClassID:
    break;
  case AMDGPU::SGPR_64RegClassID:
  case AMDGPU::TTMP_64RegClassID:
    shift = 1;
    break;
  case AMDGPU::SGPR_96RegClassID:
  case AMDGPU::TTMP_96RegClassID:
  case AMDGPU::SGPR_128RegClassID:
  case AMDGPU::TTMP_128RegClassID:
  // ToDo: unclear if s[100:104] is available on VI. Can we use VCC as SGPR in
  // this bundle?
  case AMDGPU::SGPR_256RegClassID:
  case AMDGPU::TTMP_256RegClassID:
    // ToDo: unclear if s[96:104] is available on VI. Can we use VCC as SGPR in
  // this bundle?
  case AMDGPU::SGPR_288RegClassID:
  case AMDGPU::TTMP_288RegClassID:
  case AMDGPU::SGPR_320RegClassID:
  case AMDGPU::TTMP_320RegClassID:
  case AMDGPU::SGPR_352RegClassID:
  case AMDGPU::TTMP_352RegClassID:
  case AMDGPU::SGPR_384RegClassID:
  case AMDGPU::TTMP_384RegClassID:
  case AMDGPU::SGPR_512RegClassID:
  case AMDGPU::TTMP_512RegClassID:
    shift = 2;
    break;
  // ToDo: unclear if s[88:104] is available on VI. Can we use VCC as SGPR in
  // this bundle?
  default:
    llvm_unreachable("unhandled register class");
  }

  if (Val % (1 << shift)) {
    *CommentStream << "Warning: " << getRegClassName(SRegClassID)
                   << ": scalar reg isn't aligned " << Val;
  }

  return createRegOperand(SRegClassID, Val >> shift);
}

MCOperand AMDGPUDisassembler::createVGPR16Operand(unsigned RegIdx,
                                                  bool IsHi) const {
  unsigned RegIdxInVGPR16 = RegIdx * 2 + (IsHi ? 1 : 0);
  return createRegOperand(AMDGPU::VGPR_16RegClassID, RegIdxInVGPR16);
}

// Decode Literals for insts which always have a literal in the encoding
MCOperand
AMDGPUDisassembler::decodeMandatoryLiteralConstant(unsigned Val) const {
  if (HasLiteral) {
    assert(
        AMDGPU::hasVOPD(STI) &&
        "Should only decode multiple kimm with VOPD, check VSrc operand types");
    if (Literal != Val)
      return errOperand(Val, "More than one unique literal is illegal");
  }
  HasLiteral = true;
  Literal = Val;
  return MCOperand::createImm(Literal);
}

MCOperand AMDGPUDisassembler::decodeLiteralConstant(bool ExtendFP64) const {
  // For now all literal constants are supposed to be unsigned integer
  // ToDo: deal with signed/unsigned 64-bit integer constants
  // ToDo: deal with float/double constants
  if (!HasLiteral) {
    if (Bytes.size() < 4) {
      return errOperand(0, "cannot read literal, inst bytes left " +
                        Twine(Bytes.size()));
    }
    HasLiteral = true;
    Literal = Literal64 = eatBytes<uint32_t>(Bytes);
    if (ExtendFP64)
      Literal64 <<= 32;
  }
  return MCOperand::createImm(ExtendFP64 ? Literal64 : Literal);
}

MCOperand AMDGPUDisassembler::decodeIntImmed(unsigned Imm) {
  using namespace AMDGPU::EncValues;

  assert(Imm >= INLINE_INTEGER_C_MIN && Imm <= INLINE_INTEGER_C_MAX);
  return MCOperand::createImm((Imm <= INLINE_INTEGER_C_POSITIVE_MAX) ?
    (static_cast<int64_t>(Imm) - INLINE_INTEGER_C_MIN) :
    (INLINE_INTEGER_C_POSITIVE_MAX - static_cast<int64_t>(Imm)));
      // Cast prevents negative overflow.
}

static int64_t getInlineImmVal32(unsigned Imm) {
  switch (Imm) {
  case 240:
    return llvm::bit_cast<uint32_t>(0.5f);
  case 241:
    return llvm::bit_cast<uint32_t>(-0.5f);
  case 242:
    return llvm::bit_cast<uint32_t>(1.0f);
  case 243:
    return llvm::bit_cast<uint32_t>(-1.0f);
  case 244:
    return llvm::bit_cast<uint32_t>(2.0f);
  case 245:
    return llvm::bit_cast<uint32_t>(-2.0f);
  case 246:
    return llvm::bit_cast<uint32_t>(4.0f);
  case 247:
    return llvm::bit_cast<uint32_t>(-4.0f);
  case 248: // 1 / (2 * PI)
    return 0x3e22f983;
  default:
    llvm_unreachable("invalid fp inline imm");
  }
}

static int64_t getInlineImmVal64(unsigned Imm) {
  switch (Imm) {
  case 240:
    return llvm::bit_cast<uint64_t>(0.5);
  case 241:
    return llvm::bit_cast<uint64_t>(-0.5);
  case 242:
    return llvm::bit_cast<uint64_t>(1.0);
  case 243:
    return llvm::bit_cast<uint64_t>(-1.0);
  case 244:
    return llvm::bit_cast<uint64_t>(2.0);
  case 245:
    return llvm::bit_cast<uint64_t>(-2.0);
  case 246:
    return llvm::bit_cast<uint64_t>(4.0);
  case 247:
    return llvm::bit_cast<uint64_t>(-4.0);
  case 248: // 1 / (2 * PI)
    return 0x3fc45f306dc9c882;
  default:
    llvm_unreachable("invalid fp inline imm");
  }
}

static int64_t getInlineImmVal16(unsigned Imm) {
  switch (Imm) {
  case 240:
    return 0x3800;
  case 241:
    return 0xB800;
  case 242:
    return 0x3C00;
  case 243:
    return 0xBC00;
  case 244:
    return 0x4000;
  case 245:
    return 0xC000;
  case 246:
    return 0x4400;
  case 247:
    return 0xC400;
  case 248: // 1 / (2 * PI)
    return 0x3118;
  default:
    llvm_unreachable("invalid fp inline imm");
  }
}

MCOperand AMDGPUDisassembler::decodeFPImmed(unsigned ImmWidth, unsigned Imm) {
  assert(Imm >= AMDGPU::EncValues::INLINE_FLOATING_C_MIN
      && Imm <= AMDGPU::EncValues::INLINE_FLOATING_C_MAX);

  // ToDo: case 248: 1/(2*PI) - is allowed only on VI
  // ImmWidth 0 is a default case where operand should not allow immediates.
  // Imm value is still decoded into 32 bit immediate operand, inst printer will
  // use it to print verbose error message.
  switch (ImmWidth) {
  case 0:
  case 32:
    return MCOperand::createImm(getInlineImmVal32(Imm));
  case 64:
    return MCOperand::createImm(getInlineImmVal64(Imm));
  case 16:
    return MCOperand::createImm(getInlineImmVal16(Imm));
  default:
    llvm_unreachable("implement me");
  }
}

unsigned AMDGPUDisassembler::getVgprClassId(const OpWidthTy Width) const {
  using namespace AMDGPU;

  assert(OPW_FIRST_ <= Width && Width < OPW_LAST_);
  switch (Width) {
  default: // fall
  case OPW32:
  case OPW16:
  case OPWV216:
    return VGPR_32RegClassID;
  case OPW64:
  case OPWV232: return VReg_64RegClassID;
  case OPW96: return VReg_96RegClassID;
  case OPW128: return VReg_128RegClassID;
  case OPW160: return VReg_160RegClassID;
  case OPW256: return VReg_256RegClassID;
  case OPW288: return VReg_288RegClassID;
  case OPW320: return VReg_320RegClassID;
  case OPW352: return VReg_352RegClassID;
  case OPW384: return VReg_384RegClassID;
  case OPW512: return VReg_512RegClassID;
  case OPW1024: return VReg_1024RegClassID;
  }
}

unsigned AMDGPUDisassembler::getAgprClassId(const OpWidthTy Width) const {
  using namespace AMDGPU;

  assert(OPW_FIRST_ <= Width && Width < OPW_LAST_);
  switch (Width) {
  default: // fall
  case OPW32:
  case OPW16:
  case OPWV216:
    return AGPR_32RegClassID;
  case OPW64:
  case OPWV232: return AReg_64RegClassID;
  case OPW96: return AReg_96RegClassID;
  case OPW128: return AReg_128RegClassID;
  case OPW160: return AReg_160RegClassID;
  case OPW256: return AReg_256RegClassID;
  case OPW288: return AReg_288RegClassID;
  case OPW320: return AReg_320RegClassID;
  case OPW352: return AReg_352RegClassID;
  case OPW384: return AReg_384RegClassID;
  case OPW512: return AReg_512RegClassID;
  case OPW1024: return AReg_1024RegClassID;
  }
}


unsigned AMDGPUDisassembler::getSgprClassId(const OpWidthTy Width) const {
  using namespace AMDGPU;

  assert(OPW_FIRST_ <= Width && Width < OPW_LAST_);
  switch (Width) {
  default: // fall
  case OPW32:
  case OPW16:
  case OPWV216:
    return SGPR_32RegClassID;
  case OPW64:
  case OPWV232: return SGPR_64RegClassID;
  case OPW96: return SGPR_96RegClassID;
  case OPW128: return SGPR_128RegClassID;
  case OPW160: return SGPR_160RegClassID;
  case OPW256: return SGPR_256RegClassID;
  case OPW288: return SGPR_288RegClassID;
  case OPW320: return SGPR_320RegClassID;
  case OPW352: return SGPR_352RegClassID;
  case OPW384: return SGPR_384RegClassID;
  case OPW512: return SGPR_512RegClassID;
  }
}

unsigned AMDGPUDisassembler::getTtmpClassId(const OpWidthTy Width) const {
  using namespace AMDGPU;

  assert(OPW_FIRST_ <= Width && Width < OPW_LAST_);
  switch (Width) {
  default: // fall
  case OPW32:
  case OPW16:
  case OPWV216:
    return TTMP_32RegClassID;
  case OPW64:
  case OPWV232: return TTMP_64RegClassID;
  case OPW128: return TTMP_128RegClassID;
  case OPW256: return TTMP_256RegClassID;
  case OPW288: return TTMP_288RegClassID;
  case OPW320: return TTMP_320RegClassID;
  case OPW352: return TTMP_352RegClassID;
  case OPW384: return TTMP_384RegClassID;
  case OPW512: return TTMP_512RegClassID;
  }
}

int AMDGPUDisassembler::getTTmpIdx(unsigned Val) const {
  using namespace AMDGPU::EncValues;

  unsigned TTmpMin = isGFX9Plus() ? TTMP_GFX9PLUS_MIN : TTMP_VI_MIN;
  unsigned TTmpMax = isGFX9Plus() ? TTMP_GFX9PLUS_MAX : TTMP_VI_MAX;

  return (TTmpMin <= Val && Val <= TTmpMax)? Val - TTmpMin : -1;
}

MCOperand AMDGPUDisassembler::decodeSrcOp(const OpWidthTy Width, unsigned Val,
                                          bool MandatoryLiteral,
                                          unsigned ImmWidth, bool IsFP) const {
  using namespace AMDGPU::EncValues;

  assert(Val < 1024); // enum10

  bool IsAGPR = Val & 512;
  Val &= 511;

  if (VGPR_MIN <= Val && Val <= VGPR_MAX) {
    return createRegOperand(IsAGPR ? getAgprClassId(Width)
                                   : getVgprClassId(Width), Val - VGPR_MIN);
  }
  return decodeNonVGPRSrcOp(Width, Val & 0xFF, MandatoryLiteral, ImmWidth,
                            IsFP);
}

MCOperand AMDGPUDisassembler::decodeNonVGPRSrcOp(const OpWidthTy Width,
                                                 unsigned Val,
                                                 bool MandatoryLiteral,
                                                 unsigned ImmWidth,
                                                 bool IsFP) const {
  // Cases when Val{8} is 1 (vgpr, agpr or true 16 vgpr) should have been
  // decoded earlier.
  assert(Val < (1 << 8) && "9-bit Src encoding when Val{8} is 0");
  using namespace AMDGPU::EncValues;

  if (Val <= SGPR_MAX) {
    // "SGPR_MIN <= Val" is always true and causes compilation warning.
    static_assert(SGPR_MIN == 0);
    return createSRegOperand(getSgprClassId(Width), Val - SGPR_MIN);
  }

  int TTmpIdx = getTTmpIdx(Val);
  if (TTmpIdx >= 0) {
    return createSRegOperand(getTtmpClassId(Width), TTmpIdx);
  }

  if (INLINE_INTEGER_C_MIN <= Val && Val <= INLINE_INTEGER_C_MAX)
    return decodeIntImmed(Val);

  if (INLINE_FLOATING_C_MIN <= Val && Val <= INLINE_FLOATING_C_MAX)
    return decodeFPImmed(ImmWidth, Val);

  if (Val == LITERAL_CONST) {
    if (MandatoryLiteral)
      // Keep a sentinel value for deferred setting
      return MCOperand::createImm(LITERAL_CONST);
    else
      return decodeLiteralConstant(IsFP && ImmWidth == 64);
  }

  switch (Width) {
  case OPW32:
  case OPW16:
  case OPWV216:
    return decodeSpecialReg32(Val);
  case OPW64:
  case OPWV232:
    return decodeSpecialReg64(Val);
  default:
    llvm_unreachable("unexpected immediate type");
  }
}

// Bit 0 of DstY isn't stored in the instruction, because it's always the
// opposite of bit 0 of DstX.
MCOperand AMDGPUDisassembler::decodeVOPDDstYOp(MCInst &Inst,
                                               unsigned Val) const {
  int VDstXInd =
      AMDGPU::getNamedOperandIdx(Inst.getOpcode(), AMDGPU::OpName::vdstX);
  assert(VDstXInd != -1);
  assert(Inst.getOperand(VDstXInd).isReg());
  unsigned XDstReg = MRI.getEncodingValue(Inst.getOperand(VDstXInd).getReg());
  Val |= ~XDstReg & 1;
  auto Width = llvm::AMDGPUDisassembler::OPW32;
  return createRegOperand(getVgprClassId(Width), Val);
}

MCOperand AMDGPUDisassembler::decodeSpecialReg32(unsigned Val) const {
  using namespace AMDGPU;

  switch (Val) {
  // clang-format off
  case 102: return createRegOperand(FLAT_SCR_LO);
  case 103: return createRegOperand(FLAT_SCR_HI);
  case 104: return createRegOperand(XNACK_MASK_LO);
  case 105: return createRegOperand(XNACK_MASK_HI);
  case 106: return createRegOperand(VCC_LO);
  case 107: return createRegOperand(VCC_HI);
  case 108: return createRegOperand(TBA_LO);
  case 109: return createRegOperand(TBA_HI);
  case 110: return createRegOperand(TMA_LO);
  case 111: return createRegOperand(TMA_HI);
  case 124:
    return isGFX11Plus() ? createRegOperand(SGPR_NULL) : createRegOperand(M0);
  case 125:
    return isGFX11Plus() ? createRegOperand(M0) : createRegOperand(SGPR_NULL);
  case 126: return createRegOperand(EXEC_LO);
  case 127: return createRegOperand(EXEC_HI);
  case 235: return createRegOperand(SRC_SHARED_BASE_LO);
  case 236: return createRegOperand(SRC_SHARED_LIMIT_LO);
  case 237: return createRegOperand(SRC_PRIVATE_BASE_LO);
  case 238: return createRegOperand(SRC_PRIVATE_LIMIT_LO);
  case 239: return createRegOperand(SRC_POPS_EXITING_WAVE_ID);
  case 251: return createRegOperand(SRC_VCCZ);
  case 252: return createRegOperand(SRC_EXECZ);
  case 253: return createRegOperand(SRC_SCC);
  case 254: return createRegOperand(LDS_DIRECT);
  default: break;
    // clang-format on
  }
  return errOperand(Val, "unknown operand encoding " + Twine(Val));
}

MCOperand AMDGPUDisassembler::decodeSpecialReg64(unsigned Val) const {
  using namespace AMDGPU;

  switch (Val) {
  case 102: return createRegOperand(FLAT_SCR);
  case 104: return createRegOperand(XNACK_MASK);
  case 106: return createRegOperand(VCC);
  case 108: return createRegOperand(TBA);
  case 110: return createRegOperand(TMA);
  case 124:
    if (isGFX11Plus())
      return createRegOperand(SGPR_NULL);
    break;
  case 125:
    if (!isGFX11Plus())
      return createRegOperand(SGPR_NULL);
    break;
  case 126: return createRegOperand(EXEC);
  case 235: return createRegOperand(SRC_SHARED_BASE);
  case 236: return createRegOperand(SRC_SHARED_LIMIT);
  case 237: return createRegOperand(SRC_PRIVATE_BASE);
  case 238: return createRegOperand(SRC_PRIVATE_LIMIT);
  case 239: return createRegOperand(SRC_POPS_EXITING_WAVE_ID);
  case 251: return createRegOperand(SRC_VCCZ);
  case 252: return createRegOperand(SRC_EXECZ);
  case 253: return createRegOperand(SRC_SCC);
  default: break;
  }
  return errOperand(Val, "unknown operand encoding " + Twine(Val));
}

MCOperand AMDGPUDisassembler::decodeSDWASrc(const OpWidthTy Width,
                                            const unsigned Val,
                                            unsigned ImmWidth) const {
  using namespace AMDGPU::SDWA;
  using namespace AMDGPU::EncValues;

  if (STI.hasFeature(AMDGPU::FeatureGFX9) ||
      STI.hasFeature(AMDGPU::FeatureGFX10)) {
    // XXX: cast to int is needed to avoid stupid warning:
    // compare with unsigned is always true
    if (int(SDWA9EncValues::SRC_VGPR_MIN) <= int(Val) &&
        Val <= SDWA9EncValues::SRC_VGPR_MAX) {
      return createRegOperand(getVgprClassId(Width),
                              Val - SDWA9EncValues::SRC_VGPR_MIN);
    }
    if (SDWA9EncValues::SRC_SGPR_MIN <= Val &&
        Val <= (isGFX10Plus() ? SDWA9EncValues::SRC_SGPR_MAX_GFX10
                              : SDWA9EncValues::SRC_SGPR_MAX_SI)) {
      return createSRegOperand(getSgprClassId(Width),
                               Val - SDWA9EncValues::SRC_SGPR_MIN);
    }
    if (SDWA9EncValues::SRC_TTMP_MIN <= Val &&
        Val <= SDWA9EncValues::SRC_TTMP_MAX) {
      return createSRegOperand(getTtmpClassId(Width),
                               Val - SDWA9EncValues::SRC_TTMP_MIN);
    }

    const unsigned SVal = Val - SDWA9EncValues::SRC_SGPR_MIN;

    if (INLINE_INTEGER_C_MIN <= SVal && SVal <= INLINE_INTEGER_C_MAX)
      return decodeIntImmed(SVal);

    if (INLINE_FLOATING_C_MIN <= SVal && SVal <= INLINE_FLOATING_C_MAX)
      return decodeFPImmed(ImmWidth, SVal);

    return decodeSpecialReg32(SVal);
  } else if (STI.hasFeature(AMDGPU::FeatureVolcanicIslands)) {
    return createRegOperand(getVgprClassId(Width), Val);
  }
  llvm_unreachable("unsupported target");
}

MCOperand AMDGPUDisassembler::decodeSDWASrc16(unsigned Val) const {
  return decodeSDWASrc(OPW16, Val, 16);
}

MCOperand AMDGPUDisassembler::decodeSDWASrc32(unsigned Val) const {
  return decodeSDWASrc(OPW32, Val, 32);
}

MCOperand AMDGPUDisassembler::decodeSDWAVopcDst(unsigned Val) const {
  using namespace AMDGPU::SDWA;

  assert((STI.hasFeature(AMDGPU::FeatureGFX9) ||
          STI.hasFeature(AMDGPU::FeatureGFX10)) &&
         "SDWAVopcDst should be present only on GFX9+");

  bool IsWave64 = STI.hasFeature(AMDGPU::FeatureWavefrontSize64);

  if (Val & SDWA9EncValues::VOPC_DST_VCC_MASK) {
    Val &= SDWA9EncValues::VOPC_DST_SGPR_MASK;

    int TTmpIdx = getTTmpIdx(Val);
    if (TTmpIdx >= 0) {
      auto TTmpClsId = getTtmpClassId(IsWave64 ? OPW64 : OPW32);
      return createSRegOperand(TTmpClsId, TTmpIdx);
    } else if (Val > SGPR_MAX) {
      return IsWave64 ? decodeSpecialReg64(Val)
                      : decodeSpecialReg32(Val);
    } else {
      return createSRegOperand(getSgprClassId(IsWave64 ? OPW64 : OPW32), Val);
    }
  } else {
    return createRegOperand(IsWave64 ? AMDGPU::VCC : AMDGPU::VCC_LO);
  }
}

MCOperand AMDGPUDisassembler::decodeBoolReg(unsigned Val) const {
  return STI.hasFeature(AMDGPU::FeatureWavefrontSize64)
             ? decodeSrcOp(OPW64, Val)
             : decodeSrcOp(OPW32, Val);
}

MCOperand AMDGPUDisassembler::decodeSplitBarrier(unsigned Val) const {
  return decodeSrcOp(OPW32, Val);
}

bool AMDGPUDisassembler::isVI() const {
  return STI.hasFeature(AMDGPU::FeatureVolcanicIslands);
}

bool AMDGPUDisassembler::isGFX9() const { return AMDGPU::isGFX9(STI); }

bool AMDGPUDisassembler::isGFX90A() const {
  return STI.hasFeature(AMDGPU::FeatureGFX90AInsts);
}

bool AMDGPUDisassembler::isGFX9Plus() const { return AMDGPU::isGFX9Plus(STI); }

bool AMDGPUDisassembler::isGFX10() const { return AMDGPU::isGFX10(STI); }

bool AMDGPUDisassembler::isGFX10Plus() const {
  return AMDGPU::isGFX10Plus(STI);
}

bool AMDGPUDisassembler::isGFX11() const {
  return STI.hasFeature(AMDGPU::FeatureGFX11);
}

bool AMDGPUDisassembler::isGFX11Plus() const {
  return AMDGPU::isGFX11Plus(STI);
}

bool AMDGPUDisassembler::isGFX12Plus() const {
  return AMDGPU::isGFX12Plus(STI);
}

bool AMDGPUDisassembler::hasArchitectedFlatScratch() const {
  return STI.hasFeature(AMDGPU::FeatureArchitectedFlatScratch);
}

bool AMDGPUDisassembler::hasKernargPreload() const {
  return AMDGPU::hasKernargPreload(STI);
}

//===----------------------------------------------------------------------===//
// AMDGPU specific symbol handling
//===----------------------------------------------------------------------===//
#define GET_FIELD(MASK) (AMDHSA_BITS_GET(FourByteBuffer, MASK))
#define PRINT_DIRECTIVE(DIRECTIVE, MASK)                                       \
  do {                                                                         \
    KdStream << Indent << DIRECTIVE " " << GET_FIELD(MASK) << '\n';            \
  } while (0)
#define PRINT_PSEUDO_DIRECTIVE_COMMENT(DIRECTIVE, MASK)                        \
  do {                                                                         \
    KdStream << Indent << MAI.getCommentString() << ' ' << DIRECTIVE " "       \
             << GET_FIELD(MASK) << '\n';                                       \
  } while (0)

// NOLINTNEXTLINE(readability-identifier-naming)
MCDisassembler::DecodeStatus AMDGPUDisassembler::decodeCOMPUTE_PGM_RSRC1(
    uint32_t FourByteBuffer, raw_string_ostream &KdStream) const {
  using namespace amdhsa;
  StringRef Indent = "\t";

  // We cannot accurately backward compute #VGPRs used from
  // GRANULATED_WORKITEM_VGPR_COUNT. But we are concerned with getting the same
  // value of GRANULATED_WORKITEM_VGPR_COUNT in the reassembled binary. So we
  // simply calculate the inverse of what the assembler does.

  uint32_t GranulatedWorkitemVGPRCount =
      GET_FIELD(COMPUTE_PGM_RSRC1_GRANULATED_WORKITEM_VGPR_COUNT);

  uint32_t NextFreeVGPR =
      (GranulatedWorkitemVGPRCount + 1) *
      AMDGPU::IsaInfo::getVGPREncodingGranule(&STI, EnableWavefrontSize32);

  KdStream << Indent << ".amdhsa_next_free_vgpr " << NextFreeVGPR << '\n';

  // We cannot backward compute values used to calculate
  // GRANULATED_WAVEFRONT_SGPR_COUNT. Hence the original values for following
  // directives can't be computed:
  // .amdhsa_reserve_vcc
  // .amdhsa_reserve_flat_scratch
  // .amdhsa_reserve_xnack_mask
  // They take their respective default values if not specified in the assembly.
  //
  // GRANULATED_WAVEFRONT_SGPR_COUNT
  //    = f(NEXT_FREE_SGPR + VCC + FLAT_SCRATCH + XNACK_MASK)
  //
  // We compute the inverse as though all directives apart from NEXT_FREE_SGPR
  // are set to 0. So while disassembling we consider that:
  //
  // GRANULATED_WAVEFRONT_SGPR_COUNT
  //    = f(NEXT_FREE_SGPR + 0 + 0 + 0)
  //
  // The disassembler cannot recover the original values of those 3 directives.

  uint32_t GranulatedWavefrontSGPRCount =
      GET_FIELD(COMPUTE_PGM_RSRC1_GRANULATED_WAVEFRONT_SGPR_COUNT);

  if (isGFX10Plus() && GranulatedWavefrontSGPRCount)
    return MCDisassembler::Fail;

  uint32_t NextFreeSGPR = (GranulatedWavefrontSGPRCount + 1) *
                          AMDGPU::IsaInfo::getSGPREncodingGranule(&STI);

  KdStream << Indent << ".amdhsa_reserve_vcc " << 0 << '\n';
  if (!hasArchitectedFlatScratch())
    KdStream << Indent << ".amdhsa_reserve_flat_scratch " << 0 << '\n';
  KdStream << Indent << ".amdhsa_reserve_xnack_mask " << 0 << '\n';
  KdStream << Indent << ".amdhsa_next_free_sgpr " << NextFreeSGPR << "\n";

  if (FourByteBuffer & COMPUTE_PGM_RSRC1_PRIORITY)
    return MCDisassembler::Fail;

  PRINT_DIRECTIVE(".amdhsa_float_round_mode_32",
                  COMPUTE_PGM_RSRC1_FLOAT_ROUND_MODE_32);
  PRINT_DIRECTIVE(".amdhsa_float_round_mode_16_64",
                  COMPUTE_PGM_RSRC1_FLOAT_ROUND_MODE_16_64);
  PRINT_DIRECTIVE(".amdhsa_float_denorm_mode_32",
                  COMPUTE_PGM_RSRC1_FLOAT_DENORM_MODE_32);
  PRINT_DIRECTIVE(".amdhsa_float_denorm_mode_16_64",
                  COMPUTE_PGM_RSRC1_FLOAT_DENORM_MODE_16_64);

  if (FourByteBuffer & COMPUTE_PGM_RSRC1_PRIV)
    return MCDisassembler::Fail;

  if (!isGFX12Plus())
    PRINT_DIRECTIVE(".amdhsa_dx10_clamp",
                    COMPUTE_PGM_RSRC1_GFX6_GFX11_ENABLE_DX10_CLAMP);

  if (FourByteBuffer & COMPUTE_PGM_RSRC1_DEBUG_MODE)
    return MCDisassembler::Fail;

  if (!isGFX12Plus())
    PRINT_DIRECTIVE(".amdhsa_ieee_mode",
                    COMPUTE_PGM_RSRC1_GFX6_GFX11_ENABLE_IEEE_MODE);

  if (FourByteBuffer & COMPUTE_PGM_RSRC1_BULKY)
    return MCDisassembler::Fail;

  if (FourByteBuffer & COMPUTE_PGM_RSRC1_CDBG_USER)
    return MCDisassembler::Fail;

  if (isGFX9Plus())
    PRINT_DIRECTIVE(".amdhsa_fp16_overflow", COMPUTE_PGM_RSRC1_GFX9_PLUS_FP16_OVFL);

  if (!isGFX9Plus())
    if (FourByteBuffer & COMPUTE_PGM_RSRC1_GFX6_GFX8_RESERVED0)
      return MCDisassembler::Fail;
  if (FourByteBuffer & COMPUTE_PGM_RSRC1_RESERVED1)
    return MCDisassembler::Fail;
  if (!isGFX10Plus())
    if (FourByteBuffer & COMPUTE_PGM_RSRC1_GFX6_GFX9_RESERVED2)
      return MCDisassembler::Fail;

  if (isGFX10Plus()) {
    PRINT_DIRECTIVE(".amdhsa_workgroup_processor_mode",
                    COMPUTE_PGM_RSRC1_GFX10_PLUS_WGP_MODE);
    PRINT_DIRECTIVE(".amdhsa_memory_ordered", COMPUTE_PGM_RSRC1_GFX10_PLUS_MEM_ORDERED);
    PRINT_DIRECTIVE(".amdhsa_forward_progress", COMPUTE_PGM_RSRC1_GFX10_PLUS_FWD_PROGRESS);
  }

  if (isGFX12Plus())
    PRINT_DIRECTIVE(".amdhsa_round_robin_scheduling",
                    COMPUTE_PGM_RSRC1_GFX12_PLUS_ENABLE_WG_RR_EN);

  return MCDisassembler::Success;
}

// NOLINTNEXTLINE(readability-identifier-naming)
MCDisassembler::DecodeStatus AMDGPUDisassembler::decodeCOMPUTE_PGM_RSRC2(
    uint32_t FourByteBuffer, raw_string_ostream &KdStream) const {
  using namespace amdhsa;
  StringRef Indent = "\t";
  if (hasArchitectedFlatScratch())
    PRINT_DIRECTIVE(".amdhsa_enable_private_segment",
                    COMPUTE_PGM_RSRC2_ENABLE_PRIVATE_SEGMENT);
  else
    PRINT_DIRECTIVE(".amdhsa_system_sgpr_private_segment_wavefront_offset",
                    COMPUTE_PGM_RSRC2_ENABLE_PRIVATE_SEGMENT);
  PRINT_DIRECTIVE(".amdhsa_system_sgpr_workgroup_id_x",
                  COMPUTE_PGM_RSRC2_ENABLE_SGPR_WORKGROUP_ID_X);
  PRINT_DIRECTIVE(".amdhsa_system_sgpr_workgroup_id_y",
                  COMPUTE_PGM_RSRC2_ENABLE_SGPR_WORKGROUP_ID_Y);
  PRINT_DIRECTIVE(".amdhsa_system_sgpr_workgroup_id_z",
                  COMPUTE_PGM_RSRC2_ENABLE_SGPR_WORKGROUP_ID_Z);
  PRINT_DIRECTIVE(".amdhsa_system_sgpr_workgroup_info",
                  COMPUTE_PGM_RSRC2_ENABLE_SGPR_WORKGROUP_INFO);
  PRINT_DIRECTIVE(".amdhsa_system_vgpr_workitem_id",
                  COMPUTE_PGM_RSRC2_ENABLE_VGPR_WORKITEM_ID);

  if (FourByteBuffer & COMPUTE_PGM_RSRC2_ENABLE_EXCEPTION_ADDRESS_WATCH)
    return MCDisassembler::Fail;

  if (FourByteBuffer & COMPUTE_PGM_RSRC2_ENABLE_EXCEPTION_MEMORY)
    return MCDisassembler::Fail;

  if (FourByteBuffer & COMPUTE_PGM_RSRC2_GRANULATED_LDS_SIZE)
    return MCDisassembler::Fail;

  PRINT_DIRECTIVE(
      ".amdhsa_exception_fp_ieee_invalid_op",
      COMPUTE_PGM_RSRC2_ENABLE_EXCEPTION_IEEE_754_FP_INVALID_OPERATION);
  PRINT_DIRECTIVE(".amdhsa_exception_fp_denorm_src",
                  COMPUTE_PGM_RSRC2_ENABLE_EXCEPTION_FP_DENORMAL_SOURCE);
  PRINT_DIRECTIVE(
      ".amdhsa_exception_fp_ieee_div_zero",
      COMPUTE_PGM_RSRC2_ENABLE_EXCEPTION_IEEE_754_FP_DIVISION_BY_ZERO);
  PRINT_DIRECTIVE(".amdhsa_exception_fp_ieee_overflow",
                  COMPUTE_PGM_RSRC2_ENABLE_EXCEPTION_IEEE_754_FP_OVERFLOW);
  PRINT_DIRECTIVE(".amdhsa_exception_fp_ieee_underflow",
                  COMPUTE_PGM_RSRC2_ENABLE_EXCEPTION_IEEE_754_FP_UNDERFLOW);
  PRINT_DIRECTIVE(".amdhsa_exception_fp_ieee_inexact",
                  COMPUTE_PGM_RSRC2_ENABLE_EXCEPTION_IEEE_754_FP_INEXACT);
  PRINT_DIRECTIVE(".amdhsa_exception_int_div_zero",
                  COMPUTE_PGM_RSRC2_ENABLE_EXCEPTION_INT_DIVIDE_BY_ZERO);

  if (FourByteBuffer & COMPUTE_PGM_RSRC2_RESERVED0)
    return MCDisassembler::Fail;

  return MCDisassembler::Success;
}

// NOLINTNEXTLINE(readability-identifier-naming)
MCDisassembler::DecodeStatus AMDGPUDisassembler::decodeCOMPUTE_PGM_RSRC3(
    uint32_t FourByteBuffer, raw_string_ostream &KdStream) const {
  using namespace amdhsa;
  StringRef Indent = "\t";
  if (isGFX90A()) {
    KdStream << Indent << ".amdhsa_accum_offset "
             << (GET_FIELD(COMPUTE_PGM_RSRC3_GFX90A_ACCUM_OFFSET) + 1) * 4
             << '\n';
    if (FourByteBuffer & COMPUTE_PGM_RSRC3_GFX90A_RESERVED0)
      return MCDisassembler::Fail;
    PRINT_DIRECTIVE(".amdhsa_tg_split", COMPUTE_PGM_RSRC3_GFX90A_TG_SPLIT);
    if (FourByteBuffer & COMPUTE_PGM_RSRC3_GFX90A_RESERVED1)
      return MCDisassembler::Fail;
  } else if (isGFX10Plus()) {
    // Bits [0-3].
    if (!isGFX12Plus()) {
      if (!EnableWavefrontSize32 || !*EnableWavefrontSize32) {
        PRINT_DIRECTIVE(".amdhsa_shared_vgpr_count",
                        COMPUTE_PGM_RSRC3_GFX10_GFX11_SHARED_VGPR_COUNT);
      } else {
        PRINT_PSEUDO_DIRECTIVE_COMMENT(
            "SHARED_VGPR_COUNT",
            COMPUTE_PGM_RSRC3_GFX10_GFX11_SHARED_VGPR_COUNT);
      }
    } else {
      if (FourByteBuffer & COMPUTE_PGM_RSRC3_GFX12_PLUS_RESERVED0)
        return MCDisassembler::Fail;
    }

    // Bits [4-11].
    if (isGFX11()) {
      PRINT_PSEUDO_DIRECTIVE_COMMENT("INST_PREF_SIZE",
                                     COMPUTE_PGM_RSRC3_GFX11_INST_PREF_SIZE);
      PRINT_PSEUDO_DIRECTIVE_COMMENT("TRAP_ON_START",
                                     COMPUTE_PGM_RSRC3_GFX11_TRAP_ON_START);
      PRINT_PSEUDO_DIRECTIVE_COMMENT("TRAP_ON_END",
                                     COMPUTE_PGM_RSRC3_GFX11_TRAP_ON_END);
    } else if (isGFX12Plus()) {
      PRINT_PSEUDO_DIRECTIVE_COMMENT(
          "INST_PREF_SIZE", COMPUTE_PGM_RSRC3_GFX12_PLUS_INST_PREF_SIZE);
    } else {
      if (FourByteBuffer & COMPUTE_PGM_RSRC3_GFX10_RESERVED1)
        return MCDisassembler::Fail;
    }

    // Bits [12].
    if (FourByteBuffer & COMPUTE_PGM_RSRC3_GFX10_PLUS_RESERVED2)
      return MCDisassembler::Fail;

    // Bits [13].
    if (isGFX12Plus()) {
      PRINT_PSEUDO_DIRECTIVE_COMMENT("GLG_EN",
                                     COMPUTE_PGM_RSRC3_GFX12_PLUS_GLG_EN);
    } else {
      if (FourByteBuffer & COMPUTE_PGM_RSRC3_GFX10_GFX11_RESERVED3)
        return MCDisassembler::Fail;
    }

    // Bits [14-30].
    if (FourByteBuffer & COMPUTE_PGM_RSRC3_GFX10_PLUS_RESERVED4)
      return MCDisassembler::Fail;

    // Bits [31].
    if (isGFX11Plus()) {
      PRINT_PSEUDO_DIRECTIVE_COMMENT("IMAGE_OP",
                                     COMPUTE_PGM_RSRC3_GFX11_PLUS_IMAGE_OP);
    } else {
      if (FourByteBuffer & COMPUTE_PGM_RSRC3_GFX10_RESERVED5)
        return MCDisassembler::Fail;
    }
  } else if (FourByteBuffer) {
    return MCDisassembler::Fail;
  }
  return MCDisassembler::Success;
}
#undef PRINT_PSEUDO_DIRECTIVE_COMMENT
#undef PRINT_DIRECTIVE
#undef GET_FIELD

MCDisassembler::DecodeStatus
AMDGPUDisassembler::decodeKernelDescriptorDirective(
    DataExtractor::Cursor &Cursor, ArrayRef<uint8_t> Bytes,
    raw_string_ostream &KdStream) const {
#define PRINT_DIRECTIVE(DIRECTIVE, MASK)                                       \
  do {                                                                         \
    KdStream << Indent << DIRECTIVE " "                                        \
             << ((TwoByteBuffer & MASK) >> (MASK##_SHIFT)) << '\n';            \
  } while (0)

  uint16_t TwoByteBuffer = 0;
  uint32_t FourByteBuffer = 0;

  StringRef ReservedBytes;
  StringRef Indent = "\t";

  assert(Bytes.size() == 64);
  DataExtractor DE(Bytes, /*IsLittleEndian=*/true, /*AddressSize=*/8);

  switch (Cursor.tell()) {
  case amdhsa::GROUP_SEGMENT_FIXED_SIZE_OFFSET:
    FourByteBuffer = DE.getU32(Cursor);
    KdStream << Indent << ".amdhsa_group_segment_fixed_size " << FourByteBuffer
             << '\n';
    return MCDisassembler::Success;

  case amdhsa::PRIVATE_SEGMENT_FIXED_SIZE_OFFSET:
    FourByteBuffer = DE.getU32(Cursor);
    KdStream << Indent << ".amdhsa_private_segment_fixed_size "
             << FourByteBuffer << '\n';
    return MCDisassembler::Success;

  case amdhsa::KERNARG_SIZE_OFFSET:
    FourByteBuffer = DE.getU32(Cursor);
    KdStream << Indent << ".amdhsa_kernarg_size "
             << FourByteBuffer << '\n';
    return MCDisassembler::Success;

  case amdhsa::RESERVED0_OFFSET:
    // 4 reserved bytes, must be 0.
    ReservedBytes = DE.getBytes(Cursor, 4);
    for (int I = 0; I < 4; ++I) {
      if (ReservedBytes[I] != 0) {
        return MCDisassembler::Fail;
      }
    }
    return MCDisassembler::Success;

  case amdhsa::KERNEL_CODE_ENTRY_BYTE_OFFSET_OFFSET:
    // KERNEL_CODE_ENTRY_BYTE_OFFSET
    // So far no directive controls this for Code Object V3, so simply skip for
    // disassembly.
    DE.skip(Cursor, 8);
    return MCDisassembler::Success;

  case amdhsa::RESERVED1_OFFSET:
    // 20 reserved bytes, must be 0.
    ReservedBytes = DE.getBytes(Cursor, 20);
    for (int I = 0; I < 20; ++I) {
      if (ReservedBytes[I] != 0) {
        return MCDisassembler::Fail;
      }
    }
    return MCDisassembler::Success;

  case amdhsa::COMPUTE_PGM_RSRC3_OFFSET:
    FourByteBuffer = DE.getU32(Cursor);
    return decodeCOMPUTE_PGM_RSRC3(FourByteBuffer, KdStream);

  case amdhsa::COMPUTE_PGM_RSRC1_OFFSET:
    FourByteBuffer = DE.getU32(Cursor);
    return decodeCOMPUTE_PGM_RSRC1(FourByteBuffer, KdStream);

  case amdhsa::COMPUTE_PGM_RSRC2_OFFSET:
    FourByteBuffer = DE.getU32(Cursor);
    return decodeCOMPUTE_PGM_RSRC2(FourByteBuffer, KdStream);

  case amdhsa::KERNEL_CODE_PROPERTIES_OFFSET:
    using namespace amdhsa;
    TwoByteBuffer = DE.getU16(Cursor);

    if (!hasArchitectedFlatScratch())
      PRINT_DIRECTIVE(".amdhsa_user_sgpr_private_segment_buffer",
                      KERNEL_CODE_PROPERTY_ENABLE_SGPR_PRIVATE_SEGMENT_BUFFER);
    PRINT_DIRECTIVE(".amdhsa_user_sgpr_dispatch_ptr",
                    KERNEL_CODE_PROPERTY_ENABLE_SGPR_DISPATCH_PTR);
    PRINT_DIRECTIVE(".amdhsa_user_sgpr_queue_ptr",
                    KERNEL_CODE_PROPERTY_ENABLE_SGPR_QUEUE_PTR);
    PRINT_DIRECTIVE(".amdhsa_user_sgpr_kernarg_segment_ptr",
                    KERNEL_CODE_PROPERTY_ENABLE_SGPR_KERNARG_SEGMENT_PTR);
    PRINT_DIRECTIVE(".amdhsa_user_sgpr_dispatch_id",
                    KERNEL_CODE_PROPERTY_ENABLE_SGPR_DISPATCH_ID);
    if (!hasArchitectedFlatScratch())
      PRINT_DIRECTIVE(".amdhsa_user_sgpr_flat_scratch_init",
                      KERNEL_CODE_PROPERTY_ENABLE_SGPR_FLAT_SCRATCH_INIT);
    PRINT_DIRECTIVE(".amdhsa_user_sgpr_private_segment_size",
                    KERNEL_CODE_PROPERTY_ENABLE_SGPR_PRIVATE_SEGMENT_SIZE);

    if (TwoByteBuffer & KERNEL_CODE_PROPERTY_RESERVED0)
      return MCDisassembler::Fail;

    // Reserved for GFX9
    if (isGFX9() &&
        (TwoByteBuffer & KERNEL_CODE_PROPERTY_ENABLE_WAVEFRONT_SIZE32)) {
      return MCDisassembler::Fail;
    } else if (isGFX10Plus()) {
      PRINT_DIRECTIVE(".amdhsa_wavefront_size32",
                      KERNEL_CODE_PROPERTY_ENABLE_WAVEFRONT_SIZE32);
    }

    if (AMDGPU::getAmdhsaCodeObjectVersion() >= AMDGPU::AMDHSA_COV5)
      PRINT_DIRECTIVE(".amdhsa_uses_dynamic_stack",
                      KERNEL_CODE_PROPERTY_USES_DYNAMIC_STACK);

    if (TwoByteBuffer & KERNEL_CODE_PROPERTY_RESERVED1)
      return MCDisassembler::Fail;

    return MCDisassembler::Success;

  case amdhsa::KERNARG_PRELOAD_OFFSET:
    using namespace amdhsa;
    TwoByteBuffer = DE.getU16(Cursor);
    if (TwoByteBuffer & KERNARG_PRELOAD_SPEC_LENGTH) {
      PRINT_DIRECTIVE(".amdhsa_user_sgpr_kernarg_preload_length",
                      KERNARG_PRELOAD_SPEC_LENGTH);
    }

    if (TwoByteBuffer & KERNARG_PRELOAD_SPEC_OFFSET) {
      PRINT_DIRECTIVE(".amdhsa_user_sgpr_kernarg_preload_offset",
                      KERNARG_PRELOAD_SPEC_OFFSET);
    }
    return MCDisassembler::Success;

  case amdhsa::RESERVED3_OFFSET:
    // 4 bytes from here are reserved, must be 0.
    ReservedBytes = DE.getBytes(Cursor, 4);
    for (int I = 0; I < 4; ++I) {
      if (ReservedBytes[I] != 0)
        return MCDisassembler::Fail;
    }
    return MCDisassembler::Success;

  default:
    llvm_unreachable("Unhandled index. Case statements cover everything.");
    return MCDisassembler::Fail;
  }
#undef PRINT_DIRECTIVE
}

MCDisassembler::DecodeStatus AMDGPUDisassembler::decodeKernelDescriptor(
    StringRef KdName, ArrayRef<uint8_t> Bytes, uint64_t KdAddress) const {
  // CP microcode requires the kernel descriptor to be 64 aligned.
  if (Bytes.size() != 64 || KdAddress % 64 != 0)
    return MCDisassembler::Fail;

  // FIXME: We can't actually decode "in order" as is done below, as e.g. GFX10
  // requires us to know the setting of .amdhsa_wavefront_size32 in order to
  // accurately produce .amdhsa_next_free_vgpr, and they appear in the wrong
  // order. Workaround this by first looking up .amdhsa_wavefront_size32 here
  // when required.
  if (isGFX10Plus()) {
    uint16_t KernelCodeProperties =
        support::endian::read16(&Bytes[amdhsa::KERNEL_CODE_PROPERTIES_OFFSET],
                                llvm::endianness::little);
    EnableWavefrontSize32 =
        AMDHSA_BITS_GET(KernelCodeProperties,
                        amdhsa::KERNEL_CODE_PROPERTY_ENABLE_WAVEFRONT_SIZE32);
  }

  std::string Kd;
  raw_string_ostream KdStream(Kd);
  KdStream << ".amdhsa_kernel " << KdName << '\n';

  DataExtractor::Cursor C(0);
  while (C && C.tell() < Bytes.size()) {
    MCDisassembler::DecodeStatus Status =
        decodeKernelDescriptorDirective(C, Bytes, KdStream);

    cantFail(C.takeError());

    if (Status == MCDisassembler::Fail)
      return MCDisassembler::Fail;
  }
  KdStream << ".end_amdhsa_kernel\n";
  outs() << KdStream.str();
  return MCDisassembler::Success;
}

std::optional<MCDisassembler::DecodeStatus>
AMDGPUDisassembler::onSymbolStart(SymbolInfoTy &Symbol, uint64_t &Size,
                                  ArrayRef<uint8_t> Bytes, uint64_t Address,
                                  raw_ostream &CStream) const {
  // Right now only kernel descriptor needs to be handled.
  // We ignore all other symbols for target specific handling.
  // TODO:
  // Fix the spurious symbol issue for AMDGPU kernels. Exists for both Code
  // Object V2 and V3 when symbols are marked protected.

  // amd_kernel_code_t for Code Object V2.
  if (Symbol.Type == ELF::STT_AMDGPU_HSA_KERNEL) {
    Size = 256;
    return MCDisassembler::Fail;
  }

  // Code Object V3 kernel descriptors.
  StringRef Name = Symbol.Name;
  if (Symbol.Type == ELF::STT_OBJECT && Name.ends_with(StringRef(".kd"))) {
    Size = 64; // Size = 64 regardless of success or failure.
    return decodeKernelDescriptor(Name.drop_back(3), Bytes, Address);
  }
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// AMDGPUSymbolizer
//===----------------------------------------------------------------------===//

// Try to find symbol name for specified label
bool AMDGPUSymbolizer::tryAddingSymbolicOperand(
    MCInst &Inst, raw_ostream & /*cStream*/, int64_t Value,
    uint64_t /*Address*/, bool IsBranch, uint64_t /*Offset*/,
    uint64_t /*OpSize*/, uint64_t /*InstSize*/) {

  if (!IsBranch) {
    return false;
  }

  auto *Symbols = static_cast<SectionSymbolsTy *>(DisInfo);
  if (!Symbols)
    return false;

  auto Result = llvm::find_if(*Symbols, [Value](const SymbolInfoTy &Val) {
    return Val.Addr == static_cast<uint64_t>(Value) &&
           Val.Type == ELF::STT_NOTYPE;
  });
  if (Result != Symbols->end()) {
    auto *Sym = Ctx.getOrCreateSymbol(Result->Name);
    const auto *Add = MCSymbolRefExpr::create(Sym, Ctx);
    Inst.addOperand(MCOperand::createExpr(Add));
    return true;
  }
  // Add to list of referenced addresses, so caller can synthesize a label.
  ReferencedAddresses.push_back(static_cast<uint64_t>(Value));
  return false;
}

void AMDGPUSymbolizer::tryAddingPcLoadReferenceComment(raw_ostream &cStream,
                                                       int64_t Value,
                                                       uint64_t Address) {
  llvm_unreachable("unimplemented");
}

//===----------------------------------------------------------------------===//
// Initialization
//===----------------------------------------------------------------------===//

static MCSymbolizer *createAMDGPUSymbolizer(const Triple &/*TT*/,
                              LLVMOpInfoCallback /*GetOpInfo*/,
                              LLVMSymbolLookupCallback /*SymbolLookUp*/,
                              void *DisInfo,
                              MCContext *Ctx,
                              std::unique_ptr<MCRelocationInfo> &&RelInfo) {
  return new AMDGPUSymbolizer(*Ctx, std::move(RelInfo), DisInfo);
}

static MCDisassembler *createAMDGPUDisassembler(const Target &T,
                                                const MCSubtargetInfo &STI,
                                                MCContext &Ctx) {
  return new AMDGPUDisassembler(STI, Ctx, T.createMCInstrInfo());
}

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeAMDGPUDisassembler() {
  TargetRegistry::RegisterMCDisassembler(getTheGCNTarget(),
                                         createAMDGPUDisassembler);
  TargetRegistry::RegisterMCSymbolizer(getTheGCNTarget(),
                                       createAMDGPUSymbolizer);
}
