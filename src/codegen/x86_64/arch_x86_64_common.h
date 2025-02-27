#ifndef ARCH_X86_64_COMMON_H
#define ARCH_X86_64_COMMON_H

#include <codegen/x86_64/arch_x86_64.h>

#define DEFINE_REGISTER_ENUM(name, ...) REG_##name,
enum Registers_x86_64 {
  REG_NONE,
  FOR_ALL_X86_64_REGISTERS(DEFINE_REGISTER_ENUM)
  REG_COUNT
};
#undef DEFINE_REGISTER_ENUM

#define GENERAL_REGISTER_COUNT 14

extern Register general[GENERAL_REGISTER_COUNT];

/// RDI, RSI, RDX, RCX, R8, R9
#define LINUX_ARGUMENT_REGISTER_COUNT 6
extern Register linux_argument_registers[LINUX_ARGUMENT_REGISTER_COUNT];

/// RCX, RDX, R8, R9
#define MSWIN_ARGUMENT_REGISTER_COUNT 4
extern Register mswin_argument_registers[MSWIN_ARGUMENT_REGISTER_COUNT];

/// RAX, RCX, RDX, R8, R9, R10, R11, RSI, RDI
#define LINUX_CALLER_SAVED_REGISTER_COUNT 9
extern Register linux_caller_saved_registers[LINUX_CALLER_SAVED_REGISTER_COUNT];

/// Link to MSDN documentation (surely will fall away, but it's been Internet Archive'd).
/// https://docs.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170#callercallee-saved-registers
/// https://web.archive.org/web/20220916164241/https://docs.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170
/// "The x64 ABI considers the registers RAX, RCX, RDX, R8, R9, R10, R11, and XMM0-XMM5 volatile."
/// "The x64 ABI considers registers RBX, RBP, RDI, RSI, RSP, R12, R13, R14, R15, and XMM6-XMM15 nonvolatile."
/// RAX, RCX, RDX, R8, R9, R10, R11
#define MSWIN_CALLER_SAVED_REGISTER_COUNT 7
extern Register mswin_caller_saved_registers[MSWIN_CALLER_SAVED_REGISTER_COUNT];

/// Types of conditional jump instructions (Jcc).
/// Do NOT reorder these.
typedef enum IndirectJumpType {
  JUMP_TYPE_A,
  JUMP_TYPE_AE,
  JUMP_TYPE_B,
  JUMP_TYPE_BE,
  JUMP_TYPE_C,
  JUMP_TYPE_Z,
  JUMP_TYPE_E = JUMP_TYPE_Z,
  JUMP_TYPE_G,
  JUMP_TYPE_GE,
  JUMP_TYPE_L,
  JUMP_TYPE_LE,
  JUMP_TYPE_NA,
  JUMP_TYPE_NAE,
  JUMP_TYPE_NB,
  JUMP_TYPE_NBE,
  JUMP_TYPE_NC,
  JUMP_TYPE_NE,
  JUMP_TYPE_NZ = JUMP_TYPE_NE,
  JUMP_TYPE_NG,
  JUMP_TYPE_NGE,
  JUMP_TYPE_NL,
  JUMP_TYPE_NLE,
  JUMP_TYPE_NO,
  JUMP_TYPE_NP,
  JUMP_TYPE_NS,
  JUMP_TYPE_O,
  JUMP_TYPE_P,
  JUMP_TYPE_PE,
  JUMP_TYPE_PO,
  JUMP_TYPE_S,

  JUMP_TYPE_COUNT,
} IndirectJumpType;

typedef enum InstructionForm {
  I_FORM_NONE,
  I_FORM_IMM,
  I_FORM_IMM_TO_MEM,
  I_FORM_IMM_TO_REG,
  I_FORM_INDIRECT_BRANCH,
  I_FORM_MEM,
  I_FORM_MEM_TO_REG,
  I_FORM_NAME,
  I_FORM_NAME_TO_REG,
  I_FORM_REG,
  I_FORM_REG_SHIFT,
  I_FORM_REG_TO_MEM,
  I_FORM_REG_TO_NAME,
  I_FORM_REG_TO_OFFSET_NAME,
  I_FORM_REG_TO_REG,
  I_FORM_SETCC,
  I_FORM_JCC,
  I_FORM_IRBLOCK, // Marks beginning of basic block.
  I_FORM_IRFUNCTION, // Marks beginning of function.
  I_FORM_COUNT
} InstructionForm;

extern const char *jump_type_names_x86_64[JUMP_TYPE_COUNT];

typedef enum RegSize {
  r8 = 1,
  r16 = 2,
  r32 = 4,
  r64 = 8,
} RegSize;

/// Return the corresponding RegSize enum value to the given amount of
/// bytes (smallest fit). ICE if can not contain.
RegSize regsize_from_bytes(size_t bytes);

/// Return the corresponding byte size of a valid RegSize enum value.
/// ICE if enum value invalid.
size_t regbytes_from_size(RegSize r);

const char *regname(RegisterDescriptor reg, RegSize size);
const char *regname_from_bytes(RegisterDescriptor reg, size_t bytes);

/// Lookup functions for register names.
const char *register_name(RegisterDescriptor descriptor);
const char *register_name_32(RegisterDescriptor descriptor);
const char *register_name_16(RegisterDescriptor descriptor);
const char *register_name_8(RegisterDescriptor descriptor);

IndirectJumpType negate_jump(IndirectJumpType j);
IndirectJumpType comparison_to_jump_type(enum ComparisonType comparison);

typedef enum StackFrameKind {
  FRAME_FULL,    /// Push+restore rbp.
  FRAME_MINIMAL, /// Align stack pointer.
  FRAME_NONE,    /// Nothing.
  FRAME_COUNT
} StackFrameKind;

StackFrameKind stack_frame_kind(MIRFunction *f);

#endif /* ARCH_X86_64_COMMON_H */
