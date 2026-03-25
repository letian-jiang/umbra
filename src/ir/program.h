#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ir/block.h"
#include "ir/function.h"
#include "ir/instruction.h"
#include "ir/opcode.h"

namespace umbra::ir {

/// Host function signature — up to 6 uint64_t params, returns uint64_t.
using HostFunction = std::function<uint64_t(const uint64_t*, uint8_t)>;

/// Program — the complete IR for a query.
/// Contains three contiguous buffers: instructions, blocks, and functions.
class Program {
   public:
    /// Access the instruction buffer.
    [[nodiscard]] const std::vector<uint8_t>& instBuffer() const { return instBuffer_; }

    /// Access the block buffer (array of instruction offsets).
    [[nodiscard]] const std::vector<uint32_t>& blockBuffer() const { return blockBuffer_; }

    /// Number of functions.
    [[nodiscard]] uint32_t numFunctions() const { return static_cast<uint32_t>(functions_.size()); }

    /// Get a function entry by index.
    [[nodiscard]] const FunctionEntry& functionEntry(FunctionId id) const { return functions_[id]; }

    /// Get function metadata by index.
    [[nodiscard]] const FunctionMeta& functionMeta(FunctionId id) const { return meta_[id]; }

    /// Decode an instruction at a given offset.
    [[nodiscard]] InstructionView instructionAt(IROp offset) const {
        return {instBuffer_.data(), offset};
    }

    /// Create a BlockView for a block at a given offset in the block buffer.
    [[nodiscard]] BlockView blockAt(uint32_t blockOffset) const {
        return {blockBuffer_, instBuffer_, blockOffset};
    }

    /// Register a host function.
    void registerHostFunction(FunctionId id, HostFunction func) {
        hostFunctions_[id] = std::move(func);
    }

    /// Look up a host function.
    [[nodiscard]] const HostFunction* hostFunction(FunctionId id) const {
        auto iter = hostFunctions_.find(id);
        if (iter != hostFunctions_.end()) {
            return &iter->second;
        }
        return nullptr;
    }

    /// Look up host function by name (for convenience).
    [[nodiscard]] FunctionId hostFunctionId(const std::string& name) const {
        auto iter = hostFunctionIds_.find(name);
        if (iter != hostFunctionIds_.end()) {
            return iter->second;
        }
        return UINT32_MAX;
    }

    /// Register a host function by name and get its function ID.
    FunctionId registerHostFunction(const std::string& name, HostFunction func, TypeId returnType,
                                    const std::vector<TypeId>& paramTypes) {
        auto id = static_cast<FunctionId>(functions_.size());
        functions_.push_back({INVALID_BLOCK});  // No entry block for host functions
        FunctionMeta m;
        m.numParams = static_cast<uint32_t>(paramTypes.size());
        m.returnType = returnType;
        m.name = name;
        m.paramTypes = paramTypes;
        meta_.push_back(std::move(m));
        hostFunctions_[id] = std::move(func);
        hostFunctionIds_[name] = id;
        return id;
    }

   private:
    friend class IRBuilder;

    std::vector<uint8_t> instBuffer_;
    std::vector<uint32_t> blockBuffer_;
    std::vector<FunctionEntry> functions_;
    std::vector<FunctionMeta> meta_;
    std::unordered_map<FunctionId, HostFunction> hostFunctions_;
    std::unordered_map<std::string, FunctionId> hostFunctionIds_;
};

}  // namespace umbra::ir
