// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/operand_helper.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/reinterpret.h"
#include "shader_recompiler/profile.h"
#include "video_core/amdgpu/resource.h"

namespace Shader::Optimization {
namespace {

IR::Type BufferDataType(const IR::Inst& inst, const Profile& profile) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadBufferU8:
    case IR::Opcode::StoreBufferU8:
        return IR::Type::U8;
    case IR::Opcode::LoadBufferU16:
    case IR::Opcode::StoreBufferU16:
        return IR::Type::U16;
    case IR::Opcode::LoadBufferU64:
    case IR::Opcode::StoreBufferU64:
    case IR::Opcode::BufferAtomicIAdd64:
    case IR::Opcode::BufferAtomicSMax64:
    case IR::Opcode::BufferAtomicSMin64:
    case IR::Opcode::BufferAtomicUMax64:
    case IR::Opcode::BufferAtomicUMin64:
        return IR::Type::U64;
    case IR::Opcode::BufferAtomicFMax32:
    case IR::Opcode::BufferAtomicFMin32:
        return profile.supports_buffer_fp32_atomic_min_max ? IR::Type::F32 : IR::Type::U32;
    case IR::Opcode::LoadBufferFormatF32:
    case IR::Opcode::StoreBufferFormatF32:
        // Formatted buffer loads can use a variety of types.
        return IR::Type::U32 | IR::Type::F32 | IR::Type::U16 | IR::Type::U8;
    default:
        return IR::Type::U32;
    }
}

u32 BufferAddressShift(const IR::Inst& inst, AmdGpu::DataFormat data_format) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadBufferU8:
    case IR::Opcode::StoreBufferU8:
        return 0;
    case IR::Opcode::LoadBufferU16:
    case IR::Opcode::StoreBufferU16:
        return 1;
    case IR::Opcode::LoadBufferU64:
    case IR::Opcode::StoreBufferU64:
    case IR::Opcode::BufferAtomicIAdd64:
    case IR::Opcode::BufferAtomicSMax64:
    case IR::Opcode::BufferAtomicSMin64:
    case IR::Opcode::BufferAtomicUMax64:
    case IR::Opcode::BufferAtomicUMin64:
        return 3;
    case IR::Opcode::LoadBufferFormatF32:
    case IR::Opcode::StoreBufferFormatF32: {
        switch (data_format) {
        case AmdGpu::DataFormat::Format8:
            return 0;
        case AmdGpu::DataFormat::Format8_8:
        case AmdGpu::DataFormat::Format16:
            return 1;
        case AmdGpu::DataFormat::Format8_8_8_8:
        case AmdGpu::DataFormat::Format16_16:
        case AmdGpu::DataFormat::Format10_11_11:
        case AmdGpu::DataFormat::Format2_10_10_10:
        case AmdGpu::DataFormat::Format16_16_16_16:
        case AmdGpu::DataFormat::Format32:
        case AmdGpu::DataFormat::Format32_32:
        case AmdGpu::DataFormat::Format32_32_32:
        case AmdGpu::DataFormat::Format32_32_32_32:
            return 2;
        default:
            return 0;
        }
        break;
    }
    case IR::Opcode::ReadConstBuffer:
        // Provided address is already in dwords
        return 0;
    default:
        return 2;
    }
}

class Descriptors {
public:
    explicit Descriptors(Info& info_)
        : info{info_}, buffer_resources{info_.buffers}, image_resources{info_.images},
          sampler_resources{info_.samplers}, fmask_resources(info_.fmasks) {}

    u32 Add(const BufferResource& desc) {
        const u32 index{Add(buffer_resources, desc, [&desc](const auto& existing) {
            return desc.sharp_fetch == existing.sharp_fetch && desc.post_op == existing.post_op &&
                   desc.post_op_dw1_mask == existing.post_op_dw1_mask &&
                   desc.buffer_type == existing.buffer_type;
        })};
        auto& buffer = buffer_resources[index];
        buffer.used_types |= desc.used_types;
        buffer.is_written |= desc.is_written;
        buffer.is_formatted |= desc.is_formatted;
        return index;
    }

    u32 Add(const ImageResource& desc) {
        const u32 index{Add(image_resources, desc, [&desc](const auto& existing) {
            return desc.sharp_fetch == existing.sharp_fetch && desc.is_array == existing.is_array &&
                   desc.mip_fallback_mode == existing.mip_fallback_mode &&
                   desc.constant_mip_index == existing.constant_mip_index &&
                   desc.post_op == existing.post_op;
        })};
        auto& image = image_resources[index];
        image.is_atomic |= desc.is_atomic;
        image.is_written |= desc.is_written;
        image.needs_native |= desc.needs_native;
        return index;
    }

    u32 Add(const SamplerResource& desc) {
        const u32 index{Add(sampler_resources, desc, [this, &desc](const auto& existing) {
            return desc.sharp_fetch == existing.sharp_fetch && desc.post_op == existing.post_op &&
                   desc.post_op_tsharp_dw3_off == existing.post_op_tsharp_dw3_off &&
                   desc.is_depth == existing.is_depth;
        })};
        return index;
    }

    u32 Add(const FMaskResource& desc) {
        u32 index = Add(fmask_resources, desc, [&desc](const auto& existing) {
            return desc.sharp_idx == existing.sharp_idx;
        });
        return index;
    }

private:
    template <typename Descriptors, typename Descriptor, typename Func>
    static u32 Add(Descriptors& descriptors, const Descriptor& desc, Func&& pred) {
        const auto it{std::ranges::find_if(descriptors, pred)};
        if (it != descriptors.end()) {
            return static_cast<u32>(std::distance(descriptors.begin(), it));
        }
        descriptors.push_back(desc);
        return static_cast<u32>(descriptors.size()) - 1;
    }

    const Info& info;
    BufferResourceList& buffer_resources;
    ImageResourceList& image_resources;
    SamplerResourceList& sampler_resources;
    FMaskResourceList& fmask_resources;
};

} // Anonymous namespace

using SharpSources = boost::container::small_vector<const IR::Inst*, 4>;

SharpLocation SharpLocationFromSource(const IR::Inst* inst) {
    SharpLocation location{};
    if (inst->GetOpcode() == IR::Opcode::GetUserData) {
        return static_cast<SharpLocation>(inst->Arg(0).ScalarReg());
    } else if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        location = inst->Flags<IR::BufferInstInfo>().flatbuf_off_dw;
    } else {
        location = inst->Flags<SharpLocation>();
    }
    if (location == 0) {
        LOG_WARNING(Render_Recompiler, "Sharp source was not flatenned");
        return UNKNOWN_LOCATION;
    }
    return location;
}

template <typename T>
SharpFetch<T> ConstructSharpFetch(const SharpReference& sharp) {
    SharpFetch<T> sharp_fetch{};
    sharp_fetch.and_mask_bits = sharp.and_mask_bits;
    for (u32 i = 0; i < sharp.num_dwords; i++) {
        auto dword = sharp.dwords[i];
        if (dword.IsImmediate()) {
            sharp_fetch.immediates[i] = dword.U32();
        } else {
            sharp_fetch.offsets[i] = SharpLocationFromSource(dword.Inst());
            sharp_fetch.load_mask |= (1 << i);
        }
        sharp_fetch.and_masks[i] = sharp.and_masks[i];
    }
    return sharp_fetch;
}

// gow3 stage A diagnostic: can a T# whose offsets are not statically known be described by a
// recipe built from the IR that defines its dwords? Reports only. Nothing is bound differently.
// Phis currently being expanded on this path. Seeing one again means a loop feeds the value back.
static thread_local std::vector<const IR::Inst*> g_recipe_phi_path;
// Node budget per dword, so shared Phi subtrees cannot blow up compile time.
static thread_local u32 g_recipe_budget = 0;

// Builds a structural signature of the recipe that defines a dword. A Phi is accepted only when all
// its operands produce the same signature, so the value does not depend on which path runs.
static bool RecipeSig(const IR::Value& value, int depth, std::string& sig, std::string& unsupported) {
    if (value.IsImmediate()) {
        if (value.Type() == IR::Type::U32) {
            sig += "u" + std::to_string(value.U32());
        } else {
            sig += "i" + std::to_string(static_cast<int>(value.Type()));
        }
        return true;
    }
    const IR::Inst* inst = value.TryInst();
    if (!inst) {
        unsupported = "non-inst";
        return false;
    }
    if (depth <= 0) {
        unsupported = "depth";
        return false;
    }
    if (g_recipe_budget == 0) {
        unsupported = "budget";
        return false;
    }
    --g_recipe_budget;
    const IR::Opcode op = inst->GetOpcode();
    if (op == IR::Opcode::GetUserData) {
        sig += "GUD(";
    } else if (op == IR::Opcode::Phi) {
        if (std::find(g_recipe_phi_path.begin(), g_recipe_phi_path.end(), inst) !=
            g_recipe_phi_path.end()) {
            unsupported = "Phi-cycle";
            return false;
        }
        g_recipe_phi_path.push_back(inst);
        std::string first;
        bool ok = true;
        for (size_t i = 0; i < inst->NumArgs() && ok; ++i) {
            std::string branch;
            if (!RecipeSig(inst->Arg(i), depth - 1, branch, unsupported)) {
                ok = false;
            } else if (i == 0) {
                first = branch;
            } else if (branch != first) {
                unsupported = "Phi-divergent";
                ok = false;
            }
        }
        g_recipe_phi_path.pop_back();
        if (!ok) {
            return false;
        }
        sig += first;
        return true;
    } else {
        switch (op) {
        case IR::Opcode::ReadConst:
        case IR::Opcode::ReadConstBuffer:
        case IR::Opcode::CompositeConstructU32x2:
        case IR::Opcode::CompositeConstructU32x4:
        case IR::Opcode::IAdd32:
        case IR::Opcode::IMul32:
        case IR::Opcode::FindILsb32:
        case IR::Opcode::BitFieldUExtract:
        case IR::Opcode::ShiftLeftLogical32:
        case IR::Opcode::ShiftRightLogical32:
        case IR::Opcode::BitwiseAnd32:
        case IR::Opcode::BitwiseOr32:
            break;
        default:
            unsupported = std::string{magic_enum::enum_name(op)};
            if (op == IR::Opcode::GetAttributeU32 && inst->NumArgs() > 0 &&
                inst->Arg(0).IsImmediate()) {
                // Name the vertex/instance attribute, so the log says which one blocks the recipe.
                unsupported += ":attr" + std::to_string(static_cast<u32>(inst->Arg(0).Attribute()));
            }
            return false;
        }
        sig += std::string{magic_enum::enum_name(op)} + "(";
    }
    for (size_t i = 0; i < inst->NumArgs(); ++i) {
        if (i > 0) {
            sig += ",";
        }
        if (!RecipeSig(inst->Arg(i), depth - 1, sig, unsupported)) {
            return false;
        }
    }
    sig += ")";
    return true;
}

static bool SharpRecipeSupported(const IR::Value& value, int depth, std::string& unsupported) {
    std::string sig;
    g_recipe_budget = 4096;
    g_recipe_phi_path.clear();
    return RecipeSig(value, depth, sig, unsupported);
}

static void LogSharpRecipe(const SharpReference& sharp, const ImageResource& image_res,
                           const Info& info) {
    bool unresolved = false;
    for (u32 i = 0; i < sharp.num_dwords; ++i) {
        if (((image_res.sharp_fetch.load_mask >> i) & 1) &&
            image_res.sharp_fetch.offsets[i] == UNKNOWN_LOCATION) {
            unresolved = true;
        }
    }
    if (!unresolved) {
        return;
    }
    std::string unsupported;
    bool supported = true;
    for (u32 i = 0; i < sharp.num_dwords && supported; ++i) {
        supported = SharpRecipeSupported(sharp.dwords[i], 32, unsupported);
    }
    std::printf("SHARP_RECIPE kind=image pgm=%#018llx supported=%d unsupported=%s\n",
                static_cast<unsigned long long>(info.pgm_hash), supported ? 1 : 0,
                unsupported.empty() ? "-" : unsupported.c_str());
}

static bool IsBindlessComputeTarget(u64 pgm_hash) {
    switch (pgm_hash) {
    case 0x000000002e168c7aULL:
    case 0x000000002fedcaefULL:
    case 0x0000000071b3a264ULL:
    case 0x0000000086e50a98ULL:
    case 0x0000000091f23774ULL:
    case 0x00000000939f6bcfULL:
    case 0x000000009914ae38ULL:
    case 0x00000000e775482aULL:
        return true;
    default:
        return false;
    }
}

struct PendingBindlessRecipe {
    Info::BindlessImageRecipe recipe;
    const IR::Inst* r_inst{};
    const IR::Inst* index_inst{};
    const IR::Inst* table_buf_inst{};
};

static const IR::Inst* UnwrapPhiInst(const IR::Value& value) {
    const IR::Inst* inst = value.TryInst();
    for (int hops = 0; inst && inst->GetOpcode() == IR::Opcode::Phi && hops < 8; ++hops) {
        if (inst->NumArgs() == 0) {
            return nullptr;
        }
        inst = inst->Arg(0).TryInst();
    }
    return inst;
}

static bool MatchCommutativeImm(const IR::Inst* inst, IR::Opcode op, IR::Value& out_other,
                                u32& out_imm) {
    if (!inst || inst->GetOpcode() != op || inst->NumArgs() < 2) {
        return false;
    }
    const IR::Value a0 = inst->Arg(0);
    const IR::Value a1 = inst->Arg(1);
    if (a1.IsImmediate() && a1.Type() == IR::Type::U32) {
        out_other = a0;
        out_imm = a1.U32();
        return true;
    }
    if (a0.IsImmediate() && a0.Type() == IR::Type::U32) {
        out_other = a1;
        out_imm = a0.U32();
        return true;
    }
    return false;
}

static bool MatchRWAdd(const IR::Inst* add1, const IR::Inst*& out_r, u32& out_r_offset,
                       u32& out_wg_comp) {
    if (!add1 || add1->GetOpcode() != IR::Opcode::IAdd32 || add1->NumArgs() < 2) {
        return false;
    }
    const IR::Inst* lhs = UnwrapPhiInst(add1->Arg(0));
    const IR::Inst* rhs = UnwrapPhiInst(add1->Arg(1));
    if (!lhs || !rhs) {
        return false;
    }
    auto check_pair = [&](const IR::Inst* r_cand, const IR::Inst* w_cand) -> bool {
        if (r_cand->GetOpcode() != IR::Opcode::ReadConstBuffer || r_cand->NumArgs() < 2) {
            return false;
        }
        if (!r_cand->Arg(1).IsImmediate() || r_cand->Arg(1).Type() != IR::Type::U32) {
            return false;
        }
        if (w_cand->GetOpcode() != IR::Opcode::GetAttributeU32 || w_cand->NumArgs() < 2) {
            return false;
        }
        if (!w_cand->Arg(0).IsImmediate() ||
            w_cand->Arg(0).Attribute() != IR::Attribute::WorkgroupId) {
            return false;
        }
        if (!w_cand->Arg(1).IsImmediate() || w_cand->Arg(1).Type() != IR::Type::U32) {
            return false;
        }
        out_r = r_cand;
        out_r_offset = r_cand->Arg(1).U32();
        out_wg_comp = w_cand->Arg(1).U32();
        return true;
    };
    return check_pair(lhs, rhs) || check_pair(rhs, lhs);
}

static bool TryMatchIndexChain(const IR::Inst* inst, PendingBindlessRecipe& out) {
    if (!inst || inst->GetOpcode() != IR::Opcode::ShiftRightLogical32 || inst->NumArgs() < 2) {
        return false;
    }
    if (!inst->Arg(1).IsImmediate() || inst->Arg(1).Type() != IR::Type::U32) {
        return false;
    }
    const u32 s = inst->Arg(1).U32();

    IR::Value add2_val;
    u32 m2 = 0;
    if (!MatchCommutativeImm(UnwrapPhiInst(inst->Arg(0)), IR::Opcode::IMul32, add2_val, m2)) {
        return false;
    }

    IR::Value mul1_val;
    u32 c2 = 0;
    if (!MatchCommutativeImm(UnwrapPhiInst(add2_val), IR::Opcode::IAdd32, mul1_val, c2)) {
        return false;
    }

    IR::Value add1_val;
    u32 m1 = 0;
    if (!MatchCommutativeImm(UnwrapPhiInst(mul1_val), IR::Opcode::IMul32, add1_val, m1)) {
        return false;
    }

    const IR::Inst* r_inst = nullptr;
    u32 r_offset = 0;
    u32 wg_comp = 0;
    if (!MatchRWAdd(UnwrapPhiInst(add1_val), r_inst, r_offset, wg_comp)) {
        return false;
    }

    out.r_inst = r_inst;
    out.index_inst = inst;
    out.recipe.r_offset = r_offset;
    out.recipe.workgroup_comp = wg_comp;
    out.recipe.m1 = m1;
    out.recipe.c2 = c2;
    out.recipe.m2 = m2;
    out.recipe.s = s;
    return true;
}

static bool FindIndexChainInValue(const IR::Value& value, int depth, u32& budget,
                                  PendingBindlessRecipe& out) {
    if (depth <= 0 || budget == 0 || value.IsImmediate()) {
        return false;
    }
    const IR::Inst* inst = value.TryInst();
    if (!inst) {
        return false;
    }
    --budget;
    if (TryMatchIndexChain(inst, out)) {
        return true;
    }
    for (size_t i = 0; i < inst->NumArgs(); ++i) {
        if (FindIndexChainInValue(inst->Arg(i), depth - 1, budget, out)) {
            return true;
        }
    }
    return false;
}

static bool EvalDwordOffsetWithIndex(const IR::Value& value, const IR::Inst* index_inst,
                                     u32 index_val, int depth, u32& out_val) {
    if (depth <= 0) {
        return false;
    }
    if (value.IsImmediate()) {
        if (value.Type() != IR::Type::U32) {
            return false;
        }
        out_val = value.U32();
        return true;
    }
    const IR::Inst* inst = UnwrapPhiInst(value);
    if (!inst) {
        return false;
    }
    if (inst == index_inst) {
        out_val = index_val;
        return true;
    }
    if (inst->NumArgs() < 2) {
        return false;
    }
    u32 a = 0;
    u32 b = 0;
    if (!EvalDwordOffsetWithIndex(inst->Arg(0), index_inst, index_val, depth - 1, a) ||
        !EvalDwordOffsetWithIndex(inst->Arg(1), index_inst, index_val, depth - 1, b)) {
        return false;
    }
    switch (inst->GetOpcode()) {
    case IR::Opcode::IAdd32:
        out_val = a + b;
        return true;
    case IR::Opcode::ISub32:
        out_val = a - b;
        return true;
    case IR::Opcode::IMul32:
        out_val = a * b;
        return true;
    case IR::Opcode::ShiftLeftLogical32:
        out_val = (b < 32) ? (a << b) : 0u;
        return true;
    case IR::Opcode::ShiftRightLogical32:
        out_val = (b < 32) ? (a >> b) : 0u;
        return true;
    case IR::Opcode::BitwiseAnd32:
        out_val = a & b;
        return true;
    case IR::Opcode::BitwiseOr32:
        out_val = a | b;
        return true;
    default:
        return false;
    }
}

static bool ExtractTableAddressing(const IR::Value& dw0_val, PendingBindlessRecipe& pending) {
    const IR::Inst* dw0 = UnwrapPhiInst(dw0_val);
    if (!dw0 || dw0->NumArgs() < 2) {
        return false;
    }
    u32 off0 = 0;
    u32 off1 = 0;
    if (!EvalDwordOffsetWithIndex(dw0->Arg(1), pending.index_inst, 0, 16, off0) ||
        !EvalDwordOffsetWithIndex(dw0->Arg(1), pending.index_inst, 1, 16, off1)) {
        return false;
    }
    pending.recipe.table_base_dw = off0;
    pending.recipe.table_stride_dw = off1 - off0;
    if (dw0->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        pending.recipe.table_is_buffer = true;
        pending.table_buf_inst = dw0;
        return true;
    }
    if (dw0->GetOpcode() == IR::Opcode::ReadConst) {
        const IR::Inst* base_inst = UnwrapPhiInst(dw0->Arg(0));
        if (!base_inst || base_inst->GetOpcode() != IR::Opcode::CompositeConstructU32x2 ||
            base_inst->NumArgs() < 2) {
            return false;
        }
        const IR::Inst* lo_inst = UnwrapPhiInst(base_inst->Arg(0));
        const IR::Inst* hi_inst = UnwrapPhiInst(base_inst->Arg(1));
        if (!lo_inst || !hi_inst) {
            return false;
        }
        const SharpLocation lo_loc = SharpLocationFromSource(lo_inst);
        const SharpLocation hi_loc = SharpLocationFromSource(hi_inst);
        if (lo_loc == UNKNOWN_LOCATION || hi_loc == UNKNOWN_LOCATION) {
            return false;
        }
        pending.recipe.table_is_buffer = false;
        pending.recipe.table_ptr_lo_dw = lo_loc;
        pending.recipe.table_ptr_hi_dw = hi_loc;
        return true;
    }
    return false;
}

void PatchBufferSharp(const ResourceDiscovery& resource, Info& info, Descriptors& descriptors,
                      const Profile& profile) {
    IR::Inst& inst = *resource.user;

    const u32 buffer_binding = descriptors.Add(BufferResource{
        .sharp_fetch = ConstructSharpFetch<AmdGpu::Buffer>(resource.sharps[0]),
        .used_types = BufferDataType(inst, profile),
        .buffer_type = BufferType::Guest,
        .is_written = IsBufferStore(inst),
        .is_formatted = inst.GetOpcode() == IR::Opcode::LoadBufferFormatF32 ||
                        inst.GetOpcode() == IR::Opcode::StoreBufferFormatF32,
        .post_op = resource.sharps[0].post_op,
        .post_op_dw1_mask = resource.sharps[0].post_op_data.dw1_mask,
    });

    // Replace handle with binding index in buffer resource list.
    IR::IREmitter ir{*inst.GetParent(), IR::Block::InstructionList::s_iterator_to(inst)};
    inst.SetArg(0, ir.Imm32(buffer_binding));
}

void PatchImageSharp(const ResourceDiscovery& resource, Info& info, Descriptors& descriptors,
                     const Profile& profile, std::vector<PendingBindlessRecipe>& pending_bindless) {
    IR::Inst& inst = *resource.user;

    // Read image sharp.
    const auto inst_info = inst.Flags<IR::TextureInstInfo>();
    const bool is_atomic = IsImageAtomicInstruction(inst);
    const bool is_written = inst.GetOpcode() == IR::Opcode::ImageWrite || is_atomic;
    // ImageRead with !is_written gets emitted as OpImageFetch with LOD operand, doesn't
    // need fallback (TODO is this 100% true?)
    const bool needs_mip_storage_fallback =
        inst_info.has_lod && is_written && !profile.supports_image_load_store_lod;
    const auto opcode = inst.GetOpcode();
    const bool normalized_sample =
        (opcode == IR::Opcode::ImageSampleRaw || opcode == IR::Opcode::ImageSampleImplicitLod ||
         opcode == IR::Opcode::ImageSampleExplicitLod ||
         opcode == IR::Opcode::ImageSampleDrefImplicitLod ||
         opcode == IR::Opcode::ImageSampleDrefExplicitLod || opcode == IR::Opcode::ImageGather ||
         opcode == IR::Opcode::ImageGatherDref || opcode == IR::Opcode::ImageGradient ||
         opcode == IR::Opcode::ImageQueryLod) &&
        !inst_info.has_offset && !inst_info.is_unnormalized;
    ImageResource image_res = {
        .sharp_fetch = ConstructSharpFetch<AmdGpu::Image>(resource.sharps[0]),
        .is_depth = bool(inst_info.is_depth),
        .is_atomic = is_atomic,
        .is_array = bool(inst_info.is_array),
        .is_written = is_written,
        .is_r128 = bool(inst_info.is_r128),
        .needs_native = !normalized_sample,
        .post_op = resource.sharps[0].post_op,
    };

    LogSharpRecipe(resource.sharps[0], image_res, info);
    auto image = image_res.GetSharp(info);
    ASSERT(image.GetType() != AmdGpu::ImageType::Invalid);

    if (needs_mip_storage_fallback) {
        // If the mip level to IMAGE_(LOAD/STORE)_MIP is a constant, set up ImageResource
        // so that we will only bind a single level.
        // If index is dynamic, we will bind levels as an array
        const auto view_type = image.GetViewType(image_res.is_array);

        IR::Inst* body = inst.Arg(1).Inst();
        const auto lod_arg = [&] -> IR::Value {
            switch (view_type) {
            case AmdGpu::ImageType::Color1D: // x, [lod]
                return body->Arg(1);
            case AmdGpu::ImageType::Color1DArray: // x, slice, [lod]
            case AmdGpu::ImageType::Color2D:      // x, y, [lod]
                return body->Arg(2);
            case AmdGpu::ImageType::Color2DArray: // x, y, slice, [lod]
            case AmdGpu::ImageType::Color3D:      // x, y, z, [lod]
                return body->Arg(3);
            case AmdGpu::ImageType::Color2DMsaa:
            case AmdGpu::ImageType::Color2DMsaaArray:
            default:
                UNREACHABLE_MSG("Invalid image type {}", view_type);
            }
        }();

        if (lod_arg.IsImmediate()) {
            image_res.mip_fallback_mode = MipStorageFallbackMode::ConstantIndex;
            image_res.constant_mip_index = lod_arg.U32();
        } else {
            image_res.mip_fallback_mode = MipStorageFallbackMode::DynamicIndex;
        }
    }

    // Patch image instruction if image is FMask.
    if (AmdGpu::IsFmask(image.GetDataFmt())) {
        ASSERT_MSG(!is_written, "FMask storage instructions are not supported");

        IR::IREmitter ir{*inst.GetParent(), IR::Block::InstructionList::s_iterator_to(inst)};
        switch (inst.GetOpcode()) {
        case IR::Opcode::ImageRead:
        case IR::Opcode::ImageSampleRaw: {
            IR::F32 fmaskx = ir.BitCast<IR::F32>(ir.Imm32(0x76543210));
            IR::F32 fmasky = ir.BitCast<IR::F32>(ir.Imm32(0xfedcba98));
            inst.ReplaceUsesWith(ir.CompositeConstruct(fmaskx, fmasky));
            return;
        }
        case IR::Opcode::ImageQueryLod:
            inst.ReplaceUsesWith(ir.Imm32(1));
            return;
        case IR::Opcode::ImageQueryDimensions: {
            IR::Value dims = ir.CompositeConstruct(ir.Imm32(static_cast<u32>(image.width)),  // x
                                                   ir.Imm32(static_cast<u32>(image.height)), // y
                                                   ir.Imm32(1), ir.Imm32(1)); // depth, mip
            inst.ReplaceUsesWith(dims);

            // Track FMask resource to do specialization.
            descriptors.Add(FMaskResource{
                .sharp_idx = SharpLocationFromSource(resource.sharps[0].dwords[0].Inst()),
            });
            return;
        }
        default:
            UNREACHABLE_MSG("Can't patch fmask instruction {}", inst.GetOpcode());
        }
    }

    u32 image_binding = descriptors.Add(image_res);

    if (profile.supports_bindless_images && IsBindlessComputeTarget(info.pgm_hash) &&
        !image_res.is_written) {
        for (u32 i = 0; i < resource.sharps[0].num_dwords; ++i) {
            PendingBindlessRecipe pending{};
            u32 budget = 4096;
            if (FindIndexChainInValue(resource.sharps[0].dwords[i], 32, budget, pending) &&
                ExtractTableAddressing(resource.sharps[0].dwords[0], pending)) {
                pending.recipe.image_binding = image_binding;
                pending_bindless.push_back(pending);
                break;
            }
        }
    }

    IR::IREmitter ir{*inst.GetParent(), IR::Block::InstructionList::s_iterator_to(inst)};

    if (inst.GetOpcode() == IR::Opcode::ImageSampleRaw) {
        auto& lod_prod = resource.sharps[1].post_op_data.lod_prod;
        const u32 sampler_binding = descriptors.Add(SamplerResource{
            .sharp_fetch = ConstructSharpFetch<AmdGpu::Sampler>(resource.sharps[1]),
            .post_op = resource.sharps[1].post_op,
            .post_op_tsharp_dw3_off =
                lod_prod.IsEmpty() ? UNKNOWN_LOCATION : SharpLocationFromSource(lod_prod.Inst()),
            .is_depth = bool(inst_info.is_depth), // true for the _C (compare) opcodes
        });
        inst.SetArg(0, ir.Imm32(image_binding | sampler_binding << 16));
    } else {
        inst.SetArg(0, ir.Imm32(image_binding));
    }
}

static IR::Value IsMbcntWithExec(IR::Value value, bool is_hi, IR::U1 exec) {
    if (value.IsImmediate()) {
        return {};
    }
    const IR::Inst* inst = value.Inst();
    if (inst->GetOpcode() != IR::Opcode::MaskedBitCount32 || inst->Arg(0).IsImmediate() ||
        inst->Arg(2).U1() != is_hi) {
        return {};
    }
    IR::Inst* prod = inst->Arg(0).Inst();
    if (prod->GetOpcode() != IR::Opcode::CompositeExtractU32x2 || prod->Arg(0).IsImmediate() ||
        prod->Arg(1).U32() != is_hi) {
        return {};
    }
    prod = prod->Arg(0).Inst();
    if (prod->GetOpcode() != IR::Opcode::UnpackUint2x32 || prod->Arg(0).IsImmediate()) {
        return {};
    }
    prod = prod->Arg(0).Inst();
    if (prod->GetOpcode() != IR::Opcode::Ballot || (prod->Arg(0) != exec && !is_hi)) {
        return {};
    }
    return inst->Arg(1);
}

static IR::Use FindUniqueUser(IR::Inst* inst, auto&& pred) {
    IR::Use picked{};
    for (auto& use : inst->Uses()) {
        if (pred(use.user)) {
            ASSERT(!picked.user);
            picked = use;
        }
    }
    ASSERT(picked.user);
    return picked;
}

static void RemoveAppendBufferLaneOffset(IR::Inst& vx) {
    // Attempt to detect either of the following patterns and
    // remove the lane id addition/subtraction to ds instruction.
    //
    // ds_append       vX gds
    // v_mbcnt_hi_u32_b32 vY, exec_hi, vX
    // v_mbcnt_lo_u32_b32 idx, exec_lo, vY
    //
    // v_mbcnt_hi_u32_b32 vY, exec_hi, 0
    // v_mbcnt_lo_u32_b32 vZ, exec_lo, vY
    // ds_append       vX gds
    // v_add_i32       idx, vcc, vX, vZ
    //
    // ds_consume      vX gds
    // v_mbcnt_hi_u32_b32 vY, exec_hi, 0
    // v_mbcnt_lo_u32_b32 vZ, exec_lo, vY
    // v_sub_i32       vK, vcc, vX, vZ
    // v_subrev_i32    idx, vcc, 1, vK / v_add_i32    idx, vcc, -1, vK
    //
    // ds_consume      vX gds
    // v_mbcnt_hi_u32_b32 vY, exec_hi, 0
    // v_mbcnt_lo_u32_b32 vZ, exec_lo, vY
    // v_add_i32       vK, vcc, -1, vX
    // v_sub_i32       idx, vcc, vK, vZ

    IR::U1 exec{vx.Arg(1)};
    const auto uses = vx.Uses();
    for (auto use : uses) {
        const auto& [user, operand] = use;
        if (user->GetOpcode() == IR::Opcode::MaskedBitCount32) {
            // First pattern
            ASSERT(vx.GetOpcode() == IR::Opcode::DataAppend);
            IR::Inst* vy = user;
            auto vx_2 = IsMbcntWithExec(IR::Value{vy}, true, exec);
            ASSERT(!vx_2.IsEmpty() && vx_2 == IR::Value{&vx});
            const auto [idx, operand] = FindUniqueUser(vy, [](const IR::Inst* inst) {
                return inst->GetOpcode() == IR::Opcode::MaskedBitCount32;
            });
            auto vy_2 = IsMbcntWithExec(IR::Value{idx}, false, exec);
            ASSERT(!vy_2.IsEmpty() && vy_2 == IR::Value{vy});
            idx->ReplaceUsesWithAndRemove(IR::Value{&vx});
            continue;
        }
        if (user->GetOpcode() != IR::Opcode::IAdd32 && user->GetOpcode() != IR::Opcode::ISub32) {
            continue;
        }
        const auto other = user->Arg(1 - operand);
        if (other.IsImmediate()) {
            ASSERT_MSG(other.U32() == 1u || other.U32() == std::numeric_limits<u32>::max() &&
                                                vx.GetOpcode() == IR::Opcode::DataConsume,
                       "Unexpected constant offset {} to DataConsume result", other.U32());
            use = FindUniqueUser(user, [](const IR::Inst* inst) {
                return inst->GetOpcode() == IR::Opcode::IAdd32 ||
                       inst->GetOpcode() == IR::Opcode::ISub32;
            });
        }

        // Other patterns
        auto vz = user->Arg(1 - operand);
        auto vy = IsMbcntWithExec(vz, false, exec);
        ASSERT(!vy.IsEmpty() && !vy.IsImmediate());
        auto zero_const = IsMbcntWithExec(vy, true, exec);
        ASSERT(!zero_const.IsEmpty() && zero_const.IsImmediate() && zero_const.U32() == 0u);
        vz.Inst()->ReplaceUsesWithAndRemove(IR::Value{u32{0u}});
    }
}

void PatchGlobalDataShareAccess(IR::Inst& inst, Info& info, Descriptors& descriptors,
                                const Profile& profile) {
    const u32 binding = descriptors.Add(BufferResource{
        .used_types = IR::Type::U32,
        .buffer_type = BufferType::GdsBuffer,
        .is_written = true,
    });

    IR::IREmitter ir{*inst.GetParent(), IR::Block::InstructionList::s_iterator_to(inst)};

    if (inst.GetOpcode() == IR::Opcode::DataAppend || inst.GetOpcode() == IR::Opcode::DataConsume) {
        RemoveAppendBufferLaneOffset(inst);
        const IR::Value replacement =
            inst.GetOpcode() == IR::Opcode::DataAppend
                ? ir.BufferAtomicIAdd(ir.Imm32(binding), inst.Arg(0), ir.Imm32(1u), {})
                : ir.BufferAtomicISub(ir.Imm32(binding), inst.Arg(0), ir.Imm32(1u), {});
        inst.ReplaceUsesWithAndRemove(replacement);
        return;
    }

    // Convert shared memory opcode to storage buffer atomic to GDS buffer.
    auto& buffer = info.buffers[binding];
    const IR::U32 offset = IR::U32{inst.Arg(0)};
    const IR::U32 address_words = ir.ShiftRightLogical(offset, ir.Imm32(1));
    const IR::U32 address_dwords = ir.ShiftRightLogical(offset, ir.Imm32(2));
    const IR::U32 address_qwords = ir.ShiftRightLogical(offset, ir.Imm32(3));
    const IR::U32 handle = ir.Imm32(binding);
    switch (inst.GetOpcode()) {
    case IR::Opcode::SharedAtomicIAdd32:
        inst.ReplaceUsesWith(ir.BufferAtomicIAdd(handle, address_dwords, inst.Arg(1), {}));
        break;
    case IR::Opcode::SharedAtomicIAdd64:
        inst.ReplaceUsesWith(ir.BufferAtomicIAdd(handle, address_qwords, IR::U64{inst.Arg(1)}, {}));
        break;
    case IR::Opcode::SharedAtomicISub32:
        inst.ReplaceUsesWith(ir.BufferAtomicISub(handle, address_dwords, inst.Arg(1), {}));
        break;
    case IR::Opcode::SharedAtomicSMin32:
    case IR::Opcode::SharedAtomicUMin32: {
        const bool is_signed = inst.GetOpcode() == IR::Opcode::SharedAtomicSMin32;
        inst.ReplaceUsesWith(
            ir.BufferAtomicIMin(handle, address_dwords, inst.Arg(1), is_signed, {}));
        break;
    }
    case IR::Opcode::SharedAtomicSMax32:
    case IR::Opcode::SharedAtomicUMax32: {
        const bool is_signed = inst.GetOpcode() == IR::Opcode::SharedAtomicSMax32;
        inst.ReplaceUsesWith(
            ir.BufferAtomicIMax(handle, address_dwords, inst.Arg(1), is_signed, {}));
        break;
    }
    case IR::Opcode::SharedAtomicInc32:
        inst.ReplaceUsesWith(ir.BufferAtomicInc(handle, address_dwords, {}));
        break;
    case IR::Opcode::SharedAtomicDec32:
        inst.ReplaceUsesWith(ir.BufferAtomicDec(handle, address_dwords, {}));
        break;
    case IR::Opcode::SharedAtomicAnd32:
        inst.ReplaceUsesWith(ir.BufferAtomicAnd(handle, address_dwords, inst.Arg(1), {}));
        break;
    case IR::Opcode::SharedAtomicOr32:
        inst.ReplaceUsesWith(ir.BufferAtomicOr(handle, address_dwords, inst.Arg(1), {}));
        break;
    case IR::Opcode::SharedAtomicXor32:
        inst.ReplaceUsesWith(ir.BufferAtomicXor(handle, address_dwords, inst.Arg(1), {}));
        break;
    case IR::Opcode::SharedAtomicCmpSwap32:
        // Args are (address, value, cmp_value)
        inst.ReplaceUsesWith(
            ir.BufferAtomicCmpSwap(handle, address_dwords, inst.Arg(1), inst.Arg(2), {}));
        break;
    case IR::Opcode::LoadSharedU16: {
        inst.ReplaceUsesWith(ir.LoadBufferU16(handle, address_words, {}));
        buffer.used_types |= IR::Type::U16;
        break;
    }
    case IR::Opcode::LoadSharedU32:
        inst.ReplaceUsesWith(ir.LoadBufferU32(1, handle, address_dwords, {}));
        break;
    case IR::Opcode::LoadSharedU64: {
        inst.ReplaceUsesWith(ir.LoadBufferU64(handle, address_qwords, {}));
        buffer.used_types |= IR::Type::U64;
        break;
    }
    case IR::Opcode::WriteSharedU16: {
        ir.StoreBufferU16(handle, address_words, IR::U16{inst.Arg(1)}, {});
        inst.Invalidate();
        buffer.used_types |= IR::Type::U16;
        break;
    }
    case IR::Opcode::WriteSharedU32:
        ir.StoreBufferU32(1, handle, address_dwords, inst.Arg(1), {});
        inst.Invalidate();
        break;
    case IR::Opcode::WriteSharedU64: {
        ir.StoreBufferU64(handle, address_qwords, IR::U64{inst.Arg(1)}, {});
        inst.Invalidate();
        buffer.used_types |= IR::Type::U64;
        break;
    }
    default:
        UNREACHABLE_MSG("Unexpected opcode {}", inst.GetOpcode());
    }
}

IR::U32 CalculateBufferAddress(IR::IREmitter& ir, const IR::Inst& inst, const Info& info,
                               const AmdGpu::Buffer& buffer, u32 stride) {
    const auto inst_info = inst.Flags<IR::BufferInstInfo>();
    const u32 inst_offset = inst_info.inst_offset.Value();
    const auto is_inst_typed = inst_info.inst_data_fmt != AmdGpu::DataFormat::FormatInvalid;
    const auto data_format = is_inst_typed
                                 ? AmdGpu::RemapDataFormat(inst_info.inst_data_fmt.Value())
                                 : buffer.GetDataFmt();
    const u32 shift = BufferAddressShift(inst, data_format);
    const u32 mask = (1 << shift) - 1;
    const IR::U32 soffset = IR::GetBufferSOffsetArg(&inst);

    // If address calculation is of the form "index * const_stride + offset" with offset constant
    // and both const_stride and offset are divisible with the element size, apply shift directly.
    if (inst_info.index_enable && !inst_info.voffset_enable && soffset.IsImmediate() &&
        !buffer.swizzle_enable && !buffer.add_tid_enable && (stride & mask) == 0) {
        const u32 total_offset = soffset.U32() + inst_offset;
        if ((total_offset & mask) == 0) {
            // buffer_offset = index * (const_stride >> shift) + (offset >> shift)
            const IR::U32 index = IR::GetBufferIndexArg(&inst);
            return ir.IAdd(ir.IMul(index, ir.Imm32(stride >> shift)),
                           ir.Imm32(total_offset >> shift));
        }
    }

    // index = (inst_idxen ? vgpr_index : 0) + (const_add_tid_enable ? thread_id[5:0] : 0)
    IR::U32 index = ir.Imm32(0U);
    if (inst_info.index_enable) {
        const IR::U32 vgpr_index = IR::GetBufferIndexArg(&inst);
        index = ir.IAdd(index, vgpr_index);
    }
    if (buffer.add_tid_enable) {
        if (info.sw_stage != SwStage::Compute) {
            LOG_WARNING(Render_Recompiler,
                        "Thread ID buffer addressing outside of compute stage ({})",
                        static_cast<u32>(info.sw_stage));
        }
        const IR::U32 thread_id{ir.LaneId()};
        index = ir.IAdd(index, thread_id);
    }
    // offset = (inst_offen ? vgpr_offset : 0) + inst_offset
    IR::U32 offset = ir.Imm32(inst_offset);
    offset = ir.IAdd(offset, soffset);
    if (inst_info.voffset_enable) {
        const IR::U32 voffset = IR::GetBufferVOffsetArg(&inst);
        offset = ir.IAdd(offset, voffset);
    }
    const IR::U32 const_stride = ir.Imm32(stride);
    IR::U32 buffer_offset;
    if (buffer.swizzle_enable) {
        const IR::U32 const_index_stride = ir.Imm32(buffer.GetIndexStride());
        const IR::U32 const_element_size = ir.Imm32(buffer.GetElementSize());
        // index_msb = index / const_index_stride
        const IR::U32 index_msb{ir.IDiv(index, const_index_stride)};
        // index_lsb = index % const_index_stride
        const IR::U32 index_lsb{ir.IMod(index, const_index_stride)};
        // offset_msb = offset / const_element_size
        const IR::U32 offset_msb{ir.IDiv(offset, const_element_size)};
        // offset_lsb = offset % const_element_size
        const IR::U32 offset_lsb{ir.IMod(offset, const_element_size)};
        // buffer_offset =
        //     (index_msb * const_stride + offset_msb * const_element_size) * const_index_stride
        //     + index_lsb * const_element_size + offset_lsb
        const IR::U32 buffer_offset_msb = ir.IMul(
            ir.IAdd(ir.IMul(index_msb, const_stride), ir.IMul(offset_msb, const_element_size)),
            const_index_stride);
        const IR::U32 buffer_offset_lsb =
            ir.IAdd(ir.IMul(index_lsb, const_element_size), offset_lsb);
        buffer_offset = ir.IAdd(buffer_offset_msb, buffer_offset_lsb);
    } else {
        // buffer_offset = index * const_stride + offset
        buffer_offset = ir.IAdd(ir.IMul(index, const_stride), offset);
    }
    if (shift != 0) {
        buffer_offset = ir.ShiftRightLogical(buffer_offset, ir.Imm32(shift));
    }
    return buffer_offset;
}

void PatchBufferArgs(IR::Inst& inst, Info& info) {
    const auto handle = inst.Arg(0);
    const auto buffer_res = info.buffers[handle.U32()];
    const auto buffer = buffer_res.GetSharp(info);

    // Address of constant buffer reads can be calculated at IR emission time.
    if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer) {
        return;
    }

    IR::IREmitter ir{*inst.GetParent(), IR::Block::InstructionList::s_iterator_to(inst)};
    inst.SetArg(IR::LoadBufferArgs::Address,
                CalculateBufferAddress(ir, inst, info, buffer, buffer.stride));
}

IR::Value FixCubeCoords(IR::IREmitter& ir, const AmdGpu::Image& image, const IR::Value& x,
                        const IR::Value& y, const IR::Value& face) {
    if (!image.IsCube()) {
        return ir.CompositeConstruct(x, y, face);
    }
    // AMD cube math results in coordinates in the range [1.0, 2.0]. We need
    // to convert this to the range [0.0, 1.0] to get correct results.
    const auto fixed_x = ir.FPSub(IR::F32{x}, ir.Imm32(1.f));
    const auto fixed_y = ir.FPSub(IR::F32{y}, ir.Imm32(1.f));
    const auto fixed_face =
        ir.FPFma(ir.FPFloor(ir.FPDiv(IR::F32{face}, ir.Imm32(8.f))), ir.Imm32(-2.f), IR::F32{face});
    return ir.CompositeConstruct(fixed_x, fixed_y, fixed_face);
}

void PatchImageSampleArgs(IR::Inst& inst, Info& info, const ImageResource& image_res,
                          const AmdGpu::Image& image) {
    const auto handle = inst.Arg(0);
    const auto& sampler_res = info.samplers[(handle.U32() >> 16) & 0xFFFF];
    const auto sampler = sampler_res.GetSharp(info);

    IR::IREmitter ir{*inst.GetParent(), IR::Block::InstructionList::s_iterator_to(inst)};
    const auto inst_info = inst.Flags<IR::TextureInstInfo>();
    const auto view_type = image.GetViewType(image_res.is_array);

    IR::Inst* body1 = inst.Arg(2).Inst();
    IR::Inst* body2 = inst.Arg(3).Inst();
    IR::Inst* body3 = inst.Arg(4).Inst();
    IR::F32 body4 = IR::F32{inst.Arg(5)};
    const auto get_addr_reg = [&](u32 index) -> IR::F32 {
        if (index <= 3) {
            return IR::F32{body1->Arg(index)};
        }
        if (index >= 4 && index <= 7) {
            return IR::F32{body2->Arg(index - 4)};
        }
        if (index >= 8 && index <= 11) {
            return IR::F32{body3->Arg(index - 8)};
        }
        if (index == 12) {
            return body4;
        }
        UNREACHABLE();
    };
    u32 addr_reg = 0;

    // Load first address components as denoted in 8.2.4 VGPR Usage Sea Islands Series Instruction
    // Set Architecture
    const IR::Value offset = [&] -> IR::Value {
        if (!inst_info.has_offset) {
            return IR::U32{};
        }

        // The offsets are six-bit signed integers: X=[5:0], Y=[13:8], and Z=[21:16].
        IR::Value arg = get_addr_reg(addr_reg++);
        if (const IR::Inst* offset_inst = arg.TryInst()) {
            ASSERT(offset_inst->GetOpcode() == IR::Opcode::BitCastF32U32);
            arg = offset_inst->Arg(0);
        }

        const auto read = [&](u32 off) -> IR::U32 {
            if (arg.IsImmediate()) {
                const u32 imm =
                    arg.Type() == IR::Type::F32 ? std::bit_cast<u32>(arg.F32()) : arg.U32();
                const u16 comp = (imm >> off) & 0x3F;
                return ir.Imm32(s32(comp << 26) >> 26);
            }
            return ir.BitFieldExtract(IR::U32{arg}, ir.Imm32(off), ir.Imm32(6), true);
        };

        switch (view_type) {
        case AmdGpu::ImageType::Color1D:
        case AmdGpu::ImageType::Color1DArray:
            return read(0);
        case AmdGpu::ImageType::Color2D:
        case AmdGpu::ImageType::Color2DMsaa:
        case AmdGpu::ImageType::Color2DArray:
            return ir.CompositeConstruct(read(0), read(8));
        case AmdGpu::ImageType::Color3D:
            return ir.CompositeConstruct(read(0), read(8), read(16));
        default:
            UNREACHABLE();
        }
    }();
    const IR::F32 bias = inst_info.has_bias ? get_addr_reg(addr_reg++) : IR::F32{};
    const IR::F32 dref = inst_info.is_depth ? get_addr_reg(addr_reg++) : IR::F32{};
    const auto [derivatives_dx, derivatives_dy] = [&] -> std::pair<IR::Value, IR::Value> {
        if (!inst_info.has_derivatives) {
            return {};
        }
        switch (view_type) {
        case AmdGpu::ImageType::Color1D:
        case AmdGpu::ImageType::Color1DArray:
            // du/dx, du/dy
            addr_reg = addr_reg + 2;
            return {get_addr_reg(addr_reg - 2), get_addr_reg(addr_reg - 1)};
        case AmdGpu::ImageType::Color2D:
        case AmdGpu::ImageType::Color2DMsaa:
        case AmdGpu::ImageType::Color2DArray:
            // (du/dx, dv/dx), (du/dy, dv/dy)
            addr_reg = addr_reg + 4;
            return {ir.CompositeConstruct(get_addr_reg(addr_reg - 4), get_addr_reg(addr_reg - 3)),
                    ir.CompositeConstruct(get_addr_reg(addr_reg - 2), get_addr_reg(addr_reg - 1))};
        case AmdGpu::ImageType::Color3D:
            // (du/dx, dv/dx, dw/dx), (du/dy, dv/dy, dw/dy)
            addr_reg = addr_reg + 6;
            return {ir.CompositeConstruct(get_addr_reg(addr_reg - 6), get_addr_reg(addr_reg - 5),
                                          get_addr_reg(addr_reg - 4)),
                    ir.CompositeConstruct(get_addr_reg(addr_reg - 3), get_addr_reg(addr_reg - 2),
                                          get_addr_reg(addr_reg - 1))};
        default:
            UNREACHABLE();
        }
    }();

    const bool is_msaa = view_type == AmdGpu::ImageType::Color2DMsaa ||
                         view_type == AmdGpu::ImageType::Color2DMsaaArray;
    const bool unnormalized = sampler.force_unnormalized || inst_info.is_unnormalized;
    const bool needs_dimentions = (!is_msaa && unnormalized) || (is_msaa && !unnormalized);
    const auto dimensions =
        needs_dimentions ? ir.ImageQueryDimension(handle, ir.Imm32(0u), ir.Imm1(false), inst_info)
                         : IR::Value{};
    const auto get_coord = [&](u32 coord_idx, u32 dim_idx) -> IR::Value {
        const auto coord = get_addr_reg(coord_idx);
        if (is_msaa) {
            // For MSAA images preserve the unnormalized coord or manually unnormalize it
            if (unnormalized) {
                return ir.ConvertFToU(32, coord);
            } else {
                const auto dim =
                    ir.ConvertUToF(32, 32, IR::U32{ir.CompositeExtract(dimensions, dim_idx)});
                return ir.ConvertFToU(32, ir.FPMul(coord, dim));
            }
        }
        if (unnormalized) {
            // Normalize the coordinate for sampling, dividing by its corresponding dimension.
            const auto dim =
                ir.ConvertUToF(32, 32, IR::U32{ir.CompositeExtract(dimensions, dim_idx)});
            return ir.FPDiv(coord, dim);
        }
        return coord;
    };

    // Now we can load body components as noted in Table 8.9 Image Opcodes with Sampler
    const IR::Value coords = [&] -> IR::Value {
        switch (view_type) {
        case AmdGpu::ImageType::Color1D: // x
            addr_reg = addr_reg + 1;
            return get_coord(addr_reg - 1, 0);
        case AmdGpu::ImageType::Color1DArray: // x, slice
        case AmdGpu::ImageType::Color2D:      // x, y
        case AmdGpu::ImageType::Color2DMsaa:  // x, y
            addr_reg = addr_reg + 2;
            return ir.CompositeConstruct(get_coord(addr_reg - 2, 0), get_coord(addr_reg - 1, 1));
        case AmdGpu::ImageType::Color2DArray: // x, y, slice
            addr_reg = addr_reg + 3;
            // Note we can use FixCubeCoords with fallthrough cases since it checks for image type.
            return FixCubeCoords(ir, image, get_coord(addr_reg - 3, 0), get_coord(addr_reg - 2, 1),
                                 get_addr_reg(addr_reg - 1));
        case AmdGpu::ImageType::Color3D: // x, y, z
            addr_reg = addr_reg + 3;
            return ir.CompositeConstruct(get_coord(addr_reg - 3, 0), get_coord(addr_reg - 2, 1),
                                         get_coord(addr_reg - 1, 2));
        default:
            UNREACHABLE();
        }
    }();

    ASSERT(!inst_info.has_lod || !inst_info.has_lod_clamp);
    const bool explicit_lod = inst_info.has_lod || inst_info.force_level0;
    const IR::F32 lod = inst_info.has_lod        ? get_addr_reg(addr_reg++)
                        : inst_info.force_level0 ? ir.Imm32(0.0f)
                                                 : IR::F32{};
    const IR::F32 lod_clamp = inst_info.has_lod_clamp ? get_addr_reg(addr_reg++) : IR::F32{};

    auto texel = [&] -> IR::Value {
        if (is_msaa) {
            return ir.ImageRead(handle, coords, {}, ir.Imm32(0U), inst_info);
        }
        if (inst_info.is_gather) {
            if (inst_info.is_depth) {
                return ir.ImageGatherDref(handle, coords, offset, dref, inst_info);
            }
            return ir.ImageGather(handle, coords, offset, inst_info);
        }
        if (inst_info.has_derivatives) {
            return ir.ImageGradient(handle, coords, derivatives_dx, derivatives_dy, offset,
                                    lod_clamp, inst_info);
        }
        if (inst_info.is_depth) {
            if (explicit_lod) {
                return ir.ImageSampleDrefExplicitLod(handle, coords, dref, lod, offset, inst_info);
            }
            return ir.ImageSampleDrefImplicitLod(handle, coords, dref, bias, offset, inst_info);
        }
        if (explicit_lod) {
            return ir.ImageSampleExplicitLod(handle, coords, lod, offset, inst_info);
        }
        return ir.ImageSampleImplicitLod(handle, coords, bias, offset, inst_info);
    }();

    auto converted = ApplyReadNumberConversionVec4(ir, texel, image.GetNumberConversion());
    if (sampler.force_degamma && image.GetNumberFmt() != AmdGpu::NumberFormat::Srgb) {
        converted = ApplyForceDegamma(ir, texel);
    }
    inst.ReplaceUsesWith(converted);
}

void PatchImageArgs(IR::Inst& inst, Info& info) {
    // Nothing to patch for dimension query.
    if (inst.GetOpcode() == IR::Opcode::ImageQueryDimensions) {
        return;
    }

    const auto image_handle = inst.Arg(0);
    const auto binding_index = image_handle.U32() & 0xFFFF;
    const auto& image_res = info.images[binding_index];
    auto image = image_res.GetSharp(info);

    // Sample instructions must be handled separately using address register data.
    if (inst.GetOpcode() == IR::Opcode::ImageSampleRaw) {
        return PatchImageSampleArgs(inst, info, image_res, image);
    }

    IR::IREmitter ir{*inst.GetParent(), IR::Block::InstructionList::s_iterator_to(inst)};
    auto inst_info = inst.Flags<IR::TextureInstInfo>();
    const auto view_type = image.GetViewType(image_res.is_array);

    // Now that we know the image type, adjust texture coordinate vector.
    IR::Inst* body = inst.Arg(1).Inst();
    const auto [coords, arg] = [&] -> std::pair<IR::Value, IR::Value> {
        switch (view_type) {
        case AmdGpu::ImageType::Color1D: // x, [lod]
            return {body->Arg(0), body->Arg(1)};
        case AmdGpu::ImageType::Color1DArray: // x, slice, [lod]
        case AmdGpu::ImageType::Color2D:      // x, y, [lod]
            return {ir.CompositeConstruct(body->Arg(0), body->Arg(1)), body->Arg(2)};
        case AmdGpu::ImageType::Color2DMsaa: // x, y. (sample is passed on different argument)
        {
            auto skip_lod = false;
            if (inst_info.has_lod) {
                const auto mipid = body->Arg(2);
                if (mipid.IsImmediate() && mipid.U32() == 0) {
                    // if image_x_mip refers to a MSAA image, and mipid is 0, it is safe to be
                    // skipped and fragid is taken from the next arg
                    LOG_WARNING(Render_Recompiler, "Encountered a _mip instruction with MSAA "
                                                   "image, and mipid is 0, skipping LoD");
                    inst_info.has_lod.Assign(false);
                    skip_lod = true;
                } else {
                    UNREACHABLE_MSG(
                        "Encountered a _mip instruction with MSAA image, and mipid is non-zero");
                }
            }
            return {ir.CompositeConstruct(body->Arg(0), body->Arg(1)), body->Arg(skip_lod ? 3 : 2)};
        }
        case AmdGpu::ImageType::Color2DArray: // x, y, slice, [lod]
        case AmdGpu::ImageType::Color3D:      // x, y, z, [lod]
            return {ir.CompositeConstruct(body->Arg(0), body->Arg(1), body->Arg(2)), body->Arg(3)};
        case AmdGpu::ImageType::Color2DMsaaArray: // x, y, slice. (sample is passed on different
                                                  // argument)
        {
            auto skip_lod = false;
            if (inst_info.has_lod) {
                const auto mipid = body->Arg(3);
                if (mipid.IsImmediate() && mipid.U32() == 0) {
                    LOG_WARNING(Render_Recompiler, "Encountered a _mip instruction with MSAA array "
                                                   "image, and mipid is 0, skipping LoD");
                    inst_info.has_lod.Assign(false);
                    skip_lod = true;
                } else {
                    UNREACHABLE_MSG(
                        "Encountered a _mip instruction with MSAA array image, and mipid is non-zero");
                }
            }
            return {ir.CompositeConstruct(body->Arg(0), body->Arg(1), body->Arg(2)),
                    body->Arg(skip_lod ? 4 : 3)};
        }
        default:
            UNREACHABLE_MSG("Unknown image type {}", view_type);
        }
    }();

    const auto has_ms = view_type == AmdGpu::ImageType::Color2DMsaa ||
                        view_type == AmdGpu::ImageType::Color2DMsaaArray;
    ASSERT(!inst_info.has_lod || !has_ms);
    // If we are binding a single mip level as fallback, drop the argument
    const auto lod =
        (inst_info.has_lod && image_res.mip_fallback_mode != MipStorageFallbackMode::ConstantIndex)
            ? IR::U32{arg}
            : IR::U32{};
    const auto ms = has_ms ? IR::U32{arg} : IR::U32{};

    const auto is_storage = image_res.is_written;
    if (inst.GetOpcode() == IR::Opcode::ImageRead) {
        auto texel = ir.ImageRead(image_handle, coords, lod, ms, inst_info);
        if (is_storage) {
            // Storage image requires shader swizzle.
            texel = ApplySwizzle(ir, texel, image.DstSelect());
        }
        const auto converted =
            ApplyReadNumberConversionVec4(ir, texel, image.GetNumberConversion());
        inst.ReplaceUsesWith(converted);
    } else {
        inst.SetArg(1, coords);
        if (inst.GetOpcode() == IR::Opcode::ImageWrite) {
            inst.SetArg(2, lod);
            inst.SetArg(3, ms);

            auto texel = inst.Arg(4);
            if (is_storage) {
                // Storage image requires shader swizzle.
                texel = ApplySwizzle(ir, texel, image.DstSelect().Inverse());
            }
            const auto converted =
                ApplyWriteNumberConversionVec4(ir, texel, image.GetNumberConversion());
            inst.SetArg(4, converted);
        }
    }
}

void ResourcePatchingPass(Shader::Info& info, const ResourceDiscoveryList& resources,
                          const Profile& profile) {
    // Iterate over discovered resources and patch them after finding the sharp.
    // Pass 1: Track resource sharps
    Descriptors descriptors{info};
    std::vector<PendingBindlessRecipe> pending_bindless;
    for (const auto& usage : resources) {
        IR::Inst& inst = *usage.user;
        if (IsBufferInstruction(inst)) {
            PatchBufferSharp(usage, info, descriptors, profile);
        } else if (IsImageInstruction(inst)) {
            PatchImageSharp(usage, info, descriptors, profile, pending_bindless);
        }
    }
    for (auto& pending : pending_bindless) {
        if (pending.r_inst && pending.r_inst->Arg(0).IsImmediate() &&
            pending.r_inst->Arg(0).Type() == IR::Type::U32) {
            pending.recipe.r_buffer_handle = pending.r_inst->Arg(0).U32();
            if (pending.recipe.table_is_buffer) {
                if (!pending.table_buf_inst || !pending.table_buf_inst->Arg(0).IsImmediate() ||
                    pending.table_buf_inst->Arg(0).Type() != IR::Type::U32) {
                    continue;
                }
                pending.recipe.table_buf_handle = pending.table_buf_inst->Arg(0).U32();
            }
            pending.recipe.valid = true;
            if (!info.FindBindlessImageRecipe(pending.recipe.image_binding)) {
                info.bindless_image_recipes.push_back(pending.recipe);
            }
            info.loads.Set(IR::Attribute::WorkgroupId, pending.recipe.workgroup_comp);
        }
    }

    // The rasterizer reads one table per shader (recipe 0). Recipes that disagree on the table
    // cannot be served by one array, so the shader keeps the existing per-binding path.
    if (info.bindless_image_recipes.size() > 1) {
        const auto& first = info.bindless_image_recipes.front();
        for (const auto& r : info.bindless_image_recipes) {
            if (r.table_is_buffer != first.table_is_buffer ||
                r.table_buf_handle != first.table_buf_handle ||
                r.table_ptr_lo_dw != first.table_ptr_lo_dw ||
                r.table_ptr_hi_dw != first.table_ptr_hi_dw ||
                r.table_base_dw != first.table_base_dw ||
                r.table_stride_dw != first.table_stride_dw) {
                info.bindless_image_recipes.clear();
                break;
            }
        }
    }

    // Pass 2: Patch instruction args
    for (const auto& usage : resources) {
        IR::Inst& inst = *usage.user;
        if (IsBufferInstruction(inst)) {
            PatchBufferArgs(inst, info);
        } else if (IsImageInstruction(inst)) {
            PatchImageArgs(inst, info);
        } else if (IsDataRingInstruction(inst)) {
            PatchGlobalDataShareAccess(inst, info, descriptors, profile);
        }
    }
}

} // namespace Shader::Optimization
