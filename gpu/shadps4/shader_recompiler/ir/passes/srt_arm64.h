// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace Shader::Arm64Srt {
// Keep the portable cache format: ARM64 code is regenerated from these operations on load.
enum SrtOp : std::uint32_t {
    OpEnd, OpPushImm, OpPushDyn, OpPop, OpCopyImm, OpCopyDyn, OpUserData, OpFlat,
    OpImm, OpAdd, OpSub, OpMul, OpShl, OpShr, OpAnd, OpOr, OpXor, OpNot,
    OpUMin, OpUMax, OpBitFieldUExtract, OpFindILsb,
    OpBitFieldInsert, OpSelect, OpEq, OpNe, OpULt, OpULe, OpSLt, OpSLe,
    // gow2018: dynamic guest-memory load. Pops dword offset, address hi, address lo
    // (in that order) and pushes the u32 read from (hi:lo) + off_dw * 4. Lets the
    // walker resolve offsets that depend on ReadConst/ReadConstBuffer values that
    // were never flattened into the flat buffer.
    OpLoad32,
};
constexpr std::uint32_t OpZeroed = 0x80000000u, SrtMagic = 0x42545253u;
constexpr std::uint32_t MaxDepth = 64, MaxValues = 32;

// x0=user data, x1=flattened data, x2=current pointer; x3-x7 are scratch.
// Only caller-saved registers are used. Stack layout: 64 pointers then 32 values.
inline std::vector<std::uint32_t> Compile(std::span<const std::uint32_t> program) {
    using U32 = std::uint32_t;
    constexpr U32 Frame = MaxDepth * 8 + MaxValues * 4;
    if (program.size() < 2 || program[0] != SrtMagic || program.size() > 262144) return {};
    std::vector<U32> out{0xd10003ffu | (Frame << 10), 0xaa0003e2u};
    std::size_t pc = 1;
    U32 depth = 0, count = 0;
    bool valid = true;
    const auto emit = [&](U32 code) { out.push_back(code); };
    const auto next = [&]() -> U32 {
        if (pc >= program.size()) { valid = false; return 0; }
        return program[pc++];
    };
    const auto imm = [&](U32 reg, U32 value) {
        emit(0x52800000u | ((value & 65535) << 5) | reg);
        if (value >> 16) emit(0x72a00000u | ((value >> 16) << 5) | reg);
    };
    const auto load = [&](U32 reg, U32 slot) {
        emit(0xb94003e0u | (((MaxDepth * 8 + slot * 4) / 4) << 10) | reg);
    };
    const auto store = [&](U32 reg, U32 slot) {
        emit(0xb90003e0u | (((MaxDepth * 8 + slot * 4) / 4) << 10) | reg);
    };
    while (valid && pc < program.size()) {
        const U32 word = next(), op = word & ~OpZeroed;
        const bool zero = (word & OpZeroed) != 0;
        if (op == OpEnd) {
            if (depth || count || pc != program.size()) return {};
            emit(0x910003ffu | (Frame << 10)); emit(0xd65f03c0u);
            return out;
        }
        if (op == OpPushImm || op == OpPushDyn || op == OpCopyImm || op == OpCopyDyn) {
            const bool push = op == OpPushImm || op == OpPushDyn;
            const bool dynamic = op == OpPushDyn || op == OpCopyDyn;
            if (dynamic) {
                if (!count) return {};
                load(4, --count);
                emit(0x0b040084u); emit(0x0b040084u); // 32-bit offset << 2, including wrap
            } else {
                imm(4, next() << 2);
            }
            if (push) {
                if (depth == MaxDepth) return {};
                emit(0xf90003e2u | (depth++ << 10));
                if (zero) {
                    emit(0xd2800002u); // mov x2, #0
                } else {
                    emit(0xaa0203e3u); // mov x3, x2
                    emit(0xd2800002u); // mov x2, #0
                    emit(0xf140047fu); // cmp x3, #0x1000
                    emit(0x54000063u); // b.lo +12 (skip ldr + ubfx)
                    emit(0xf8646862u); // ldr x2, [x3, w4, uxtw]
                    emit(0xd340bc42u); // mask guest pointer to 48 bits
                }
            } else {
                if (zero) {
                    emit(0x52800003u); // mov w3, #0
                } else {
                    emit(0x52800003u); // mov w3, #0
                    emit(0xf140045fu); // cmp x2, #0x1000
                    emit(0x54000043u); // b.lo +8 (skip ldr)
                    emit(0xb8646843u); // ldr w3, [x2, w4, uxtw]
                }
                const U32 offset = next();
                if (offset > 262143) return {};
                imm(5, offset * 4); emit(0xb8256823u);
            }
        } else if (op == OpPop) {
            if (!depth) return {};
            emit(0xf94003e2u | (--depth << 10));
        } else if (op == OpImm || op == OpFlat || op == OpUserData) {
            if (count == MaxValues) return {};
            const U32 value = next();
            if (op == OpImm) imm(3, value);
            else {
                if (value > 262143) return {};
                imm(5, value * 4); emit(0xb8656823u);
            }
            store(3, count++);
        } else if (op == OpNot) {
            if (!count) return {};
            load(3, count - 1); emit(0x2a2303e3u); store(3, count - 1);
        } else if (op == OpFindILsb) {
            if (!count) return {};
            load(3, count - 1);
            emit(0x7100007fu); // cmp w3, #0
            emit(0x5ac00063u); // rbit w3, w3
            emit(0x5ac01063u); // clz w3, w3
            emit(0x5a9f1063u); // csinv w3, w3, wzr, ne
            store(3, count - 1);
        } else if (op == OpBitFieldUExtract) {
            if (count < 3) return {};
            load(5, --count); load(4, --count); load(3, count - 1);
            emit(0x1ac42463u); imm(4, 1); emit(0x1ac52084u);
            emit(0x51000484u); emit(0x0a040063u); store(3, count - 1);
        } else if (op == OpBitFieldInsert) {
            if (count < 4) return {};
            load(6, --count); load(5, --count); load(4, --count); load(3, count - 1);
            emit(0x710080dfu); // cmp w6, #32
            imm(7, 32);        // mov w7, #32
            emit(0x1a8730c6u); // csel w6, w6, w7, lo
            emit(0xd2800027u); // mov x7, #1
            emit(0x9ac620e7u); // lsl x7, x7, x6
            emit(0xd10004e7u); // sub x7, x7, #1 -> w7 = mask
            emit(0x0a070084u); // and w4, w4, w7 -> insert & mask
            emit(0x1ac52084u); // lsl w4, w4, w5 -> (insert & mask) << offset
            emit(0x1ac520e7u); // lsl w7, w7, w5 -> mask << offset
            emit(0x0a270063u); // bic w3, w3, w7 -> base & ~(mask << offset)
            emit(0x2a040063u); // orr w3, w3, w4
            store(3, count - 1);
        } else if (op == OpSelect) {
            if (count < 3) return {};
            load(5, --count); load(4, --count); load(3, count - 1);
            emit(0x7100007fu); // cmp w3, #0
            emit(0x1a851083u); // csel w3, w4, w5, ne
            store(3, count - 1);
        } else if (op == OpLoad32) {
            // gow2018 dynamic guest load: pops off_dw (top), hi, lo; pushes u32 at
            // (hi:lo) + off_dw * 4. Guards small/null addresses like OpCopyImm.
            if (count < 3) return {};
            load(4, --count);        // off_dw
            load(5, --count);        // addr hi
            load(6, count - 1);      // addr lo
            emit(0xd3607ca5u);       // lsl x5, x5, #32
            emit(0xaa0600a5u);       // orr x5, x5, x6
            emit(0x0b040084u);       // add w4, w4, w4
            emit(0x0b040084u);       // add w4, w4, w4 (off_dw * 4)
            emit(0x52800003u);       // mov w3, #0
            emit(0xf14004bfu);       // cmp x5, #0x1000
            emit(0x54000043u);       // b.lo +8 (skip ldr)
            emit(0xb86468a3u);       // ldr w3, [x5, w4, uxtw]
            store(3, count - 1);
        } else {
            if (count < 2) return {};
            load(4, --count); load(3, count - 1);
            switch (op) {
            case OpAdd: emit(0x0b040063u); break;
            case OpSub: emit(0x4b040063u); break;
            case OpMul: emit(0x1b047c63u); break;
            case OpShl: emit(0x1ac42063u); break;
            case OpShr: emit(0x1ac42463u); break;
            case OpAnd: emit(0x0a040063u); break;
            case OpOr: emit(0x2a040063u); break;
            case OpXor: emit(0x4a040063u); break;
            case OpUMin: emit(0x6b04007fu); emit(0x1a843063u); break;
            case OpUMax: emit(0x6b04007fu); emit(0x1a848063u); break;
            case OpEq: emit(0x6b04007fu); emit(0x1a9f17e3u); break;
            case OpNe: emit(0x6b04007fu); emit(0x1a9f07e3u); break;
            case OpULt: emit(0x6b04007fu); emit(0x1a9f27e3u); break;
            case OpULe: emit(0x6b04007fu); emit(0x1a9f87e3u); break;
            case OpSLt: emit(0x6b04007fu); emit(0x1a9fa7e3u); break;
            case OpSLe: emit(0x6b04007fu); emit(0x1a9fc7e3u); break;
            default: return {};
            }
            store(3, count - 1);
        }
    }
    return {};
}
} // namespace Shader::Arm64Srt
