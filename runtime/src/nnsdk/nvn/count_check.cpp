#include "nnsdk/nvn/count_check.h"

#include <initializer_list>

namespace nvn {

namespace {

// SPIR-V opcodes, decorations and storage classes used here (the specification's numbers)
enum : uint32_t {
    kOpEntryPoint = 15, kOpTypeBool = 20, kOpTypeInt = 21, kOpTypeVector = 23, kOpTypeRuntimeArray = 29,
    kOpTypeStruct = 30, kOpTypePointer = 32, kOpConstant = 43, kOpConstantNull = 46, kOpFunction = 54,
    kOpFunctionParameter = 55, kOpVariable = 59, kOpLoad = 61, kOpStore = 62, kOpAccessChain = 65, kOpDecorate = 71,
    kOpMemberDecorate = 72, kOpUGreaterThanEqual = 174, kOpSelectionMerge = 247, kOpLabel = 248,
    kOpBranchConditional = 250, kOpReturn = 253,
};
enum : uint32_t { kBlock = 2, kArrayStride = 6, kBuiltIn = 11, kNonWritable = 24, kBinding = 33, kDescriptorSet = 34, kOffset = 35 };
enum : uint32_t { kOutput = 3, kPushConstant = 9, kStorageBuffer = 12, kFunctionClass = 7 };
constexpr uint32_t kPosition = 0;

void emit(std::vector<uint32_t>& out, uint32_t op, std::initializer_list<uint32_t> operands) {
    out.push_back(static_cast<uint32_t>(operands.size() + 1) << 16 | op);
    out.insert(out.end(), operands.begin(), operands.end());
}

}  // namespace

bool add_count_check(std::vector<uint32_t>& w) {
    if (w.size() < 5 || w[0] != 0x07230203u) return false;
    uint32_t bound = w[3];
    uint32_t entry = 0, position = 0, uint_type = 0, bool_type = 0;
    size_t first_type = 0, first_function = 0;
    std::vector<std::pair<uint32_t, uint32_t>> pointers;  // OpTypePointer: id -> pointee (Output only)
    std::vector<std::pair<uint32_t, uint32_t>> outputs;   // OpVariable Output: id -> pointer type
    std::vector<uint32_t> position_candidates;
    for (size_t i = 5; i < w.size();) {
        const uint32_t n = w[i] >> 16, op = w[i] & 0xFFFF;
        if (!n || i + n > w.size()) return false;
        const uint32_t* a = &w[i + 1];
        if (op == kOpFunction) {
            first_function = i;
            break;
        }
        if (!first_type && ((op >= 19 && op <= 52) || op == kOpVariable)) first_type = i;
        switch (op) {
            case kOpEntryPoint:
                if (n >= 3) {
                    if (entry) return false;  // (one entry point only)
                    entry = a[1];
                }
                break;
            case kOpDecorate:
                if (n >= 4 && a[1] == kBuiltIn && a[2] == kPosition) position_candidates.push_back(a[0]);
                break;
            case kOpMemberDecorate:
                if (n >= 5 && a[2] == kBuiltIn && a[3] == kPosition) return false;  // (gl_PerVertex block: not handled)
                break;
            case kOpTypeInt: if (n == 4 && a[1] == 32 && a[2] == 0) uint_type = a[0]; break;
            case kOpTypeBool: if (n == 2) bool_type = a[0]; break;
            case kOpTypePointer: if (n == 4 && a[1] == kOutput) pointers.emplace_back(a[0], a[2]); break;
            case kOpVariable: if (n >= 4 && a[2] == kOutput) outputs.emplace_back(a[1], a[0]); break;
            default: break;
        }
        i += n;
    }
    if (!entry || !first_function || !first_type) return false;
    uint32_t vec4_type = 0;
    for (uint32_t id : position_candidates)
        for (const auto& [var, ptr] : outputs)
            if (var == id)
                for (const auto& [p, pointee] : pointers)
                    if (p == ptr) position = var, vec4_type = pointee;
    if (!position || !vec4_type) return false;

    // The entry function and its first block: OpLabel, then its OpVariables
    size_t label = 0;
    for (size_t i = first_function; i < w.size();) {
        const uint32_t n = w[i] >> 16, op = w[i] & 0xFFFF;
        if (!n || i + n > w.size()) return false;
        if (op == kOpFunction && w[i + 2] == entry) {
            size_t j = i + n;
            while (j < w.size() && (w[j] & 0xFFFF) == kOpFunctionParameter) j += w[j] >> 16;
            if (j < w.size() && (w[j] & 0xFFFF) == kOpLabel) label = j;
            break;
        }
        i += n;
    }
    if (!label) return false;
    const uint32_t old_label = w[label + 1];
    size_t body = label + 2;  // past the OpLabel
    std::vector<uint32_t> variables;  // the block's OpVariables: they have to stay in the function's first block
    while (body < w.size() && (w[body] & 0xFFFF) == kOpVariable) {
        const uint32_t n = w[body] >> 16;
        if (!n || body + n > w.size() || w[body + 3] != kFunctionClass) return false;
        variables.insert(variables.end(), w.begin() + body, w.begin() + body + n);
        body += n;
    }

    auto id = [&bound] { return bound++; };
    std::vector<uint32_t> decorations, globals, entry_block;
    if (!uint_type) emit(globals, kOpTypeInt, {uint_type = id(), 32, 0});
    if (!bool_type) emit(globals, kOpTypeBool, {bool_type = id()});
    const uint32_t array = id(), ssbo_struct = id(), ssbo_ptr = id(), ssbo_uint_ptr = id();
    const uint32_t push_struct = id(), push_ptr = id(), push_uint_ptr = id();
    const uint32_t zero = id(), one = id(), null4 = id(), ssbo = id(), push = id();
    emit(globals, kOpTypeRuntimeArray, {array, uint_type});
    emit(globals, kOpTypeStruct, {ssbo_struct, array});
    emit(globals, kOpTypePointer, {ssbo_ptr, kStorageBuffer, ssbo_struct});
    emit(globals, kOpTypePointer, {ssbo_uint_ptr, kStorageBuffer, uint_type});
    emit(globals, kOpTypeStruct, {push_struct, uint_type, uint_type});
    emit(globals, kOpTypePointer, {push_ptr, kPushConstant, push_struct});
    emit(globals, kOpTypePointer, {push_uint_ptr, kPushConstant, uint_type});
    emit(globals, kOpConstant, {uint_type, zero, 0});
    emit(globals, kOpConstant, {uint_type, one, 1});
    emit(globals, kOpConstantNull, {vec4_type, null4});
    emit(globals, kOpVariable, {ssbo_ptr, ssbo, kStorageBuffer});
    emit(globals, kOpVariable, {push_ptr, push, kPushConstant});
    emit(decorations, kOpDecorate, {array, kArrayStride, 4});
    emit(decorations, kOpMemberDecorate, {ssbo_struct, 0, kOffset, 0});
    emit(decorations, kOpMemberDecorate, {ssbo_struct, 0, kNonWritable});  // (vertex stage: no storage writes allowed)
    emit(decorations, kOpDecorate, {ssbo_struct, kBlock});
    emit(decorations, kOpDecorate, {ssbo, kDescriptorSet, kCountCheckSet});
    emit(decorations, kOpDecorate, {ssbo, kBinding, 0});
    emit(decorations, kOpMemberDecorate, {push_struct, 0, kOffset, 0});
    emit(decorations, kOpMemberDecorate, {push_struct, 1, kOffset, 4});
    emit(decorations, kOpDecorate, {push_struct, kBlock});

    // New first block: the variables, then "if (draw >= count[word]) { gl_Position = 0; return; }", then on to the old one
    const uint32_t first = id(), dropped = id();
    const uint32_t draw_ptr = id(), draw = id(), word_ptr = id(), word = id(), count_ptr = id(), count = id(), past = id();
    emit(entry_block, kOpLabel, {first});
    entry_block.insert(entry_block.end(), variables.begin(), variables.end());
    emit(entry_block, kOpAccessChain, {push_uint_ptr, draw_ptr, push, zero});
    emit(entry_block, kOpLoad, {uint_type, draw, draw_ptr});
    emit(entry_block, kOpAccessChain, {push_uint_ptr, word_ptr, push, one});
    emit(entry_block, kOpLoad, {uint_type, word, word_ptr});
    emit(entry_block, kOpAccessChain, {ssbo_uint_ptr, count_ptr, ssbo, zero, word});
    emit(entry_block, kOpLoad, {uint_type, count, count_ptr});
    emit(entry_block, kOpUGreaterThanEqual, {bool_type, past, draw, count});
    emit(entry_block, kOpSelectionMerge, {old_label, 0});
    emit(entry_block, kOpBranchConditional, {past, dropped, old_label});
    emit(entry_block, kOpLabel, {dropped});
    emit(entry_block, kOpStore, {position, null4});
    emit(entry_block, kOpReturn, {});
    emit(entry_block, kOpLabel, {old_label});

    std::vector<uint32_t> out;
    out.reserve(w.size() + decorations.size() + globals.size() + entry_block.size());
    out.insert(out.end(), w.begin(), w.begin() + first_type);
    out.insert(out.end(), decorations.begin(), decorations.end());
    out.insert(out.end(), w.begin() + first_type, w.begin() + first_function);
    out.insert(out.end(), globals.begin(), globals.end());
    out.insert(out.end(), w.begin() + first_function, w.begin() + label);
    out.insert(out.end(), entry_block.begin(), entry_block.end());
    out.insert(out.end(), w.begin() + body, w.end());
    out[3] = bound;
    w.swap(out);
    return true;
}

}  // namespace nvn
