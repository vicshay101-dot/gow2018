// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <string_view>
#include <unordered_map>
#include <boost/container/flat_map.hpp>
#include <cstring>
#include <queue>
#include "common/arch.h"
#ifdef ARCH_X86_64
#include <mutex>
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>
#else
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <bit>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdlib>
#ifdef ARCH_ARM64
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "shader_recompiler/ir/passes/srt_arm64.h"
#include "../../../../../src/runtime.h"
#endif
#include "common/decoder.h"
#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/signal_context.h"
#include "core/emulator_settings.h"
#include "core/signals.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/ir/srt_gvn_table.h"
#include "shader_recompiler/ir/value.h"

// gow3: the same unsupported opcode or SRT offset failure repeats for every descriptor of every
// shader compiled on the ShaderComp threads. Log each (what, detail) pair at first occurrence,
// then once every 1024 occurrences, instead of on every call.
// Hash of the shader whose flatten pass is running on this thread, so each SRT error can be tied to
// the shader that produced it. Set at the start of FlattenExtendedUserdataPass.
static thread_local u64 g_srt_pgm_hash = 0;

static void LogSrtError(std::string_view what, std::string_view detail) {
    static std::mutex mutex;
    static std::unordered_map<std::string, u32> counts;
    std::scoped_lock lock{mutex};
    // Count per (shader, message, detail) so each shader gets its own first occurrences logged.
    const u32 count =
        ++counts[std::to_string(g_srt_pgm_hash) + '|' + std::string{what} + '|' + std::string{detail}];
    if (count <= 4 || (count & 1023) == 0) {
        LOG_ERROR(Render_Recompiler, "{} [{}] pgm={:#018x} (occurrence {})", what, detail,
                  g_srt_pgm_hash, count);
    }
}

#ifdef ARCH_X86_64

using namespace Xbyak::util;

static Xbyak::CodeGenerator g_srt_codegen(32_MB);
static const u8* g_srt_codegen_start = nullptr;
// gow3: the shader compiler and the background cache warm-up append walkers from different
// threads.
static std::mutex g_srt_codegen_mutex;

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    std::scoped_lock lock{g_srt_codegen_mutex};
    const auto func_addr = (PFN_SrtWalker)g_srt_codegen.getCurr();
    g_srt_codegen.db(ptr, size);
    g_srt_codegen.ready();
    return func_addr;
}

} // namespace Shader

namespace {

static void DumpSrtProgram(const Shader::Info& info, const u8* code, size_t codesize) {
    using namespace Common::FS;

    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}_{:#018x}.srtprogram.txt", info.hw_stage, info.pgm_hash);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create, FileType::TextFile};

    u64 address = reinterpret_cast<u64>(code);
    u64 code_end = address + codesize;
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    ZyanStatus status = ZYAN_STATUS_SUCCESS;
    while (address < code_end && ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
                                     instruction, operands, reinterpret_cast<void*>(address)))) {
        std::string s =
            Common::Decoder::Instance()->disassembleInst(instruction, operands, address);
        s += "\n";
        file.WriteString(s);
        address += instruction.length;
    }
}

static bool SrtWalkerSignalHandler(void* context, void* fault_address) {
    // Only handle if the fault address is within the SRT code range
    const u8* code_start = g_srt_codegen_start;
    const u8* code_end = code_start + g_srt_codegen.getSize();
    const void* code = Common::GetRip(context);
    if (code < code_start || code >= code_end) {
        return false; // Not in SRT code range
    }

    // Patch instruction to zero register
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    ZyanStatus status = Common::Decoder::Instance()->decodeInstruction(instruction, operands,
                                                                       const_cast<void*>(code), 15);

    ASSERT(ZYAN_SUCCESS(status) && instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
           operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
           operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY);

    size_t len = instruction.length;
    const size_t patch_size = 3;
    u8* code_patch = const_cast<u8*>(reinterpret_cast<const u8*>(code));

    // We can only encounter rdi or r10d as the first operand in a
    // fault memory access for SRT walker.
    switch (operands[0].reg.value) {
    case ZYDIS_REGISTER_RDI:
        // mov rdi, [rdi + (off_dw << 2)] -> xor rdi, rdi
        code_patch[0] = 0x48;
        code_patch[1] = 0x31;
        code_patch[2] = 0xFF;
        break;
    case ZYDIS_REGISTER_R10D:
        // mov r10d, [rdi + (off_dw << 2)] -> xor r10d, r10d
        code_patch[0] = 0x45;
        code_patch[1] = 0x31;
        code_patch[2] = 0xD2;
        break;
    default:
        UNREACHABLE_MSG("Unsupported register for SRT walker patch");
        return false;
    }

    // Fill nops
    memset(code_patch + patch_size, 0x90, len - patch_size);

    LOG_WARNING(Render_Recompiler, "Patched SRT walker at {}, fault address {}", code,
                fault_address);

    return true;
}

#else

namespace {
using namespace Shader::Arm64Srt;
} // namespace

namespace Shader {

struct NativeSrtCode {
    using Function = void (*)(const u32*, u32*);
    Function function{};
    std::atomic<bool> enabled{true};
    void* mapping = nullptr;
    size_t size{};
    ~NativeSrtCode() {
#ifdef ARCH_ARM64
        if (mapping) munmap(mapping, size);
#endif
    }
};

static NativeSrtCode* CompileNativeSrt(std::span<const u32> program) {
#ifdef ARCH_ARM64
    static const bool enabled = [] {
        const char* value = std::getenv("GOW3_SRT_NATIVE");
        return !value || value[0] != '0';
    }();
    if (!enabled) return nullptr;
    const auto code = Arm64Srt::Compile(program);
    if (code.empty()) return nullptr;
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return nullptr;
    auto native = std::make_unique<NativeSrtCode>();
    native->size = (code.size() * sizeof(u32) + size_t(page) - 1) & ~(size_t(page) - 1);
    native->mapping = mmap(nullptr, native->size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (native->mapping == MAP_FAILED) { native->mapping = nullptr; return nullptr; }
    std::memcpy(native->mapping, code.data(), code.size() * sizeof(u32));
    auto* begin = static_cast<char*>(native->mapping);
    __builtin___clear_cache(begin, begin + native->size);
    if (mprotect(native->mapping, native->size, PROT_READ | PROT_EXEC)) return nullptr;
    native->function = reinterpret_cast<NativeSrtCode::Function>(native->mapping);
    static std::mutex mutex;
    static std::vector<std::unique_ptr<NativeSrtCode>> mappings;
    std::scoped_lock lock{mutex};
    auto* result = native.get();
    mappings.push_back(std::move(native));
    return result;
#else
    (void)program;
    return nullptr;
#endif
}

struct SrtProgram {
    std::vector<u32> words;
    NativeSrtCode* native{};
};

const void* SrtProgramCode(const SrtProgram* program) {
    return program ? program->words.data() : nullptr;
}

void RunSrtWalker(SrtProgram* stored, const u32* user_data, u32* flat) {
    const auto* program = stored->words.data();
    auto* native = stored->native;
    if (native && native->enabled.load(std::memory_order_relaxed)) {
        // Use the runtime's existing fault boundary. A fault retries with the interpreter,
        // which zeros just the failing operation and retains the original cache bytecode.
        sigjmp_buf* const previous = runtime_fault_recover;
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0) == 0) {
            runtime_fault_recover = &recover;
            native->function(user_data, flat);
            runtime_fault_recover = previous;
            return;
        }
        runtime_fault_recover = previous;
        native->enabled.store(false, std::memory_order_relaxed);
    }
    RunSrtProgram(program, user_data, flat);
}


PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    u32 magic = 0;
    if (size < 8 || size % 4 != 0 ||
        (std::memcpy(&magic, ptr, sizeof(magic)), magic != SrtMagic)) {
        LOG_ERROR(Render_Recompiler, "SRT walker in shader cache is not an SRT program");
        return {};
    }
    auto program = std::make_unique<SrtProgram>();
    program->words.resize(size / sizeof(u32));
    std::memcpy(program->words.data(), ptr, size);
    program->native = CompileNativeSrt(program->words);
    static std::mutex mutex;
    static std::vector<std::unique_ptr<SrtProgram>> programs;
    std::scoped_lock lock{mutex};
    auto* result = program.get();
    programs.push_back(std::move(program));
    return SrtWalker{result};
}

void RunSrtProgram(const u32* program, const u32* user_data, u32* flat) {
    u32* const code = const_cast<u32*>(program);
    sigjmp_buf* const previous = runtime_fault_recover;
    sigjmp_buf recover;
    constexpr u32 MaxCallFaults = 32;
    u32 faulted_pcs[MaxCallFaults];
    volatile u32 num_faulted = 0;
    volatile u32 fault_pc = 0;
    volatile u64 fault_addr = 0;
    volatile u32 fault_off = 0;
    if (sigsetjmp(recover, 0)) {
        const u32 fpc = fault_pc;
        const u64 faddr = fault_addr;
        const u32 foff = fault_off;
        (void)faddr;
        (void)foff;
        LOG_DEBUG(Render_Recompiler,
                  "SRT walker load faulted at pc={} addr={:#x} off={:#x}; using zero",
                  fpc, faddr, foff);
        const u32 n = num_faulted;
        if (n < MaxCallFaults) {
            faulted_pcs[n] = fpc;
            num_faulted = n + 1;
        } else {
            __atomic_fetch_or(&code[fpc], OpZeroed, __ATOMIC_RELAXED);
        }
    }
    runtime_fault_recover = &recover;
    const auto is_call_zeroed = [&](u32 op_pc) -> bool {
        const u32 n = num_faulted;
        for (u32 i = 0; i < n; ++i) {
            if (faulted_pcs[i] == op_pc) {
                return true;
            }
        }
        return false;
    };
    const u8* stack[MaxDepth];
    u32 values[MaxValues];
    u32 depth = 0, count = 0;
    const u8* pointer = reinterpret_cast<const u8*>(user_data);

    for (u32 pc = 1;;) {
        const u32 op_pc = pc;
        u32* const at = &code[pc++];
        const u32 word = __atomic_load_n(at, __ATOMIC_RELAXED);
        const u32 op = word & ~OpZeroed;
        const bool zeroed = ((word & OpZeroed) != 0) || is_call_zeroed(op_pc);
        switch (op) {
        case OpEnd:
            runtime_fault_recover = previous;
            return;
        case OpPushImm:
        case OpPushDyn: {
            const u32 offset = (op == OpPushImm ? code[pc++] : values[--count]) << 2;
            u64 next = 0;
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(pointer);
            if (!zeroed && base >= 0x10000ULL) {
                fault_pc = op_pc;
                fault_addr = static_cast<u64>(base + offset);
                fault_off = offset;
                std::memcpy(&next, pointer + offset, sizeof(next));
            }
            stack[depth++] = pointer;
            pointer = reinterpret_cast<const u8*>(next & 0xFFFFFFFFFFFFull);
            break;
        }
        case OpPop:
            pointer = stack[--depth];
            break;
        case OpCopyImm:
        case OpCopyDyn: {
            const u32 offset = (op == OpCopyImm ? code[pc++] : values[--count]) << 2;
            u32 value = 0;
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(pointer);
            if (!zeroed && base >= 0x10000ULL) {
                fault_pc = op_pc;
                fault_addr = static_cast<u64>(base + offset);
                fault_off = offset;
                std::memcpy(&value, pointer + offset, sizeof(value));
            }
            flat[code[pc++]] = value;
            break;
        }
        case OpUserData:
        case OpFlat:
            values[count++] = flat[code[pc++]];
            break;
        case OpImm:
            values[count++] = code[pc++];
            break;
        case OpNot:
            values[count - 1] = ~values[count - 1];
            break;
        case OpFindILsb: {
            u32& v = values[count - 1];
            v = v ? static_cast<u32>(std::countr_zero(v)) : ~0u;
            break;
        }
        case OpBitFieldUExtract: {
            const u32 bits = values[--count];
            const u32 offset = values[--count];
            u32& base = values[count - 1];
            base = (base >> (offset & 31)) & ((1u << (bits & 31)) - 1);
            break;
        }
        case OpBitFieldInsert: {
            const u32 bits = values[--count];
            const u32 offset = values[--count] & 31u;
            const u32 insert = values[--count];
            u32& base = values[count - 1];
            const u32 mask = bits >= 32u ? 0xFFFFFFFFu : ((1u << bits) - 1u);
            base = (base & ~(mask << offset)) | ((insert & mask) << offset);
            break;
        }
        case OpSelect: {
            const u32 false_val = values[--count];
            const u32 true_val = values[--count];
            u32& cond = values[count - 1];
            cond = cond ? true_val : false_val;
            break;
        }
        case OpLoad32: {
            // gow2018 dynamic fallback: load a u32 straight from guest memory.
            // Stack order pushed by the emitter: lo, hi, off_dw (off on top).
            const u32 off_dw = values[--count];
            const u32 hi = values[--count];
            const u32 lo = values[--count];
            const u64 addr = (static_cast<u64>(hi) << 32 | lo) +
                             (static_cast<u64>(off_dw) << 2);
            u32 value = 0;
            if (!zeroed && addr >= 0x10000ULL) {
                fault_pc = op_pc;
                fault_addr = addr;
                fault_off = off_dw;
                std::memcpy(&value, reinterpret_cast<const void*>(addr), sizeof(value));
            }
            values[count++] = value;
            break;
        }
        default: {
            const u32 b = values[--count];
            u32& a = values[count - 1];
            switch (op) {
            case OpAdd: a += b; break;
            case OpSub: a -= b; break;
            case OpMul: a *= b; break;
            case OpShl: a <<= b & 31; break;
            case OpShr: a >>= b & 31; break;
            case OpAnd: a &= b; break;
            case OpOr: a |= b; break;
            case OpXor: a ^= b; break;
            case OpUMin: a = std::min(a, b); break;
            case OpUMax: a = std::max(a, b); break;
            case OpEq: a = (a == b) ? 1u : 0u; break;
            case OpNe: a = (a != b) ? 1u : 0u; break;
            case OpULt: a = (a < b) ? 1u : 0u; break;
            case OpULe: a = (a <= b) ? 1u : 0u; break;
            case OpSLt: a = (static_cast<s32>(a) < static_cast<s32>(b)) ? 1u : 0u; break;
            case OpSLe: a = (static_cast<s32>(a) <= static_cast<s32>(b)) ? 1u : 0u; break;
            default: UNREACHABLE_MSG("Bad SRT program op {}", op);
            }
            break;
        }
        }
    }
}

} // namespace Shader

#endif

namespace {
using namespace Shader;

struct PassInfo {
    struct PointerListCompare {
        bool operator()(const IR::Value& a, const IR::Value& b) const {
            // Keep order with the following criteria:
            // 1. If immediate, sort by immediate value
            // 2. If not immediate, sort by instruction order
            // 3. Immediates come first, then instructions
            if (a.IsImmediate() && b.IsImmediate()) {
                return a.U32() < b.U32();
            } else if (!a.IsImmediate() && !b.IsImmediate()) {
                auto a_inst = a.Inst();
                auto b_inst = b.Inst();
                auto a_block = a_inst->GetParent();
                auto b_block = b_inst->GetParent();
                if (a_block != b_block) {
                    return a_block < b_block; // Sort by block address
                }

                auto it = a_block->Instructions().iterator_to(*a_inst);
                auto end = a_block->Instructions().end();
                for (; it != end; ++it) {
                    if (&*it == b_inst) {
                        return true;
                    }
                }
                return false;
            } else {
                return a.IsImmediate(); // Immediates come first
            }
        }
    };

    // map offset to inst
    using PtrUserList =
        boost::container::flat_map<IR::Value, Shader::IR::Inst*, PointerListCompare>;

    Optimization::SrtGvnTable gvn_table;
    // keys are GetUserData, ReadConst or ReadConstBuffer instructions that are used as pointers
    std::unordered_map<IR::Inst*, PtrUserList> pointer_uses;
    // GetUserData instructions corresponding to sgpr_base of SRT roots
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst*, 1> srt_roots;

    // pick a single inst for a given value number
    std::unordered_map<u32, IR::Inst*> vn_to_inst;

    // Bumped during codegen to assign offsets to readconsts
    u16 dst_off_dw;

    PtrUserList* GetUsesAsPointer(IR::Inst* inst) {
        auto it = pointer_uses.find(inst);
        if (it != pointer_uses.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // Return a single instruction that this instruction is identical to, according
    // to value number
    // The "original" is arbitrary. Here it's the first instruction found for a given value number
    IR::Inst* DeduplicateInstruction(IR::Inst* inst) {
        auto it = vn_to_inst.try_emplace(gvn_table.GetValueNumber(inst), inst);
        return it.first->second;
    }
};
} // namespace

namespace Shader::Optimization {

namespace {

static inline u16 GetFlatbufOffset(const IR::Inst* inst) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        auto inst_info = inst->Flags<IR::BufferInstInfo>();
        return inst_info.flatbuf_off_dw;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        return inst->Flags<u16>();
    }
    UNREACHABLE_MSG("Instruction not supported");
}

static inline void SetFlatbufOffset(IR::Inst* inst, u16 offset) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        auto inst_info = inst->Flags<IR::BufferInstInfo>();
        inst_info.flatbuf_off_dw.Assign(offset);
        inst->SetFlags(inst_info);
        return;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        inst->SetFlags(offset);
        return;
    }
    UNREACHABLE_MSG("Instruction not supported");
}

#ifdef ARCH_X86_64
static bool ComputeOffset(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                          const IR::Value& off_dw);

#define ABORT_ON_FAILURE(expr)                                                                     \
    if (!(expr)) {                                                                                 \
        return false;                                                                              \
    }

#define POP_ABORT_ON_FAILURE(expr)                                                                 \
    if (!(expr)) {                                                                                 \
        c.add(rsp, 8);                                                                             \
        return false;                                                                              \
    }

static bool EmitComputeOffsetIAdd32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.add(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.add(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.add(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetISub32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.neg(reg);
        c.add(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.sub(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.neg(reg);
        c.add(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetIMul32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.imul(reg, reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.imul(reg, reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.imul(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetShiftLeftLogical32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                                PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.shl(reg, inst->Arg(0).U32());
        c.mov(ecx, reg);
        c.mov(reg, inst->Arg(0).U32());
        c.shl(reg, cl);
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.shl(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.pop(rcx);
        c.shl(reg, cl);
    }
    return true;
}

static bool EmitComputeOffsetShiftRightLogical32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                                 PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.mov(ecx, reg);
        c.mov(reg, inst->Arg(0).U32());
        c.shr(reg, cl);
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.shr(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.pop(rcx);
        c.shr(reg, cl);
    }
    return true;
}

static bool EmitComputeOffsetBitwiseAnd32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                          PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.and_(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.and_(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.and_(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetBitwiseOr32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                         PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.or_(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.or_(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.or_(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetBitwiseXor32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                          PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.xor_(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.xor_(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.xor_(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetBitwiseNot32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                          PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
    c.not_(reg);
    return true;
}

static bool EmitComputeOffsetUMin32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.mov(ecx, inst->Arg(0).U32());
        c.cmp(reg, ecx);
        c.cmova(reg, ecx);
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.mov(ecx, inst->Arg(1).U32());
        c.cmp(reg, ecx);
        c.cmova(reg, ecx);
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.cmp(reg, dword[rsp]);
        c.cmova(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetUMax32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.mov(ecx, inst->Arg(0).U32());
        c.cmp(reg, ecx);
        c.cmovb(reg, ecx);
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.mov(ecx, inst->Arg(1).U32());
        c.cmp(reg, ecx);
        c.cmovb(reg, ecx);
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.cmp(reg, dword[rsp]);
        c.cmovb(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetBitFieldUExtract(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                              PassInfo& pass_info, IR::Inst* inst) {
    // We asume that the count is always less than 32, Is this correct?
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        c.mov(reg, inst->Arg(0).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
    }
    bool in_stack = false;
    if (inst->Arg(1).IsImmediate()) {
        c.shr(reg, inst->Arg(1).U32());
    } else {
        c.push(reg.cvt64());
        in_stack = true;
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.mov(ecx, reg);
        c.shr(dword[rsp], cl);
    }
    if (inst->Arg(2).IsImmediate()) {
        if (in_stack) {
            c.pop(reg.cvt64());
        }
        c.and_(reg, (1U << inst->Arg(2).U32()) - 1);
    } else {
        if (!in_stack) {
            c.push(reg.cvt64());
        }
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(2)));
        c.mov(ecx, reg);
        c.mov(edx, 1);
        c.shl(edx, cl);
        c.dec(edx);
        c.and_(dword[rsp], edx);
        c.pop(reg.cvt64());
    }
    return true;
}

#endif

static bool IsAllowedOffsetInstruction(const IR::Inst* inst) {
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
    case IR::Opcode::IAdd32:
    case IR::Opcode::ISub32:
    case IR::Opcode::IMul32:
    case IR::Opcode::ShiftLeftLogical32:
    case IR::Opcode::ShiftRightLogical32:
    case IR::Opcode::BitwiseAnd32:
    case IR::Opcode::BitwiseOr32:
    case IR::Opcode::BitwiseXor32:
    case IR::Opcode::BitwiseNot32:
    case IR::Opcode::UMin32:
    case IR::Opcode::UMax32:
    case IR::Opcode::BitFieldUExtract:
    case IR::Opcode::BitFieldInsert32:
    case IR::Opcode::FindILsb32:
    case IR::Opcode::SelectU32:
    case IR::Opcode::SelectU1:
    case IR::Opcode::IEqual32:
    case IR::Opcode::INotEqual32:
    case IR::Opcode::ULessThan32:
    case IR::Opcode::ULessThanEqual32:
    case IR::Opcode::UGreaterThan32:
    case IR::Opcode::UGreaterThanEqual32:
    case IR::Opcode::SLessThan32:
    case IR::Opcode::SLessThanEqual32:
    case IR::Opcode::SGreaterThan32:
    case IR::Opcode::SGreaterThanEqual32:
    case IR::Opcode::LogicalAnd:
    case IR::Opcode::LogicalOr:
    case IR::Opcode::LogicalXor:
    case IR::Opcode::LogicalNot:
    case IR::Opcode::Phi:
    case IR::Opcode::ReadFirstLane:
        return true;
    default:
        return false;
    }
}

#ifdef ARCH_X86_64
static bool ComputeOffset(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                          const IR::Value& off_dw) {
    ASSERT(reg != ecx && reg != edx);
    auto inst = off_dw.Inst();
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
        c.mov(reg, ptr[rsi + (static_cast<u32>(inst->Arg(0).ScalarReg()) << 2)]);
        return true;
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
        if (u16 offset = GetFlatbufOffset(pass_info.DeduplicateInstruction(inst)); offset != 0) {
            c.mov(reg, ptr[rsi + (offset << 2)]);
            return true;
        }
        return false;
    case IR::Opcode::ReadFirstLane:
        if (inst->Arg(0).IsImmediate()) {
            c.mov(reg, inst->Arg(0).U32());
            return true;
        }
        return ComputeOffset(c, reg, pass_info, inst->Arg(0));
    case IR::Opcode::IAdd32:
        ABORT_ON_FAILURE(EmitComputeOffsetIAdd32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::ISub32:
        ABORT_ON_FAILURE(EmitComputeOffsetISub32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::IMul32:
        ABORT_ON_FAILURE(EmitComputeOffsetIMul32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::ShiftLeftLogical32:
        ABORT_ON_FAILURE(EmitComputeOffsetShiftLeftLogical32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::ShiftRightLogical32:
        ABORT_ON_FAILURE(EmitComputeOffsetShiftRightLogical32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitwiseAnd32:
        ABORT_ON_FAILURE(EmitComputeOffsetBitwiseAnd32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitwiseOr32:
        ABORT_ON_FAILURE(EmitComputeOffsetBitwiseOr32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitwiseXor32:
        ABORT_ON_FAILURE(EmitComputeOffsetBitwiseXor32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitwiseNot32:
        ABORT_ON_FAILURE(EmitComputeOffsetBitwiseNot32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::UMin32:
        ABORT_ON_FAILURE(EmitComputeOffsetUMin32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::UMax32:
        ABORT_ON_FAILURE(EmitComputeOffsetUMax32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitFieldUExtract:
        ABORT_ON_FAILURE(EmitComputeOffsetBitFieldUExtract(c, reg, pass_info, inst));
        return true;
    default:
        LogSrtError("Unexpected instruction for offset computation", magic_enum::enum_name(inst->GetOpcode()));
        return false;
    }
}

#endif

#ifdef ARCH_X86_64
static inline bool PushPtr(Xbyak::CodeGenerator& c, PassInfo& pass_info, const IR::Value& off_dw) {
    c.push(rdi);
    if (off_dw.IsImmediate()) {
        c.mov(rdi, ptr[rdi + (off_dw.U32() << 2)]);
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, r10d, pass_info, off_dw));
        c.shl(r10d, 2);
        c.mov(r10d, r10d);
        c.mov(rdi, ptr[rdi + r10]);
    }
    c.mov(r10, 0xFFFFFFFFFFFFULL);
    c.and_(rdi, r10);
    return true;
}

static inline void PopPtr(Xbyak::CodeGenerator& c) {
    c.pop(rdi);
}

static void VisitPointer(const IR::Value& off_dw, IR::Inst* subtree, PassInfo& pass_info,
                         Xbyak::CodeGenerator& c) {
    if (subtree->GetOpcode() == IR::Opcode::ReadConst && subtree->Flags<u16>() == 0 ||
        subtree->GetOpcode() == IR::Opcode::ReadConstBuffer &&
            subtree->Flags<IR::BufferInstInfo>().flatbuf_off_dw == 0) {
        return;
    }

    if (!PushPtr(c, pass_info, off_dw)) {
        LogSrtError("Failed to compute offset for SRT walker", "");
        return;
    }
    PassInfo::PtrUserList* use_list = pass_info.GetUsesAsPointer(subtree);
    ASSERT(use_list);

    // First copy all the src data from this tree level
    // That way, all data that was contiguous in the guest SRT is also contiguous in the
    // flattened buffer.
    // TODO src and dst are contiguous. Optimize with wider loads/stores
    // TODO if this subtree is dynamically indexed, don't compact it (keep it sparse)
    for (auto [src_off_dw, use] : *use_list) {
        if (src_off_dw.IsImmediate()) {
            c.mov(r10d, ptr[rdi + (src_off_dw.U32() << 2)]);
        } else {
            if (!ComputeOffset(c, r10d, pass_info, src_off_dw)) {
                LogSrtError("Failed to compute offset for SRT walker", "");
                continue;
            }
            c.shl(r10d, 2);
            c.mov(r10d, r10d);
            c.mov(r10d, dword[rdi + r10]);
        }
        c.mov(ptr[rsi + (pass_info.dst_off_dw << 2)], r10d);

        SetFlatbufOffset(use, pass_info.dst_off_dw);
        pass_info.dst_off_dw++;
    }

    // Then visit any children used as pointers
    for (const auto [src_off_dw, use] : *use_list) {
        if (pass_info.GetUsesAsPointer(use)) {
            VisitPointer(src_off_dw, use, pass_info, c);
        }
    }

    PopPtr(c);
}

static void GenerateSrtProgram(Info& info, PassInfo& pass_info) {
    Xbyak::CodeGenerator& c = g_srt_codegen;

    if (pass_info.srt_roots.empty()) {
        return;
    }
    std::scoped_lock lock{g_srt_codegen_mutex};

    // Register the signal handler for SRT walker, if not already registered
    if (g_srt_codegen_start == nullptr) {
        g_srt_codegen_start = c.getCurr();
        auto* signals = Core::Signals::Instance();
        // Call after the memory invalidation handler
        constexpr u32 priority = 1;
        signals->RegisterAccessViolationHandler(SrtWalkerSignalHandler, priority);
    }

    info.srt_info.walker_func = c.getCurr<PFN_SrtWalker>();
    pass_info.dst_off_dw = NUM_USER_DATA_REGS;
    ASSERT(pass_info.dst_off_dw == info.srt_info.flattened_bufsize_dw);

    for (const auto& [sgpr_base, root] : pass_info.srt_roots) {
        VisitPointer(IR::Value(static_cast<u32>(sgpr_base)), root, pass_info, c);
    }

    c.ret();
    c.ready();

    info.srt_info.walker_func_size =
        c.getCurr() - reinterpret_cast<const u8*>(info.srt_info.walker_func);

    if (EmulatorSettings.IsDumpShaders()) {
        DumpSrtProgram(info, reinterpret_cast<const u8*>(info.srt_info.walker_func),
                       info.srt_info.walker_func_size);
    }

    info.srt_info.flattened_bufsize_dw = pass_info.dst_off_dw;
}

#else

using SrtProgram = std::vector<u32>;

#define ABORT_ON_FAILURE(expr)                                                                     \
    if (!(expr)) {                                                                                 \
        return false;                                                                              \
    }

static bool ComputeOffset(SrtProgram& p, PassInfo& pass_info, const IR::Value& off_dw);

static bool EmitOperand(SrtProgram& p, PassInfo& pass_info, const IR::Value& value) {
    if (value.IsImmediate()) {
        const u32 imm = value.Type() == IR::Type::U1 ? (value.U1() ? 1u : 0u) : value.U32();
        p.insert(p.end(), {OpImm, imm});
        return true;
    }
    return ComputeOffset(p, pass_info, value);
}

static bool EmitComputeOffsetOp(SrtProgram& p, PassInfo& pass_info, IR::Inst* inst,
                                SrtOp op, size_t args) {
    for (size_t i = 0; i < args; ++i) {
        ABORT_ON_FAILURE(EmitOperand(p, pass_info, inst->Arg(i)));
    }
    p.push_back(op);
    return true;
}

static bool EmitComputeOffsetRevBinaryOp(SrtProgram& p, PassInfo& pass_info, IR::Inst* inst,
                                         SrtOp op) {
    ABORT_ON_FAILURE(EmitOperand(p, pass_info, inst->Arg(1)));
    ABORT_ON_FAILURE(EmitOperand(p, pass_info, inst->Arg(0)));
    p.push_back(op);
    return true;
}

// gow3 diagnostic: prints the instruction chain behind the first unflattened ReadConstBuffer of each
// shader, so the log shows what the offset depends on. Does not change any decision.
static void DumpSrtChain(const IR::Inst* inst, int depth, int indent) {
    if (!inst) return;
    std::printf("SRT_CHAIN pgm=%#018llx %*s%s", static_cast<unsigned long long>(g_srt_pgm_hash),
                indent * 2, "", std::string{magic_enum::enum_name(inst->GetOpcode())}.c_str());
    for (size_t i = 0; i < inst->NumArgs(); ++i) {
        const IR::Value arg = inst->Arg(i);
        if (arg.TryInst()) {
            std::printf(" [%zu]", i);
        } else {
            std::printf(" [%zu:imm]", i);
        }
    }
    std::printf("\n");
    if (depth <= 0) return;
    for (size_t i = 0; i < inst->NumArgs(); ++i) {
        const IR::Value arg = inst->Arg(i);
        if (const IR::Inst* child = arg.TryInst()) {
            DumpSrtChain(child, depth - 1, indent + 1);
        }
    }
}

static thread_local bool g_suppress_unflattened_log = false;
static thread_local bool g_allow_dynamic_index_fallback = false;
static thread_local u32 g_dynamic_fallback_hits = 0;

static bool DependsOnAttribute(const IR::Value& value, int depth,
                               std::vector<const IR::Inst*>& visited) {
    if (depth <= 0 || value.IsImmediate()) {
        return false;
    }
    const IR::Inst* inst = value.TryInst();
    if (!inst) {
        return false;
    }
    if (inst->GetOpcode() == IR::Opcode::GetAttributeU32) {
        return true;
    }
    if (std::find(visited.begin(), visited.end(), inst) != visited.end()) {
        return false;
    }
    visited.push_back(inst);
    for (size_t i = 0; i < inst->NumArgs(); ++i) {
        if (DependsOnAttribute(inst->Arg(i), depth - 1, visited)) {
            return true;
        }
    }
    return false;
}

static bool ComputeOffset(SrtProgram& p, PassInfo& pass_info, const IR::Value& off_dw) {
    auto inst = off_dw.Inst();
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
        p.insert(p.end(), {OpUserData, static_cast<u32>(inst->Arg(0).ScalarReg())});
        return true;
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer: {
        if (u16 offset = GetFlatbufOffset(pass_info.DeduplicateInstruction(inst)); offset != 0) {
            p.insert(p.end(), {OpFlat, offset});
            return true;
        }
        // gow2018 dynamic fallback: this read was never flattened into the flat buffer
        // (it is not reachable as an SRT pointer subtree), but the walker runs on the CPU
        // with the guest address space mapped. Emit a dynamic guest load of the value:
        // push base lo, base hi, offset, then OpLoad32. This fixes shaders whose SRT
        // offsets depend on descriptor contents (e.g. terrain/ground draws reading a
        // buffer pointer stored inside another buffer), which previously aborted the
        // whole flatten pass and left those draws with garbage descriptors.
        static thread_local u32 dyn_load_depth = 0;
        if (dyn_load_depth >= 4) {
            if (!g_suppress_unflattened_log) {
                LogSrtError("Offset depends on unflattened",
                            magic_enum::enum_name(inst->GetOpcode()));
            }
            return false;
        }
        IR::Inst* base = inst->Arg(0).Inst();
        if (!base || base->NumArgs() < 2) {
            return false;
        }
        // ReadConst carries a 64-bit address pair; ReadConstBuffer carries a 128-bit
        // descriptor whose low 64 bits are the buffer base address.
        const IR::Value& lo = base->Arg(0);
        const IR::Value& hi = base->Arg(1);
        struct DepthGuard {
            u32& depth;
            explicit DepthGuard(u32& d) : depth(d) { ++depth; }
            ~DepthGuard() { --depth; }
        } guard{dyn_load_depth};
        SrtProgram dyn;
        if (!EmitOperand(dyn, pass_info, lo) || !EmitOperand(dyn, pass_info, hi) ||
            !EmitOperand(dyn, pass_info, inst->Arg(1))) {
            if (!g_suppress_unflattened_log) {
                LogSrtError("Offset depends on unflattened",
                            magic_enum::enum_name(inst->GetOpcode()));
                static thread_local std::vector<u64> dumped_shaders;
                if (std::find(dumped_shaders.begin(), dumped_shaders.end(), g_srt_pgm_hash) ==
                    dumped_shaders.end()) {
                    dumped_shaders.push_back(g_srt_pgm_hash);
                    DumpSrtChain(inst, 6, 0);
                }
            }
            return false;
        }
        p.insert(p.end(), dyn.begin(), dyn.end());
        p.push_back(OpLoad32);
        return true;
    }
    case IR::Opcode::IAdd32: return EmitComputeOffsetOp(p, pass_info, inst, OpAdd, 2);
    case IR::Opcode::ISub32: return EmitComputeOffsetOp(p, pass_info, inst, OpSub, 2);
    case IR::Opcode::IMul32: return EmitComputeOffsetOp(p, pass_info, inst, OpMul, 2);
    case IR::Opcode::ShiftLeftLogical32: return EmitComputeOffsetOp(p, pass_info, inst, OpShl, 2);
    case IR::Opcode::ShiftRightLogical32: return EmitComputeOffsetOp(p, pass_info, inst, OpShr, 2);
    case IR::Opcode::BitwiseAnd32: return EmitComputeOffsetOp(p, pass_info, inst, OpAnd, 2);
    case IR::Opcode::BitwiseOr32: return EmitComputeOffsetOp(p, pass_info, inst, OpOr, 2);
    case IR::Opcode::BitwiseXor32: return EmitComputeOffsetOp(p, pass_info, inst, OpXor, 2);
    case IR::Opcode::BitwiseNot32: return EmitComputeOffsetOp(p, pass_info, inst, OpNot, 1);
    case IR::Opcode::UMin32: return EmitComputeOffsetOp(p, pass_info, inst, OpUMin, 2);
    case IR::Opcode::UMax32: return EmitComputeOffsetOp(p, pass_info, inst, OpUMax, 2);
    case IR::Opcode::BitFieldUExtract:
        return EmitComputeOffsetOp(p, pass_info, inst, OpBitFieldUExtract, 3);
    case IR::Opcode::BitFieldInsert32:
        return EmitComputeOffsetOp(p, pass_info, inst, OpBitFieldInsert, 4);
    case IR::Opcode::FindILsb32: {
        if (g_allow_dynamic_index_fallback) {
            std::vector<const IR::Inst*> visited;
            if (DependsOnAttribute(inst->Arg(0), 24, visited)) {
                // Only collapse 12-bit tile-mask material lookups (FindILsb32) to table slot 0.
                // Never collapse direct GetAttributeU32 particle emitter indexing!
                p.insert(p.end(), {OpImm, 0u});
                ++g_dynamic_fallback_hits;
                return true;
            }
        }
        return EmitComputeOffsetOp(p, pass_info, inst, OpFindILsb, 1);
    }
    case IR::Opcode::SelectU32:
    case IR::Opcode::SelectU1:
        return EmitComputeOffsetOp(p, pass_info, inst, OpSelect, 3);
    case IR::Opcode::IEqual32:
        return EmitComputeOffsetOp(p, pass_info, inst, OpEq, 2);
    case IR::Opcode::INotEqual32:
        return EmitComputeOffsetOp(p, pass_info, inst, OpNe, 2);
    case IR::Opcode::ULessThan32:
        return EmitComputeOffsetOp(p, pass_info, inst, OpULt, 2);
    case IR::Opcode::ULessThanEqual32:
        return EmitComputeOffsetOp(p, pass_info, inst, OpULe, 2);
    case IR::Opcode::UGreaterThan32:
        return EmitComputeOffsetRevBinaryOp(p, pass_info, inst, OpULt);
    case IR::Opcode::UGreaterThanEqual32:
        return EmitComputeOffsetRevBinaryOp(p, pass_info, inst, OpULe);
    case IR::Opcode::SLessThan32:
        return EmitComputeOffsetOp(p, pass_info, inst, OpSLt, 2);
    case IR::Opcode::SLessThanEqual32:
        return EmitComputeOffsetOp(p, pass_info, inst, OpSLe, 2);
    case IR::Opcode::SGreaterThan32:
        return EmitComputeOffsetRevBinaryOp(p, pass_info, inst, OpSLt);
    case IR::Opcode::SGreaterThanEqual32:
        return EmitComputeOffsetRevBinaryOp(p, pass_info, inst, OpSLe);
    case IR::Opcode::LogicalAnd:
        return EmitComputeOffsetOp(p, pass_info, inst, OpAnd, 2);
    case IR::Opcode::LogicalOr:
        return EmitComputeOffsetOp(p, pass_info, inst, OpOr, 2);
    case IR::Opcode::LogicalXor:
        return EmitComputeOffsetOp(p, pass_info, inst, OpXor, 2);
    case IR::Opcode::LogicalNot:
        ABORT_ON_FAILURE(EmitOperand(p, pass_info, inst->Arg(0)));
        p.insert(p.end(), {OpImm, 0u, OpEq});
        return true;
    case IR::Opcode::ReadFirstLane:
        return EmitOperand(p, pass_info, inst->Arg(0));
    case IR::Opcode::GetAttributeU32:
        // Direct WorkgroupId/vertex attribute indexing (e.g. particle emitter arrays 38c7b95b)
        // must not be collapsed to 0, otherwise all workgroups overwrite emitter 0's vertices.
        if (!g_suppress_unflattened_log) {
            LogSrtError("Unexpected instruction for offset computation",
                        magic_enum::enum_name(inst->GetOpcode()));
        }
        return false;
    case IR::Opcode::Phi: {
        // gow3: loop/branch merges cannot be walked statically. Take the first operand that can
        // be computed (the value entering the loop); the loop-carried operand loops back here
        // and is rejected by the active list. Better than leaving the descriptor unflattened.
        static thread_local std::vector<const IR::Inst*> active;
        if (std::find(active.begin(), active.end(), inst) != active.end()) {
            return false;
        }
        active.push_back(inst);
        bool ok = false;
        for (size_t i = 0; i < inst->NumArgs() && !ok; ++i) {
            SrtProgram attempt;
            if (EmitOperand(attempt, pass_info, inst->Arg(i))) {
                p.insert(p.end(), attempt.begin(), attempt.end());
                ok = true;
            }
        }
        active.pop_back();
        return ok;
    }
    default:
        if (!g_suppress_unflattened_log) {
            LogSrtError("Unexpected instruction for offset computation",
                        magic_enum::enum_name(inst->GetOpcode()));
        }
        return false;
    }
}

static void VisitPointer(const IR::Value& off_dw, IR::Inst* subtree, PassInfo& pass_info,
                         SrtProgram& p, bool& any_progress, bool& any_unresolved, bool log_errors) {
    if (subtree->GetOpcode() == IR::Opcode::ReadConst && subtree->Flags<u16>() == 0 ||
        subtree->GetOpcode() == IR::Opcode::ReadConstBuffer &&
            subtree->Flags<IR::BufferInstInfo>().flatbuf_off_dw == 0) {
        return;
    }
    SrtProgram push_code;
    if (off_dw.IsImmediate()) {
        push_code.insert(push_code.end(), {OpPushImm, off_dw.U32()});
    } else {
        if (!ComputeOffset(push_code, pass_info, off_dw)) {
            any_unresolved = true;
            if (log_errors) {
                LogSrtError("Failed to compute offset for SRT walker", "");
            }
            return;
        }
        push_code.push_back(OpPushDyn);
    }
    auto* use_list = pass_info.GetUsesAsPointer(subtree);
    ASSERT(use_list);
    SrtProgram body;
    std::vector<std::pair<IR::Value, IR::Inst*>> pending;
    for (auto [src_off_dw, use] : *use_list) {
        if (GetFlatbufOffset(use) != 0) {
            continue;
        }
        if (src_off_dw.IsImmediate()) {
            if (!log_errors) {
                body.insert(body.end(), {OpCopyImm, src_off_dw.U32(), pass_info.dst_off_dw});
                SetFlatbufOffset(use, pass_info.dst_off_dw++);
                any_progress = true;
            }
        } else {
            pending.emplace_back(src_off_dw, use);
        }
    }
    if (!log_errors) {
        while (!pending.empty()) {
            std::vector<std::pair<IR::Value, IR::Inst*>> next_pending;
            next_pending.reserve(pending.size());
            bool progress = false;
            for (const auto& [src_off_dw, use] : pending) {
                SrtProgram offset;
                if (ComputeOffset(offset, pass_info, src_off_dw)) {
                    body.insert(body.end(), offset.begin(), offset.end());
                    body.insert(body.end(), {OpCopyDyn, pass_info.dst_off_dw});
                    SetFlatbufOffset(use, pass_info.dst_off_dw++);
                    progress = true;
                    any_progress = true;
                } else {
                    next_pending.emplace_back(src_off_dw, use);
                }
            }
            pending = std::move(next_pending);
            if (!progress) {
                break;
            }
        }
        if (!pending.empty()) {
            any_unresolved = true;
        }
    } else {
        for (const auto& entry : pending) {
            SrtProgram offset;
            (void)ComputeOffset(offset, pass_info, entry.first);
            LogSrtError("Failed to compute offset for SRT walker", "");
        }
    }
    for (const auto [src_off_dw, use] : *use_list) {
        if (pass_info.GetUsesAsPointer(use)) {
            VisitPointer(src_off_dw, use, pass_info, body, any_progress, any_unresolved,
                         log_errors);
        }
    }
    if (!log_errors && !body.empty()) {
        p.insert(p.end(), push_code.begin(), push_code.end());
        p.insert(p.end(), body.begin(), body.end());
        p.push_back(OpPop);
    }
}

static void GenerateSrtProgram(Info& info, PassInfo& pass_info) {
    if (pass_info.srt_roots.empty()) {
        return;
    }
    SrtProgram program{SrtMagic};
    pass_info.dst_off_dw = NUM_USER_DATA_REGS;
    ASSERT(pass_info.dst_off_dw == info.srt_info.flattened_bufsize_dw);
    constexpr u32 MaxFlattenPasses = 16;
    const bool prev_suppress = g_suppress_unflattened_log;
    g_suppress_unflattened_log = true;
    bool any_unresolved = false;
    for (u32 pass = 0; pass < MaxFlattenPasses; ++pass) {
        bool any_progress = false;
        any_unresolved = false;
        for (const auto& [sgpr_base, root] : pass_info.srt_roots) {
            VisitPointer(IR::Value(static_cast<u32>(sgpr_base)), root, pass_info, program,
                         any_progress, any_unresolved, false);
        }
        if (!any_unresolved || !any_progress) {
            break;
        }
    }
    static const bool srt_tile_fallback_enabled = [] {
        const char* env = std::getenv("GOW3_SRT_ATTR_FALLBACK");
        return !env || env[0] != '0';
    }();
    if (any_unresolved && srt_tile_fallback_enabled) {
        g_allow_dynamic_index_fallback = true;
        g_dynamic_fallback_hits = 0;
        for (u32 pass = 0; pass < MaxFlattenPasses; ++pass) {
            bool any_progress = false;
            any_unresolved = false;
            for (const auto& [sgpr_base, root] : pass_info.srt_roots) {
                VisitPointer(IR::Value(static_cast<u32>(sgpr_base)), root, pass_info, program,
                             any_progress, any_unresolved, false);
            }
            if (!any_unresolved || !any_progress) {
                break;
            }
        }
        g_allow_dynamic_index_fallback = false;
        if (g_dynamic_fallback_hits > 0) {
            std::printf("GOW3_SRT_TILE_FALLBACK: pgm=%#018llx hits=%u unresolved=%d\n",
                        static_cast<unsigned long long>(info.pgm_hash), g_dynamic_fallback_hits,
                        any_unresolved ? 1 : 0);
        }
    }
    g_suppress_unflattened_log = prev_suppress;
    if (any_unresolved) {
        SrtProgram dummy;
        bool dummy_progress = false;
        bool dummy_unresolved = false;
        for (const auto& [sgpr_base, root] : pass_info.srt_roots) {
            VisitPointer(IR::Value(static_cast<u32>(sgpr_base)), root, pass_info, dummy,
                         dummy_progress, dummy_unresolved, true);
        }
    }
    program.push_back(OpEnd);
    const size_t size = program.size() * sizeof(u32);
    info.srt_info.walker_func = RegisterWalkerCode(reinterpret_cast<const u8*>(program.data()), size);
    info.srt_info.walker_func_size = size;
    info.srt_info.flattened_bufsize_dw = pass_info.dst_off_dw;
}

#endif

static bool IsReadConstSource(const IR::Value base) {
    auto* inst = base.TryInst();
    if (!inst) {
        return false;
    }
    return inst->GetOpcode() == IR::Opcode::GetUserData ||
           inst->GetOpcode() == IR::Opcode::ReadConst ||
           inst->GetOpcode() == IR::Opcode::ReadConstBuffer;
}

void SimplifyReadConstAddressAdd(IR::Inst& inst) {
    // This handles the following pattern by combining the addition with the offset
    // s_add_u32    s12, s2, <offset>
    // s_addc_u32   s13, s3, 0
    // s_load_dword s16, s[12:13], 0x0

    if (inst.GetOpcode() != IR::Opcode::ReadConst) {
        return;
    }
    IR::Inst* addr = inst.Arg(0).Inst();
    IR::Inst* hi = addr->Arg(1).TryInst();
    IR::Inst* lo = addr->Arg(0).TryInst();
    if (!hi || !lo) {
        return;
    }

    if (lo->GetOpcode() != IR::Opcode::IAdd32 || hi->GetOpcode() != IR::Opcode::IAdd32) {
        return;
    }

    IR::Value add_offset;
    IR::Value base;
    if (base = lo->Arg(0); IsReadConstSource(base)) {
        add_offset = lo->Arg(1);
    } else if (base = lo->Arg(1); IsReadConstSource(base)) {
        add_offset = lo->Arg(0);
    } else {
        return;
    }

    IR::Inst* sel = hi->Arg(1).TryInst();
    if (!sel || sel->GetOpcode() != IR::Opcode::SelectU32) {
        return;
    }

    IR::Value sel_true = sel->Arg(1);
    IR::Value sel_false = sel->Arg(2);
    if (!sel_true.IsImmediate() || !sel_false.IsImmediate() || sel_true.U32() != 1 ||
        sel_false.U32() != 0) {
        return;
    }

    addr->SetArg(0, base);
    addr->SetArg(1, hi->Arg(0));
    for (auto [user, operand] : addr->Uses()) {
        ASSERT(user->GetOpcode() == IR::Opcode::ReadConst && operand == 0);
        IR::IREmitter ir{*user->GetParent(), IR::Block::InstructionList::s_iterator_to(*user)};
        IR::U32 offset = IR::U32{user->Arg(1)};
        IR::U32 dw_add_offset =
            add_offset.IsImmediate()
                ? ir.Imm32(add_offset.U32() >> 2u)
                : IR::U32{ir.ShiftRightLogical(IR::U32{add_offset}, ir.Imm32(2u))};
        if (offset.IsImmediate() && dw_add_offset.IsImmediate()) {
            user->SetArg(1, ir.Imm32(offset.U32() + dw_add_offset.U32()));
        } else {
            user->SetArg(1, ir.IAdd(offset, dw_add_offset));
        }
    }
}

} // Anonymous namespace

void FlattenExtendedUserdataPass(IR::Program& program) {
    g_srt_pgm_hash = program.info.pgm_hash;
    auto& post_order = program.post_order_blocks;
    PassInfo pass_info;

    // traverse at end and assign offsets to duplicate readconsts, using
    // vn_to_inst as the source
    boost::container::small_vector<IR::Inst*, 32> all_readconsts, visited;
    std::queue<IR::Inst*> queue;

    for (auto it = post_order.rbegin(); it != post_order.rend(); it++) {
        IR::Block* block{*it};
        for (IR::Inst& inst : *block) {
            if (inst.GetOpcode() != IR::Opcode::ReadConst &&
                inst.GetOpcode() != IR::Opcode::ReadConstBuffer) {
                continue;
            }

            if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer) {
                // Only flatten ReadConstBuffer if it was marked as a sharp source in the
                // resource discovery pass
                auto inst_info = inst.Flags<IR::BufferInstInfo>();
                if (!inst_info.sharp_source) {
                    continue;
                }
            }

            SimplifyReadConstAddressAdd(inst);

            all_readconsts.push_back(&inst);

            auto enqueue = [&](IR::Inst* node) {
                if (std::ranges::find(visited, node) == visited.end()) {
                    visited.push_back(node);
                    queue.push(node);
                }
            };

            auto offset = inst.Arg(1);
            if (!offset.IsImmediate()) {
                enqueue(offset.Inst());
            }
            if (auto* base = inst.Arg(0).TryInst(); base && base->NumArgs() >= 2) {
                for (size_t i = 0; i < 2; ++i) {
                    if (auto* base_arg = base->Arg(i).TryInst();
                        base_arg && base_arg->GetOpcode() == IR::Opcode::ReadConstBuffer) {
                        enqueue(base_arg);
                    }
                }
            }

            while (!queue.empty()) {
                IR::Inst* cur{queue.front()};
                queue.pop();

                if (cur->GetOpcode() == IR::Opcode::ReadConstBuffer) {
                    auto buffer_inst_info = cur->Flags<IR::BufferInstInfo>();
                    if (!buffer_inst_info.sharp_source.Value()) {
                        all_readconsts.push_back(cur);
                        auto cur_offset = cur->Arg(1);
                        if (!cur_offset.IsImmediate()) {
                            enqueue(cur_offset.Inst());
                        }
                        if (auto* cur_base = cur->Arg(0).TryInst();
                            cur_base && cur_base->NumArgs() >= 2) {
                            for (size_t i = 0; i < 2; ++i) {
                                if (auto* base_arg = cur_base->Arg(i).TryInst();
                                    base_arg &&
                                    base_arg->GetOpcode() == IR::Opcode::ReadConstBuffer) {
                                    enqueue(base_arg);
                                }
                            }
                        }
                    }
                    continue;
                }
                if (cur->GetOpcode() == IR::Opcode::GetUserData ||
                    cur->GetOpcode() == IR::Opcode::ReadConst ||
                    !IsAllowedOffsetInstruction(cur)) {
                    continue;
                }
                for (size_t arg = cur->NumArgs(); arg--;) {
                    const IR::Value arg_value = cur->Arg(arg);
                    if (auto* arg_inst = arg_value.TryInst()) {
                        enqueue(arg_inst);
                    }
                }
            }
        }
    }

    for (auto inst : all_readconsts) {
        if (pass_info.DeduplicateInstruction(inst) != inst) {
            // This is a duplicate of a readconst we've already visited
            continue;
        }

        IR::Inst* base = inst->Arg(0).TryInst();
        if (!base || base->NumArgs() < 2) {
            continue;
        }
        if (auto* base_lo = base->Arg(0).TryInst();
            base_lo && base_lo->GetOpcode() == IR::Opcode::ReadFirstLane) {
            continue;
        }
        if (!IsReadConstSource(base->Arg(0)) || !IsReadConstSource(base->Arg(1))) {
            continue;
        }

        IR::Inst* ptr_lo = base->Arg(0).Inst();
        ptr_lo = pass_info.DeduplicateInstruction(ptr_lo);

        auto ptr_uses_kv = pass_info.pointer_uses.try_emplace(ptr_lo, PassInfo::PtrUserList{});
        PassInfo::PtrUserList& user_list = ptr_uses_kv.first->second;

        user_list[inst->Arg(1)] = inst;

        if (ptr_lo->GetOpcode() == IR::Opcode::GetUserData) {
            IR::ScalarReg ud_reg = ptr_lo->Arg(0).ScalarReg();
            pass_info.srt_roots[ud_reg] = ptr_lo;
        }
    }

    GenerateSrtProgram(program.info, pass_info);

    // Assign offsets to duplicate readconsts
    for (IR::Inst* readconst : all_readconsts) {
        ASSERT(pass_info.vn_to_inst.contains(pass_info.gvn_table.GetValueNumber(readconst)));
        IR::Inst* original = pass_info.DeduplicateInstruction(readconst);
        SetFlatbufOffset(readconst, GetFlatbufOffset(original));
    }

    program.info.RefreshFlatBuf();
}

} // namespace Shader::Optimization
