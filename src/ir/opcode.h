#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace umbra::ir {

/// IR type identifiers — maps directly to machine capabilities.
enum class TypeId : uint8_t {
    VOID = 0,    // No value (stores, branches)
    BOOL = 1,    // 1-byte boolean
    INT8 = 2,    // Signed 8-bit integer
    INT32 = 3,   // Signed 32-bit integer
    INT64 = 4,   // Signed 64-bit integer
    UINT64 = 5,  // Unsigned 64-bit integer
    PTR = 6,     // Opaque pointer
};

/// Size of a TypeId in bytes.
inline uint8_t typeSize(TypeId id) {
    switch (id) {
        case TypeId::VOID:
            return 0;
        case TypeId::BOOL:
        case TypeId::INT8:
            return 1;
        case TypeId::INT32:
            return 4;
        case TypeId::INT64:
        case TypeId::UINT64:
        case TypeId::PTR:
            return 8;
    }
    return 0;
}

/// Short type name for textual printing.
inline std::string_view typeName(TypeId id) {
    switch (id) {
        case TypeId::VOID:
            return "void";
        case TypeId::BOOL:
            return "bool";
        case TypeId::INT8:
            return "i8";
        case TypeId::INT32:
            return "i32";
        case TypeId::INT64:
            return "i64";
        case TypeId::UINT64:
            return "u64";
        case TypeId::PTR:
            return "ptr";
    }
    return "???";
}

/// Opcode enum — ~63 opcodes covering constants, arithmetic, bitwise,
/// comparison, logical, cast, memory, control flow, SSA, and DB-specific.
/// Variable-argument instructions (phi, call) use per-arity variants.
enum class Opcode : uint8_t {
    // Constants and arguments (§5.2)
    CONST = 0,
    FUNARG = 1,

    // Arithmetic (§5.3)
    ADD = 2,
    SUB = 3,
    MUL = 4,
    DIV = 5,
    UDIV = 6,
    MOD = 7,
    NEG = 8,

    // Checked arithmetic (§5.3)
    CHECKED_ADD = 9,
    CHECKED_SUB = 10,
    CHECKED_MUL = 11,

    // Bitwise (§5.4)
    AND = 12,
    OR = 13,
    XOR = 14,
    SHL = 15,
    ASHR = 16,
    LSHR = 17,
    ROTL = 18,
    ROTR = 19,

    // Comparison (§5.5)
    CMP_EQ = 20,
    CMP_NE = 21,
    CMP_LT = 22,
    CMP_LE = 23,
    CMP_GT = 24,
    CMP_GE = 25,

    // Logical (§5.6)
    LNOT = 26,
    LAND = 27,
    LOR = 28,
    SELECT = 29,

    // Cast (§5.7)
    SEXT = 30,
    ZEXT = 31,
    TRUNC = 32,
    BITCAST = 33,
    PTR2INT = 34,
    INT2PTR = 35,

    // Memory (§5.8)
    LOAD = 36,
    STORE = 37,
    GEP = 38,

    // Control flow (§5.9)
    BR = 39,
    COND_BR = 40,
    RET = 41,
    RET_VOID = 42,
    SWITCH = 43,

    // Call variants — call0 through call6 (callee + 0..6 args)
    CALL0 = 44,
    CALL1 = 45,
    CALL2 = 46,
    CALL3 = 47,
    CALL4 = 48,
    CALL5 = 49,
    CALL6 = 50,

    // PHI variants — phi2 through phi6 (2..6 incoming edges)
    PHI2 = 51,
    PHI3 = 52,
    PHI4 = 53,
    PHI5 = 54,
    PHI6 = 55,

    // DB-specific (§5.11)
    IS_NULL = 56,
    CRC32 = 57,
    BSWAP = 58,

    // NOP — used by DCE to mark dead instructions
    NOP = 59,

    NUM_OPCODES = 60,
};

/// Metadata about each opcode.
struct OpcodeInfo {
    std::string_view name;
    uint8_t argCount;
    bool isTerminator;
    bool hasSideEffect;  // stores, calls, branches
};

/// Static metadata table indexed by opcode.
// NOLINTNEXTLINE(readability-identifier-naming)
inline constexpr std::array<OpcodeInfo, static_cast<size_t>(Opcode::NUM_OPCODES)> OPCODE_INFO = {{
    // CONST: 0 args in encoding; payload appended separately
    {"const", 0, false, false},
    // FUNARG: 1 arg (index)
    {"funarg", 1, false, false},

    // Arithmetic: 2 args (lhs, rhs)
    {"add", 2, false, false},
    {"sub", 2, false, false},
    {"mul", 2, false, false},
    {"div", 2, false, false},
    {"udiv", 2, false, false},
    {"mod", 2, false, false},
    {"neg", 2, false, false},  // encoded as binary with 0 first arg

    // Checked arithmetic: 4 args (lhs, rhs, cont_block, overflow_block)
    {"checkedadd", 4, true, false},
    {"checkedsub", 4, true, false},
    {"checkedmul", 4, true, false},

    // Bitwise: 2 args
    {"and", 2, false, false},
    {"or", 2, false, false},
    {"xor", 2, false, false},
    {"shl", 2, false, false},
    {"ashr", 2, false, false},
    {"lshr", 2, false, false},
    {"rotl", 2, false, false},
    {"rotr", 2, false, false},

    // Comparison: 2 args → Bool
    {"cmpeq", 2, false, false},
    {"cmpne", 2, false, false},
    {"cmplt", 2, false, false},
    {"cmple", 2, false, false},
    {"cmpgt", 2, false, false},
    {"cmpge", 2, false, false},

    // Logical
    {"lnot", 1, false, false},
    {"land", 2, false, false},
    {"lor", 2, false, false},
    {"select", 3, false, false},  // cond, true_val, false_val

    // Cast: 1 arg
    {"sext", 1, false, false},
    {"zext", 1, false, false},
    {"trunc", 1, false, false},
    {"bitcast", 1, false, false},
    {"ptr2int", 1, false, false},
    {"int2ptr", 1, false, false},

    // Memory
    {"load", 1, false, false},           // ptr
    {"store", 2, false, true},           // val, ptr
    {"getelementptr", 2, false, false},  // base, index (type encodes element type)

    // Control flow
    {"br", 1, true, true},      // target_block
    {"condbr", 3, true, true},  // cond, true_bb, false_bb
    {"ret", 1, true, true},     // value
    {"retvoid", 0, true, true},
    {"switch", 0, true, true},  // placeholder — not implemented in MVP

    // Call variants: 1+N args (callee + N params)
    {"call0", 1, false, true},
    {"call1", 2, false, true},
    {"call2", 3, false, true},
    {"call3", 4, false, true},
    {"call4", 5, false, true},
    {"call5", 6, false, true},
    {"call6", 7, false, true},

    // PHI variants: 2*N args (N pairs of block, value)
    {"phi2", 4, false, false},
    {"phi3", 6, false, false},
    {"phi4", 8, false, false},
    {"phi5", 10, false, false},
    {"phi6", 12, false, false},

    // DB-specific
    {"isnull", 1, false, false},
    {"crc32", 2, false, false},
    {"bswap", 1, false, false},

    // NOP
    {"nop", 0, false, false},
}};

/// Get arg count for an opcode.
inline uint8_t opcodeArgCount(Opcode op) {
    return OPCODE_INFO[static_cast<uint8_t>(op)].argCount;
}

/// Get name for an opcode.
inline std::string_view opcodeName(Opcode op) {
    return OPCODE_INFO[static_cast<uint8_t>(op)].name;
}

/// Check if opcode is a terminator.
inline bool isTerminator(Opcode op) {
    return OPCODE_INFO[static_cast<uint8_t>(op)].isTerminator;
}

/// Check if opcode has side effects.
inline bool hasSideEffect(Opcode op) {
    return OPCODE_INFO[static_cast<uint8_t>(op)].hasSideEffect;
}

/// Check if opcode is a PHI variant.
inline bool isPhi(Opcode op) {
    return op >= Opcode::PHI2 && op <= Opcode::PHI6;
}

/// Check if opcode is a call variant.
inline bool isCall(Opcode op) {
    return op >= Opcode::CALL0 && op <= Opcode::CALL6;
}

/// Get the PHI incoming count from opcode.
inline uint8_t phiIncomingCount(Opcode op) {
    return static_cast<uint8_t>(static_cast<uint8_t>(op) - static_cast<uint8_t>(Opcode::PHI2) + 2);
}

/// Get the call arg count (excluding callee) from opcode.
inline uint8_t callParamCount(Opcode op) {
    return static_cast<uint8_t>(static_cast<uint8_t>(op) - static_cast<uint8_t>(Opcode::CALL0));
}

/// Get the PHI opcode for a given incoming count.
inline Opcode phiOpcode(uint8_t incomingCount) {
    return static_cast<Opcode>(static_cast<uint8_t>(Opcode::PHI2) + incomingCount - 2);
}

/// Get the call opcode for a given parameter count.
inline Opcode callOpcode(uint8_t paramCount) {
    return static_cast<Opcode>(static_cast<uint8_t>(Opcode::CALL0) + paramCount);
}

}  // namespace umbra::ir
