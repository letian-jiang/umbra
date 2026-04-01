#include "ir/printer.h"

#include <cstdint>
#include <cstring>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace umbra::ir {

namespace {

/// Format a constant value for printing.
std::string formatConstValue(TypeId type, uint64_t val) {
    switch (type) {
        case TypeId::VOID:
            return "void";
        case TypeId::BOOL:
            return (val != 0) ? "true" : "false";
        case TypeId::INT8:
            return std::to_string(static_cast<int8_t>(val));
        case TypeId::INT32:
            return std::to_string(static_cast<int32_t>(val));
        case TypeId::INT64:
            return std::to_string(static_cast<int64_t>(val));
        case TypeId::UINT64:
            return std::to_string(val);
        case TypeId::PTR:
            return "0x" + ([&] {
                       std::ostringstream oss;
                       oss << std::hex << val;
                       return oss.str();
                   })();
    }
    return "???";
}

/// Build a mapping from block buffer offsets to block names for a function.
std::unordered_map<uint32_t, std::string> buildBlockNames(const Program& program,
                                                          FunctionId funcId) {
    std::unordered_map<uint32_t, std::string> names;
    const auto& entry = program.functionEntry(funcId);
    if (entry.entryBlockOffset == IR_INVALID_BLOCK) {
        return names;
    }

    std::queue<uint32_t> worklist;
    worklist.push(entry.entryBlockOffset);
    int blockIndex = 0;

    while (!worklist.empty()) {
        uint32_t blockOff = worklist.front();
        worklist.pop();

        if (names.count(blockOff) != 0) {
            continue;
        }

        std::string name = (blockIndex == 0) ? "entry" : ("bb" + std::to_string(blockIndex));
        names[blockOff] = name;
        ++blockIndex;

        // Walk instructions to find terminators with branch targets.
        const auto& blockBuf = program.blockBuffer();
        const auto& instBuf = program.instBuffer();
        uint32_t pos = blockOff;
        while (pos < static_cast<uint32_t>(blockBuf.size())) {
            uint32_t instOff = blockBuf[pos];
            auto opcode = static_cast<Opcode>(instBuf[instOff]);
            ++pos;

            if (isTerminator(opcode)) {
                if (opcode == Opcode::BR) {
                    uint32_t target = 0;
                    std::memcpy(&target, instBuf.data() + instOff + 2, 4);
                    worklist.push(target);
                } else if (opcode == Opcode::COND_BR) {
                    uint32_t trueT = 0;
                    uint32_t falseT = 0;
                    std::memcpy(&trueT, instBuf.data() + instOff + 2 + 4, 4);
                    std::memcpy(&falseT, instBuf.data() + instOff + 2 + 8, 4);
                    worklist.push(trueT);
                    worklist.push(falseT);
                } else if (opcode == Opcode::CHECKED_ADD || opcode == Opcode::CHECKED_SUB ||
                           opcode == Opcode::CHECKED_MUL) {
                    uint32_t contT = 0;
                    uint32_t overflowT = 0;
                    std::memcpy(&contT, instBuf.data() + instOff + 2 + 8, 4);
                    std::memcpy(&overflowT, instBuf.data() + instOff + 2 + 12, 4);
                    worklist.push(contT);
                    worklist.push(overflowT);
                }
                break;
            }
        }
    }

    return names;
}

}  // namespace

std::string IRPrinter::print(const Program& program) {
    std::ostringstream out;
    for (uint32_t i = 0; i < program.numFunctions(); ++i) {
        if (program.hostFunction(i) != nullptr) {
            out << "; host function @" << program.functionMeta(i).name << "\n";
            continue;
        }
        out << printFunction(program, i);
        out << "\n";
    }
    return out.str();
}

std::string IRPrinter::printFunction(const Program& program, FunctionId funcId) {
    std::ostringstream out;
    const auto& meta = program.functionMeta(funcId);
    const auto& entry = program.functionEntry(funcId);

    // Function header.
    out << "define " << typeName(meta.returnType) << " @" << meta.name << "(";
    for (uint32_t i = 0; i < meta.numParams; ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << typeName(meta.paramTypes[i]) << " %" << i;
    }
    out << ") {\n";

    if (entry.entryBlockOffset == IR_INVALID_BLOCK) {
        out << "  ; no entry block\n}\n";
        return out.str();
    }

    auto blockNames = buildBlockNames(program, funcId);

    // Visit blocks in BFS order.
    std::queue<uint32_t> worklist;
    std::unordered_set<uint32_t> visited;
    worklist.push(entry.entryBlockOffset);

    const auto& blockBuf = program.blockBuffer();
    const auto& instBuf = program.instBuffer();

    while (!worklist.empty()) {
        uint32_t blockOff = worklist.front();
        worklist.pop();

        if (visited.count(blockOff) != 0) {
            continue;
        }
        visited.insert(blockOff);

        // Print block label.
        auto nameIt = blockNames.find(blockOff);
        if (nameIt != blockNames.end()) {
            out << nameIt->second << ":\n";
        } else {
            out << "block_" << blockOff << ":\n";
        }

        // Print instructions.
        uint32_t pos = blockOff;
        while (pos < static_cast<uint32_t>(blockBuf.size())) {
            uint32_t instOff = blockBuf[pos];
            auto opcode = static_cast<Opcode>(instBuf[instOff]);
            auto type = static_cast<TypeId>(instBuf[instOff + 1]);
            ++pos;

            if (opcode == Opcode::NOP) {
                if (isTerminator(opcode)) {
                    break;
                }
                continue;  // Skip dead instructions.
            }

            out << "    ";

            // Print result assignment for non-void instructions.
            bool hasResult = (type != TypeId::VOID);
            if (hasResult && !isTerminator(opcode)) {
                out << "%" << instOff << " = ";
            }

            out << opcodeName(opcode) << " " << typeName(type);

            InstructionView view(instBuf.data(), instOff);

            if (opcode == Opcode::CONST) {
                out << " " << formatConstValue(type, view.constPayload());
            } else if (isPhi(opcode)) {
                // Print PHI as: phi type [block0: %val0, block1: %val1, ...]
                auto count = phiIncomingCount(opcode);
                for (uint8_t j = 0; j < count; ++j) {
                    uint32_t boff = view.arg(j * 2);
                    uint32_t voff = view.arg(j * 2 + 1);
                    if (j > 0) {
                        out << ",";
                    }
                    out << " [";
                    auto bName = blockNames.find(boff);
                    if (bName != blockNames.end()) {
                        out << bName->second;
                    } else {
                        out << "block_" << boff;
                    }
                    out << ": %" << voff << "]";
                }
            } else if (opcode == Opcode::BR) {
                uint32_t target = view.arg(0);
                auto bName = blockNames.find(target);
                if (bName != blockNames.end()) {
                    out << " " << bName->second;
                } else {
                    out << " block_" << target;
                }
            } else if (opcode == Opcode::COND_BR) {
                out << " %" << view.arg(0);
                uint32_t trueTarget = view.arg(1);
                uint32_t falseTarget = view.arg(2);
                auto trueName = blockNames.find(trueTarget);
                auto falseName = blockNames.find(falseTarget);
                out << ", ";
                if (trueName != blockNames.end()) {
                    out << trueName->second;
                } else {
                    out << "block_" << trueTarget;
                }
                out << ", ";
                if (falseName != blockNames.end()) {
                    out << falseName->second;
                } else {
                    out << "block_" << falseTarget;
                }
            } else {
                // Generic: print args as %offset.
                for (uint8_t j = 0; j < view.numArgs(); ++j) {
                    if (j > 0) {
                        out << ",";
                    }
                    out << " %" << view.arg(j);
                }
            }

            out << "\n";

            if (isTerminator(opcode)) {
                // Enqueue branch targets.
                if (opcode == Opcode::BR) {
                    worklist.push(view.arg(0));
                } else if (opcode == Opcode::COND_BR) {
                    worklist.push(view.arg(1));
                    worklist.push(view.arg(2));
                } else if (opcode == Opcode::CHECKED_ADD || opcode == Opcode::CHECKED_SUB ||
                           opcode == Opcode::CHECKED_MUL) {
                    worklist.push(view.arg(2));
                    worklist.push(view.arg(3));
                }
                break;
            }
        }

        out << "\n";
    }

    out << "}\n";
    return out.str();
}

std::string IRPrinter::printInstruction(const Program& program, IROp offset) {
    std::ostringstream out;
    InstructionView view(program.instBuffer().data(), offset);
    auto opcode = view.opcode();
    auto type = view.type();

    bool hasResult = (type != TypeId::VOID);
    if (hasResult && !isTerminator(opcode)) {
        out << "%" << offset << " = ";
    }

    out << opcodeName(opcode) << " " << typeName(type);

    if (opcode == Opcode::CONST) {
        out << " " << formatConstValue(type, view.constPayload());
    } else {
        for (uint8_t i = 0; i < view.numArgs(); ++i) {
            if (i > 0) {
                out << ",";
            }
            out << " %" << view.arg(i);
        }
    }

    return out.str();
}

}  // namespace umbra::ir
