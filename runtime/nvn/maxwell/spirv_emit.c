// Direct Maxwell (GM20B / SM 5.x) binary -> Vulkan SPIR-V 1.5 binary emitter.
// Bypasses GLSL text generation and shaderc for supported vertex and fragment shaders,
// returning NULL cleanly for any unsupported stage/instruction so the caller falls back
// to translate.c (GLSL + shaderc).
//
// Matches the exact Vulkan descriptor & I/O contract documented in shader.h:
//   - Descriptor sets: vertex = 0, fragment = 1
//   - cbuf k -> binding k (std140 uniform block { uvec4 cb[N]; })
//   - rscale -> binding 18 (std140 uniform block { vec4 rscale[65|66]; })
//   - tint   -> binding 19 (std140 uniform block { vec4 ltint[2]; })
//   - tex i  -> binding 32 + i (combined image sampler)
//   - Vertex inputs a0..a31 (location i, float/int/uint per key->attr_kind[i])
//   - Vertex outputs gl_PerVertex { invariant Position, PointSize } + v0..v31 (location i)
//   - Fragment inputs v0..v31 (location i, flat/noperspective), FragCoord, FrontFacing, PointCoord
//   - Fragment outputs c0..c7 (location rt, float/int/uint per key->rt_kind[rt]), FragDepth
//   - Vertex float arithmetic decorated with NoContraction (matches shaders.c no_contraction())

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader.h"

#define RZ 255
#define PT 7

// ------------------------------------------------------------------ SPIR-V 1.5 constants
enum {
    SPV_MAGIC       = 0x07230203u,
    SPV_VERSION_1_5 = 0x00010500u,

    // Capabilities
    CapShader                        = 1,
    CapInt64                         = 11,
    CapSampledCubeArray              = 34,
    CapSampled1D                     = 43,
    CapImageQuery                    = 50,
    CapStorageImageReadWithoutFormat = 55,
    CapStorageImageWriteWithoutFormat = 56,
    CapGroupNonUniform               = 61,
    CapGroupNonUniformShuffle        = 65,
    CapGroupNonUniformShuffleRelative = 66,
    CapGroupNonUniformQuad           = 68,

    // Addressing / Memory models
    AddrLogical = 0,
    MemGLSL450  = 1,

    // Execution models / modes
    ExecVertex             = 0,
    ExecFragment           = 4,
    ModeOriginUpperLeft    = 7,
    ModeEarlyFragmentTests = 9,
    ModeDepthReplacing     = 12,

    // Storage classes
    ScUniformConstant = 0,
    ScInput           = 1,
    ScUniform         = 2,
    ScOutput          = 3,
    ScFunction        = 7,
    ScImage           = 11,
    ScStorageBuffer   = 12,

    // Scopes
    ScopeDevice   = 1,
    ScopeSubgroup = 3,

    // Dimensions & Image formats
    Dim1D         = 0,
    Dim2D         = 1,
    Dim3D         = 2,
    DimCube       = 3,
    ImgFmtUnknown = 0,
    ImgFmtR32ui   = 33,

    // Decorations
    DecBlock          = 2,
    DecArrayStride    = 6,
    DecBuiltIn        = 11,
    DecNoPerspective  = 13,
    DecFlat           = 14,
    DecInvariant      = 18,
    DecNonWritable    = 24,
    DecLocation       = 30,
    DecBinding        = 33,
    DecDescriptorSet  = 34,
    DecOffset         = 35,
    DecNoContraction  = 42,

    // BuiltIns
    BiPosition                  = 0,
    BiPointSize                 = 1,
    BiFragCoord                 = 15,
    BiPointCoord                = 16,
    BiFrontFacing               = 17,
    BiFragDepth                 = 22,
    BiSubgroupLocalInvocationId = 41,
    BiVertexIndex               = 42,
    BiInstanceIndex             = 43,

    // ImageOperands
    ImgOpBias = 0x1,
    ImgOpLod  = 0x2,

    // Core opcodes
    OpExtInstImport            = 11,
    OpExtInst                  = 12,
    OpMemoryModel              = 14,
    OpEntryPoint               = 15,
    OpExecutionMode            = 16,
    OpCapability               = 17,
    OpTypeVoid                 = 19,
    OpTypeBool                 = 20,
    OpTypeInt                  = 21,
    OpTypeFloat                = 22,
    OpTypeVector               = 23,
    OpTypeImage                = 25,
    OpTypeSampledImage         = 27,
    OpTypeArray                = 28,
    OpTypeRuntimeArray         = 29,
    OpTypeStruct               = 30,
    OpTypePointer              = 32,
    OpTypeFunction             = 33,
    OpConstantTrue             = 41,
    OpConstantFalse            = 42,
    OpConstant                 = 43,
    OpConstantComposite        = 44,
    OpConstantNull             = 46,
    OpFunction                 = 54,
    OpFunctionEnd              = 56,
    OpVariable                 = 59,
    OpImageTexelPointer        = 60,
    OpLoad                     = 61,
    OpStore                    = 62,
    OpAccessChain              = 65,
    OpDecorate                 = 71,
    OpMemberDecorate           = 72,
    OpVectorShuffle            = 79,
    OpCompositeConstruct       = 80,
    OpCompositeExtract         = 81,
    OpImageSampleImplicitLod   = 87,
    OpImageSampleExplicitLod   = 88,
    OpImageSampleDrefImplicitLod = 89,
    OpImageSampleDrefExplicitLod = 90,
    OpImageFetch               = 95,
    OpImageGather              = 96,
    OpImageDrefGather          = 97,
    OpImageRead                = 98,
    OpImageWrite               = 99,
    OpImage                    = 100,
    OpImageQuerySizeLod        = 103,
    OpImageQueryLevels         = 106,
    OpConvertFToU              = 109,
    OpConvertFToS              = 110,
    OpConvertSToF              = 111,
    OpConvertUToF              = 112,
    OpUConvert                 = 113,
    OpBitcast                  = 124,
    OpSNegate                  = 126,
    OpFNegate                  = 127,
    OpIAdd                     = 128,
    OpFAdd                     = 129,
    OpISub                     = 130,
    OpFSub                     = 131,
    OpIMul                     = 132,
    OpFMul                     = 133,
    OpFDiv                     = 136,
    OpVectorTimesScalar        = 142,
    OpDot                      = 148,
    OpIAddCarry                = 149,
    OpUMulExtended             = 151,
    OpSMulExtended             = 152,
    OpIsNan                    = 156,
    OpLogicalEqual             = 164,
    OpLogicalNotEqual          = 165,
    OpLogicalOr                = 166,
    OpLogicalAnd               = 167,
    OpLogicalNot               = 168,
    OpSelect                   = 169,
    OpIEqual                   = 170,
    OpINotEqual                = 171,
    OpUGreaterThan             = 172,
    OpSGreaterThan             = 173,
    OpUGreaterThanEqual        = 174,
    OpSGreaterThanEqual        = 175,
    OpULessThan                = 176,
    OpSLessThan                = 177,
    OpULessThanEqual           = 178,
    OpSLessThanEqual           = 179,
    OpFOrdEqual                = 180,
    OpFUnordEqual              = 181,
    OpFOrdNotEqual             = 182,
    OpFUnordNotEqual           = 183,
    OpFOrdLessThan             = 184,
    OpFUnordLessThan           = 185,
    OpFOrdGreaterThan          = 186,
    OpFUnordGreaterThan        = 187,
    OpFOrdLessThanEqual        = 188,
    OpFUnordLessThanEqual      = 189,
    OpFOrdGreaterThanEqual     = 190,
    OpFUnordGreaterThanEqual   = 191,
    OpShiftRightLogical        = 194,
    OpShiftRightArithmetic     = 195,
    OpShiftLeftLogical         = 196,
    OpBitwiseOr                = 197,
    OpBitwiseXor               = 198,
    OpBitwiseAnd               = 199,
    OpNot                      = 200,
    OpBitFieldInsert           = 201,
    OpBitFieldSExtract         = 202,
    OpBitFieldUExtract         = 203,
    OpBitReverse               = 204,
    OpBitCount                 = 205,
    OpAtomicExchange           = 229,
    OpAtomicIAdd               = 234,
    OpAtomicUMin               = 237,
    OpAtomicUMax               = 239,
    OpAtomicAnd                = 240,
    OpAtomicOr                 = 241,
    OpAtomicXor                = 242,
    OpLoopMerge                = 246,
    OpSelectionMerge           = 247,
    OpLabel                    = 248,
    OpBranch                   = 249,
    OpBranchConditional        = 250,
    OpSwitch                   = 251,
    OpKill                     = 252,
    OpReturn                   = 253,
    OpGroupNonUniformShuffle     = 345,
    OpGroupNonUniformShuffleXor  = 346,
    OpGroupNonUniformShuffleUp   = 347,
    OpGroupNonUniformShuffleDown = 348,
    OpGroupNonUniformQuadSwap    = 366,

    // GLSL.std.450 extended instructions
    GlslRoundEven      = 2,
    GlslTrunc          = 3,
    GlslFAbs           = 4,
    GlslSAbs           = 5,
    GlslFloor          = 8,
    GlslCeil           = 9,
    GlslSin            = 13,
    GlslCos            = 14,
    GlslPow            = 26,
    GlslExp2           = 29,
    GlslLog2           = 30,
    GlslSqrt           = 31,
    GlslInverseSqrt    = 32,
    GlslFMin           = 37,
    GlslUMin           = 38,
    GlslSMin           = 39,
    GlslFMax           = 40,
    GlslUMax           = 41,
    GlslSMax           = 42,
    GlslFClamp         = 43,
    GlslUClamp         = 44,
    GlslSClamp         = 45,
    GlslFMix           = 46,
    GlslFma            = 50,
    GlslPackHalf2x16   = 58,
    GlslUnpackHalf2x16 = 62,
    GlslFindUMsb       = 75,
    GlslFindSMsb       = 76,
    GlslNMin           = 79,
    GlslNMax           = 80,
};

// ------------------------------------------------------------------ bit / word-buffer helpers
static inline uint64_t bits(uint64_t w, int lo, int n) {
    return (w >> lo) & ((n == 64) ? ~0ull : ((1ull << n) - 1));
}
static inline int64_t sbits(uint64_t w, int lo, int n) {
    int64_t v = (int64_t)bits(w, lo, n);
    return (v & (1ll << (n - 1))) ? v - (1ll << n) : v;
}
static inline uint32_t norm_target(int64_t t) {
    if (t >= 0 && ((t / 8) % 4) == 0) t += 8;
    return (uint32_t)t;
}
#define OPM(w, val, mask) (((w) & (uint64_t)(mask)) == (uint64_t)(val))

typedef struct {
    uint32_t *w;
    size_t n, cap;
} WBuf;

static void wb_push(WBuf *b, uint32_t v) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->w = realloc(b->w, b->cap * sizeof(uint32_t));
    }
    b->w[b->n++] = v;
}
static void wb_op(WBuf *b, uint32_t op, uint32_t wc) { wb_push(b, (wc << 16) | op); }
static void wb_str(WBuf *b, const char *s) {
    size_t len = strlen(s) + 1;
    size_t nw = (len + 3) / 4;
    for (size_t i = 0; i < nw; i++) {
        uint32_t word = 0;
        for (size_t k = 0; k < 4; k++) {
            size_t p = i * 4 + k;
            if (p < len) word |= (uint32_t)(uint8_t)s[p] << (k * 8);
        }
        wb_push(b, word);
    }
}

// ------------------------------------------------------------------ SSY/PBK/PCNT stack resolution (same algorithm as translate.c)
typedef struct { uint8_t n[3]; uint32_t s[3][16]; } StkState;
static uint32_t *spv_resolve_stacks(const uint64_t *code, size_t end) {
    if (end < 2) return NULL;
    StkState *st = calloc(end, sizeof *st);
    uint8_t *seen = calloc(end, 1);
    uint32_t *tgt = calloc(end, sizeof *tgt);
    size_t *work = malloc(end * sizeof *work);
    size_t nw = 0;
    bool ok = true;
#define STK_REACH(i_, S_)                                                                     \
    do {                                                                                      \
        size_t ri = (i_);                                                                     \
        if (ri % 4 == 0) ri++;                                                                \
        if (ri >= end) break;                                                                 \
        if (!seen[ri]) { seen[ri] = 1; st[ri] = *(S_); work[nw++] = ri; }                     \
        else if (memcmp(&st[ri], (S_), sizeof(StkState))) ok = false;                         \
    } while (0)
    StkState s0;
    memset(&s0, 0, sizeof s0);
    STK_REACH(1, &s0);
    while (nw && ok) {
        size_t i = work[--nw];
        uint64_t w = code[i];
        StkState S = st[i];
        uint32_t pc = (uint32_t)(i * 8);
        size_t next = i + 1;
        uint64_t top12 = w & 0xfff0000000000000ull;
        int pr = (int)bits(w, 16, 3);
        bool prn = bits(w, 19, 1);
        bool push = top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull || top12 == 0xe2b0000000000000ull;
        if (push || top12 == 0xe270000000000000ull) { pr = PT; prn = false; }
        if (pr == PT && prn) { STK_REACH(next, &S); continue; }
        bool guarded = !(pr == PT && !prn);
        int k = top12 == 0xe290000000000000ull ? 0 : top12 == 0xe2a0000000000000ull ? 1 : 2;
        if (push && (w & 0x20)) push = false;
        if (push) {
            if (S.n[k] >= 16) { ok = false; break; }
            S.s[k][S.n[k]++] = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
            STK_REACH(next, &S);
        } else if (OPM(w, 0xe240000000000000ull, 0xfff0000000000020ull)) {  // BRA
            STK_REACH(norm_target((int64_t)pc + 8 + sbits(w, 20, 24)) / 8, &S);
            if (guarded) STK_REACH(next, &S);
        } else if (OPM(w, 0xf0f8000000000000ull, 0xfff8000000000000ull) || top12 == 0xe340000000000000ull ||
                   top12 == 0xe350000000000000ull) {  // SYNC / BRK / CONT
            int sk = top12 == 0xe340000000000000ull ? 1 : top12 == 0xe350000000000000ull ? 2 : 0;
            if (!S.n[sk]) { ok = false; break; }
            uint32_t t = S.s[sk][S.n[sk] - 1];
            if (tgt[i] && tgt[i] != t) { ok = false; break; }
            tgt[i] = t;
            StkState T = S;
            if (sk != 2) T.s[sk][--T.n[sk]] = 0;
            STK_REACH(t / 8, &T);
            if (guarded) STK_REACH(next, &S);
        } else if (top12 == 0xe300000000000000ull) {  // EXIT
            if (guarded) STK_REACH(next, &S);
        } else if (top12 >= 0xe200000000000000ull && top12 <= 0xe3f0000000000000ull && top12 != 0xe330000000000000ull) {
            ok = false;
            break;
        } else {
            STK_REACH(next, &S);
        }
    }
#undef STK_REACH
    free(st); free(seen); free(work);
    if (!ok) { free(tgt); return NULL; }
    return tgt;
}

#define SPV_LMEM_WORDS 256

// ------------------------------------------------------------------ Emitter context
typedef struct { uint32_t key, id; } ConstEnt;
typedef struct { uint64_t key; uint32_t id; } ConstEnt64;

typedef struct {
    int stage;
    const ShaderKey *key;
    ShaderInfo *info;
    const uint64_t *code;
    size_t end;
    bool branchy;
    bool unsupported;
    const char *bail_reason;
    uint64_t bail_word;
    uint32_t *stk_tgt;
    uint8_t *label;

    // Pre-scan usage metadata
    uint8_t reg_used[256];
    uint8_t pred_used[8];
    uint32_t in_attr_mask, out_attr_mask, flat_mask, noperspective_mask;
    uint32_t omap_target, omap_depth;
    uint32_t cbuf_need[18], cbuf_arr_len[18];
    int tex_slot[8192];
    bool tex_weak[64];
    bool uses_cc, uses_vid, uses_iid, uses_frag_coord, uses_front_facing, uses_point_coord;
    bool uses_rscale, uses_tint, uses_1d, uses_cube_arr, uses_img_query;
    bool uses_lane, uses_quad, uses_shfl, uses_shfl_rel;
    bool uses_pmem, uses_local, uses_sust;
    bool uses_simg_read, uses_simg_write;

    // SPIR-V sections
    uint32_t next_id;
    uint32_t glsl_ext;
    WBuf sec_dec, sec_types, sec_vars, sec_fn;
    uint32_t iface[256];
    int n_iface;

    // Cached common types
    uint32_t t_void, t_bool, t_u32, t_s32, t_f32, t_u64;
    uint32_t t_v2f, t_v3f, t_v4f, t_v2u, t_v3u, t_v4u, t_v2i, t_v3i, t_v4i;
    uint32_t t_struct_uu, t_struct_ii;  // for OpIAddCarry / OpUMulExtended / OpSMulExtended
    uint32_t t_lmem_arr, c_lmem_null, p_fn_lmem;
    uint32_t p_fn_u32, p_fn_u64, p_fn_bool;
    uint32_t p_in_f32, p_in_s32, p_in_u32, p_in_bool, p_in_v2f, p_in_v4f, p_in_v4i, p_in_v4u;
    uint32_t p_out_f32, p_out_v4f, p_out_v4i, p_out_v4u;
    uint32_t p_uni_u32, p_uni_f32, p_uni_v4u, p_uni_v4f;
    uint32_t p_sb_u32, p_img_u32;

    // Cached scalar constants
    uint32_t c_false, c_true;
    ConstEnt *cu32; size_t n_cu32, cap_cu32;
    ConstEnt *cs32; size_t n_cs32, cap_cs32;
    ConstEnt *cf32; size_t n_cf32, cap_cf32;
    ConstEnt64 *cu64_tab; size_t n_cu64, cap_cu64;

    // Global & local variable IDs
    uint32_t v_cb[18];
    uint32_t v_gmem, v_pmem, v_pmemh, v_ptab;
    uint32_t v_rscale, v_tint;
    uint32_t v_tex[64], t_img[64], t_samp[64];
    uint32_t v_simg[64], t_simg[64];
    uint32_t v_in[32], v_out[32];
    uint32_t v_pervertex, v_vid, v_iid, v_fragcoord, v_frontfacing, v_pointcoord, v_fragdepth, v_lane;
    uint32_t v_reg[256], v_pred[7];
    uint32_t v_cc, v_ccz, v_ccs;
    uint32_t v_pc, v_guard;
    uint32_t v_ga, v_gok, v_gpm, v_gph, v_gtmp;
    uint32_t v_lmem;

    // Loop / CFG labels
    uint32_t lbl_epilogue, lbl_loop_cont;
    bool blk_terminated;
    bool no_contract;  // true in vertex shaders
    bool d16_ref, rsc_pixel, no_discard, quad_shfl, early_fs;
} Spv;

static inline uint32_t new_id(Spv *s) { return s->next_id++; }
static void add_iface(Spv *s, uint32_t id) { if (s->n_iface < 256) s->iface[s->n_iface++] = id; }

static void dec0(Spv *s, uint32_t id, uint32_t d) {
    wb_op(&s->sec_dec, OpDecorate, 3); wb_push(&s->sec_dec, id); wb_push(&s->sec_dec, d);
}
static void dec1(Spv *s, uint32_t id, uint32_t d, uint32_t a) {
    wb_op(&s->sec_dec, OpDecorate, 4); wb_push(&s->sec_dec, id); wb_push(&s->sec_dec, d); wb_push(&s->sec_dec, a);
}
static void mdec0(Spv *s, uint32_t id, uint32_t m, uint32_t d) {
    wb_op(&s->sec_dec, OpMemberDecorate, 4); wb_push(&s->sec_dec, id); wb_push(&s->sec_dec, m); wb_push(&s->sec_dec, d);
}
static void mdec1(Spv *s, uint32_t id, uint32_t m, uint32_t d, uint32_t a) {
    wb_op(&s->sec_dec, OpMemberDecorate, 5); wb_push(&s->sec_dec, id); wb_push(&s->sec_dec, m); wb_push(&s->sec_dec, d); wb_push(&s->sec_dec, a);
}

static uint32_t const_lookup(Spv *s, ConstEnt **arr, size_t *n, size_t *cap, uint32_t ty, uint32_t bits_val) {
    for (size_t i = 0; i < *n; i++) if ((*arr)[i].key == bits_val) return (*arr)[i].id;
    uint32_t id = new_id(s);
    wb_op(&s->sec_types, OpConstant, 4); wb_push(&s->sec_types, ty); wb_push(&s->sec_types, id); wb_push(&s->sec_types, bits_val);
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *arr = realloc(*arr, *cap * sizeof(ConstEnt)); }
    (*arr)[(*n)++] = (ConstEnt){bits_val, id};
    return id;
}
static uint32_t cu(Spv *s, uint32_t v) { return const_lookup(s, &s->cu32, &s->n_cu32, &s->cap_cu32, s->t_u32, v); }
static uint32_t ci(Spv *s, int32_t v) { return const_lookup(s, &s->cs32, &s->n_cs32, &s->cap_cs32, s->t_s32, (uint32_t)v); }
static uint32_t cf(Spv *s, float f) { uint32_t u; memcpy(&u, &f, 4); return const_lookup(s, &s->cf32, &s->n_cf32, &s->cap_cf32, s->t_f32, u); }
static uint32_t cu64(Spv *s, uint64_t v) {
    for (size_t i = 0; i < s->n_cu64; i++) if (s->cu64_tab[i].key == v) return s->cu64_tab[i].id;
    uint32_t id = new_id(s);
    wb_op(&s->sec_types, OpConstant, 5); wb_push(&s->sec_types, s->t_u64); wb_push(&s->sec_types, id);
    wb_push(&s->sec_types, (uint32_t)v); wb_push(&s->sec_types, (uint32_t)(v >> 32));
    if (s->n_cu64 == s->cap_cu64) { s->cap_cu64 = s->cap_cu64 ? s->cap_cu64 * 2 : 32; s->cu64_tab = realloc(s->cu64_tab, s->cap_cu64 * sizeof(ConstEnt64)); }
    s->cu64_tab[s->n_cu64++] = (ConstEnt64){v, id};
    return id;
}

// ------------------------------------------------------------------ SPIR-V instruction emitters
static uint32_t emit1(Spv *s, uint32_t op, uint32_t ty, uint32_t a) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_fn, op, 4); wb_push(&s->sec_fn, ty); wb_push(&s->sec_fn, id); wb_push(&s->sec_fn, a);
    return id;
}
static uint32_t emit2(Spv *s, uint32_t op, uint32_t ty, uint32_t a, uint32_t b) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_fn, op, 5); wb_push(&s->sec_fn, ty); wb_push(&s->sec_fn, id); wb_push(&s->sec_fn, a); wb_push(&s->sec_fn, b);
    return id;
}
static uint32_t emit3(Spv *s, uint32_t op, uint32_t ty, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_fn, op, 6); wb_push(&s->sec_fn, ty); wb_push(&s->sec_fn, id); wb_push(&s->sec_fn, a); wb_push(&s->sec_fn, b); wb_push(&s->sec_fn, c);
    return id;
}
static uint32_t emit_f1(Spv *s, uint32_t op, uint32_t ty, uint32_t a) {
    uint32_t id = emit1(s, op, ty, a);
    if (s->no_contract) dec0(s, id, DecNoContraction);
    return id;
}
static uint32_t emit_f2(Spv *s, uint32_t op, uint32_t ty, uint32_t a, uint32_t b) {
    uint32_t id = emit2(s, op, ty, a, b);
    if (s->no_contract) dec0(s, id, DecNoContraction);
    return id;
}
static uint32_t glsl1(Spv *s, uint32_t ext_op, uint32_t ty, uint32_t a) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_fn, OpExtInst, 6); wb_push(&s->sec_fn, ty); wb_push(&s->sec_fn, id);
    wb_push(&s->sec_fn, s->glsl_ext); wb_push(&s->sec_fn, ext_op); wb_push(&s->sec_fn, a);
    return id;
}
static uint32_t glsl2(Spv *s, uint32_t ext_op, uint32_t ty, uint32_t a, uint32_t b) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_fn, OpExtInst, 7); wb_push(&s->sec_fn, ty); wb_push(&s->sec_fn, id);
    wb_push(&s->sec_fn, s->glsl_ext); wb_push(&s->sec_fn, ext_op); wb_push(&s->sec_fn, a); wb_push(&s->sec_fn, b);
    return id;
}
static uint32_t glsl3(Spv *s, uint32_t ext_op, uint32_t ty, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_fn, OpExtInst, 8); wb_push(&s->sec_fn, ty); wb_push(&s->sec_fn, id);
    wb_push(&s->sec_fn, s->glsl_ext); wb_push(&s->sec_fn, ext_op);
    wb_push(&s->sec_fn, a); wb_push(&s->sec_fn, b); wb_push(&s->sec_fn, c);
    if (s->no_contract && ext_op == GlslFma) dec0(s, id, DecNoContraction);
    return id;
}
static uint32_t extract(Spv *s, uint32_t ty, uint32_t comp, uint32_t idx) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_fn, OpCompositeExtract, 5); wb_push(&s->sec_fn, ty); wb_push(&s->sec_fn, id); wb_push(&s->sec_fn, comp); wb_push(&s->sec_fn, idx);
    return id;
}
static uint32_t vec2(Spv *s, uint32_t ty, uint32_t a, uint32_t b) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_fn, OpCompositeConstruct, 5); wb_push(&s->sec_fn, ty); wb_push(&s->sec_fn, id); wb_push(&s->sec_fn, a); wb_push(&s->sec_fn, b);
    return id;
}
static uint32_t vec3(Spv *s, uint32_t ty, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_fn, OpCompositeConstruct, 6); wb_push(&s->sec_fn, ty); wb_push(&s->sec_fn, id);
    wb_push(&s->sec_fn, a); wb_push(&s->sec_fn, b); wb_push(&s->sec_fn, c);
    return id;
}
static uint32_t vec4(Spv *s, uint32_t ty, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_fn, OpCompositeConstruct, 7); wb_push(&s->sec_fn, ty); wb_push(&s->sec_fn, id);
    wb_push(&s->sec_fn, a); wb_push(&s->sec_fn, b); wb_push(&s->sec_fn, c); wb_push(&s->sec_fn, d);
    return id;
}
static uint32_t load(Spv *s, uint32_t ty, uint32_t ptr) { return emit1(s, OpLoad, ty, ptr); }
static void store(Spv *s, uint32_t ptr, uint32_t val) {
    wb_op(&s->sec_fn, OpStore, 3); wb_push(&s->sec_fn, ptr); wb_push(&s->sec_fn, val);
}
static uint32_t ac1(Spv *s, uint32_t pty, uint32_t base, uint32_t i0) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_fn, OpAccessChain, 5); wb_push(&s->sec_fn, pty); wb_push(&s->sec_fn, id); wb_push(&s->sec_fn, base); wb_push(&s->sec_fn, i0);
    return id;
}
static uint32_t ac2(Spv *s, uint32_t pty, uint32_t base, uint32_t i0, uint32_t i1) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_fn, OpAccessChain, 6); wb_push(&s->sec_fn, pty); wb_push(&s->sec_fn, id);
    wb_push(&s->sec_fn, base); wb_push(&s->sec_fn, i0); wb_push(&s->sec_fn, i1);
    return id;
}
static uint32_t ac3(Spv *s, uint32_t pty, uint32_t base, uint32_t i0, uint32_t i1, uint32_t i2) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_fn, OpAccessChain, 7); wb_push(&s->sec_fn, pty); wb_push(&s->sec_fn, id);
    wb_push(&s->sec_fn, base); wb_push(&s->sec_fn, i0); wb_push(&s->sec_fn, i1); wb_push(&s->sec_fn, i2);
    return id;
}
static void emit_label(Spv *s, uint32_t lbl) { wb_op(&s->sec_fn, OpLabel, 2); wb_push(&s->sec_fn, lbl); s->blk_terminated = false; }
static void emit_branch(Spv *s, uint32_t lbl) { wb_op(&s->sec_fn, OpBranch, 2); wb_push(&s->sec_fn, lbl); s->blk_terminated = true; }
static void emit_cond_branch(Spv *s, uint32_t cond, uint32_t t, uint32_t f) {
    wb_op(&s->sec_fn, OpBranchConditional, 4); wb_push(&s->sec_fn, cond); wb_push(&s->sec_fn, t); wb_push(&s->sec_fn, f);
    s->blk_terminated = true;
}
static void emit_sel_merge(Spv *s, uint32_t merge) {
    wb_op(&s->sec_fn, OpSelectionMerge, 3); wb_push(&s->sec_fn, merge); wb_push(&s->sec_fn, 0);
}
static void emit_loop_merge(Spv *s, uint32_t merge, uint32_t cont) {
    wb_op(&s->sec_fn, OpLoopMerge, 4); wb_push(&s->sec_fn, merge); wb_push(&s->sec_fn, cont); wb_push(&s->sec_fn, 0);
}

static inline uint32_t u2f(Spv *s, uint32_t u) { return emit1(s, OpBitcast, s->t_f32, u); }
static inline uint32_t f2u(Spv *s, uint32_t f) { return emit1(s, OpBitcast, s->t_u32, f); }
static inline uint32_t u2i(Spv *s, uint32_t u) { return emit1(s, OpBitcast, s->t_s32, u); }
static inline uint32_t i2u(Spv *s, uint32_t i) { return emit1(s, OpBitcast, s->t_u32, i); }

static uint32_t rd_reg(Spv *s, int r) { return r == RZ ? cu(s, 0) : load(s, s->t_u32, s->v_reg[r]); }
static void wr_reg(Spv *s, int r, uint32_t val) { if (r != RZ) store(s, s->v_reg[r], val); }
static uint32_t rd_pred(Spv *s, int p, bool neg) {
    if (p == PT) return neg ? s->c_false : s->c_true;
    uint32_t v = load(s, s->t_bool, s->v_pred[p]);
    return neg ? emit1(s, OpLogicalNot, s->t_bool, v) : v;
}
static void wr_pred(Spv *s, int p, uint32_t val) { if (p != PT) store(s, s->v_pred[p], val); }

static void mark_unsupported(Spv *s, const char *reason, uint64_t w) {
    if (!s->unsupported) { s->unsupported = true; s->bail_reason = reason; s->bail_word = w; }
}

// ------------------------------------------------------------------ Pre-scan helpers
static int scan_tex_slot(Spv *s, uint32_t index, int type, bool shadow, bool array) {
    if (index >= 8192) { mark_unsupported(s, "tex_idx", index); return 0; }
    if (s->tex_slot[index]) {
        int sl = s->tex_slot[index] - 1;
        if (type >= 0 && s->tex_weak[sl]) {
            ShaderTexture *t = &s->info->tex[sl];
            t->type = (uint8_t)type; t->shadow = shadow; t->is_array = array;
            s->tex_weak[sl] = false;
        } else if (type >= 0 && (s->info->tex[sl].type != type || s->info->tex[sl].shadow != shadow)) {
            mark_unsupported(s, "tex_conflict", ((uint64_t)s->info->tex[sl].type << 8) | (uint64_t)type);
        }
        return sl;
    }
    bool weak = type < 0;
    if (weak) type = TEXTYPE_2D;
    int sl = s->info->ntex++;
    if (sl >= 31) { mark_unsupported(s, "tex_limit", sl); return 0; }
    s->tex_slot[index] = sl + 1;
    s->tex_weak[sl] = weak;
    ShaderTexture *t = &s->info->tex[sl];
    t->handle_offset = index * 4;
    int unit = ((int)(index * 4) - 0x20) / 8;
    t->is_uint = s->key && unit >= 0 && unit < 32 && ((s->key->tex_uint >> unit) & 1u);
    t->type = (uint8_t)type;
    t->shadow = shadow;
    t->is_array = array;
    return sl;
}

static void scan_cbuf(Spv *s, int idx, int64_t off, bool dyn) {
    if (idx < 0 || idx >= 18) { s->unsupported = true; return; }
    s->info->cbuf_mask |= 1u << idx;
    if (off + 16 > (int64_t)s->info->cbuf_size[idx]) s->info->cbuf_size[idx] = (uint32_t)((off + 16 + 255) & ~255);
    if (dyn) { s->info->cbuf_dynamic |= 1u << idx; s->info->cbuf_size[idx] = 0x10000; }
    if (!dyn && off >= 0 && (uint32_t)(off >> 4) + 1 > s->cbuf_need[idx]) s->cbuf_need[idx] = (uint32_t)(off >> 4) + 1;
}
static void scan_reg(Spv *s, int r) { if (r >= 0 && r < RZ) s->reg_used[r] = 1; else if (r > RZ) s->unsupported = true; }
static void scan_pred(Spv *s, int p) { if (p >= 0 && p < PT) s->pred_used[p] = 1; }

typedef enum { BF_REG, BF_CBUF, BF_IMM } BF;
static inline BF bform(uint64_t w) { uint64_t t = w >> 60; return t == 5 ? BF_REG : t == 4 ? BF_CBUF : BF_IMM; }

static void scan_b(Spv *s, uint64_t w) {
    BF f = bform(w);
    if (f == BF_REG) scan_reg(s, (int)bits(w, 20, 8));
    else if (f == BF_CBUF) scan_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, false);
}

static void scan_attr_load(Spv *s, int dst, uint32_t addr, int ipa_mode, int ipa_mul) {
    scan_reg(s, dst);
    if (s->stage == STAGE_VERTEX) {
        if (addr >= 0x80 && addr < 0x280) s->in_attr_mask |= 1u << ((addr - 0x80) >> 4);
        else if (addr == 0x2F8) s->uses_iid = true;
        else if (addr == 0x2FC) s->uses_vid = true;
    } else if (s->stage == STAGE_FRAGMENT) {
        if (addr >= 0x70 && addr < 0x80) {
            s->uses_frag_coord = true;
            if (((addr >> 2) & 3) < 2) s->uses_rscale = true;
        } else if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4;
            if (!s->key || (s->key->vs_out_mask & (1u << a))) {
                s->in_attr_mask |= 1u << a;
                if (ipa_mode == 0 || (ipa_mode == 1 && ipa_mul != RZ)) s->uses_frag_coord = true;
                if (ipa_mode == 1 && ipa_mul != RZ) scan_reg(s, ipa_mul);
            }
        } else if (addr == 0x3FC) s->uses_front_facing = true;
        else if (addr == 0x2E0 || addr == 0x2E4) s->uses_point_coord = true;
    }
}

static void scan_attr_store(Spv *s, int src, uint32_t addr) {
    scan_reg(s, src);
    if (addr >= 0x80 && addr < 0x280) s->out_attr_mask |= 1u << ((addr - 0x80) >> 4);
    else if (addr == 0x6C) s->info->writes_point_size = true;
}

static void scan_tex_res(Spv *s, uint32_t mask, int d0, int d1, bool texs, bool f16) {
    if (f16) { scan_reg(s, d0); scan_reg(s, d1); return; }
    int regs[4], n = 0;
    if (texs) {
        regs[0] = d0; regs[1] = d0 == RZ ? RZ : d0 + 1;
        regs[2] = d1; regs[3] = d1 == RZ ? RZ : d1 + 1;
    } else {
        for (int i = 0; i < 4; i++) regs[i] = d0 == RZ ? RZ : d0 + i;
    }
    for (int c = 0; c < 4; c++) if (mask & (1u << c)) scan_reg(s, regs[n++]);
}

static bool scan_half(Spv *s, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    uint16_t op = (uint16_t)(w >> 48);
    int kind = -1, form = -1;
    if ((op & 0xfff8) == 0x5d10) { kind = 0; form = 0; }
    else if ((op & 0xfff8) == 0x5d08) { kind = 1; form = 0; }
    else if ((op & 0xfe80) == 0x7a80) { kind = 0; form = 1; }
    else if ((op & 0xfe80) == 0x7a00) { kind = 0; form = 2; }
    else if ((op & 0xfe80) == 0x7880) { kind = 1; form = 1; }
    else if ((op & 0xfe80) == 0x7800) { kind = 1; form = 2; }
    else if ((op & 0xfe00) == 0x2c00) { kind = 0; form = 3; }
    else if ((op & 0xfe00) == 0x2a00) { kind = 1; form = 3; }
    if (kind >= 0) {
        scan_reg(s, d); scan_reg(s, a);
        if (form == 0) scan_reg(s, (int)bits(w, 20, 8));
        else if (form == 1) scan_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, false);
        return true;
    }
    form = -1;
    if ((op & 0xfff8) == 0x5d00) form = 0;
    else if ((op & 0xf880) == 0x7080) form = 1;
    else if ((op & 0xf880) == 0x6080) form = 2;
    else if ((op & 0xf880) == 0x7000) form = 3;
    else if ((op & 0xfe00) == 0x2800) form = 4;
    if (form >= 0) {
        scan_reg(s, d); scan_reg(s, a);
        if (form == 0) { scan_reg(s, (int)bits(w, 20, 8)); scan_reg(s, (int)bits(w, 39, 8)); }
        else if (form == 1) { scan_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, false); scan_reg(s, (int)bits(w, 39, 8)); }
        else if (form == 2) { scan_reg(s, (int)bits(w, 39, 8)); scan_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, false); }
        else if (form == 3) { scan_reg(s, (int)bits(w, 39, 8)); }
        return true;
    }
    if ((op & 0xfff8) == 0x5d20 || (op & 0xfff8) == 0x5d18) {
        scan_reg(s, a); scan_reg(s, (int)bits(w, 20, 8));
        scan_pred(s, (int)bits(w, 39, 3));
        if ((op & 0xfff8) == 0x5d20) { scan_pred(s, (int)bits(w, 3, 3)); scan_pred(s, (int)bits(w, 0, 3)); }
        else scan_reg(s, d);
        return true;
    }
    return false;
}

static int scan_surface_slot(Spv *s, uint64_t w, bool is_store, bool is_atomic, uint32_t *mask_out) {
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int type = (int)bits(w, 33, 3);
    if (bits(w, 28, 1)) { mark_unsupported(s, "surf_bindless", w); return -1; }
    if (type != 3) { mark_unsupported(s, "surf_type", (uint64_t)type); return -1; }
    uint32_t mask = 1;
    bool dmode = true;
    if (!is_atomic) {
        dmode = bits(w, 52, 1);
        if (dmode) {
            int size = (int)bits(w, 20, 3);
            if (size < 4 || size > 6) { mark_unsupported(s, "surf_size", (uint64_t)size); return -1; }
            mask = size == 4 ? 1u : size == 5 ? 3u : 15u;
        } else {
            mask = (uint32_t)bits(w, 20, 4);
            if (!mask) { mark_unsupported(s, "surf_mask0", w); return -1; }
        }
    }
    int slot = scan_tex_slot(s, index, TEXTYPE_2D, false, false);
    if (s->unsupported) return -1;
    s->info->tex[slot].is_image = true;
    if (!dmode) s->info->tex[slot].image_p = true;
    if (is_store) { s->info->tex[slot].image_store = true; s->uses_sust = true; }
    if (is_atomic) s->info->tex[slot].image_atomic = true;
    if (s->stage == STAGE_FRAGMENT) { s->uses_rscale = true; s->uses_frag_coord = true; }
    if (mask_out) *mask_out = mask;
    return slot;
}

static void scan_insn(Spv *s, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    int pr = (int)bits(w, 16, 3);
    bool prn = bits(w, 19, 1);
    uint64_t top12 = w & 0xfff0000000000000ull;
    if (top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull || top12 == 0xe2b0000000000000ull ||
        top12 == 0xe270000000000000ull) { pr = PT; prn = false; }
    if (pr == PT && prn) return;
    scan_pred(s, pr);

    if (OPM(w, 0xe300000000000000ull, 0xfff0000000000000ull)) {  // EXIT
    } else if (OPM(w, 0xe330000000000000ull, 0xfff0000000000000ull)) {  // KIL
        s->info->uses_discard = true;
    } else if (OPM(w, 0xe240000000000000ull, 0xfff0000000000020ull) ||
               OPM(w, 0xe290000000000000ull, 0xfff0000000000020ull) ||
               OPM(w, 0xe2a0000000000000ull, 0xfff0000000000020ull) ||
               OPM(w, 0xe2b0000000000000ull, 0xfff0000000000020ull) ||
               OPM(w, 0xf0f8000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0xe340000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0xe350000000000000ull, 0xfff0000000000000ull)) {
        // Control flow (validated by resolve_stacks)
    } else if (OPM(w, 0xf0f0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x50b0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0xef98000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0xf0a8000000000000ull, 0xfff8000000000000ull)) {
        // DEPBAR / NOP / MEMBAR / BAR
    } else if (OPM(w, 0xe00000000000ff00ull, 0xff0000400000ff00ull) || OPM(w, 0xe000004000000000ull, 0xff00004000000000ull)) {  // IPA
        if (bits(w, 38, 1)) { mark_unsupported(s, "ipa_idx", w); return; }
        uint32_t addr = (uint32_t)bits(w, 28, 10);
        int mode = (int)bits(w, 54, 2);
        if (addr >= 0x80 && addr < 0x280 && mode == 2) s->flat_mask |= 1u << ((addr - 0x80) >> 4);
        scan_attr_load(s, d, addr, mode, (int)bits(w, 20, 8));
    } else if (OPM(w, 0xefd8000000000000ull, 0xfff8000000000000ull)) {  // LD a[]
        if (a != RZ) { mark_unsupported(s, "ld_idx", w); return; }
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        for (int i = 0; i < n; i++) scan_attr_load(s, d == RZ ? RZ : d + i, addr + 4 * i, -1, RZ);
    } else if (OPM(w, 0xeff0000000000000ull, 0xfff8000000000000ull)) {  // ST a[]
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        for (int i = 0; i < n; i++) scan_attr_store(s, d == RZ ? RZ : d + i, addr + 4 * i);
    } else if (OPM(w, 0xef90000000000000ull, 0xfff8000000000000ull)) {  // LDC
        int idx = (int)bits(w, 36, 5);
        int64_t off = (int64_t)bits(w, 20, 16);
        int sz = (int)bits(w, 48, 3);
        int n = sz == 5 ? 2 : 1;
        bool dyn = a != RZ;
        if (dyn) { scan_reg(s, a); if (off >= 0x8000) off -= 0x10000; }
        for (int i = 0; i < n; i++) {
            scan_cbuf(s, idx, off + 4 * i, dyn);
            scan_reg(s, d == RZ ? RZ : d + i);
        }
    } else if (OPM(w, 0xd000000000000000ull, 0xf600000000000000ull) ||
               OPM(w, 0xd200000000000000ull, 0xf600000000000000ull)) {  // TEXS / TLDS
        bool fetch = OPM(w, 0xd200000000000000ull, 0xf600000000000000ull);
        int d0 = d, d1 = (int)bits(w, 28, 8), b = (int)bits(w, 20, 8);
        uint32_t index = (uint32_t)bits(w, 36, 13);
        int info = (int)bits(w, 53, 4);
        int msel = (int)bits(w, 50, 3);
        static const uint32_t m2[] = {1, 2, 4, 8, 3, 9, 10, 12}, m4[] = {7, 11, 13, 14, 15, 15, 15, 15};
        uint32_t mask = d1 == RZ ? m2[msel] : m4[msel];
        scan_reg(s, a); scan_reg(s, a == RZ ? RZ : a + 1);
        scan_reg(s, b); scan_reg(s, b == RZ ? RZ : b + 1);
        scan_tex_res(s, mask, d0, d1, true, !bits(w, 59, 1));
        int ty = -1; bool sh = false, ar = false;
        if (!fetch) {
            switch (info) {
            case 0: ty = TEXTYPE_1D; s->uses_1d = true; break;
            case 1: case 2: case 3: ty = TEXTYPE_2D; break;
            case 4: case 5: case 6: ty = TEXTYPE_2D; sh = true; break;
            case 7: case 8: ty = TEXTYPE_2D_ARRAY; ar = true; s->uses_img_query = true; break;
            case 9: ty = TEXTYPE_2D_ARRAY; sh = true; ar = true; break;
            case 10: case 11: ty = TEXTYPE_3D; break;
            case 12: case 13: ty = TEXTYPE_CUBE; break;
            default: mark_unsupported(s, "texs_info", (uint64_t)info); return;
            }
            if (sh && s->d16_ref && s->stage == STAGE_FRAGMENT) s->uses_rscale = true;
        } else {
            switch (info) {
            case 0: case 1: ty = TEXTYPE_1D; s->uses_1d = true; break;
            case 2: case 4: case 5: case 12:
                ty = TEXTYPE_2D;
                if (s->stage == STAGE_FRAGMENT) { s->uses_rscale = true; if (s->rsc_pixel) s->uses_frag_coord = true; }
                break;
            case 7: ty = TEXTYPE_3D; break;
            case 8:
                ty = TEXTYPE_2D_ARRAY; ar = true;
                if (s->stage == STAGE_FRAGMENT) { s->uses_rscale = true; if (s->rsc_pixel) s->uses_frag_coord = true; }
                break;
            default: mark_unsupported(s, "tlds_info", (uint64_t)info); return;
            }
        }
        scan_tex_slot(s, index, ty, sh, ar);
    } else if (OPM(w, 0xc038000000000000ull, 0xfc38000000000000ull)) {  // TEX (standard)
        int target = (int)bits(w, 29, 2), lod_mode = (int)bits(w, 55, 3);
        bool array = bits(w, 28, 1), dc = bits(w, 50, 1), aoffi = bits(w, 54, 1);
        uint32_t mask = (uint32_t)bits(w, 31, 4), index = (uint32_t)bits(w, 36, 13);
        int b = (int)bits(w, 20, 8);
        if (lod_mode == 4 || lod_mode == 5) { mark_unsupported(s, "tex_lod", w); return; }
        int ty = target == 0 ? (array ? TEXTYPE_1D_ARRAY : TEXTYPE_1D)
               : target == 1 ? (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D)
               : target == 2 ? TEXTYPE_3D
               : (array ? TEXTYPE_CUBE_ARRAY : TEXTYPE_CUBE);
        if (target == 2 && (array || dc)) { mark_unsupported(s, "tex_3d", w); return; }
        if (ty == TEXTYPE_1D || ty == TEXTYPE_1D_ARRAY) s->uses_1d = true;
        if (ty == TEXTYPE_CUBE_ARRAY) s->uses_cube_arr = true;
        if (array || aoffi) s->uses_img_query = true;
        if (dc && s->d16_ref && s->stage == STAGE_FRAGMENT) s->uses_rscale = true;
        int nc = (target == 0 ? 1 : target == 1 ? 2 : 3) + (array ? 1 : 0);
        for (int i = 0; i < nc; i++) scan_reg(s, a == RZ ? RZ : a + i);
        bool has_lod_reg = lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7;
        int nb = (has_lod_reg ? 1 : 0) + (aoffi ? 1 : 0) + (dc ? 1 : 0);
        for (int i = 0; i < nb; i++) scan_reg(s, b == RZ ? RZ : b + i);
        scan_tex_res(s, mask, d, RZ, false, false);
        scan_tex_slot(s, index, ty, dc, array);
    } else if (OPM(w, 0xdf00000000000000ull, 0xff40000000000000ull)) {  // TLD4S
        if (bits(w, 50, 1)) { mark_unsupported(s, "tld4s_dc", w); return; }
        int d1 = (int)bits(w, 28, 8), b = (int)bits(w, 20, 8);
        bool aoffi = bits(w, 51, 1);
        scan_reg(s, a); if (aoffi) { scan_reg(s, a == RZ ? RZ : a + 1); s->uses_img_query = true; }
        scan_reg(s, b);
        scan_tex_res(s, 0xF, d, d1, true, bits(w, 55, 1));
        scan_tex_slot(s, (uint32_t)bits(w, 36, 13), TEXTYPE_2D, false, false);
    } else if (OPM(w, 0xc838000000000000ull, 0xfc38000000000000ull)) {  // TLD4
        int target = (int)bits(w, 29, 2), offmode = (int)bits(w, 54, 2);
        bool array = bits(w, 28, 1), dc = bits(w, 50, 1);
        if (target != 1 || array || offmode == 3) { mark_unsupported(s, "tld4_mode", w); return; }
        int b = (int)bits(w, 20, 8);
        scan_reg(s, a); scan_reg(s, a == RZ ? RZ : a + 1);
        int prb = b;
        if (offmode == 1) { scan_reg(s, prb); prb++; s->uses_img_query = true; }
        else if (offmode == 2) { scan_reg(s, prb); scan_reg(s, prb + 1); prb += 2; }
        if (dc) scan_reg(s, prb);
        scan_tex_res(s, (uint32_t)bits(w, 31, 4), d, RZ, false, false);
        scan_tex_slot(s, (uint32_t)bits(w, 36, 13), TEXTYPE_2D, dc, false);
    } else if (OPM(w, 0xdf48000000000000ull, 0xfff8000000000000ull)) {  // TXQ
        int qt = (int)bits(w, 22, 6);
        if (qt != 1) { mark_unsupported(s, "txq_qt", (uint64_t)qt); return; }
        uint32_t mask = (uint32_t)bits(w, 31, 4);
        scan_reg(s, a);
        int n = 0; for (int i = 0; i < 4; i++) if (mask & (1u << i)) scan_reg(s, d == RZ ? RZ : d + n++);
        if (s->stage != STAGE_VERTEX) s->uses_rscale = true;
        s->uses_img_query = true;
        scan_tex_slot(s, (uint32_t)bits(w, 36, 13), -1, false, false);
    } else if (OPM(w, 0x5080000000000000ull, 0xfff8000000000000ull)) {  // MUFU
        int fn = (int)bits(w, 20, 4);
        if (fn > 5 && fn != 8) { mark_unsupported(s, "mufu_fn", (uint64_t)fn); return; }
        scan_reg(s, d); scan_reg(s, a);
    } else if (OPM(w, 0x5c90000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c90000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3890000000000000ull, 0xfef8000000000000ull)) {  // RRO
        scan_reg(s, d); scan_b(s, w);
    } else if (OPM(w, 0x5c58000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c58000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3858000000000000ull, 0xfef8000000000000ull) ||
               OPM(w, 0x5c68000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c68000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3868000000000000ull, 0xfef8000000000000ull)) {  // FADD / FMUL
        scan_reg(s, d); scan_reg(s, a); scan_b(s, w);
    } else if (OPM(w, 0x0800000000000000ull, 0xfc00000000000000ull) ||
               OPM(w, 0x1e00000000000000ull, 0xff00000000000000ull) ||
               OPM(w, 0x0c00000000000000ull, 0xfc00000000000000ull)) {  // FADD32I / FMUL32I / FFMA32I
        scan_reg(s, d); scan_reg(s, a);
    } else if (OPM(w, 0x5980000000000000ull, 0xff80000000000000ull) || OPM(w, 0x4980000000000000ull, 0xff80000000000000ull) ||
               OPM(w, 0x5180000000000000ull, 0xff80000000000000ull) || OPM(w, 0x3280000000000000ull, 0xfe80000000000000ull)) {  // FFMA
        scan_reg(s, d); scan_reg(s, a);
        uint64_t op8 = w >> 56;
        if ((op8 & 0xF8) == 0x50 && (op8 & 7) != 0) {
            scan_reg(s, (int)bits(w, 39, 8)); scan_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, false);
        } else { scan_b(s, w); scan_reg(s, (int)bits(w, 39, 8)); }
    } else if (OPM(w, 0x5c60000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c60000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3860000000000000ull, 0xfef8000000000000ull)) {  // FMNMX
        scan_reg(s, d); scan_reg(s, a); scan_b(s, w); scan_pred(s, (int)bits(w, 39, 3));
    } else if (OPM(w, 0x5bb0000000000000ull, 0xfff0000000000000ull) || OPM(w, 0x4bb0000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x36b0000000000000ull, 0xfef0000000000000ull)) {  // FSETP
        scan_reg(s, a); scan_b(s, w); scan_pred(s, (int)bits(w, 39, 3));
        scan_pred(s, (int)bits(w, 3, 3)); scan_pred(s, (int)bits(w, 0, 3));
    } else if (OPM(w, 0x5800000000000000ull, 0xff00000000000000ull) || OPM(w, 0x4800000000000000ull, 0xfe00000000000000ull) ||
               OPM(w, 0x3000000000000000ull, 0xfe00000000000000ull)) {  // FSET
        scan_reg(s, d); scan_reg(s, a); scan_b(s, w); scan_pred(s, (int)bits(w, 39, 3));
    } else if (OPM(w, 0x5ba0000000000000ull, 0xfff0000000000000ull) || OPM(w, 0x4ba0000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x36a0000000000000ull, 0xfef0000000000000ull) || OPM(w, 0x53a0000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x5b40000000000000ull, 0xfff0000000000000ull) || OPM(w, 0x4b40000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x3640000000000000ull, 0xfef0000000000000ull) || OPM(w, 0x5340000000000000ull, 0xfff0000000000000ull)) {  // FCMP / ICMP
        scan_reg(s, d); scan_reg(s, a); scan_reg(s, (int)bits(w, 39, 8));
        if ((w >> 56) == 0x53) scan_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, false);
        else scan_b(s, w);
    } else if (OPM(w, 0x5c98000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c98000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3898000000000000ull, 0xfef8000000000000ull) ||
               OPM(w, 0x5cb0000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4cb0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x38b0000000000000ull, 0xfef8000000000000ull) ||
               OPM(w, 0x5cb8000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4cb8000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x38b8000000000000ull, 0xfef8000000000000ull) ||
               OPM(w, 0x5ca8000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4ca8000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x38a8000000000000ull, 0xfef8000000000000ull) ||
               OPM(w, 0x5c30000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c30000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3830000000000000ull, 0xfef8000000000000ull) ||
               OPM(w, 0x5c08000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c08000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3808000000000000ull, 0xfef8000000000000ull)) {  // MOV / F2I / I2F / F2F / FLO / POPC
        scan_reg(s, d); scan_b(s, w);
    } else if (OPM(w, 0x0100000000000000ull, 0xfff0000000000000ull)) {  // MOV32I
        scan_reg(s, d);
    } else if (OPM(w, 0x5ca0000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4ca0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x38a0000000000000ull, 0xfef8000000000000ull) ||
               OPM(w, 0x5c20000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c20000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3820000000000000ull, 0xfef8000000000000ull)) {  // SEL / IMNMX
        scan_reg(s, d); scan_reg(s, a); scan_b(s, w); scan_pred(s, (int)bits(w, 39, 3));
    } else if (OPM(w, 0x5c10000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c10000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3810000000000000ull, 0xfef8000000000000ull) ||
               OPM(w, 0x5c18000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c18000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3818000000000000ull, 0xfef8000000000000ull)) {  // IADD / ISCADD
        if (bits(w, 43, 1) || bits(w, 47, 1)) s->uses_cc = true;
        scan_reg(s, d); scan_reg(s, a); scan_b(s, w);
    } else if (OPM(w, 0x1c00000000000000ull, 0xfe00000000000000ull)) {  // IADD32I
        if (bits(w, 52, 1) || bits(w, 53, 1)) s->uses_cc = true;
        scan_reg(s, d); scan_reg(s, a);
    } else if (OPM(w, 0x5cc0000000000000ull, 0xfff0000000000000ull) || OPM(w, 0x4cc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x38c0000000000000ull, 0xfef0000000000000ull)) {  // IADD3
        if (bits(w, 47, 1) || bits(w, 48, 1)) s->uses_cc = true;
        scan_reg(s, d); scan_reg(s, a); scan_b(s, w); scan_reg(s, (int)bits(w, 39, 8));
    } else if (OPM(w, 0x1400000000000000ull, 0xfc00000000000000ull) ||
               OPM(w, 0x0400000000000000ull, 0xfc00000000000000ull) ||
               OPM(w, 0x1f00000000000000ull, 0xff00000000000000ull)) {  // ISCADD32I / LOP32I / IMUL32I
        scan_reg(s, d); scan_reg(s, a);
    } else if (OPM(w, 0x5c48000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c48000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3848000000000000ull, 0xfef8000000000000ull) ||
               OPM(w, 0x5c28000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c28000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3828000000000000ull, 0xfef8000000000000ull) ||
               OPM(w, 0x5c38000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c38000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3838000000000000ull, 0xfef8000000000000ull) ||
               OPM(w, 0x5c00000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c00000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3800000000000000ull, 0xfef8000000000000ull)) {  // SHL / SHR / IMUL / BFE
        scan_reg(s, d); scan_reg(s, a); scan_b(s, w);
    } else if (OPM(w, 0x5c40000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c40000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3840000000000000ull, 0xfef8000000000000ull)) {  // LOP
        scan_reg(s, d); scan_reg(s, a); scan_b(s, w); scan_pred(s, (int)bits(w, 48, 3));
    } else if (OPM(w, 0x5be0000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x3c00000000000000ull, 0xfc00000000000000ull) ||
               OPM(w, 0x0200000000000000ull, 0xfe00000000000000ull)) {  // LOP3
        scan_reg(s, d); scan_reg(s, a); scan_reg(s, (int)bits(w, 39, 8));
        if ((w >> 56) == 0x5b) scan_reg(s, (int)bits(w, 20, 8));
        else if ((w >> 58) != 0xf) scan_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, false);
    } else if (OPM(w, 0x5bf0000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4bf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x36f0000000000000ull, 0xfef8000000000000ull) || OPM(w, 0x53f0000000000000ull, 0xfff8000000000000ull)) {  // BFI
        scan_reg(s, d); scan_reg(s, a); scan_reg(s, (int)bits(w, 39, 8));
        if (OPM(w, 0x53f0000000000000ull, 0xfff8000000000000ull)) scan_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, false);
        else scan_b(s, w);
    } else if (OPM(w, 0x5b60000000000000ull, 0xfff0000000000000ull) || OPM(w, 0x4b60000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x3660000000000000ull, 0xfef0000000000000ull)) {  // ISETP
        scan_reg(s, a); scan_b(s, w); scan_pred(s, (int)bits(w, 39, 3));
        scan_pred(s, (int)bits(w, 3, 3)); scan_pred(s, (int)bits(w, 0, 3));
    } else if (OPM(w, 0x5b50000000000000ull, 0xfff0000000000000ull) || OPM(w, 0x4b50000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x3650000000000000ull, 0xfef0000000000000ull)) {  // ISET
        scan_reg(s, d); scan_reg(s, a); scan_b(s, w); scan_pred(s, (int)bits(w, 39, 3));
    } else if (OPM(w, 0x5b00000000000000ull, 0xffc0000000000000ull) || OPM(w, 0x4e00000000000000ull, 0xfe00000000000000ull) ||
               OPM(w, 0x3600000000000000ull, 0xfec0000000000000ull) || OPM(w, 0x5100000000000000ull, 0xff80000000000000ull)) {  // XMAD
        uint64_t top = w >> 56;
        bool ext = (top == 0x5b || (top & 0xfe) == 0x36) ? bits(w, 38, 1) : bits(w, 54, 1);
        if (ext || bits(w, 47, 1)) s->uses_cc = true;
        scan_reg(s, d); scan_reg(s, a); scan_reg(s, (int)bits(w, 39, 8));
        if (top == 0x5b) scan_reg(s, (int)bits(w, 20, 8));
        else if ((top & 0xfe) == 0x4e || (top & 0xfe) == 0x50) scan_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, false);
    } else if (OPM(w, 0x5090000000000000ull, 0xfff8000000000000ull)) {  // PSETP
        scan_pred(s, (int)bits(w, 12, 3)); scan_pred(s, (int)bits(w, 29, 3)); scan_pred(s, (int)bits(w, 39, 3));
        scan_pred(s, (int)bits(w, 3, 3)); scan_pred(s, (int)bits(w, 0, 3));
    } else if (OPM(w, 0x5088000000000000ull, 0xfff8000000000000ull)) {  // PSET
        scan_reg(s, d); scan_pred(s, (int)bits(w, 12, 3)); scan_pred(s, (int)bits(w, 29, 3)); scan_pred(s, (int)bits(w, 39, 3));
    } else if (OPM(w, 0xf0c8000000000000ull, 0xfff8000000000000ull)) {  // S2R
        scan_reg(s, d);
    } else if (OPM(w, 0x5ce0000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4ce0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x38e0000000000000ull, 0xfef8000000000000ull)) {  // I2I
        if (bits(w, 47, 1)) s->uses_cc = true;
        scan_reg(s, d); scan_b(s, w);
    } else if (OPM(w, 0x5bc0000000000000ull, 0xfff0000000000000ull) || OPM(w, 0x4bc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x36c0000000000000ull, 0xfef0000000000000ull) || OPM(w, 0x53c0000000000000ull, 0xfff0000000000000ull)) {  // PRMT
        if (bits(w, 48, 4)) { mark_unsupported(s, "prmt_mode", w); return; }
        scan_reg(s, d); scan_reg(s, a); scan_reg(s, (int)bits(w, 39, 8));
        if (OPM(w, 0x53c0000000000000ull, 0xfff0000000000000ull)) scan_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, false);
        else scan_b(s, w);
    } else if (OPM(w, 0x5f00000000000000ull, 0xff00000000000000ull) ||
               OPM(w, 0x50f0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3a00000000000000ull, 0xfe00000000000000ull)) {  // VMAD / VSETP / VMNMX
        if (OPM(w, 0x5f00000000000000ull, 0xff00000000000000ull)) {
            if (bits(w, 51, 2)) { mark_unsupported(s, "vmad_mode", w); return; }
            scan_reg(s, d); scan_reg(s, (int)bits(w, 39, 8));
        } else if (OPM(w, 0x3a00000000000000ull, 0xfe00000000000000ull)) {
            int op = (int)bits(w, 51, 3);
            if ((op != 5 && op != 6) || bits(w, 55, 1) || bits(w, 47, 1)) { mark_unsupported(s, "vmnmx_mode", w); return; }
            scan_reg(s, d); scan_reg(s, (int)bits(w, 39, 8));
        } else {
            scan_pred(s, (int)bits(w, 39, 3)); scan_pred(s, (int)bits(w, 3, 3)); scan_pred(s, (int)bits(w, 0, 3));
        }
        scan_reg(s, a);
        if (bits(w, 50, 1)) scan_reg(s, (int)bits(w, 20, 8));
    } else if (OPM(w, 0x38f0000000000000ull, 0xfef8000000000000ull) || OPM(w, 0x5cf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x4cf0000000000000ull, 0xfff8000000000000ull)) {  // R2P
        if (bits(w, 40, 1)) { mark_unsupported(s, "r2p_cc", w); return; }
        scan_reg(s, a); scan_b(s, w);
        for (int i = 0; i < 7; i++) scan_pred(s, i);
    } else if (OPM(w, 0x38e8000000000000ull, 0xfef8000000000000ull) || OPM(w, 0x5ce8000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x4ce8000000000000ull, 0xfff8000000000000ull)) {  // P2R
        scan_reg(s, d); scan_reg(s, a); scan_b(s, w);
        if (bits(w, 40, 1)) s->uses_cc = true;
        else for (int i = 0; i < 7; i++) scan_pred(s, i);
    } else if (OPM(w, 0x50a0000000000000ull, 0xfff8000000000000ull)) {  // CSETP
        int test = (int)bits(w, 8, 5);
        if (test != 0 && test != 15 && test != 1 && test != 2 && test != 3 && test != 4 && test != 5 && test != 6 &&
            test != 9 && test != 10 && test != 11 && test != 12 && test != 13 && test != 14) { mark_unsupported(s, "csetp", (uint64_t)test); return; }
        s->uses_cc = true;
        scan_pred(s, (int)bits(w, 39, 3)); scan_pred(s, (int)bits(w, 3, 3)); scan_pred(s, (int)bits(w, 0, 3));
    } else if (OPM(w, 0xeb00000000000000ull, 0xffe0000000000000ull) ||
               OPM(w, 0xeb20000000000000ull, 0xffe0000000000000ull)) {  // SULD / SUST
        bool is_st = OPM(w, 0xeb20000000000000ull, 0xffe0000000000000ull);
        uint32_t mask = 0;
        int sl = scan_surface_slot(s, w, is_st, false, &mask);
        if (sl < 0) return;
        scan_reg(s, a); scan_reg(s, a == RZ ? RZ : a + 1);
        int n = 0;
        for (int c = 0; c < 4; c++) if (mask & (1u << c)) { scan_reg(s, d == RZ ? RZ : d + n); n++; }
        if (is_st) s->uses_simg_write = true;
        else s->uses_simg_read = true;
    } else if (OPM(w, 0xea00000000000000ull, 0xffc0000000000000ull)) {  // SUATOM
        int op = (int)bits(w, 29, 4), ty = (int)bits(w, 51, 3);
        if (ty > 1 || op > 8 || op == 3 || op == 4 || (ty == 1 && (op == 1 || op == 2))) { mark_unsupported(s, "suatom_op", w); return; }
        if (scan_surface_slot(s, w, true, true, NULL) < 0) return;
        scan_reg(s, d); scan_reg(s, a); scan_reg(s, a == RZ ? RZ : a + 1); scan_reg(s, (int)bits(w, 20, 8));
    } else if (OPM(w, 0x5cf8000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x38f8000000000000ull, 0xfef8000000000000ull) ||
               OPM(w, 0x5bf8000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x36f8000000000000ull, 0xfef8000000000000ull)) {  // SHF
        scan_reg(s, d); scan_reg(s, a); scan_reg(s, (int)bits(w, 39, 8));
        if ((w >> 60) != 3) scan_reg(s, (int)bits(w, 20, 8));
    } else if (OPM(w, 0xeed0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0xeed8000000000000ull, 0xfff8000000000000ull)) {  // LDG / STG
        bool is_st = bits(w, 51, 1);
        bool wide = bits(w, 45, 1);
        int sz = (int)bits(w, 48, 3);
        int nw = sz == 5 ? 2 : sz >= 6 ? 4 : 1;
        s->info->uses_gmem = true;
        if (maxwell_pmem_size) s->uses_pmem = true;
        if (is_st) s->info->uses_gmem_store = true;
        scan_reg(s, a);
        if (wide && a != RZ) scan_reg(s, a + 1);
        for (int k = 0; k < nw; k++) if (d != RZ && d + k < RZ) scan_reg(s, d + k);
    } else if (OPM(w, 0xed00000000000000ull, 0xff00000000000000ull)) {  // ATOM
        int op = (int)bits(w, 52, 4), ty = (int)bits(w, 49, 3);
        if (ty > 1 || op > 8 || op == 3 || op == 4 || (ty == 1 && (op == 1 || op == 2))) { mark_unsupported(s, "atom_op", w); return; }
        s->info->uses_gmem = true;
        s->info->uses_gmem_store = true;
        if (maxwell_pmem_size) s->uses_pmem = true;
        scan_reg(s, d); scan_reg(s, a);
        if (bits(w, 48, 1) && a != RZ) scan_reg(s, a + 1);
        scan_reg(s, (int)bits(w, 20, 8));
    } else if (OPM(w, 0x50f8000000000000ull, 0xfff8000000000000ull)) {  // FSWZADD
        scan_reg(s, d); scan_reg(s, a); scan_reg(s, (int)bits(w, 20, 8));
        if (s->stage == STAGE_FRAGMENT) s->uses_lane = true;
    } else if (OPM(w, 0xef40000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0xef50000000000000ull, 0xfff8000000000000ull)) {  // LDL / STL
        int sz = (int)bits(w, 48, 3);
        int nw = sz == 5 ? 2 : sz >= 6 ? 4 : 1;
        s->uses_local = true;
        scan_reg(s, a);
        for (int k = 0; k < nw; k++) if (d != RZ && d + k < RZ) scan_reg(s, d + k);
    } else if (OPM(w, 0xef10000000000000ull, 0xfff8000000000000ull)) {  // SHFL
        int mode = (int)bits(w, 30, 2);
        bool imm = bits(w, 28, 1);
        uint32_t lm = (uint32_t)bits(w, 20, 5);
        scan_reg(s, d); scan_reg(s, a);
        if (!imm) scan_reg(s, (int)bits(w, 20, 8));
        scan_pred(s, (int)bits(w, 48, 3));
        if (s->stage == STAGE_FRAGMENT) {
            if (s->quad_shfl && mode == 3 && imm && lm >= 1 && lm <= 3) s->uses_quad = true;
            else if (mode == 1 || mode == 2) s->uses_shfl_rel = true;
            else s->uses_shfl = true;
        }
    } else if (scan_half(s, w)) {
    } else {
        // Unsupported instruction (LDG/STG/ATOM/SULD/SUST/SUATOM/SHFL/FSWZADD/LDL/STL/SHF/TEX.B/VOTE/etc.) -> fallback
        mark_unsupported(s, "opcode", w);
    }
}

// ------------------------------------------------------------------ SPIR-V value / math helpers
static uint32_t emit_cbuf(Spv *s, int idx, int64_t off, uint32_t dyn_u32) {
    uint32_t v4idx, lane;
    if (dyn_u32) {
        uint32_t boff = off < 0 ? emit2(s, OpISub, s->t_u32, dyn_u32, cu(s, (uint32_t)(-off)))
                      : off > 0 ? emit2(s, OpIAdd, s->t_u32, dyn_u32, cu(s, (uint32_t)off))
                      : dyn_u32;
        uint32_t raw_i = emit2(s, OpShiftRightLogical, s->t_u32, boff, cu(s, 4));
        v4idx = glsl2(s, GlslUMin, s->t_u32, raw_i, cu(s, s->cbuf_arr_len[idx] - 1));
        lane = emit2(s, OpBitwiseAnd, s->t_u32, emit2(s, OpShiftRightLogical, s->t_u32, boff, cu(s, 2)), cu(s, 3));
    } else {
        v4idx = ci(s, (int32_t)(off >> 4));
        lane = ci(s, (int32_t)((off >> 2) & 3));
    }
    uint32_t ptr = ac3(s, s->p_uni_u32, s->v_cb[idx], ci(s, 0), v4idx, lane);
    return load(s, s->t_u32, ptr);
}

static uint32_t emit_b(Spv *s, uint64_t w, bool float_imm) {
    switch (bform(w)) {
    case BF_REG: return rd_reg(s, (int)bits(w, 20, 8));
    case BF_CBUF: return emit_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, 0);
    default: {
        uint32_t v = (uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19);
        if (float_imm) v <<= 12;
        else if (v & 0x80000u) v |= 0xFFF00000u;
        return cu(s, v);
    }
    }
}

static uint32_t emit_na(Spv *s, uint32_t f, bool neg, bool abs_) {
    if (abs_) f = glsl1(s, GlslFAbs, s->t_f32, f);
    if (neg) f = emit_f1(s, OpFNegate, s->t_f32, f);
    return f;
}
static uint32_t emit_sat(Spv *s, uint32_t f) {
    uint32_t isn = emit1(s, OpIsNan, s->t_bool, f);
    uint32_t cl = glsl3(s, GlslFClamp, s->t_f32, f, cf(s, 0.0f), cf(s, 1.0f));
    return emit3(s, OpSelect, s->t_f32, isn, cf(s, 0.0f), cl);
}
static uint32_t emit_sat2(Spv *s, uint32_t v2) {
    uint32_t x = emit_sat(s, extract(s, s->t_f32, v2, 0));
    uint32_t y = emit_sat(s, extract(s, s->t_f32, v2, 1));
    return vec2(s, s->t_v2f, x, y);
}

static uint32_t emit_bool_op(Spv *s, int bo, uint32_t a, uint32_t b) {
    if (bo == 0) return emit2(s, OpLogicalAnd, s->t_bool, a, b);
    if (bo == 1) return emit2(s, OpLogicalOr, s->t_bool, a, b);
    return emit2(s, OpLogicalNotEqual, s->t_bool, a, b);
}

static uint32_t emit_fcmp(Spv *s, int op, uint32_t a, uint32_t b) {
    switch (op & 15) {
    case 0: return s->c_false;
    case 1: return emit2(s, OpFOrdLessThan, s->t_bool, a, b);
    case 2: return emit2(s, OpFOrdEqual, s->t_bool, a, b);
    case 3: return emit2(s, OpFOrdLessThanEqual, s->t_bool, a, b);
    case 4: return emit2(s, OpFOrdGreaterThan, s->t_bool, a, b);
    case 5: return emit2(s, OpFOrdNotEqual, s->t_bool, a, b);
    case 6: return emit2(s, OpFOrdGreaterThanEqual, s->t_bool, a, b);
    case 7: {
        uint32_t na = emit1(s, OpLogicalNot, s->t_bool, emit1(s, OpIsNan, s->t_bool, a));
        uint32_t nb = emit1(s, OpLogicalNot, s->t_bool, emit1(s, OpIsNan, s->t_bool, b));
        return emit2(s, OpLogicalAnd, s->t_bool, na, nb);
    }
    case 8: return emit2(s, OpLogicalOr, s->t_bool, emit1(s, OpIsNan, s->t_bool, a), emit1(s, OpIsNan, s->t_bool, b));
    case 9: return emit2(s, OpFUnordLessThan, s->t_bool, a, b);
    case 10: return emit2(s, OpFUnordEqual, s->t_bool, a, b);
    case 11: return emit2(s, OpFUnordLessThanEqual, s->t_bool, a, b);
    case 12: return emit2(s, OpFUnordGreaterThan, s->t_bool, a, b);
    case 13: return emit2(s, OpFUnordNotEqual, s->t_bool, a, b);
    case 14: return emit2(s, OpFUnordGreaterThanEqual, s->t_bool, a, b);
    default: return s->c_true;
    }
}

static uint32_t emit_icmp(Spv *s, int op, uint32_t a, uint32_t b, bool sgn) {
    if (sgn) { a = u2i(s, a); b = u2i(s, b); }
    switch (op & 7) {
    case 0: return s->c_false;
    case 1: return emit2(s, sgn ? OpSLessThan : OpULessThan, s->t_bool, a, b);
    case 2: return emit2(s, OpIEqual, s->t_bool, a, b);
    case 3: return emit2(s, sgn ? OpSLessThanEqual : OpULessThanEqual, s->t_bool, a, b);
    case 4: return emit2(s, sgn ? OpSGreaterThan : OpUGreaterThan, s->t_bool, a, b);
    case 5: return emit2(s, OpINotEqual, s->t_bool, a, b);
    case 6: return emit2(s, sgn ? OpSGreaterThanEqual : OpUGreaterThanEqual, s->t_bool, a, b);
    default: return s->c_true;
    }
}

// Bit-exact half -> float (matches translate.c h2f: prevents NVIDIA fp16 promotion mismatches in vertex shaders)
static uint32_t emit_h2f(Spv *s, uint32_t h16) {
    if (s->stage == STAGE_FRAGMENT) {
        uint32_t v2 = glsl1(s, GlslUnpackHalf2x16, s->t_v2f, h16);
        return extract(s, s->t_f32, v2, 0);
    }
    uint32_t sgn = emit2(s, OpShiftLeftLogical, s->t_u32, emit2(s, OpBitwiseAnd, s->t_u32, h16, cu(s, 0x8000u)), cu(s, 16));
    uint32_t exp = emit2(s, OpBitwiseAnd, s->t_u32, emit2(s, OpShiftRightLogical, s->t_u32, h16, cu(s, 10)), cu(s, 0x1Fu));
    uint32_t man = emit2(s, OpBitwiseAnd, s->t_u32, h16, cu(s, 0x3FFu));
    // normal: sgn | ((exp + 112) << 23) | (man << 13)
    uint32_t e_norm = emit2(s, OpShiftLeftLogical, s->t_u32, emit2(s, OpIAdd, s->t_u32, exp, cu(s, 112)), cu(s, 23));
    uint32_t m_sh = emit2(s, OpShiftLeftLogical, s->t_u32, man, cu(s, 13));
    uint32_t f_norm = u2f(s, emit2(s, OpBitwiseOr, s->t_u32, emit2(s, OpBitwiseOr, s->t_u32, sgn, e_norm), m_sh));
    // inf/nan: sgn | 0x7F800000 | (man << 13)
    uint32_t f_inf = u2f(s, emit2(s, OpBitwiseOr, s->t_u32, emit2(s, OpBitwiseOr, s->t_u32, sgn, cu(s, 0x7F800000u)), m_sh));
    // subnormal/zero: +/- float(man) * 2^-24
    uint32_t d_pos = emit_f2(s, OpFMul, s->t_f32, emit1(s, OpConvertUToF, s->t_f32, man), cf(s, 5.9604644775390625e-8f));
    uint32_t f_sub = emit3(s, OpSelect, s->t_f32, emit2(s, OpINotEqual, s->t_bool, sgn, cu(s, 0)), emit_f1(s, OpFNegate, s->t_f32, d_pos), d_pos);
    uint32_t r1 = emit3(s, OpSelect, s->t_f32, emit2(s, OpIEqual, s->t_bool, exp, cu(s, 31)), f_inf, f_norm);
    return emit3(s, OpSelect, s->t_f32, emit2(s, OpIEqual, s->t_bool, exp, cu(s, 0)), f_sub, r1);
}

static uint32_t emit_unpack_h2(Spv *s, uint32_t u) {
    if (s->stage == STAGE_FRAGMENT) return glsl1(s, GlslUnpackHalf2x16, s->t_v2f, u);
    uint32_t lo = emit_h2f(s, emit2(s, OpBitwiseAnd, s->t_u32, u, cu(s, 0xFFFFu)));
    uint32_t hi = emit_h2f(s, emit2(s, OpShiftRightLogical, s->t_u32, u, cu(s, 16)));
    return vec2(s, s->t_v2f, lo, hi);
}

static uint32_t emit_hx(Spv *s, uint32_t u, int swz, bool neg, bool abs_) {
    uint32_t v;
    if (swz == 1) { uint32_t f = u2f(s, u); v = vec2(s, s->t_v2f, f, f); }
    else {
        uint32_t h = emit_unpack_h2(s, u);
        if (swz == 2) { uint32_t x = extract(s, s->t_f32, h, 0); v = vec2(s, s->t_v2f, x, x); }
        else if (swz == 3) { uint32_t y = extract(s, s->t_f32, h, 1); v = vec2(s, s->t_v2f, y, y); }
        else v = h;
    }
    if (abs_) v = glsl1(s, GlslFAbs, s->t_v2f, v);
    if (neg) v = emit_f1(s, OpFNegate, s->t_v2f, v);
    return v;
}

static void emit_hstore(Spv *s, int d, uint32_t v2, int merge, bool sat) {
    if (d == RZ) return;
    if (sat) v2 = emit_sat2(s, v2);
    uint32_t res;
    if (merge == 1) {
        res = f2u(s, extract(s, s->t_f32, v2, 0));
    } else if (merge == 0) {
        res = glsl1(s, GlslPackHalf2x16, s->t_u32, v2);
    } else {
        uint32_t old = rd_reg(s, d);
        uint32_t pk = glsl1(s, GlslPackHalf2x16, s->t_u32, v2);
        if (merge == 2) {
            res = emit2(s, OpBitwiseOr, s->t_u32,
                        emit2(s, OpBitwiseAnd, s->t_u32, old, cu(s, 0xFFFF0000u)),
                        emit2(s, OpBitwiseAnd, s->t_u32, pk, cu(s, 0xFFFFu)));
        } else {
            res = emit2(s, OpBitwiseOr, s->t_u32,
                        emit2(s, OpBitwiseAnd, s->t_u32, old, cu(s, 0xFFFFu)),
                        emit2(s, OpBitwiseAnd, s->t_u32, pk, cu(s, 0xFFFF0000u)));
        }
    }
    wr_reg(s, d, res);
}

static uint32_t emit_rscale_v4(Spv *s, int idx) {
    uint32_t ptr = ac2(s, s->p_uni_v4f, s->v_rscale, ci(s, 0), ci(s, idx));
    return load(s, s->t_v4f, ptr);
}

// ------------------------------------------------------------------ Texture helpers
static uint32_t emit_tsize(Spv *s, int slot, uint32_t lod_s32) {
    const ShaderTexture *t = &s->info->tex[slot];
    uint32_t samp = load(s, s->t_samp[slot], s->v_tex[slot]);
    uint32_t img = emit1(s, OpImage, s->t_img[slot], samp);
    switch (t->type) {
    case TEXTYPE_1D: {
        uint32_t w = emit2(s, OpImageQuerySizeLod, s->t_s32, img, lod_s32);
        return vec3(s, s->t_v3i, w, ci(s, 1), ci(s, 1));
    }
    case TEXTYPE_1D_ARRAY: {
        uint32_t wh = emit2(s, OpImageQuerySizeLod, s->t_v2i, img, lod_s32);
        return vec3(s, s->t_v3i, extract(s, s->t_s32, wh, 0), ci(s, 1), extract(s, s->t_s32, wh, 1));
    }
    case TEXTYPE_3D: case TEXTYPE_2D_ARRAY: case TEXTYPE_CUBE_ARRAY:
        return emit2(s, OpImageQuerySizeLod, s->t_v3i, img, lod_s32);
    default: {
        uint32_t wh = emit2(s, OpImageQuerySizeLod, s->t_v2i, img, lod_s32);
        return vec3(s, s->t_v3i, extract(s, s->t_s32, wh, 0), extract(s, s->t_s32, wh, 1), ci(s, 1));
    }
    }
}

static uint32_t emit_rsc2_i2(Spv *s, int slot, uint32_t cx_u, uint32_t cy_u) {
    if (s->stage == STAGE_VERTEX) return vec2(s, s->t_v2i, u2i(s, cx_u), u2i(s, cy_u));
    uint32_t rs = emit_rscale_v4(s, slot + 1);
    uint32_t sx = extract(s, s->t_f32, rs, 0), sy = extract(s, s->t_f32, rs, 1);
    uint32_t is1 = emit2(s, OpLogicalAnd, s->t_bool,
                         emit2(s, OpFOrdEqual, s->t_bool, sx, cf(s, 1.0f)),
                         emit2(s, OpFOrdEqual, s->t_bool, sy, cf(s, 1.0f)));
    uint32_t cx_i = u2i(s, cx_u), cy_i = u2i(s, cy_u);
    uint32_t fx = emit_f2(s, OpFMul, s->t_f32, emit_f2(s, OpFAdd, s->t_f32, emit1(s, OpConvertUToF, s->t_f32, cx_u), cf(s, 0.5f)), sx);
    uint32_t fy = emit_f2(s, OpFMul, s->t_f32, emit_f2(s, OpFAdd, s->t_f32, emit1(s, OpConvertUToF, s->t_f32, cy_u), cf(s, 0.5f)), sy);
    uint32_t scx = u2i(s, emit1(s, OpConvertFToU, s->t_u32, glsl1(s, GlslFloor, s->t_f32, fx)));
    uint32_t scy = u2i(s, emit1(s, OpConvertFToU, s->t_u32, glsl1(s, GlslFloor, s->t_f32, fy)));
    if (s->rsc_pixel && s->stage == STAGE_FRAGMENT) {
        uint32_t rt = emit_rscale_v4(s, 0);
        uint32_t rtx = extract(s, s->t_f32, rt, 0), rty = extract(s, s->t_f32, rt, 1);
        uint32_t rtz = extract(s, s->t_f32, rt, 2), rtw = extract(s, s->t_f32, rt, 3);
        uint32_t fc = load(s, s->t_v4f, s->v_fragcoord);
        uint32_t fcx = emit_f2(s, OpFSub, s->t_f32, extract(s, s->t_f32, fc, 0), rtz);
        uint32_t fcy = emit_f2(s, OpFSub, s->t_f32, extract(s, s->t_f32, fc, 1), rtw);
        uint32_t gx = emit1(s, OpConvertFToS, s->t_s32, glsl1(s, GlslFloor, s->t_f32, emit_f2(s, OpFDiv, s->t_f32, fcx, rtx)));
        uint32_t gy = emit1(s, OpConvertFToS, s->t_s32, glsl1(s, GlslFloor, s->t_f32, emit_f2(s, OpFDiv, s->t_f32, fcy, rty)));
        uint32_t same_s = emit2(s, OpLogicalAnd, s->t_bool,
                                emit2(s, OpFOrdEqual, s->t_bool, sx, rtx),
                                emit2(s, OpFOrdEqual, s->t_bool, sy, rty));
        uint32_t same_c = emit2(s, OpLogicalAnd, s->t_bool,
                                emit2(s, OpIEqual, s->t_bool, cx_i, gx),
                                emit2(s, OpIEqual, s->t_bool, cy_i, gy));
        uint32_t use_fc = emit2(s, OpLogicalAnd, s->t_bool, same_s, same_c);
        uint32_t px = u2i(s, emit1(s, OpConvertFToU, s->t_u32, glsl1(s, GlslFloor, s->t_f32, fcx)));
        uint32_t py = u2i(s, emit1(s, OpConvertFToU, s->t_u32, glsl1(s, GlslFloor, s->t_f32, fcy)));
        scx = emit3(s, OpSelect, s->t_s32, use_fc, px, scx);
        scy = emit3(s, OpSelect, s->t_s32, use_fc, py, scy);
    }
    uint32_t rx = emit3(s, OpSelect, s->t_s32, is1, cx_i, scx);
    uint32_t ry = emit3(s, OpSelect, s->t_s32, is1, cy_i, scy);
    return vec2(s, s->t_v2i, rx, ry);
}

static uint32_t emit_sample(Spv *s, int slot, const uint32_t *co, uint32_t layer_u, uint32_t dref_f,
                            int lod_kind, uint32_t lod_f, uint32_t off_x_i, uint32_t off_y_i, bool has_off) {
    const ShaderTexture *t = &s->info->tex[slot];
    int type = t->type;
    bool cube = type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY;
    int n = (type == TEXTYPE_1D || type == TEXTYPE_1D_ARRAY) ? 1 : (type == TEXTYPE_2D || type == TEXTYPE_2D_ARRAY) ? 2 : 3;
    if (s->stage != STAGE_FRAGMENT && lod_kind != 1) {
        if (lod_kind == 0) lod_f = cf(s, 0.0f);
        lod_kind = 1;
    }
    if (t->shadow && dref_f && s->d16_ref && s->stage == STAGE_FRAGMENT) {
        uint32_t rs = emit_rscale_v4(s, slot + 1);
        uint32_t is_d16 = emit2(s, OpFOrdGreaterThan, s->t_bool, extract(s, s->t_f32, rs, 2), cf(s, 0.5f));
        uint32_t cl = glsl3(s, GlslFClamp, s->t_f32, dref_f, cf(s, 0.0f), cf(s, 1.0f));
        uint32_t q = emit_f2(s, OpFDiv, s->t_f32,
                             glsl1(s, GlslFloor, s->t_f32, emit_f2(s, OpFAdd, s->t_f32, emit_f2(s, OpFMul, s->t_f32, cl, cf(s, 65535.0f)), cf(s, 0.5f))),
                             cf(s, 65535.0f));
        dref_f = emit3(s, OpSelect, s->t_f32, is_d16, q, dref_f);
    }
    uint32_t c0 = co[0], c1 = n >= 2 ? co[1] : 0, c2 = n >= 3 ? co[2] : 0;
    if (has_off && !cube) {
        uint32_t lod_i = lod_kind == 1 ? emit1(s, OpConvertFToS, s->t_s32, lod_f) : ci(s, 0);
        uint32_t ts = emit_tsize(s, slot, lod_i);
        c0 = emit_f2(s, OpFAdd, s->t_f32, c0, emit_f2(s, OpFDiv, s->t_f32, emit1(s, OpConvertSToF, s->t_f32, off_x_i),
                                                      emit1(s, OpConvertSToF, s->t_f32, extract(s, s->t_s32, ts, 0))));
        if (n >= 2)
            c1 = emit_f2(s, OpFAdd, s->t_f32, c1, emit_f2(s, OpFDiv, s->t_f32, emit1(s, OpConvertSToF, s->t_f32, off_y_i),
                                                          emit1(s, OpConvertSToF, s->t_f32, extract(s, s->t_s32, ts, 1))));
    }
    uint32_t coord;
    if (!layer_u) {
        coord = n == 1 ? c0 : n == 2 ? vec2(s, s->t_v2f, c0, c1) : vec3(s, s->t_v3f, c0, c1, c2);
    } else {
        uint32_t lf = emit1(s, OpConvertUToF, s->t_f32, layer_u);
        coord = n == 1 ? vec2(s, s->t_v2f, c0, lf) : n == 2 ? vec3(s, s->t_v3f, c0, c1, lf) : vec4(s, s->t_v4f, c0, c1, c2, lf);
    }
    uint32_t samp = load(s, s->t_samp[slot], s->v_tex[slot]);
    if (t->shadow) {
        uint32_t res = new_id(s);
        if (lod_kind == 0) {
            wb_op(&s->sec_fn, OpImageSampleDrefImplicitLod, 6);
            wb_push(&s->sec_fn, s->t_f32); wb_push(&s->sec_fn, res); wb_push(&s->sec_fn, samp); wb_push(&s->sec_fn, coord); wb_push(&s->sec_fn, dref_f);
        } else if (lod_kind == 1) {
            wb_op(&s->sec_fn, OpImageSampleDrefExplicitLod, 8);
            wb_push(&s->sec_fn, s->t_f32); wb_push(&s->sec_fn, res); wb_push(&s->sec_fn, samp); wb_push(&s->sec_fn, coord); wb_push(&s->sec_fn, dref_f);
            wb_push(&s->sec_fn, ImgOpLod); wb_push(&s->sec_fn, lod_f);
        } else {
            wb_op(&s->sec_fn, OpImageSampleDrefImplicitLod, 8);
            wb_push(&s->sec_fn, s->t_f32); wb_push(&s->sec_fn, res); wb_push(&s->sec_fn, samp); wb_push(&s->sec_fn, coord); wb_push(&s->sec_fn, dref_f);
            wb_push(&s->sec_fn, ImgOpBias); wb_push(&s->sec_fn, lod_f);
        }
        return vec4(s, s->t_v4f, res, res, res, res);
    }
    uint32_t rty = t->is_uint ? s->t_v4u : s->t_v4f;
    uint32_t res = new_id(s);
    if (lod_kind == 0) {
        wb_op(&s->sec_fn, OpImageSampleImplicitLod, 5);
        wb_push(&s->sec_fn, rty); wb_push(&s->sec_fn, res); wb_push(&s->sec_fn, samp); wb_push(&s->sec_fn, coord);
    } else if (lod_kind == 1) {
        wb_op(&s->sec_fn, OpImageSampleExplicitLod, 7);
        wb_push(&s->sec_fn, rty); wb_push(&s->sec_fn, res); wb_push(&s->sec_fn, samp); wb_push(&s->sec_fn, coord);
        wb_push(&s->sec_fn, ImgOpLod); wb_push(&s->sec_fn, lod_f);
    } else {
        wb_op(&s->sec_fn, OpImageSampleImplicitLod, 7);
        wb_push(&s->sec_fn, rty); wb_push(&s->sec_fn, res); wb_push(&s->sec_fn, samp); wb_push(&s->sec_fn, coord);
        wb_push(&s->sec_fn, ImgOpBias); wb_push(&s->sec_fn, lod_f);
    }
    return t->is_uint ? emit1(s, OpBitcast, s->t_v4f, res) : res;
}

static uint32_t emit_fetch(Spv *s, int slot, uint32_t coord_i, uint32_t lod_i) {
    const ShaderTexture *t = &s->info->tex[slot];
    uint32_t samp = load(s, s->t_samp[slot], s->v_tex[slot]);
    uint32_t img = emit1(s, OpImage, s->t_img[slot], samp);
    uint32_t rty = t->is_uint ? s->t_v4u : s->t_v4f;
    uint32_t res = new_id(s);
    wb_op(&s->sec_fn, OpImageFetch, 7);
    wb_push(&s->sec_fn, rty); wb_push(&s->sec_fn, res); wb_push(&s->sec_fn, img); wb_push(&s->sec_fn, coord_i);
    wb_push(&s->sec_fn, ImgOpLod); wb_push(&s->sec_fn, lod_i);
    return t->is_uint ? emit1(s, OpBitcast, s->t_v4f, res) : res;
}

static void emit_tex_write(Spv *s, uint32_t v4f, uint32_t mask, int d0, int d1, bool texs, bool f16) {
    if (f16) {
        uint32_t vals[4]; int n = 0;
        for (int c = 0; c < 4; c++) if (mask & (1u << c)) vals[n++] = extract(s, s->t_f32, v4f, (uint32_t)c);
        while (n < 4) vals[n++] = cf(s, 0.0f);
        if (d0 != RZ) wr_reg(s, d0, glsl1(s, GlslPackHalf2x16, s->t_u32, vec2(s, s->t_v2f, vals[0], vals[1])));
        if (d1 != RZ) wr_reg(s, d1, glsl1(s, GlslPackHalf2x16, s->t_u32, vec2(s, s->t_v2f, vals[2], vals[3])));
        return;
    }
    int regs[4], n = 0;
    if (texs) {
        regs[0] = d0; regs[1] = d0 == RZ ? RZ : d0 + 1;
        regs[2] = d1; regs[3] = d1 == RZ ? RZ : d1 + 1;
    } else {
        for (int i = 0; i < 4; i++) regs[i] = d0 == RZ ? RZ : d0 + i;
    }
    for (int c = 0; c < 4; c++) {
        if (!(mask & (1u << c))) continue;
        int r = regs[n++];
        if (r != RZ) wr_reg(s, r, f2u(s, extract(s, s->t_f32, v4f, (uint32_t)c)));
    }
}

static uint32_t emit_alayer(Spv *s, uint32_t r_u, int slot) {
    uint32_t ts = emit_tsize(s, slot, ci(s, 0));
    uint32_t layers = i2u(s, extract(s, s->t_s32, ts, 2));
    uint32_t maxl = emit2(s, OpISub, s->t_u32, layers, cu(s, 1));
    return glsl2(s, GlslUMin, s->t_u32, r_u, maxl);
}

// Storage image coordinate scaling (matches translate.c isc() for SULD / SUST / SUATOM)
static uint32_t emit_img_coord_i2(Spv *s, int slot, uint32_t cx_u, uint32_t cy_u) {
    uint32_t cx_i = u2i(s, cx_u), cy_i = u2i(s, cy_u);
    if (s->stage == STAGE_VERTEX) return vec2(s, s->t_v2i, cx_i, cy_i);
    uint32_t rs = emit_rscale_v4(s, slot + 1);
    uint32_t sx = extract(s, s->t_f32, rs, 0), sy = extract(s, s->t_f32, rs, 1);
    uint32_t is1 = emit2(s, OpLogicalAnd, s->t_bool,
                         emit2(s, OpFOrdEqual, s->t_bool, sx, cf(s, 1.0f)),
                         emit2(s, OpFOrdEqual, s->t_bool, sy, cf(s, 1.0f)));
    uint32_t fx = emit_f2(s, OpFMul, s->t_f32, emit_f2(s, OpFAdd, s->t_f32, emit1(s, OpConvertSToF, s->t_f32, cx_i), cf(s, 0.5f)), sx);
    uint32_t fy = emit_f2(s, OpFMul, s->t_f32, emit_f2(s, OpFAdd, s->t_f32, emit1(s, OpConvertSToF, s->t_f32, cy_i), cf(s, 0.5f)), sy);
    uint32_t scx = emit1(s, OpConvertFToS, s->t_s32, glsl1(s, GlslFloor, s->t_f32, fx));
    uint32_t scy = emit1(s, OpConvertFToS, s->t_s32, glsl1(s, GlslFloor, s->t_f32, fy));
    uint32_t rt = emit_rscale_v4(s, 0);
    uint32_t rtx = extract(s, s->t_f32, rt, 0), rty = extract(s, s->t_f32, rt, 1);
    uint32_t rtz = extract(s, s->t_f32, rt, 2), rtw = extract(s, s->t_f32, rt, 3);
    uint32_t fc = load(s, s->t_v4f, s->v_fragcoord);
    uint32_t fcx = emit_f2(s, OpFSub, s->t_f32, extract(s, s->t_f32, fc, 0), rtz);
    uint32_t fcy = emit_f2(s, OpFSub, s->t_f32, extract(s, s->t_f32, fc, 1), rtw);
    uint32_t gx = emit1(s, OpConvertFToS, s->t_s32, glsl1(s, GlslFloor, s->t_f32, emit_f2(s, OpFDiv, s->t_f32, fcx, rtx)));
    uint32_t gy = emit1(s, OpConvertFToS, s->t_s32, glsl1(s, GlslFloor, s->t_f32, emit_f2(s, OpFDiv, s->t_f32, fcy, rty)));
    uint32_t same_s = emit2(s, OpLogicalAnd, s->t_bool,
                            emit2(s, OpFOrdEqual, s->t_bool, sx, rtx),
                            emit2(s, OpFOrdEqual, s->t_bool, sy, rty));
    uint32_t same_c = emit2(s, OpLogicalAnd, s->t_bool,
                            emit2(s, OpIEqual, s->t_bool, cx_i, gx),
                            emit2(s, OpIEqual, s->t_bool, cy_i, gy));
    uint32_t use_fc = emit2(s, OpLogicalAnd, s->t_bool, same_s, same_c);
    uint32_t px = emit1(s, OpConvertFToS, s->t_s32, glsl1(s, GlslFloor, s->t_f32, fcx));
    uint32_t py = emit1(s, OpConvertFToS, s->t_s32, glsl1(s, GlslFloor, s->t_f32, fcy));
    scx = emit3(s, OpSelect, s->t_s32, use_fc, px, scx);
    scy = emit3(s, OpSelect, s->t_s32, use_fc, py, scy);
    uint32_t rx = emit3(s, OpSelect, s->t_s32, is1, cx_i, scx);
    uint32_t ry = emit3(s, OpSelect, s->t_s32, is1, cy_i, scy);
    return vec2(s, s->t_v2i, rx, ry);
}

// Global memory address resolution (matches translate.c gmem_access / gw / gst / gat)
static void emit_gmem_addr(Spv *s, int a, bool wide, int64_t off, bool atom) {
    uint32_t ad;
    if (a == RZ) {
        ad = cu64(s, (uint64_t)off);
    } else if (wide) {
        uint32_t lo = emit1(s, OpUConvert, s->t_u64, rd_reg(s, a));
        uint32_t hi = emit2(s, OpShiftLeftLogical, s->t_u64, emit1(s, OpUConvert, s->t_u64, rd_reg(s, a + 1)), cu64(s, 32));
        ad = emit2(s, OpBitwiseOr, s->t_u64, hi, lo);
        if (off) ad = emit2(s, OpIAdd, s->t_u64, ad, cu64(s, (uint64_t)off));
    } else {
        ad = emit1(s, OpUConvert, s->t_u64, rd_reg(s, a));
        if (off) ad = emit2(s, OpIAdd, s->t_u64, ad, cu64(s, (uint64_t)off));
    }
    uint32_t ga = emit2(s, OpISub, s->t_u64, ad, cu64(s, maxwell_gmem_base));
    uint32_t ok = emit2(s, OpULessThan, s->t_bool, ga, cu64(s, maxwell_gmem_size));
    store(s, s->v_ga, ga);
    store(s, s->v_gok, ok);
    if (s->uses_pmem) {
        store(s, s->v_gpm, s->c_false);
        store(s, s->v_gph, s->c_false);
        uint32_t poff = emit2(s, OpISub, s->t_u64, ad, cu64(s, maxwell_pmem_base));
        uint32_t in_pm = emit2(s, OpULessThan, s->t_bool, poff, cu64(s, maxwell_pmem_size));
        uint32_t try_pm = emit2(s, OpLogicalAnd, s->t_bool, emit1(s, OpLogicalNot, s->t_bool, ok), in_pm);
        uint32_t lbl_do = new_id(s), lbl_done = new_id(s);
        emit_sel_merge(s, lbl_done);
        emit_cond_branch(s, try_pm, lbl_do, lbl_done);
        emit_label(s, lbl_do);
        uint32_t pti = emit1(s, OpUConvert, s->t_u32, emit2(s, OpShiftRightLogical, s->t_u64, poff, cu64(s, 16)));
        uint32_t pe = load(s, s->t_u32, ac2(s, s->p_sb_u32, s->v_ptab, ci(s, 0), pti));
        uint32_t mapped = emit2(s, OpINotEqual, s->t_bool, pe, cu(s, 0));
        uint32_t lbl_map = new_id(s), lbl_mdone = new_id(s);
        emit_sel_merge(s, lbl_mdone);
        emit_cond_branch(s, mapped, lbl_map, lbl_mdone);
        emit_label(s, lbl_map);
        uint32_t ph = emit2(s, OpUGreaterThan, s->t_bool, pe, cu(s, PMEM_LO_PAGES));
        uint32_t sub = emit3(s, OpSelect, s->t_u32, ph, cu(s, PMEM_LO_PAGES), cu(s, 0));
        uint32_t pg = emit2(s, OpISub, s->t_u32, emit2(s, OpISub, s->t_u32, pe, cu(s, 1)), sub);
        uint32_t pga = emit2(s, OpBitwiseOr, s->t_u64,
                             emit2(s, OpShiftLeftLogical, s->t_u64, emit1(s, OpUConvert, s->t_u64, pg), cu64(s, 16)),
                             emit2(s, OpBitwiseAnd, s->t_u64, ad, cu64(s, 0xFFFF)));
        store(s, s->v_gph, ph);
        store(s, s->v_ga, pga);
        store(s, s->v_gok, s->c_true);
        store(s, s->v_gpm, s->c_true);
        emit_branch(s, lbl_mdone);
        emit_label(s, lbl_mdone);
        emit_branch(s, lbl_done);
        emit_label(s, lbl_done);
    }
    if (atom) {
        uint32_t cur_ga = load(s, s->t_u64, s->v_ga);
        uint32_t al = emit2(s, OpIEqual, s->t_bool, emit2(s, OpBitwiseAnd, s->t_u64, cur_ga, cu64(s, 3)), cu64(s, 0));
        store(s, s->v_gok, emit2(s, OpLogicalAnd, s->t_bool, load(s, s->t_bool, s->v_gok), al));
    }
}

static uint32_t emit_gmem_ldw(Spv *s, uint32_t wi) {
    if (!s->uses_pmem) return load(s, s->t_u32, ac2(s, s->p_sb_u32, s->v_gmem, ci(s, 0), wi));
    uint32_t pm = load(s, s->t_bool, s->v_gpm);
    uint32_t lbl_pm = new_id(s), lbl_gm = new_id(s), lbl_done = new_id(s);
    emit_sel_merge(s, lbl_done);
    emit_cond_branch(s, pm, lbl_pm, lbl_gm);
    emit_label(s, lbl_gm);
    store(s, s->v_gtmp, load(s, s->t_u32, ac2(s, s->p_sb_u32, s->v_gmem, ci(s, 0), wi)));
    emit_branch(s, lbl_done);
    emit_label(s, lbl_pm);
    uint32_t ph = load(s, s->t_bool, s->v_gph);
    uint32_t lbl_hi = new_id(s), lbl_lo = new_id(s), lbl_pm_end = new_id(s);
    emit_sel_merge(s, lbl_pm_end);
    emit_cond_branch(s, ph, lbl_hi, lbl_lo);
    emit_label(s, lbl_hi);
    store(s, s->v_gtmp, load(s, s->t_u32, ac2(s, s->p_sb_u32, s->v_pmemh, ci(s, 0), wi)));
    emit_branch(s, lbl_pm_end);
    emit_label(s, lbl_lo);
    store(s, s->v_gtmp, load(s, s->t_u32, ac2(s, s->p_sb_u32, s->v_pmem, ci(s, 0), wi)));
    emit_branch(s, lbl_pm_end);
    emit_label(s, lbl_pm_end);
    emit_branch(s, lbl_done);
    emit_label(s, lbl_done);
    return load(s, s->t_u32, s->v_gtmp);
}

static void emit_gmem_stw(Spv *s, uint32_t wi, uint32_t val) {
    if (!s->uses_pmem) { store(s, ac2(s, s->p_sb_u32, s->v_gmem, ci(s, 0), wi), val); return; }
    uint32_t pm = load(s, s->t_bool, s->v_gpm);
    uint32_t lbl_pm = new_id(s), lbl_gm = new_id(s), lbl_done = new_id(s);
    emit_sel_merge(s, lbl_done);
    emit_cond_branch(s, pm, lbl_pm, lbl_gm);
    emit_label(s, lbl_gm);
    store(s, ac2(s, s->p_sb_u32, s->v_gmem, ci(s, 0), wi), val);
    emit_branch(s, lbl_done);
    emit_label(s, lbl_pm);
    uint32_t ph = load(s, s->t_bool, s->v_gph);
    uint32_t lbl_hi = new_id(s), lbl_lo = new_id(s), lbl_pm_end = new_id(s);
    emit_sel_merge(s, lbl_pm_end);
    emit_cond_branch(s, ph, lbl_hi, lbl_lo);
    emit_label(s, lbl_hi);
    store(s, ac2(s, s->p_sb_u32, s->v_pmemh, ci(s, 0), wi), val);
    emit_branch(s, lbl_pm_end);
    emit_label(s, lbl_lo);
    store(s, ac2(s, s->p_sb_u32, s->v_pmem, ci(s, 0), wi), val);
    emit_branch(s, lbl_pm_end);
    emit_label(s, lbl_pm_end);
    emit_branch(s, lbl_done);
    emit_label(s, lbl_done);
}

static uint32_t emit_atomic_ptr(Spv *s, uint32_t spv_op, uint32_t ptr, uint32_t val) {
    uint32_t sc = cu(s, ScopeDevice), sem = cu(s, 0);
    uint32_t id = new_id(s);
    wb_op(&s->sec_fn, spv_op, 7);
    wb_push(&s->sec_fn, s->t_u32); wb_push(&s->sec_fn, id);
    wb_push(&s->sec_fn, ptr); wb_push(&s->sec_fn, sc); wb_push(&s->sec_fn, sem); wb_push(&s->sec_fn, val);
    return id;
}

static uint32_t emit_gmem_atomw(Spv *s, uint32_t spv_op, uint32_t wi, uint32_t val) {
    if (!s->uses_pmem) return emit_atomic_ptr(s, spv_op, ac2(s, s->p_sb_u32, s->v_gmem, ci(s, 0), wi), val);
    uint32_t pm = load(s, s->t_bool, s->v_gpm);
    uint32_t lbl_pm = new_id(s), lbl_gm = new_id(s), lbl_done = new_id(s);
    emit_sel_merge(s, lbl_done);
    emit_cond_branch(s, pm, lbl_pm, lbl_gm);
    emit_label(s, lbl_gm);
    store(s, s->v_gtmp, emit_atomic_ptr(s, spv_op, ac2(s, s->p_sb_u32, s->v_gmem, ci(s, 0), wi), val));
    emit_branch(s, lbl_done);
    emit_label(s, lbl_pm);
    uint32_t ph = load(s, s->t_bool, s->v_gph);
    uint32_t lbl_hi = new_id(s), lbl_lo = new_id(s), lbl_pm_end = new_id(s);
    emit_sel_merge(s, lbl_pm_end);
    emit_cond_branch(s, ph, lbl_hi, lbl_lo);
    emit_label(s, lbl_hi);
    store(s, s->v_gtmp, emit_atomic_ptr(s, spv_op, ac2(s, s->p_sb_u32, s->v_pmemh, ci(s, 0), wi), val));
    emit_branch(s, lbl_pm_end);
    emit_label(s, lbl_lo);
    store(s, s->v_gtmp, emit_atomic_ptr(s, spv_op, ac2(s, s->p_sb_u32, s->v_pmem, ci(s, 0), wi), val));
    emit_branch(s, lbl_pm_end);
    emit_label(s, lbl_pm_end);
    emit_branch(s, lbl_done);
    emit_label(s, lbl_done);
    return load(s, s->t_u32, s->v_gtmp);
}

// ------------------------------------------------------------------ Instruction emitter
static uint32_t emit_video_part(Spv *s, uint32_t v, int sel) {
    int s7 = sel & 7; bool sg = (sel & 8) != 0;
    if (s7 < 4) {
        uint32_t sh = s7 ? emit2(s, OpShiftRightLogical, s->t_u32, v, cu(s, (uint32_t)(s7 * 8))) : v;
        return sg ? i2u(s, emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, sh), ci(s, 0), ci(s, 8)))
                  : emit2(s, OpBitwiseAnd, s->t_u32, sh, cu(s, 0xFFu));
    } else if (s7 < 6) {
        uint32_t sh = s7 == 5 ? emit2(s, OpShiftRightLogical, s->t_u32, v, cu(s, 16)) : v;
        return sg ? i2u(s, emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, sh), ci(s, 0), ci(s, 16)))
                  : emit2(s, OpBitwiseAnd, s->t_u32, sh, cu(s, 0xFFFFu));
    }
    return v;
}

static uint32_t emit_himm(uint64_t w) {
    uint32_t lo = ((uint32_t)bits(w, 20, 9) << 6) | ((uint32_t)bits(w, 29, 1) << 15);
    uint32_t hi = ((uint32_t)bits(w, 30, 9) << 6) | ((uint32_t)bits(w, 56, 1) << 15);
    return lo | (hi << 16);
}

static bool emit_half_spv(Spv *s, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    uint16_t op = (uint16_t)(w >> 48);
    int kind = -1, form = -1;
    if ((op & 0xfff8) == 0x5d10) { kind = 0; form = 0; }
    else if ((op & 0xfff8) == 0x5d08) { kind = 1; form = 0; }
    else if ((op & 0xfe80) == 0x7a80) { kind = 0; form = 1; }
    else if ((op & 0xfe80) == 0x7a00) { kind = 0; form = 2; }
    else if ((op & 0xfe80) == 0x7880) { kind = 1; form = 1; }
    else if ((op & 0xfe80) == 0x7800) { kind = 1; form = 2; }
    else if ((op & 0xfe00) == 0x2c00) { kind = 0; form = 3; }
    else if ((op & 0xfe00) == 0x2a00) { kind = 1; form = 3; }
    if (kind >= 0) {
        int merge, swa, swb = 0;
        bool sat, nega, absa = false, negb = false, absb = false;
        uint32_t bv;
        if (form == 3) {
            merge = 0; sat = bits(w, 52, 1); swa = (int)bits(w, 53, 2); nega = kind == 0 ? bits(w, 56, 1) : false;
            bv = cu(s, (uint32_t)bits(w, 20, 32));
        } else {
            merge = (int)bits(w, 49, 2); swa = (int)bits(w, 47, 2); nega = bits(w, 43, 1); absa = bits(w, 44, 1);
            if (form == 0) {
                sat = bits(w, 32, 1); absb = bits(w, 30, 1); swb = (int)bits(w, 28, 2);
                if (kind == 0) negb = bits(w, 31, 1);
                else { nega = bits(w, 31, 1); negb = false; }
                bv = rd_reg(s, (int)bits(w, 20, 8));
            } else if (form == 1) {
                sat = bits(w, 52, 1); negb = kind == 0 ? bits(w, 56, 1) : false; absb = bits(w, 54, 1); swb = 1;
                bv = emit_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, 0);
            } else {
                sat = bits(w, 52, 1); swb = 0; bv = cu(s, emit_himm(w));
            }
        }
        uint32_t xa = emit_hx(s, rd_reg(s, a), swa, nega, absa);
        uint32_t xb = emit_hx(s, bv, swb, negb, absb);
        uint32_t r = emit_f2(s, kind == 0 ? OpFAdd : OpFMul, s->t_v2f, xa, xb);
        emit_hstore(s, d, r, merge, sat);
        return true;
    }
    form = -1;
    if ((op & 0xfff8) == 0x5d00) form = 0;
    else if ((op & 0xf880) == 0x7080) form = 1;
    else if ((op & 0xf880) == 0x6080) form = 2;
    else if ((op & 0xf880) == 0x7000) form = 3;
    else if ((op & 0xfe00) == 0x2800) form = 4;
    if (form >= 0) {
        int merge, swa = (int)bits(w, 47, 2), swb = 0, swc = 0;
        bool sat, negb, negc;
        uint32_t bv, cv;
        switch (form) {
        case 0:
            merge = (int)bits(w, 49, 2); sat = bits(w, 32, 1); negb = bits(w, 31, 1); negc = bits(w, 30, 1);
            swb = (int)bits(w, 28, 2); swc = (int)bits(w, 35, 2);
            bv = rd_reg(s, (int)bits(w, 20, 8)); cv = rd_reg(s, (int)bits(w, 39, 8)); break;
        case 1:
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = bits(w, 56, 1); negc = bits(w, 51, 1);
            swb = 1; swc = (int)bits(w, 53, 2);
            bv = emit_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, 0); cv = rd_reg(s, (int)bits(w, 39, 8)); break;
        case 2:
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = bits(w, 56, 1); negc = bits(w, 51, 1);
            swb = (int)bits(w, 53, 2); swc = 1;
            bv = rd_reg(s, (int)bits(w, 39, 8)); cv = emit_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, 0); break;
        case 3:
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = false; negc = bits(w, 51, 1);
            swb = 0; swc = (int)bits(w, 53, 2);
            bv = cu(s, emit_himm(w)); cv = rd_reg(s, (int)bits(w, 39, 8)); break;
        default:
            merge = 0; sat = false; negb = false; negc = bits(w, 51, 1);
            swa = (int)bits(w, 47, 2); swb = 0; swc = 0;
            bv = cu(s, (uint32_t)bits(w, 20, 32)); cv = rd_reg(s, d); break;
        }
        uint32_t xa = emit_hx(s, rd_reg(s, a), swa, false, false);
        uint32_t xb = emit_hx(s, bv, swb, negb, false);
        uint32_t xc = emit_hx(s, cv, swc, negc, false);
        uint32_t r = glsl3(s, GlslFma, s->t_v2f, xa, xb, xc);
        emit_hstore(s, d, r, merge, sat);
        return true;
    }
    if ((op & 0xfff8) == 0x5d20 || (op & 0xfff8) == 0x5d18) {
        bool setp = (op & 0xfff8) == 0x5d20;
        uint32_t x = emit_hx(s, rd_reg(s, a), (int)bits(w, 47, 2), bits(w, 43, 1), bits(w, 44, 1));
        uint32_t y = emit_hx(s, rd_reg(s, (int)bits(w, 20, 8)), (int)bits(w, 28, 2), bits(w, 31, 1), bits(w, 30, 1));
        int cop = (int)bits(w, 35, 4), bop = (int)bits(w, 45, 2);
        uint32_t pp = rd_pred(s, (int)bits(w, 39, 3), bits(w, 42, 1));
        uint32_t l = emit_fcmp(s, cop, extract(s, s->t_f32, x, 0), extract(s, s->t_f32, y, 0));
        uint32_t h = emit_fcmp(s, cop, extract(s, s->t_f32, x, 1), extract(s, s->t_f32, y, 1));
        if (setp) {
            int pa = (int)bits(w, 3, 3), pb = (int)bits(w, 0, 3);
            if (bits(w, 49, 1)) { l = emit2(s, OpLogicalAnd, s->t_bool, l, h); h = emit1(s, OpLogicalNot, s->t_bool, l); }
            wr_pred(s, pa, emit_bool_op(s, bop, l, pp));
            wr_pred(s, pb, emit_bool_op(s, bop, h, pp));
        } else if (d != RZ) {
            bool bf = bits(w, 49, 1);
            l = emit_bool_op(s, bop, l, pp); h = emit_bool_op(s, bop, h, pp);
            uint32_t lo = emit3(s, OpSelect, s->t_u32, l, cu(s, bf ? 0x3c00u : 0xFFFFu), cu(s, 0));
            uint32_t hi = emit3(s, OpSelect, s->t_u32, h, cu(s, bf ? 0x3c000000u : 0xFFFF0000u), cu(s, 0));
            wr_reg(s, d, emit2(s, OpBitwiseOr, s->t_u32, lo, hi));
        }
        return true;
    }
    return false;
}

static void emit_attr_load_spv(Spv *s, int dst, uint32_t addr, int ipa_mode, int ipa_mul) {
    if (dst == RZ) return;
    uint32_t val = cu(s, 0);
    if (s->stage == STAGE_VERTEX) {
        if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            int kind = s->key ? s->key->attr_kind[a] : 0;
            uint32_t vty = kind == 1 ? s->t_v4i : kind == 2 ? s->t_v4u : s->t_v4f;
            uint32_t sty = kind == 1 ? s->t_s32 : kind == 2 ? s->t_u32 : s->t_f32;
            uint32_t c = extract(s, sty, load(s, vty, s->v_in[a]), (uint32_t)comp);
            val = kind == 2 ? c : emit1(s, OpBitcast, s->t_u32, c);
        } else if (addr == 0x2F8) {
            val = i2u(s, load(s, s->t_s32, s->v_iid));
        } else if (addr == 0x2FC) {
            val = i2u(s, load(s, s->t_s32, s->v_vid));
        }
    } else if (s->stage == STAGE_FRAGMENT) {
        if (addr >= 0x70 && addr < 0x80) {
            int comp = (addr >> 2) & 3;
            uint32_t fc = load(s, s->t_v4f, s->v_fragcoord);
            uint32_t f = extract(s, s->t_f32, fc, (uint32_t)comp);
            if (comp == 0 && s->key && s->key->flip_x) {
                uint32_t r0 = emit_rscale_v4(s, 0), r65 = emit_rscale_v4(s, 65);
                uint32_t gx = emit_f2(s, OpFDiv, s->t_f32, emit_f2(s, OpFSub, s->t_f32, f, extract(s, s->t_f32, r0, 2)), extract(s, s->t_f32, r0, 0));
                f = emit_f2(s, OpFSub, s->t_f32, extract(s, s->t_f32, r65, 0), gx);
            } else if (comp < 2) {
                uint32_t r0 = emit_rscale_v4(s, 0);
                f = emit_f2(s, OpFDiv, s->t_f32, emit_f2(s, OpFSub, s->t_f32, f, extract(s, s->t_f32, r0, (uint32_t)(comp + 2))),
                            extract(s, s->t_f32, r0, (uint32_t)comp));
            }
            val = f2u(s, f);
        } else if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            if (s->key && !(s->key->vs_out_mask & (1u << a))) {
                val = cu(s, comp == 3 ? 0x3f800000u : 0u);
            } else {
                uint32_t f = extract(s, s->t_f32, load(s, s->t_v4f, s->v_in[a]), (uint32_t)comp);
                if (ipa_mode == 0) {
                    uint32_t w = extract(s, s->t_f32, load(s, s->t_v4f, s->v_fragcoord), 3);
                    f = emit_f2(s, OpFMul, s->t_f32, f, w);
                } else if (ipa_mode == 1 && ipa_mul != RZ) {
                    uint32_t w = extract(s, s->t_f32, load(s, s->t_v4f, s->v_fragcoord), 3);
                    f = emit_f2(s, OpFMul, s->t_f32, emit_f2(s, OpFMul, s->t_f32, f, w), u2f(s, rd_reg(s, ipa_mul)));
                }
                val = f2u(s, f);
            }
        } else if (addr == 0x3FC) {
            val = emit3(s, OpSelect, s->t_u32, load(s, s->t_bool, s->v_frontfacing), cu(s, 0xFFFFFFFFu), cu(s, 0));
        } else if (addr == 0x2E0 || addr == 0x2E4) {
            uint32_t pc = load(s, s->t_v2f, s->v_pointcoord);
            val = f2u(s, extract(s, s->t_f32, pc, (addr >> 2) & 1));
        }
    }
    wr_reg(s, dst, val);
}

static void emit_attr_store_spv(Spv *s, int src, uint32_t addr) {
    if (s->stage != STAGE_VERTEX) return;
    uint32_t f = u2f(s, rd_reg(s, src));
    if (addr >= 0x70 && addr < 0x80) {
        uint32_t ptr = ac2(s, s->p_out_f32, s->v_pervertex, ci(s, 0), ci(s, (int32_t)((addr >> 2) & 3)));
        store(s, ptr, f);
    } else if (addr >= 0x80 && addr < 0x280) {
        int a = (addr - 0x80) >> 4;
        uint32_t ptr = ac1(s, s->p_out_f32, s->v_out[a], ci(s, (int32_t)((addr >> 2) & 3)));
        store(s, ptr, f);
    } else if (addr == 0x6C) {
        uint32_t ptr = ac1(s, s->p_out_f32, s->v_pervertex, ci(s, 1));
        store(s, ptr, f);
    }
}

static void emit_insn_spv(Spv *s, uint64_t w, uint32_t pc) {
    if (s->blk_terminated) return;
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    int pr = (int)bits(w, 16, 3);
    bool prn = bits(w, 19, 1);
    uint64_t top12 = w & 0xfff0000000000000ull;
    if (top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull || top12 == 0xe2b0000000000000ull ||
        top12 == 0xe270000000000000ull) { pr = PT; prn = false; }
    if (pr == PT && prn) return;
    bool guarded = !(pr == PT && !prn);
    uint32_t lbl_after = 0;
    if (guarded) {
        uint32_t cond = rd_pred(s, pr, prn);
        uint32_t lbl_then = new_id(s);
        lbl_after = new_id(s);
        emit_sel_merge(s, lbl_after);
        emit_cond_branch(s, cond, lbl_then, lbl_after);
        emit_label(s, lbl_then);
    }

    if (OPM(w, 0xe300000000000000ull, 0xfff0000000000000ull)) {  // EXIT
        emit_branch(s, s->lbl_epilogue);
    } else if (OPM(w, 0xe330000000000000ull, 0xfff0000000000000ull)) {  // KIL
        if (!s->no_discard && s->stage == STAGE_FRAGMENT) { wb_op(&s->sec_fn, OpKill, 1); s->blk_terminated = true; }
    } else if (OPM(w, 0xe240000000000000ull, 0xfff0000000000020ull)) {  // BRA
        uint32_t target = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
        store(s, s->v_pc, cu(s, target));
        emit_branch(s, s->lbl_loop_cont);
    } else if (OPM(w, 0xe290000000000000ull, 0xfff0000000000020ull) ||
               OPM(w, 0xe2a0000000000000ull, 0xfff0000000000020ull) ||
               OPM(w, 0xe2b0000000000000ull, 0xfff0000000000020ull)) {
        // SSY / PBK / PCNT (statically resolved)
    } else if (OPM(w, 0xf0f8000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0xe340000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0xe350000000000000ull, 0xfff0000000000000ull)) {  // SYNC / BRK / CONT
        uint32_t target = s->stk_tgt ? s->stk_tgt[pc / 8] : 0;
        if (target) { store(s, s->v_pc, cu(s, target)); emit_branch(s, s->lbl_loop_cont); }
        else emit_branch(s, s->lbl_epilogue);  // unreachable path
    } else if (OPM(w, 0xf0f0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x50b0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0xef98000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0xf0a8000000000000ull, 0xfff8000000000000ull)) {
        // DEPBAR / NOP / MEMBAR / BAR
    } else if (OPM(w, 0xe00000000000ff00ull, 0xff0000400000ff00ull) || OPM(w, 0xe000004000000000ull, 0xff00004000000000ull)) {  // IPA
        emit_attr_load_spv(s, d, (uint32_t)bits(w, 28, 10), (int)bits(w, 54, 2), (int)bits(w, 20, 8));
        if (bits(w, 51, 1) && d != RZ) wr_reg(s, d, f2u(s, emit_sat(s, u2f(s, rd_reg(s, d)))));
    } else if (OPM(w, 0xefd8000000000000ull, 0xfff8000000000000ull)) {  // LD a[]
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        for (int i = 0; i < n; i++) emit_attr_load_spv(s, d == RZ ? RZ : d + i, addr + 4 * i, -1, RZ);
    } else if (OPM(w, 0xeff0000000000000ull, 0xfff8000000000000ull)) {  // ST a[]
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        for (int i = 0; i < n; i++) emit_attr_store_spv(s, d == RZ ? RZ : d + i, addr + 4 * i);
    } else if (OPM(w, 0xef90000000000000ull, 0xfff8000000000000ull)) {  // LDC
        int idx = (int)bits(w, 36, 5);
        int64_t off = (int64_t)bits(w, 20, 16);
        int n = bits(w, 48, 3) == 5 ? 2 : 1;
        uint32_t dyn = a == RZ ? 0 : rd_reg(s, a);  // snapshot before overwriting d (T127)
        if (dyn && off >= 0x8000) off -= 0x10000;
        for (int i = 0; i < n; i++) wr_reg(s, d == RZ ? RZ : d + i, emit_cbuf(s, idx, off + 4 * i, dyn));
    } else if (OPM(w, 0xd000000000000000ull, 0xf600000000000000ull) ||
               OPM(w, 0xd200000000000000ull, 0xf600000000000000ull)) {  // TEXS / TLDS
        bool fetch = OPM(w, 0xd200000000000000ull, 0xf600000000000000ull);
        int d0 = d, d1 = (int)bits(w, 28, 8), b = (int)bits(w, 20, 8);
        uint32_t index = (uint32_t)bits(w, 36, 13);
        int info = (int)bits(w, 53, 4), msel = (int)bits(w, 50, 3);
        static const uint32_t m2[] = {1, 2, 4, 8, 3, 9, 10, 12}, m4[] = {7, 11, 13, 14, 15, 15, 15, 15};
        uint32_t mask = d1 == RZ ? m2[msel] : m4[msel];
        int slot = s->tex_slot[index] - 1;
        uint32_t A = rd_reg(s, a), A1 = rd_reg(s, a == RZ ? RZ : a + 1);
        uint32_t Bv = rd_reg(s, b), B1 = rd_reg(s, b == RZ ? RZ : b + 1);
        uint32_t res = 0;
        if (!fetch) {
            uint32_t fa = u2f(s, A), fa1 = u2f(s, A1), fb = u2f(s, Bv), fb1 = u2f(s, B1);
            uint32_t co[3];
            switch (info) {
            case 0: co[0] = fa; res = emit_sample(s, slot, co, 0, 0, 0, 0, 0, 0, false); break;
            case 1: co[0] = fa; co[1] = fb; res = emit_sample(s, slot, co, 0, 0, 0, 0, 0, 0, false); break;
            case 2: co[0] = fa; co[1] = fb; res = emit_sample(s, slot, co, 0, 0, 1, cf(s, 0.0f), 0, 0, false); break;
            case 3: co[0] = fa; co[1] = fa1; res = emit_sample(s, slot, co, 0, 0, 1, fb, 0, 0, false); break;
            case 4: co[0] = fa; co[1] = fa1; res = emit_sample(s, slot, co, 0, fb, 0, 0, 0, 0, false); break;
            case 5: co[0] = fa; co[1] = fa1; res = emit_sample(s, slot, co, 0, fb1, 1, fb, 0, 0, false); break;
            case 6: co[0] = fa; co[1] = fa1; res = emit_sample(s, slot, co, 0, fb, 1, cf(s, 0.0f), 0, 0, false); break;
            case 7: co[0] = fa1; co[1] = fb; res = emit_sample(s, slot, co, emit_alayer(s, A, slot), 0, 0, 0, 0, 0, false); break;
            case 8: co[0] = fa1; co[1] = fb; res = emit_sample(s, slot, co, emit_alayer(s, A, slot), 0, 1, cf(s, 0.0f), 0, 0, false); break;
            case 9: co[0] = fa1; co[1] = fb; res = emit_sample(s, slot, co, A, fb1, 1, cf(s, 0.0f), 0, 0, false); break;
            case 10: co[0] = fa; co[1] = fa1; co[2] = fb; res = emit_sample(s, slot, co, 0, 0, 0, 0, 0, 0, false); break;
            case 11: co[0] = fa; co[1] = fa1; co[2] = fb; res = emit_sample(s, slot, co, 0, 0, 1, cf(s, 0.0f), 0, 0, false); break;
            case 12: co[0] = fa; co[1] = fa1; co[2] = fb; res = emit_sample(s, slot, co, 0, 0, 0, 0, 0, 0, false); break;
            case 13: co[0] = fa; co[1] = fa1; co[2] = fb; res = emit_sample(s, slot, co, 0, 0, 1, fb1, 0, 0, false); break;
            }
        } else {
            switch (info) {
            case 0: res = emit_fetch(s, slot, u2i(s, A), ci(s, 0)); break;
            case 1: res = emit_fetch(s, slot, u2i(s, A), u2i(s, Bv)); break;
            case 2: res = emit_fetch(s, slot, emit_rsc2_i2(s, slot, A, Bv), ci(s, 0)); break;
            case 4: {
                uint32_t ox = emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, Bv), ci(s, 0), ci(s, 4));
                uint32_t oy = emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, Bv), ci(s, 4), ci(s, 4));
                res = emit_fetch(s, slot, emit_rsc2_i2(s, slot, emit2(s, OpIAdd, s->t_u32, A, i2u(s, ox)),
                                                       emit2(s, OpIAdd, s->t_u32, A1, i2u(s, oy))), ci(s, 0));
                break;
            }
            case 5: res = emit_fetch(s, slot, emit_rsc2_i2(s, slot, A, A1), u2i(s, Bv)); break;
            case 7: res = emit_fetch(s, slot, vec3(s, s->t_v3i, u2i(s, A), u2i(s, A1), u2i(s, Bv)), ci(s, 0)); break;
            case 8: {
                uint32_t xy = emit_rsc2_i2(s, slot, A1, Bv);
                res = emit_fetch(s, slot, vec3(s, s->t_v3i, extract(s, s->t_s32, xy, 0), extract(s, s->t_s32, xy, 1), u2i(s, A)), ci(s, 0));
                break;
            }
            case 12: {
                uint32_t ox = emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, B1), ci(s, 0), ci(s, 4));
                uint32_t oy = emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, B1), ci(s, 4), ci(s, 4));
                res = emit_fetch(s, slot, emit_rsc2_i2(s, slot, emit2(s, OpIAdd, s->t_u32, A, i2u(s, ox)),
                                                       emit2(s, OpIAdd, s->t_u32, A1, i2u(s, oy))), u2i(s, Bv));
                break;
            }
            }
        }
        emit_tex_write(s, res, mask, d0, d1, true, !bits(w, 59, 1));
    } else if (OPM(w, 0xc038000000000000ull, 0xfc38000000000000ull)) {  // TEX
        int target = (int)bits(w, 29, 2), lod_mode = (int)bits(w, 55, 3);
        bool array = bits(w, 28, 1), dc = bits(w, 50, 1), aoffi = bits(w, 54, 1);
        uint32_t mask = (uint32_t)bits(w, 31, 4), index = (uint32_t)bits(w, 36, 13);
        int b = (int)bits(w, 20, 8), slot = s->tex_slot[index] - 1;
        int ra = a, rb = b;
        uint32_t layer_u = 0;
        if (array) { layer_u = emit_alayer(s, rd_reg(s, ra), slot); if (ra != RZ) ra++; }
        int nc = target == 0 ? 1 : target == 1 ? 2 : 3;
        uint32_t co[3] = {0};
        for (int i = 0; i < nc; i++) { co[i] = u2f(s, rd_reg(s, ra)); if (ra != RZ) ra++; }
        int lk = 0; uint32_t lod_f = 0;
        if (lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7) {
            lk = (lod_mode == 2 || lod_mode == 6) ? 2 : 1;
            lod_f = u2f(s, rd_reg(s, rb)); if (rb != RZ) rb++;
        } else if (lod_mode == 1) {
            lk = 1; lod_f = cf(s, 0.0f);
        }
        uint32_t ox = 0, oy = 0;
        if (aoffi) {
            uint32_t ov = u2i(s, rd_reg(s, rb));
            ox = emit3(s, OpBitFieldSExtract, s->t_s32, ov, ci(s, 0), ci(s, 4));
            oy = emit3(s, OpBitFieldSExtract, s->t_s32, ov, ci(s, 4), ci(s, 4));
            if (rb != RZ) rb++;
        }
        uint32_t dref_f = 0;
        if (dc) { dref_f = u2f(s, rd_reg(s, rb)); }
        uint32_t res = emit_sample(s, slot, co, layer_u, dref_f, lk, lod_f, ox, oy, aoffi);
        emit_tex_write(s, res, mask, d, RZ, false, false);
    } else if (OPM(w, 0xdf00000000000000ull, 0xff40000000000000ull)) {  // TLD4S
        int d1 = (int)bits(w, 28, 8), b = (int)bits(w, 20, 8);
        int slot = s->tex_slot[bits(w, 36, 13)] - 1;
        bool aoffi = bits(w, 51, 1), f16 = bits(w, 55, 1);
        int comp = (int)bits(w, 52, 2);
        uint32_t ux, uy;
        if (aoffi) {
            ux = u2f(s, rd_reg(s, a)); uy = u2f(s, rd_reg(s, a == RZ ? RZ : a + 1));
            uint32_t ov = u2i(s, rd_reg(s, b));
            uint32_t ox = emit3(s, OpBitFieldSExtract, s->t_s32, ov, ci(s, 0), ci(s, 6));
            uint32_t oy = emit3(s, OpBitFieldSExtract, s->t_s32, ov, ci(s, 8), ci(s, 6));
            uint32_t ts = emit_tsize(s, slot, ci(s, 0));
            ux = emit_f2(s, OpFAdd, s->t_f32, ux, emit_f2(s, OpFDiv, s->t_f32, emit1(s, OpConvertSToF, s->t_f32, ox),
                                                          emit1(s, OpConvertSToF, s->t_f32, extract(s, s->t_s32, ts, 0))));
            uy = emit_f2(s, OpFAdd, s->t_f32, uy, emit_f2(s, OpFDiv, s->t_f32, emit1(s, OpConvertSToF, s->t_f32, oy),
                                                          emit1(s, OpConvertSToF, s->t_f32, extract(s, s->t_s32, ts, 1))));
        } else {
            ux = u2f(s, rd_reg(s, a)); uy = u2f(s, rd_reg(s, b));
        }
        uint32_t samp = load(s, s->t_samp[slot], s->v_tex[slot]);
        uint32_t rty = s->info->tex[slot].is_uint ? s->t_v4u : s->t_v4f;
        uint32_t res = emit3(s, OpImageGather, rty, samp, vec2(s, s->t_v2f, ux, uy), ci(s, comp));
        if (s->info->tex[slot].is_uint) res = emit1(s, OpBitcast, s->t_v4f, res);
        emit_tex_write(s, res, 0xF, d, d1, true, f16);
    } else if (OPM(w, 0xc838000000000000ull, 0xfc38000000000000ull)) {  // TLD4
        int b = (int)bits(w, 20, 8), comp = (int)bits(w, 56, 2), offmode = (int)bits(w, 54, 2);
        bool dc = bits(w, 50, 1);
        int slot = s->tex_slot[bits(w, 36, 13)] - 1;
        uint32_t ux = u2f(s, rd_reg(s, a)), uy = u2f(s, rd_reg(s, a == RZ ? RZ : a + 1));
        int rb = b;
        if (offmode == 1) {
            uint32_t ov = u2i(s, rd_reg(s, rb)); rb++;
            uint32_t ox = emit3(s, OpBitFieldSExtract, s->t_s32, ov, ci(s, 0), ci(s, 6));
            uint32_t oy = emit3(s, OpBitFieldSExtract, s->t_s32, ov, ci(s, 8), ci(s, 6));
            uint32_t ts = emit_tsize(s, slot, ci(s, 0));
            ux = emit_f2(s, OpFAdd, s->t_f32, ux, emit_f2(s, OpFDiv, s->t_f32, emit1(s, OpConvertSToF, s->t_f32, ox),
                                                          emit1(s, OpConvertSToF, s->t_f32, extract(s, s->t_s32, ts, 0))));
            uy = emit_f2(s, OpFAdd, s->t_f32, uy, emit_f2(s, OpFDiv, s->t_f32, emit1(s, OpConvertSToF, s->t_f32, oy),
                                                          emit1(s, OpConvertSToF, s->t_f32, extract(s, s->t_s32, ts, 1))));
        } else if (offmode == 2) rb += 2;
        uint32_t samp = load(s, s->t_samp[slot], s->v_tex[slot]);
        uint32_t uv = vec2(s, s->t_v2f, ux, uy);
        uint32_t res;
        if (dc) {
            res = emit3(s, OpImageDrefGather, s->t_v4f, samp, uv, u2f(s, rd_reg(s, rb)));
        } else {
            uint32_t rty = s->info->tex[slot].is_uint ? s->t_v4u : s->t_v4f;
            res = emit3(s, OpImageGather, rty, samp, uv, ci(s, comp));
            if (s->info->tex[slot].is_uint) res = emit1(s, OpBitcast, s->t_v4f, res);
        }
        emit_tex_write(s, res, (uint32_t)bits(w, 31, 4), d, RZ, false, false);
    } else if (OPM(w, 0xdf48000000000000ull, 0xfff8000000000000ull)) {  // TXQ
        int slot = s->tex_slot[bits(w, 36, 13)] - 1;
        uint32_t mask = (uint32_t)bits(w, 31, 4);
        uint32_t lv = u2i(s, rd_reg(s, a));
        uint32_t ts = emit_tsize(s, slot, lv);
        uint32_t samp = load(s, s->t_samp[slot], s->v_tex[slot]);
        uint32_t img = emit1(s, OpImage, s->t_img[slot], samp);
        uint32_t lvls = i2u(s, emit1(s, OpImageQueryLevels, s->t_s32, img));
        uint32_t qw = i2u(s, extract(s, s->t_s32, ts, 0));
        uint32_t qh = i2u(s, extract(s, s->t_s32, ts, 1));
        if (s->stage != STAGE_VERTEX) {
            uint32_t rs = emit_rscale_v4(s, slot + 1);
            qw = emit1(s, OpConvertFToU, s->t_u32, glsl1(s, GlslFloor, s->t_f32,
                       emit_f2(s, OpFAdd, s->t_f32, emit_f2(s, OpFDiv, s->t_f32, emit1(s, OpConvertSToF, s->t_f32, extract(s, s->t_s32, ts, 0)),
                                                            extract(s, s->t_f32, rs, 0)), cf(s, 0.5f))));
            qh = emit1(s, OpConvertFToU, s->t_u32, glsl1(s, GlslFloor, s->t_f32,
                       emit_f2(s, OpFAdd, s->t_f32, emit_f2(s, OpFDiv, s->t_f32, emit1(s, OpConvertSToF, s->t_f32, extract(s, s->t_s32, ts, 1)),
                                                            extract(s, s->t_f32, rs, 1)), cf(s, 0.5f))));
        }
        uint32_t q[4] = {qw, qh, cu(s, 1), lvls};
        int n = 0;
        for (int i = 0; i < 4; i++) if (mask & (1u << i)) wr_reg(s, d == RZ ? RZ : d + n++, q[i]);
    } else if (OPM(w, 0x5080000000000000ull, 0xfff8000000000000ull)) {  // MUFU
        int fn = (int)bits(w, 20, 4);
        uint32_t x = emit_na(s, u2f(s, rd_reg(s, a)), bits(w, 48, 1), bits(w, 46, 1));
        uint32_t r = x;
        switch (fn) {
        case 0: r = glsl1(s, GlslCos, s->t_f32, x); break;
        case 1: r = glsl1(s, GlslSin, s->t_f32, x); break;
        case 2: r = glsl1(s, GlslExp2, s->t_f32, x); break;
        case 3: r = glsl1(s, GlslLog2, s->t_f32, x); break;
        case 4: r = emit_f2(s, OpFDiv, s->t_f32, cf(s, 1.0f), x); break;
        case 5: r = glsl1(s, GlslInverseSqrt, s->t_f32, x); break;
        case 8: r = glsl1(s, GlslSqrt, s->t_f32, x); break;
        }
        if (bits(w, 50, 1)) r = emit_sat(s, r);
        wr_reg(s, d, f2u(s, r));
    } else if (OPM(w, 0x5c90000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c90000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3890000000000000ull, 0xfef8000000000000ull)) {  // RRO
        uint32_t x = emit_na(s, u2f(s, emit_b(s, w, true)), bits(w, 45, 1), bits(w, 49, 1));
        wr_reg(s, d, f2u(s, x));
    } else if (OPM(w, 0x5c58000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c58000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3858000000000000ull, 0xfef8000000000000ull)) {  // FADD
        uint32_t x = emit_na(s, u2f(s, rd_reg(s, a)), bits(w, 48, 1), bits(w, 46, 1));
        uint32_t y = emit_na(s, u2f(s, emit_b(s, w, true)), bits(w, 45, 1), bits(w, 49, 1));
        uint32_t r = emit_f2(s, OpFAdd, s->t_f32, x, y);
        if (bits(w, 50, 1)) r = emit_sat(s, r);
        wr_reg(s, d, f2u(s, r));
    } else if (OPM(w, 0x0800000000000000ull, 0xfc00000000000000ull)) {  // FADD32I
        uint32_t x = emit_na(s, u2f(s, rd_reg(s, a)), bits(w, 56, 1), bits(w, 54, 1));
        uint32_t y = emit_na(s, u2f(s, cu(s, (uint32_t)bits(w, 20, 32))), bits(w, 53, 1), bits(w, 57, 1));
        wr_reg(s, d, f2u(s, emit_f2(s, OpFAdd, s->t_f32, x, y)));
    } else if (OPM(w, 0x5c68000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c68000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3868000000000000ull, 0xfef8000000000000ull)) {  // FMUL
        static const float scale[8] = {1.0f, 0.5f, 0.25f, 0.125f, 8.0f, 4.0f, 2.0f, 1.0f};
        uint32_t x = u2f(s, rd_reg(s, a));
        uint32_t y = emit_na(s, u2f(s, emit_b(s, w, true)), bits(w, 48, 1), false);
        uint32_t r = emit_f2(s, OpFMul, s->t_f32, x, y);
        int sc = (int)bits(w, 41, 3);
        if (scale[sc] != 1.0f) r = emit_f2(s, OpFMul, s->t_f32, r, cf(s, scale[sc]));
        if (bits(w, 50, 1)) r = emit_sat(s, r);
        wr_reg(s, d, f2u(s, r));
    } else if (OPM(w, 0x1e00000000000000ull, 0xff00000000000000ull)) {  // FMUL32I
        uint32_t r = emit_f2(s, OpFMul, s->t_f32, u2f(s, rd_reg(s, a)), u2f(s, cu(s, (uint32_t)bits(w, 20, 32))));
        if (bits(w, 55, 1)) r = emit_sat(s, r);
        wr_reg(s, d, f2u(s, r));
    } else if (OPM(w, 0x5980000000000000ull, 0xff80000000000000ull) || OPM(w, 0x4980000000000000ull, 0xff80000000000000ull) ||
               OPM(w, 0x5180000000000000ull, 0xff80000000000000ull) || OPM(w, 0x3280000000000000ull, 0xfe80000000000000ull)) {  // FFMA
        uint32_t bv, cv;
        uint64_t op8 = w >> 56;
        if ((op8 & 0xF8) == 0x50 && (op8 & 7) != 0) {
            bv = rd_reg(s, (int)bits(w, 39, 8));
            cv = emit_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, 0);
        } else {
            bv = emit_b(s, w, true);
            cv = rd_reg(s, (int)bits(w, 39, 8));
        }
        uint32_t r = glsl3(s, GlslFma, s->t_f32, u2f(s, rd_reg(s, a)), emit_na(s, u2f(s, bv), bits(w, 48, 1), false),
                           emit_na(s, u2f(s, cv), bits(w, 49, 1), false));
        if (bits(w, 50, 1)) r = emit_sat(s, r);
        wr_reg(s, d, f2u(s, r));
    } else if (OPM(w, 0x0c00000000000000ull, 0xfc00000000000000ull)) {  // FFMA32I
        uint32_t r = glsl3(s, GlslFma, s->t_f32, emit_na(s, u2f(s, rd_reg(s, a)), bits(w, 56, 1), false),
                           u2f(s, cu(s, (uint32_t)bits(w, 20, 32))), emit_na(s, u2f(s, rd_reg(s, d)), bits(w, 57, 1), false));
        if (bits(w, 55, 1)) r = emit_sat(s, r);
        wr_reg(s, d, f2u(s, r));
    } else if (OPM(w, 0x5c60000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c60000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3860000000000000ull, 0xfef8000000000000ull)) {  // FMNMX
        uint32_t x = emit_na(s, u2f(s, rd_reg(s, a)), bits(w, 48, 1), bits(w, 46, 1));
        uint32_t y = emit_na(s, u2f(s, emit_b(s, w, true)), bits(w, 45, 1), bits(w, 49, 1));
        uint32_t p = rd_pred(s, (int)bits(w, 39, 3), bits(w, 42, 1));
        uint32_t r = emit3(s, OpSelect, s->t_f32, p, glsl2(s, GlslNMin, s->t_f32, x, y), glsl2(s, GlslNMax, s->t_f32, x, y));
        wr_reg(s, d, f2u(s, r));
    } else if (OPM(w, 0x5bb0000000000000ull, 0xfff0000000000000ull) || OPM(w, 0x4bb0000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x36b0000000000000ull, 0xfef0000000000000ull)) {  // FSETP
        uint32_t x = emit_na(s, u2f(s, rd_reg(s, a)), bits(w, 43, 1), bits(w, 7, 1));
        uint32_t y = emit_na(s, u2f(s, emit_b(s, w, true)), bits(w, 6, 1), bits(w, 44, 1));
        uint32_t cmp = emit_fcmp(s, (int)bits(w, 48, 4), x, y);
        uint32_t pp = rd_pred(s, (int)bits(w, 39, 3), bits(w, 42, 1));
        int bo = (int)bits(w, 45, 2);
        wr_pred(s, (int)bits(w, 3, 3), emit_bool_op(s, bo, cmp, pp));
        wr_pred(s, (int)bits(w, 0, 3), emit_bool_op(s, bo, emit1(s, OpLogicalNot, s->t_bool, cmp), pp));
    } else if (OPM(w, 0x5800000000000000ull, 0xff00000000000000ull) || OPM(w, 0x4800000000000000ull, 0xfe00000000000000ull) ||
               OPM(w, 0x3000000000000000ull, 0xfe00000000000000ull)) {  // FSET
        uint32_t x = emit_na(s, u2f(s, rd_reg(s, a)), bits(w, 43, 1), bits(w, 54, 1));
        uint32_t y = emit_na(s, u2f(s, emit_b(s, w, true)), bits(w, 53, 1), bits(w, 44, 1));
        uint32_t cmp = emit_fcmp(s, (int)bits(w, 48, 4), x, y);
        uint32_t pp = rd_pred(s, (int)bits(w, 39, 3), bits(w, 42, 1));
        uint32_t res = emit_bool_op(s, (int)bits(w, 45, 2), cmp, pp);
        wr_reg(s, d, emit3(s, OpSelect, s->t_u32, res, cu(s, bits(w, 52, 1) ? 0x3f800000u : 0xFFFFFFFFu), cu(s, 0)));
    } else if (OPM(w, 0x5ba0000000000000ull, 0xfff0000000000000ull) || OPM(w, 0x4ba0000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x36a0000000000000ull, 0xfef0000000000000ull) || OPM(w, 0x53a0000000000000ull, 0xfff0000000000000ull)) {  // FCMP
        uint32_t bv, cv;
        if (OPM(w, 0x53a0000000000000ull, 0xfff0000000000000ull)) {
            bv = rd_reg(s, (int)bits(w, 39, 8)); cv = emit_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, 0);
        } else { bv = emit_b(s, w, true); cv = rd_reg(s, (int)bits(w, 39, 8)); }
        uint32_t cmp = emit_fcmp(s, (int)bits(w, 48, 4), u2f(s, cv), cf(s, 0.0f));
        wr_reg(s, d, emit3(s, OpSelect, s->t_u32, cmp, rd_reg(s, a), bv));
    } else if (OPM(w, 0x5c98000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c98000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3898000000000000ull, 0xfef8000000000000ull)) {  // MOV
        wr_reg(s, d, emit_b(s, w, false));
    } else if (OPM(w, 0x0100000000000000ull, 0xfff0000000000000ull)) {  // MOV32I
        wr_reg(s, d, cu(s, (uint32_t)bits(w, 20, 32)));
    } else if (OPM(w, 0x5ca0000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4ca0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x38a0000000000000ull, 0xfef8000000000000ull)) {  // SEL
        wr_reg(s, d, emit3(s, OpSelect, s->t_u32, rd_pred(s, (int)bits(w, 39, 3), bits(w, 42, 1)), rd_reg(s, a), emit_b(s, w, false)));
    } else if (OPM(w, 0x5cb0000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4cb0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x38b0000000000000ull, 0xfef8000000000000ull)) {  // F2I
        uint32_t x;
        if (bits(w, 10, 2) == 1) {
            uint32_t b = emit_b(s, w, false);
            if (bits(w, 41, 1)) b = emit2(s, OpShiftRightLogical, s->t_u32, b, cu(s, 16));
            x = emit_h2f(s, emit2(s, OpBitwiseAnd, s->t_u32, b, cu(s, 0xFFFFu)));
        } else x = u2f(s, emit_b(s, w, true));
        x = emit_na(s, x, bits(w, 45, 1), bits(w, 49, 1));
        static const uint32_t rf[4] = {GlslRoundEven, GlslFloor, GlslCeil, GlslTrunc};
        x = glsl1(s, rf[bits(w, 39, 2)], s->t_f32, x);
        bool sgn = bits(w, 12, 1), i16 = bits(w, 8, 2) == 1;
        float lo = sgn ? (i16 ? -32768.0f : -2147483648.0f) : 0.0f;
        float hi = sgn ? (i16 ? 32767.0f : 2147483520.0f) : (i16 ? 65535.0f : 4294967040.0f);
        x = glsl3(s, GlslFClamp, s->t_f32, x, cf(s, lo), cf(s, hi));
        wr_reg(s, d, sgn ? i2u(s, emit1(s, OpConvertFToS, s->t_s32, x)) : emit1(s, OpConvertFToU, s->t_u32, x));
    } else if (OPM(w, 0x5cb8000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4cb8000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x38b8000000000000ull, 0xfef8000000000000ull)) {  // I2F
        bool sgn = bits(w, 13, 1);
        int sz = (int)bits(w, 10, 2), sel = (int)bits(w, 41, 2);
        uint32_t bv = emit_b(s, w, false), f;
        if (sz == 0) {
            uint32_t sh = sel ? emit2(s, OpShiftRightLogical, s->t_u32, bv, cu(s, (uint32_t)(sel * 8))) : bv;
            f = sgn ? emit1(s, OpConvertSToF, s->t_f32, emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, sh), ci(s, 0), ci(s, 8)))
                    : emit1(s, OpConvertUToF, s->t_f32, emit2(s, OpBitwiseAnd, s->t_u32, sh, cu(s, 0xFFu)));
        } else if (sz == 1) {
            uint32_t sh = sel ? emit2(s, OpShiftRightLogical, s->t_u32, bv, cu(s, (uint32_t)(sel * 8))) : bv;
            f = sgn ? emit1(s, OpConvertSToF, s->t_f32, emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, sh), ci(s, 0), ci(s, 16)))
                    : emit1(s, OpConvertUToF, s->t_f32, emit2(s, OpBitwiseAnd, s->t_u32, sh, cu(s, 0xFFFFu)));
        } else {
            f = sgn ? emit1(s, OpConvertSToF, s->t_f32, u2i(s, bv)) : emit1(s, OpConvertUToF, s->t_f32, bv);
        }
        f = emit_na(s, f, bits(w, 45, 1), bits(w, 49, 1));
        if (bits(w, 8, 2) == 1) wr_reg(s, d, glsl1(s, GlslPackHalf2x16, s->t_u32, vec2(s, s->t_v2f, f, cf(s, 0.0f))));
        else wr_reg(s, d, f2u(s, f));
    } else if (OPM(w, 0x5ca8000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4ca8000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x38a8000000000000ull, 0xfef8000000000000ull)) {  // F2F
        int dfmt = (int)bits(w, 8, 2), sfmt = (int)bits(w, 10, 2);
        uint32_t x;
        if (sfmt == 1) {
            uint32_t b = emit_b(s, w, false);
            if (bits(w, 41, 1)) b = emit2(s, OpShiftRightLogical, s->t_u32, b, cu(s, 16));
            x = emit_h2f(s, emit2(s, OpBitwiseAnd, s->t_u32, b, cu(s, 0xFFFFu)));
        } else x = u2f(s, emit_b(s, w, true));
        x = emit_na(s, x, bits(w, 45, 1), bits(w, 49, 1));
        int mode = (int)(bits(w, 39, 2) | (bits(w, 42, 1) << 2));
        if (mode == 4) x = glsl1(s, GlslRoundEven, s->t_f32, x);
        else if (mode == 5) x = glsl1(s, GlslFloor, s->t_f32, x);
        else if (mode == 6) x = glsl1(s, GlslCeil, s->t_f32, x);
        else if (mode == 7) x = glsl1(s, GlslTrunc, s->t_f32, x);
        if (bits(w, 50, 1)) x = emit_sat(s, x);
        if (dfmt == 1) wr_reg(s, d, glsl1(s, GlslPackHalf2x16, s->t_u32, vec2(s, s->t_v2f, x, cf(s, 0.0f))));
        else wr_reg(s, d, f2u(s, x));
    } else if (OPM(w, 0x5c10000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c10000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3810000000000000ull, 0xfef8000000000000ull)) {  // IADD
        uint32_t x = rd_reg(s, a), y = emit_b(s, w, false);
        if (bits(w, 49, 1)) x = emit2(s, OpISub, s->t_u32, cu(s, 0), x);
        if (bits(w, 48, 1)) y = emit2(s, OpISub, s->t_u32, cu(s, 0), y);
        uint32_t res = emit2(s, OpIAdd, s->t_u32, x, y);
        if (bits(w, 43, 1)) res = emit2(s, OpIAdd, s->t_u32, res, load(s, s->t_u32, s->v_cc));
        if (bits(w, 47, 1)) store(s, s->v_cc, emit3(s, OpSelect, s->t_u32, emit2(s, OpULessThan, s->t_bool, res, x), cu(s, 1), cu(s, 0)));
        wr_reg(s, d, res);
    } else if (OPM(w, 0x1c00000000000000ull, 0xfe00000000000000ull)) {  // IADD32I
        bool wcc = bits(w, 52, 1), xi = bits(w, 53, 1);
        uint32_t x = rd_reg(s, a);
        if (bits(w, 56, 1)) x = emit2(s, OpISub, s->t_u32, cu(s, 0), x);
        uint32_t imm = cu(s, (uint32_t)bits(w, 20, 32));
        uint32_t ac = emit2(s, OpIAddCarry, s->t_struct_uu, x, imm);
        uint32_t res = extract(s, s->t_u32, ac, 0), cry = extract(s, s->t_u32, ac, 1);
        if (xi) {
            uint32_t ac2_ = emit2(s, OpIAddCarry, s->t_struct_uu, res, load(s, s->t_u32, s->v_cc));
            res = extract(s, s->t_u32, ac2_, 0);
            cry = emit2(s, OpBitwiseOr, s->t_u32, cry, extract(s, s->t_u32, ac2_, 1));
        }
        if (wcc) store(s, s->v_cc, emit2(s, OpBitwiseAnd, s->t_u32, cry, cu(s, 1)));
        wr_reg(s, d, res);
    } else if (OPM(w, 0x5cc0000000000000ull, 0xfff0000000000000ull) || OPM(w, 0x4cc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x38c0000000000000ull, 0xfef0000000000000ull)) {  // IADD3
        bool wcc = bits(w, 47, 1), x3 = bits(w, 48, 1);
        uint32_t xa = rd_reg(s, a), xb = emit_b(s, w, false), xc = rd_reg(s, (int)bits(w, 39, 8));
        if (bits(w, 51, 1)) xa = emit2(s, OpISub, s->t_u32, cu(s, 0), xa);
        if (bits(w, 50, 1)) xb = emit2(s, OpISub, s->t_u32, cu(s, 0), xb);
        if (bits(w, 49, 1)) xc = emit2(s, OpISub, s->t_u32, cu(s, 0), xc);
        if (wcc || x3) {
            uint32_t s1 = emit2(s, OpIAddCarry, s->t_struct_uu, xa, xb);
            uint32_t s2 = emit2(s, OpIAddCarry, s->t_struct_uu, extract(s, s->t_u32, s1, 0), xc);
            uint32_t res = extract(s, s->t_u32, s2, 0);
            uint32_t cry = emit2(s, OpIAdd, s->t_u32, extract(s, s->t_u32, s1, 1), extract(s, s->t_u32, s2, 1));
            if (x3) {
                uint32_t s3 = emit2(s, OpIAddCarry, s->t_struct_uu, res, load(s, s->t_u32, s->v_cc));
                res = extract(s, s->t_u32, s3, 0);
                cry = emit2(s, OpIAdd, s->t_u32, cry, extract(s, s->t_u32, s3, 1));
            }
            if (wcc) store(s, s->v_cc, emit2(s, OpBitwiseAnd, s->t_u32, cry, cu(s, 1)));
            wr_reg(s, d, res);
        } else {
            wr_reg(s, d, emit2(s, OpIAdd, s->t_u32, emit2(s, OpIAdd, s->t_u32, xa, xb), xc));
        }
    } else if (OPM(w, 0x5c18000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c18000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3818000000000000ull, 0xfef8000000000000ull)) {  // ISCADD
        uint32_t x = rd_reg(s, a), y = emit_b(s, w, false);
        if (bits(w, 49, 1)) x = emit2(s, OpISub, s->t_u32, cu(s, 0), x);
        if (bits(w, 48, 1)) y = emit2(s, OpISub, s->t_u32, cu(s, 0), y);
        x = emit2(s, OpShiftLeftLogical, s->t_u32, x, cu(s, (uint32_t)bits(w, 39, 5)));
        uint32_t res = emit2(s, OpIAdd, s->t_u32, x, y);
        if (bits(w, 47, 1)) store(s, s->v_cc, emit3(s, OpSelect, s->t_u32, emit2(s, OpULessThan, s->t_bool, res, x), cu(s, 1), cu(s, 0)));
        wr_reg(s, d, res);
    } else if (OPM(w, 0x1400000000000000ull, 0xfc00000000000000ull)) {  // ISCADD32I
        uint32_t x = emit2(s, OpShiftLeftLogical, s->t_u32, rd_reg(s, a), cu(s, (uint32_t)bits(w, 53, 5)));
        wr_reg(s, d, emit2(s, OpIAdd, s->t_u32, x, cu(s, (uint32_t)bits(w, 20, 32))));
    } else if (OPM(w, 0x5f00000000000000ull, 0xff00000000000000ull) ||
               OPM(w, 0x50f0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3a00000000000000ull, 0xfe00000000000000ull)) {  // VMAD / VSETP / VMNMX
        int asel = (int)((bits(w, 48, 1) << 3) | bits(w, 36, 3));
        int bsel = (int)((bits(w, 49, 1) << 3) | bits(w, 28, 3));
        uint32_t av = emit_video_part(s, rd_reg(s, a), asel), bv;
        if (bits(w, 50, 1)) bv = emit_video_part(s, rd_reg(s, (int)bits(w, 20, 8)), bsel);
        else {
            uint32_t imm = (uint32_t)bits(w, 20, 16);
            if (bsel & 8) imm = (uint32_t)(int32_t)(int16_t)imm;
            bv = cu(s, imm);
        }
        bool sgn = ((asel & 8) || (bsel & 8)) != 0;
        if (OPM(w, 0x5f00000000000000ull, 0xff00000000000000ull)) {  // VMAD
            uint32_t res = emit2(s, OpIAdd, s->t_u32, emit2(s, OpIMul, s->t_u32, av, bv), rd_reg(s, (int)bits(w, 39, 8)));
            if (bits(w, 55, 1) && sgn) res = i2u(s, glsl3(s, GlslSClamp, s->t_s32, u2i(s, res), ci(s, 0), ci(s, 0x7fffffff)));
            wr_reg(s, d, res);
        } else if (OPM(w, 0x3a00000000000000ull, 0xfe00000000000000ull)) {  // VMNMX
            int op = (int)bits(w, 51, 3);
            bool s1 = bits(w, 49, 1), s2 = bits(w, 54, 1), mx1 = bits(w, 56, 1), mx2 = op == 6;
            uint32_t u = s1 ? i2u(s, glsl2(s, mx1 ? GlslSMax : GlslSMin, s->t_s32, u2i(s, av), u2i(s, bv)))
                            : glsl2(s, mx1 ? GlslUMax : GlslUMin, s->t_u32, av, bv);
            uint32_t cv = rd_reg(s, (int)bits(w, 39, 8));
            uint32_t res = s2 ? i2u(s, glsl2(s, mx2 ? GlslSMax : GlslSMin, s->t_s32, u2i(s, u), u2i(s, cv)))
                              : glsl2(s, mx2 ? GlslUMax : GlslUMin, s->t_u32, u, cv);
            wr_reg(s, d, res);
        } else {  // VSETP
            uint32_t cmp = emit_icmp(s, (int)bits(w, 43, 3), av, bv, sgn);
            uint32_t pp = rd_pred(s, (int)bits(w, 39, 3), bits(w, 42, 1));
            int bo = (int)bits(w, 45, 2);
            wr_pred(s, (int)bits(w, 3, 3), emit_bool_op(s, bo, cmp, pp));
            wr_pred(s, (int)bits(w, 0, 3), emit_bool_op(s, bo, emit1(s, OpLogicalNot, s->t_bool, cmp), pp));
        }
    } else if (OPM(w, 0x5b60000000000000ull, 0xfff0000000000000ull) || OPM(w, 0x4b60000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x3660000000000000ull, 0xfef0000000000000ull)) {  // ISETP
        uint32_t cmp = emit_icmp(s, (int)bits(w, 49, 3), rd_reg(s, a), emit_b(s, w, false), bits(w, 48, 1));
        uint32_t pp = rd_pred(s, (int)bits(w, 39, 3), bits(w, 42, 1));
        int bo = (int)bits(w, 45, 2);
        wr_pred(s, (int)bits(w, 3, 3), emit_bool_op(s, bo, cmp, pp));
        wr_pred(s, (int)bits(w, 0, 3), emit_bool_op(s, bo, emit1(s, OpLogicalNot, s->t_bool, cmp), pp));
    } else if (OPM(w, 0x5b50000000000000ull, 0xfff0000000000000ull) || OPM(w, 0x4b50000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x3650000000000000ull, 0xfef0000000000000ull)) {  // ISET
        uint32_t cmp = emit_icmp(s, (int)bits(w, 49, 3), rd_reg(s, a), emit_b(s, w, false), bits(w, 48, 1));
        uint32_t res = emit_bool_op(s, (int)bits(w, 45, 2), cmp, rd_pred(s, (int)bits(w, 39, 3), bits(w, 42, 1)));
        wr_reg(s, d, emit3(s, OpSelect, s->t_u32, res, cu(s, bits(w, 44, 1) ? 0x3f800000u : 0xFFFFFFFFu), cu(s, 0)));
    } else if (OPM(w, 0x5b40000000000000ull, 0xfff0000000000000ull) || OPM(w, 0x4b40000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x3640000000000000ull, 0xfef0000000000000ull) || OPM(w, 0x5340000000000000ull, 0xfff0000000000000ull)) {  // ICMP
        uint32_t bv, cv;
        if (OPM(w, 0x5340000000000000ull, 0xfff0000000000000ull)) {
            bv = rd_reg(s, (int)bits(w, 39, 8)); cv = emit_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, 0);
        } else { bv = emit_b(s, w, false); cv = rd_reg(s, (int)bits(w, 39, 8)); }
        uint32_t cmp = emit_icmp(s, (int)bits(w, 49, 3), cv, cu(s, 0), bits(w, 48, 1));
        wr_reg(s, d, emit3(s, OpSelect, s->t_u32, cmp, rd_reg(s, a), bv));
    } else if (OPM(w, 0x5c20000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c20000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3820000000000000ull, 0xfef8000000000000ull)) {  // IMNMX
        bool sgn = bits(w, 48, 1);
        uint32_t x = rd_reg(s, a), y = emit_b(s, w, false), p = rd_pred(s, (int)bits(w, 39, 3), bits(w, 42, 1));
        uint32_t mn = sgn ? i2u(s, glsl2(s, GlslSMin, s->t_s32, u2i(s, x), u2i(s, y))) : glsl2(s, GlslUMin, s->t_u32, x, y);
        uint32_t mx = sgn ? i2u(s, glsl2(s, GlslSMax, s->t_s32, u2i(s, x), u2i(s, y))) : glsl2(s, GlslUMax, s->t_u32, x, y);
        wr_reg(s, d, emit3(s, OpSelect, s->t_u32, p, mn, mx));
    } else if (OPM(w, 0x5c48000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c48000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3848000000000000ull, 0xfef8000000000000ull)) {  // SHL
        uint32_t x = rd_reg(s, a), bv = emit_b(s, w, false);
        uint32_t sh = emit2(s, OpBitwiseAnd, s->t_u32, bv, cu(s, 31));
        uint32_t r = emit2(s, OpShiftLeftLogical, s->t_u32, x, sh);
        if (!bits(w, 39, 1)) r = emit3(s, OpSelect, s->t_u32, emit2(s, OpUGreaterThanEqual, s->t_bool, bv, cu(s, 32)), cu(s, 0), r);
        wr_reg(s, d, r);
    } else if (OPM(w, 0x5c28000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c28000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3828000000000000ull, 0xfef8000000000000ull)) {  // SHR
        uint32_t x = rd_reg(s, a), bv = emit_b(s, w, false);
        if (bits(w, 48, 1)) {
            uint32_t sh = glsl2(s, GlslUMin, s->t_u32, bv, cu(s, 31));
            wr_reg(s, d, i2u(s, emit2(s, OpShiftRightArithmetic, s->t_s32, u2i(s, x), sh)));
        } else {
            uint32_t sh = emit2(s, OpBitwiseAnd, s->t_u32, bv, cu(s, 31));
            uint32_t r = emit2(s, OpShiftRightLogical, s->t_u32, x, sh);
            wr_reg(s, d, emit3(s, OpSelect, s->t_u32, emit2(s, OpUGreaterThanEqual, s->t_bool, bv, cu(s, 32)), cu(s, 0), r));
        }
    } else if (OPM(w, 0x5c40000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c40000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3840000000000000ull, 0xfef8000000000000ull)) {  // LOP
        uint32_t x = rd_reg(s, a), y = emit_b(s, w, false);
        if (bits(w, 39, 1)) x = emit1(s, OpNot, s->t_u32, x);
        if (bits(w, 40, 1)) y = emit1(s, OpNot, s->t_u32, y);
        int op = (int)bits(w, 41, 2);
        uint32_t res = op == 0 ? emit2(s, OpBitwiseAnd, s->t_u32, x, y)
                     : op == 1 ? emit2(s, OpBitwiseOr, s->t_u32, x, y)
                     : op == 2 ? emit2(s, OpBitwiseXor, s->t_u32, x, y) : y;
        wr_reg(s, d, res);
        int pd = (int)bits(w, 48, 3);
        if (pd != PT) {
            int pop = (int)bits(w, 44, 2);
            uint32_t pv = pop == 2 ? emit2(s, OpIEqual, s->t_bool, res, cu(s, 0))
                        : pop == 3 ? emit2(s, OpINotEqual, s->t_bool, res, cu(s, 0))
                        : pop == 1 ? s->c_true : s->c_false;
            wr_pred(s, pd, pv);
        }
    } else if (OPM(w, 0x0400000000000000ull, 0xfc00000000000000ull)) {  // LOP32I
        uint32_t x = rd_reg(s, a), y = cu(s, (uint32_t)bits(w, 20, 32));
        if (bits(w, 55, 1)) x = emit1(s, OpNot, s->t_u32, x);
        if (bits(w, 56, 1)) y = emit1(s, OpNot, s->t_u32, y);
        int op = (int)bits(w, 53, 2);
        uint32_t res = op == 0 ? emit2(s, OpBitwiseAnd, s->t_u32, x, y)
                     : op == 1 ? emit2(s, OpBitwiseOr, s->t_u32, x, y)
                     : op == 2 ? emit2(s, OpBitwiseXor, s->t_u32, x, y) : y;
        wr_reg(s, d, res);
    } else if (OPM(w, 0x5be0000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x3c00000000000000ull, 0xfc00000000000000ull) ||
               OPM(w, 0x0200000000000000ull, 0xfe00000000000000ull)) {  // LOP3
        uint32_t lut, bv, cv;
        if ((w >> 56) == 0x5b) { lut = (uint32_t)bits(w, 28, 8); bv = rd_reg(s, (int)bits(w, 20, 8)); cv = rd_reg(s, (int)bits(w, 39, 8)); }
        else if ((w >> 58) == 0xf) {
            lut = (uint32_t)bits(w, 48, 8);
            uint32_t im = (uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19) | (bits(w, 56, 1) ? 0xFFF00000u : 0);
            bv = cu(s, im); cv = rd_reg(s, (int)bits(w, 39, 8));
        } else {
            lut = (uint32_t)bits(w, 48, 8);
            bv = emit_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, 0);
            cv = rd_reg(s, (int)bits(w, 39, 8));
        }
        uint32_t av = rd_reg(s, a), na = emit1(s, OpNot, s->t_u32, av), nb = emit1(s, OpNot, s->t_u32, bv), nc = emit1(s, OpNot, s->t_u32, cv);
        uint32_t acc = 0;
        for (int i = 0; i < 8; i++) {
            if (!(lut & (1u << i))) continue;
            uint32_t t = emit2(s, OpBitwiseAnd, s->t_u32, emit2(s, OpBitwiseAnd, s->t_u32, (i & 4) ? av : na, (i & 2) ? bv : nb), (i & 1) ? cv : nc);
            acc = acc ? emit2(s, OpBitwiseOr, s->t_u32, acc, t) : t;
        }
        wr_reg(s, d, acc ? acc : cu(s, 0));
    } else if (OPM(w, 0x5c00000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c00000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3800000000000000ull, 0xfef8000000000000ull)) {  // BFE
        uint32_t v = rd_reg(s, a), ctl = emit_b(s, w, false);
        if (bits(w, 40, 1)) v = emit1(s, OpBitReverse, s->t_u32, v);
        uint32_t pos = emit2(s, OpBitwiseAnd, s->t_u32, ctl, cu(s, 0xFFu));
        uint32_t len = emit2(s, OpBitwiseAnd, s->t_u32, emit2(s, OpShiftRightLogical, s->t_u32, ctl, cu(s, 8)), cu(s, 0xFFu));
        uint32_t pos_ok = emit2(s, OpULessThan, s->t_bool, pos, cu(s, 32));
        uint32_t safe_pos = glsl2(s, GlslUMin, s->t_u32, pos, cu(s, 31));
        uint32_t safe_len = glsl2(s, GlslUMin, s->t_u32, len, emit2(s, OpISub, s->t_u32, cu(s, 32), safe_pos));
        uint32_t ext;
        if (bits(w, 48, 1)) {
            uint32_t ex = i2u(s, emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, v), u2i(s, safe_pos), u2i(s, safe_len)));
            uint32_t sfill = emit3(s, OpSelect, s->t_u32, emit2(s, OpSLessThan, s->t_bool, u2i(s, v), ci(s, 0)), cu(s, 0xFFFFFFFFu), cu(s, 0));
            ext = emit3(s, OpSelect, s->t_u32, pos_ok, ex, sfill);
        } else {
            uint32_t ex = emit3(s, OpBitFieldUExtract, s->t_u32, v, u2i(s, safe_pos), u2i(s, safe_len));
            ext = emit3(s, OpSelect, s->t_u32, pos_ok, ex, cu(s, 0));
        }
        wr_reg(s, d, emit3(s, OpSelect, s->t_u32, emit2(s, OpIEqual, s->t_bool, len, cu(s, 0)), cu(s, 0), ext));
    } else if (OPM(w, 0x5bf0000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4bf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x36f0000000000000ull, 0xfef8000000000000ull) || OPM(w, 0x53f0000000000000ull, 0xfff8000000000000ull)) {  // BFI
        uint32_t ins = rd_reg(s, a), ctl, base;
        if (OPM(w, 0x53f0000000000000ull, 0xfff8000000000000ull)) {
            ctl = rd_reg(s, (int)bits(w, 39, 8)); base = emit_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, 0);
        } else { ctl = emit_b(s, w, false); base = rd_reg(s, (int)bits(w, 39, 8)); }
        uint32_t pos = emit2(s, OpBitwiseAnd, s->t_u32, ctl, cu(s, 0xFFu));
        uint32_t len = emit2(s, OpBitwiseAnd, s->t_u32, emit2(s, OpShiftRightLogical, s->t_u32, ctl, cu(s, 8)), cu(s, 0xFFu));
        uint32_t ok = emit2(s, OpLogicalAnd, s->t_bool, emit2(s, OpINotEqual, s->t_bool, len, cu(s, 0)), emit2(s, OpULessThan, s->t_bool, pos, cu(s, 32)));
        uint32_t sp = glsl2(s, GlslUMin, s->t_u32, pos, cu(s, 31));
        uint32_t sl = glsl2(s, GlslUMin, s->t_u32, len, emit2(s, OpISub, s->t_u32, cu(s, 32), sp));
        uint32_t spi = u2i(s, sp), sli = u2i(s, sl);
        uint32_t bfi = new_id(s);
        wb_op(&s->sec_fn, OpBitFieldInsert, 7);
        wb_push(&s->sec_fn, s->t_u32); wb_push(&s->sec_fn, bfi);
        wb_push(&s->sec_fn, base); wb_push(&s->sec_fn, ins); wb_push(&s->sec_fn, spi); wb_push(&s->sec_fn, sli);
        wr_reg(s, d, emit3(s, OpSelect, s->t_u32, ok, bfi, base));
    } else if (OPM(w, 0x5b00000000000000ull, 0xffc0000000000000ull) || OPM(w, 0x4e00000000000000ull, 0xfe00000000000000ull) ||
               OPM(w, 0x3600000000000000ull, 0xfec0000000000000ull) || OPM(w, 0x5100000000000000ull, 0xff80000000000000ull)) {  // XMAD
        uint32_t bv, cv;
        bool hb, psl, mrg, ext, ha = bits(w, 53, 1);
        int cmode;
        uint64_t top = w >> 56;
        if (top == 0x5b) {
            bv = rd_reg(s, (int)bits(w, 20, 8)); cv = rd_reg(s, (int)bits(w, 39, 8));
            hb = bits(w, 35, 1); psl = bits(w, 36, 1); mrg = bits(w, 37, 1); ext = bits(w, 38, 1); cmode = (int)bits(w, 50, 3);
        } else if ((top & 0xfe) == 0x4e) {
            bv = emit_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, 0); cv = rd_reg(s, (int)bits(w, 39, 8));
            hb = bits(w, 52, 1); psl = bits(w, 55, 1); mrg = bits(w, 56, 1); ext = bits(w, 54, 1); cmode = (int)bits(w, 50, 2);
        } else if ((top & 0xfe) == 0x36) {
            bv = cu(s, (uint32_t)bits(w, 20, 16)); cv = rd_reg(s, (int)bits(w, 39, 8));
            hb = false; psl = bits(w, 36, 1); mrg = bits(w, 37, 1); ext = bits(w, 38, 1); cmode = (int)bits(w, 50, 3);
        } else {
            bv = rd_reg(s, (int)bits(w, 39, 8)); cv = emit_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, 0);
            hb = bits(w, 52, 1); psl = false; mrg = false; ext = bits(w, 54, 1); cmode = (int)bits(w, 50, 2);
        }
        bool sa = bits(w, 48, 1), sb = bits(w, 49, 1), wcc = bits(w, 47, 1);
        uint32_t xa = ha ? emit2(s, OpShiftRightLogical, s->t_u32, rd_reg(s, a), cu(s, 16)) : rd_reg(s, a);
        uint32_t xb = hb ? emit2(s, OpShiftRightLogical, s->t_u32, bv, cu(s, 16)) : bv;
        xa = sa ? i2u(s, emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, xa), ci(s, 0), ci(s, 16))) : emit2(s, OpBitwiseAnd, s->t_u32, xa, cu(s, 0xFFFFu));
        xb = sb ? i2u(s, emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, xb), ci(s, 0), ci(s, 16))) : emit2(s, OpBitwiseAnd, s->t_u32, xb, cu(s, 0xFFFFu));
        uint32_t prod = emit2(s, OpIMul, s->t_u32, xa, xb);
        if (psl) prod = emit2(s, OpShiftLeftLogical, s->t_u32, prod, cu(s, 16));
        uint32_t cc_ = cv;
        if (cmode == 1) cc_ = emit2(s, OpBitwiseAnd, s->t_u32, cv, cu(s, 0xFFFFu));
        else if (cmode == 2) cc_ = emit2(s, OpShiftRightLogical, s->t_u32, cv, cu(s, 16));
        else if (cmode == 3) {
            uint32_t s1 = emit2(s, OpShiftLeftLogical, s->t_u32, emit2(s, OpShiftRightLogical, s->t_u32, xa, cu(s, 31)), cu(s, 16));
            uint32_t s2 = emit2(s, OpShiftLeftLogical, s->t_u32, emit2(s, OpShiftRightLogical, s->t_u32, xb, cu(s, 31)), cu(s, 16));
            cc_ = emit2(s, OpISub, s->t_u32, cv, emit2(s, OpIAdd, s->t_u32, s1, s2));
        } else if (cmode == 4) {
            cc_ = emit2(s, OpIAdd, s->t_u32, cv, emit2(s, OpShiftLeftLogical, s->t_u32, bv, cu(s, 16)));
        }
        uint32_t res = ext ? emit2(s, OpIAdd, s->t_u32, prod, emit2(s, OpBitwiseAnd, s->t_u32, load(s, s->t_u32, s->v_cc), cu(s, 1)))
                           : emit2(s, OpIAdd, s->t_u32, prod, cc_);
        if (wcc) store(s, s->v_cc, emit3(s, OpSelect, s->t_u32, emit2(s, OpULessThan, s->t_bool, res, prod), cu(s, 1), cu(s, 0)));
        if (mrg) res = emit2(s, OpBitwiseOr, s->t_u32, emit2(s, OpBitwiseAnd, s->t_u32, res, cu(s, 0xFFFFu)),
                             emit2(s, OpShiftLeftLogical, s->t_u32, bv, cu(s, 16)));
        wr_reg(s, d, res);
    } else if (OPM(w, 0x5090000000000000ull, 0xfff8000000000000ull)) {  // PSETP
        uint32_t v = emit_bool_op(s, (int)bits(w, 24, 2), rd_pred(s, (int)bits(w, 12, 3), bits(w, 15, 1)),
                                  rd_pred(s, (int)bits(w, 29, 3), bits(w, 32, 1)));
        uint32_t pp = rd_pred(s, (int)bits(w, 39, 3), bits(w, 42, 1));
        int bo2 = (int)bits(w, 45, 2);
        wr_pred(s, (int)bits(w, 3, 3), emit_bool_op(s, bo2, v, pp));
        wr_pred(s, (int)bits(w, 0, 3), emit_bool_op(s, bo2, emit1(s, OpLogicalNot, s->t_bool, v), pp));
    } else if (OPM(w, 0x5088000000000000ull, 0xfff8000000000000ull)) {  // PSET
        uint32_t v = emit_bool_op(s, (int)bits(w, 24, 2), rd_pred(s, (int)bits(w, 12, 3), bits(w, 15, 1)),
                                  rd_pred(s, (int)bits(w, 29, 3), bits(w, 32, 1)));
        uint32_t res = emit_bool_op(s, (int)bits(w, 45, 2), v, rd_pred(s, (int)bits(w, 39, 3), bits(w, 42, 1)));
        wr_reg(s, d, emit3(s, OpSelect, s->t_u32, res, cu(s, bits(w, 44, 1) ? 0x3f800000u : 0xFFFFFFFFu), cu(s, 0)));
    } else if (OPM(w, 0xf0c8000000000000ull, 0xfff8000000000000ull)) {  // S2R
        int sr = (int)bits(w, 20, 8);
        wr_reg(s, d, cu(s, sr == 0x12 ? 0xbf800000u : 0u));
    } else if (OPM(w, 0x5c30000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c30000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3830000000000000ull, 0xfef8000000000000ull)) {  // FLO
        uint32_t bv = emit_b(s, w, false);
        if (bits(w, 40, 1)) bv = emit1(s, OpNot, s->t_u32, bv);
        uint32_t msb = bits(w, 48, 1) ? i2u(s, glsl1(s, GlslFindSMsb, s->t_s32, u2i(s, bv)))
                                      : glsl1(s, GlslFindUMsb, s->t_u32, bv);
        if (bits(w, 41, 1)) {
            uint32_t inv = emit2(s, OpISub, s->t_u32, cu(s, 31), msb);
            msb = emit3(s, OpSelect, s->t_u32, emit2(s, OpIEqual, s->t_bool, msb, cu(s, 0xFFFFFFFFu)), cu(s, 0xFFFFFFFFu), inv);
        }
        wr_reg(s, d, msb);
    } else if (OPM(w, 0x5c08000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c08000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3808000000000000ull, 0xfef8000000000000ull)) {  // POPC
        uint32_t bv = emit_b(s, w, false);
        if (bits(w, 40, 1)) bv = emit1(s, OpNot, s->t_u32, bv);
        wr_reg(s, d, emit1(s, OpBitCount, s->t_u32, bv));
    } else if (OPM(w, 0x5c38000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4c38000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x3838000000000000ull, 0xfef8000000000000ull)) {  // IMUL
        uint32_t x = rd_reg(s, a), y = emit_b(s, w, false);
        if (bits(w, 39, 1)) {
            if (bits(w, 40, 1)) wr_reg(s, d, i2u(s, extract(s, s->t_s32, emit2(s, OpSMulExtended, s->t_struct_ii, u2i(s, x), u2i(s, y)), 1)));
            else wr_reg(s, d, extract(s, s->t_u32, emit2(s, OpUMulExtended, s->t_struct_uu, x, y), 1));
        } else wr_reg(s, d, emit2(s, OpIMul, s->t_u32, x, y));
    } else if (OPM(w, 0x1f00000000000000ull, 0xff00000000000000ull)) {  // IMUL32I
        wr_reg(s, d, emit2(s, OpIMul, s->t_u32, rd_reg(s, a), cu(s, (uint32_t)bits(w, 20, 32))));
    } else if (OPM(w, 0x5ce0000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x4ce0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x38e0000000000000ull, 0xfef8000000000000ull)) {  // I2I
        int dt = (int)(bits(w, 8, 2) | (bits(w, 12, 1) << 2)), st = (int)(bits(w, 10, 2) | (bits(w, 13, 1) << 2));
        int sel = (int)bits(w, 41, 2), ssz = st & 3; bool ssg = (st & 4) != 0;
        uint32_t bv = emit_b(s, w, false), sv;
        if (ssz == 0) {
            uint32_t sh = sel ? emit2(s, OpShiftRightLogical, s->t_u32, bv, cu(s, (uint32_t)(sel * 8))) : bv;
            sv = ssg ? emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, sh), ci(s, 0), ci(s, 8)) : u2i(s, emit2(s, OpBitwiseAnd, s->t_u32, sh, cu(s, 0xFFu)));
        } else if (ssz == 1) {
            uint32_t sh = sel ? emit2(s, OpShiftRightLogical, s->t_u32, bv, cu(s, (uint32_t)(sel * 8))) : bv;
            sv = ssg ? emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, sh), ci(s, 0), ci(s, 16)) : u2i(s, emit2(s, OpBitwiseAnd, s->t_u32, sh, cu(s, 0xFFFFu)));
        } else sv = u2i(s, bv);
        if (bits(w, 49, 1)) sv = glsl1(s, GlslSAbs, s->t_s32, sv);
        if (bits(w, 45, 1)) sv = emit1(s, OpSNegate, s->t_s32, sv);
        int dsz = dt & 3; bool dsg = (dt & 4) != 0, sat = bits(w, 50, 1);
        uint32_t res;
        if (dsz == 0) res = sat ? i2u(s, glsl3(s, GlslSClamp, s->t_s32, sv, ci(s, dsg ? -128 : 0), ci(s, dsg ? 127 : 255)))
                                : emit2(s, OpBitwiseAnd, s->t_u32, i2u(s, sv), cu(s, 0xFFu));
        else if (dsz == 1) res = sat ? i2u(s, glsl3(s, GlslSClamp, s->t_s32, sv, ci(s, dsg ? -32768 : 0), ci(s, dsg ? 32767 : 65535)))
                                     : emit2(s, OpBitwiseAnd, s->t_u32, i2u(s, sv), cu(s, 0xFFFFu));
        else res = i2u(s, sv);
        wr_reg(s, d, res);
        if (bits(w, 47, 1)) {
            store(s, s->v_ccz, emit2(s, OpIEqual, s->t_bool, res, cu(s, 0)));
            store(s, s->v_ccs, emit2(s, OpSLessThan, s->t_bool, u2i(s, res), ci(s, 0)));
        }
    } else if (OPM(w, 0x5bc0000000000000ull, 0xfff0000000000000ull) || OPM(w, 0x4bc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(w, 0x36c0000000000000ull, 0xfef0000000000000ull) || OPM(w, 0x53c0000000000000ull, 0xfff0000000000000ull)) {  // PRMT
        uint32_t av = rd_reg(s, a), bv, cv;
        if (OPM(w, 0x53c0000000000000ull, 0xfff0000000000000ull)) {
            bv = rd_reg(s, (int)bits(w, 39, 8)); cv = emit_cbuf(s, (int)bits(w, 34, 5), (int64_t)bits(w, 20, 14) * 4, 0);
        } else { bv = emit_b(s, w, false); cv = rd_reg(s, (int)bits(w, 39, 8)); }
        uint32_t res = cu(s, 0);
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t sl = emit2(s, OpBitwiseAnd, s->t_u32, emit2(s, OpShiftRightLogical, s->t_u32, bv, cu(s, i * 4)), cu(s, 7));
            uint32_t use_a = emit2(s, OpULessThan, s->t_bool, sl, cu(s, 4));
            uint32_t src = emit3(s, OpSelect, s->t_u32, use_a, av, cv);
            uint32_t sh = emit2(s, OpShiftLeftLogical, s->t_u32, emit2(s, OpBitwiseAnd, s->t_u32, sl, cu(s, 3)), cu(s, 3));
            uint32_t bt = emit2(s, OpBitwiseAnd, s->t_u32, emit2(s, OpShiftRightLogical, s->t_u32, src, sh), cu(s, 0xFFu));
            if (i) bt = emit2(s, OpShiftLeftLogical, s->t_u32, bt, cu(s, i * 8));
            res = i ? emit2(s, OpBitwiseOr, s->t_u32, res, bt) : bt;
        }
        wr_reg(s, d, res);
    } else if (OPM(w, 0x38f0000000000000ull, 0xfef8000000000000ull) || OPM(w, 0x5cf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x4cf0000000000000ull, 0xfff8000000000000ull)) {  // R2P
        int shift = (int)bits(w, 41, 2) * 8;
        uint32_t av = rd_reg(s, a), mask = emit_b(s, w, false);
        for (int i = 0; i < 7; i++) {
            uint32_t en = emit2(s, OpINotEqual, s->t_bool, emit2(s, OpBitwiseAnd, s->t_u32, mask, cu(s, 1u << i)), cu(s, 0));
            uint32_t bv = emit2(s, OpINotEqual, s->t_bool, emit2(s, OpBitwiseAnd, s->t_u32, av, cu(s, 1u << (shift + i))), cu(s, 0));
            wr_pred(s, i, emit3(s, OpSelect, s->t_bool, en, bv, load(s, s->t_bool, s->v_pred[i])));
        }
    } else if (OPM(w, 0x38e8000000000000ull, 0xfef8000000000000ull) || OPM(w, 0x5ce8000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0x4ce8000000000000ull, 0xfff8000000000000ull)) {  // P2R
        int shift = (int)bits(w, 41, 2) * 8;
        uint32_t av = rd_reg(s, a), mask = emit_b(s, w, false), pv;
        if (bits(w, 40, 1)) {
            uint32_t z = emit3(s, OpSelect, s->t_u32, load(s, s->t_bool, s->v_ccz), cu(s, 1), cu(s, 0));
            uint32_t sg = emit3(s, OpSelect, s->t_u32, load(s, s->t_bool, s->v_ccs), cu(s, 2), cu(s, 0));
            uint32_t cr = emit2(s, OpShiftLeftLogical, s->t_u32, emit2(s, OpBitwiseAnd, s->t_u32, load(s, s->t_u32, s->v_cc), cu(s, 1)), cu(s, 2));
            pv = emit2(s, OpBitwiseOr, s->t_u32, emit2(s, OpBitwiseOr, s->t_u32, z, sg), cr);
            mask = emit2(s, OpBitwiseAnd, s->t_u32, mask, cu(s, 0xFu));
        } else {
            pv = cu(s, 0);
            for (int i = 0; i < 7; i++) {
                uint32_t bit = emit3(s, OpSelect, s->t_u32, load(s, s->t_bool, s->v_pred[i]), cu(s, 1u << i), cu(s, 0));
                pv = i ? emit2(s, OpBitwiseOr, s->t_u32, pv, bit) : bit;
            }
            mask = emit2(s, OpBitwiseAnd, s->t_u32, mask, cu(s, 0x7Fu));
        }
        uint32_t m = shift ? emit2(s, OpShiftLeftLogical, s->t_u32, mask, cu(s, (uint32_t)shift)) : mask;
        uint32_t ps = shift ? emit2(s, OpShiftLeftLogical, s->t_u32, pv, cu(s, (uint32_t)shift)) : pv;
        wr_reg(s, d, emit2(s, OpBitwiseOr, s->t_u32, emit2(s, OpBitwiseAnd, s->t_u32, av, emit1(s, OpNot, s->t_u32, m)),
                           emit2(s, OpBitwiseAnd, s->t_u32, ps, m)));
    } else if (OPM(w, 0x50a0000000000000ull, 0xfff8000000000000ull)) {  // CSETP
        int test = (int)bits(w, 8, 5);
        uint32_t cond = s->c_false;
        switch (test) {
        case 0: cond = s->c_false; break;
        case 15: cond = s->c_true; break;
        case 2: case 10: cond = load(s, s->t_bool, s->v_ccz); break;
        case 5: case 13: cond = emit1(s, OpLogicalNot, s->t_bool, load(s, s->t_bool, s->v_ccz)); break;
        case 1: case 9: cond = load(s, s->t_bool, s->v_ccs); break;
        case 6: case 14: cond = emit1(s, OpLogicalNot, s->t_bool, load(s, s->t_bool, s->v_ccs)); break;
        case 4: case 12: cond = emit2(s, OpLogicalAnd, s->t_bool, emit1(s, OpLogicalNot, s->t_bool, load(s, s->t_bool, s->v_ccz)),
                                      emit1(s, OpLogicalNot, s->t_bool, load(s, s->t_bool, s->v_ccs))); break;
        case 3: case 11: cond = emit2(s, OpLogicalOr, s->t_bool, load(s, s->t_bool, s->v_ccz), load(s, s->t_bool, s->v_ccs)); break;
        }
        uint32_t pp = rd_pred(s, (int)bits(w, 39, 3), bits(w, 42, 1));
        int bo = (int)bits(w, 45, 2);
        wr_pred(s, (int)bits(w, 3, 3), emit_bool_op(s, bo, cond, pp));
        wr_pred(s, (int)bits(w, 0, 3), emit_bool_op(s, bo, emit1(s, OpLogicalNot, s->t_bool, cond), pp));
    } else if (OPM(w, 0xeb00000000000000ull, 0xffe0000000000000ull) ||
               OPM(w, 0xeb20000000000000ull, 0xffe0000000000000ull)) {  // SULD / SUST
        bool is_st = OPM(w, 0xeb20000000000000ull, 0xffe0000000000000ull);
        uint32_t index = (uint32_t)bits(w, 36, 13);
        int slot = s->tex_slot[index] - 1;
        bool dmode = bits(w, 52, 1);
        uint32_t mask;
        if (dmode) { int sz = (int)bits(w, 20, 3); mask = sz == 4 ? 1u : sz == 5 ? 3u : 15u; }
        else mask = (uint32_t)bits(w, 20, 4);
        uint32_t coord = emit_img_coord_i2(s, slot, rd_reg(s, a), rd_reg(s, a == RZ ? RZ : a + 1));
        uint32_t img = load(s, s->t_simg[slot], s->v_simg[slot]);
        if (!is_st) {
            uint32_t tv = emit2(s, OpImageRead, s->t_v4u, img, coord);
            int n = 0;
            for (int c = 0; c < 4; c++) {
                if (!(mask & (1u << c))) continue;
                int r = d == RZ ? RZ : d + n;
                n++;
                if (r != RZ) wr_reg(s, r, extract(s, s->t_u32, tv, (uint32_t)c));
            }
        } else {
            uint32_t comps[4];
            int n = 0;
            for (int k = 0; k < 4; k++) {
                if (mask & (1u << k)) {
                    int r = d == RZ ? RZ : d + n;
                    n++;
                    comps[k] = rd_reg(s, r);
                } else {
                    comps[k] = cu(s, 0);
                }
            }
            uint32_t tv = vec4(s, s->t_v4u, comps[0], comps[1], comps[2], comps[3]);
            wb_op(&s->sec_fn, OpImageWrite, 4);
            wb_push(&s->sec_fn, img); wb_push(&s->sec_fn, coord); wb_push(&s->sec_fn, tv);
        }
    } else if (OPM(w, 0xea00000000000000ull, 0xffc0000000000000ull)) {  // SUATOM
        static const uint32_t aops[9] = {OpAtomicIAdd, OpAtomicUMin, OpAtomicUMax, 0, 0, OpAtomicAnd, OpAtomicOr, OpAtomicXor, OpAtomicExchange};
        int slot = s->tex_slot[(uint32_t)bits(w, 36, 13)] - 1;
        int op = (int)bits(w, 29, 4), v = (int)bits(w, 20, 8);
        uint32_t coord = emit_img_coord_i2(s, slot, rd_reg(s, a), rd_reg(s, a == RZ ? RZ : a + 1));
        uint32_t c0 = ci(s, 0);
        uint32_t tptr = new_id(s);
        wb_op(&s->sec_fn, OpImageTexelPointer, 6);
        wb_push(&s->sec_fn, s->p_img_u32); wb_push(&s->sec_fn, tptr);
        wb_push(&s->sec_fn, s->v_simg[slot]); wb_push(&s->sec_fn, coord); wb_push(&s->sec_fn, c0);
        uint32_t res = emit_atomic_ptr(s, aops[op], tptr, rd_reg(s, v));
        wr_reg(s, d, res);
    } else if (OPM(w, 0x5cf8000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x38f8000000000000ull, 0xfef8000000000000ull) ||
               OPM(w, 0x5bf8000000000000ull, 0xfff8000000000000ull) || OPM(w, 0x36f8000000000000ull, 0xfef8000000000000ull)) {  // SHF
        bool right = (w >> 56) == 0x5c || (w >> 57) == (0x38 >> 1);
        uint32_t sh = ((w >> 60) == 3) ? emit_b(s, w, false) : rd_reg(s, (int)bits(w, 20, 8));
        uint32_t lo = rd_reg(s, a), hi = rd_reg(s, (int)bits(w, 39, 8));
        uint32_t sh5 = emit2(s, OpBitwiseAnd, s->t_u32, sh, cu(s, 31));
        uint32_t inv = emit2(s, OpBitwiseAnd, s->t_u32, emit2(s, OpISub, s->t_u32, cu(s, 32), sh5), cu(s, 31));
        uint32_t is0 = emit2(s, OpIEqual, s->t_bool, sh5, cu(s, 0));
        uint32_t comb = right
            ? emit2(s, OpBitwiseOr, s->t_u32, emit2(s, OpShiftRightLogical, s->t_u32, lo, sh5), emit2(s, OpShiftLeftLogical, s->t_u32, hi, inv))
            : emit2(s, OpBitwiseOr, s->t_u32, emit2(s, OpShiftLeftLogical, s->t_u32, hi, sh5), emit2(s, OpShiftRightLogical, s->t_u32, lo, inv));
        wr_reg(s, d, emit3(s, OpSelect, s->t_u32, is0, right ? lo : hi, comb));
    } else if (OPM(w, 0xeed0000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0xeed8000000000000ull, 0xfff8000000000000ull)) {  // LDG / STG
        bool is_st = bits(w, 51, 1);
        bool wide = bits(w, 45, 1);
        int sz = (int)bits(w, 48, 3);
        int64_t off = sbits(w, 20, 24);
        int nw = sz == 5 ? 2 : sz >= 6 ? 4 : 1;
        emit_gmem_addr(s, a, wide, off, false);
        uint32_t ok = load(s, s->t_bool, s->v_gok);
        uint32_t lbl_ok = new_id(s), lbl_no = new_id(s), lbl_done = new_id(s);
        emit_sel_merge(s, lbl_done);
        emit_cond_branch(s, ok, lbl_ok, is_st ? lbl_done : lbl_no);
        emit_label(s, lbl_ok);
        uint32_t ga = load(s, s->t_u64, s->v_ga);
        uint32_t wi0 = emit1(s, OpUConvert, s->t_u32, emit2(s, OpShiftRightLogical, s->t_u64, ga, cu64(s, 2)));
        if (!is_st) {
            if (sz < 4) {
                if (d != RZ) {
                    uint32_t wd = emit_gmem_ldw(s, wi0);
                    uint32_t bm = sz < 2 ? 3u : 2u;
                    uint32_t sh = emit2(s, OpShiftLeftLogical, s->t_u32,
                                        emit1(s, OpUConvert, s->t_u32, emit2(s, OpBitwiseAnd, s->t_u64, ga, cu64(s, bm))), cu(s, 3));
                    uint32_t ws = emit2(s, OpShiftRightLogical, s->t_u32, wd, sh);
                    uint32_t val;
                    if (sz == 0) val = emit2(s, OpBitwiseAnd, s->t_u32, ws, cu(s, 0xFFu));
                    else if (sz == 1) val = i2u(s, emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, ws), ci(s, 0), ci(s, 8)));
                    else if (sz == 2) val = emit2(s, OpBitwiseAnd, s->t_u32, ws, cu(s, 0xFFFFu));
                    else val = i2u(s, emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, ws), ci(s, 0), ci(s, 16)));
                    wr_reg(s, d, val);
                }
            } else {
                for (int k = 0; k < nw; k++) {
                    if (d == RZ || d + k >= RZ) continue;
                    uint32_t wi = k ? emit2(s, OpIAdd, s->t_u32, wi0, cu(s, (uint32_t)k)) : wi0;
                    wr_reg(s, d + k, emit_gmem_ldw(s, wi));
                }
            }
            emit_branch(s, lbl_done);
            emit_label(s, lbl_no);
            if (sz < 4) {
                if (d != RZ) wr_reg(s, d, cu(s, 0));
            } else {
                for (int k = 0; k < nw; k++) if (d != RZ && d + k < RZ) wr_reg(s, d + k, cu(s, 0));
            }
            emit_branch(s, lbl_done);
        } else {
            if (sz < 4) {
                uint32_t m = sz < 2 ? 0xFFu : 0xFFFFu;
                uint32_t bm = sz < 2 ? 3u : 2u;
                uint32_t sh = emit2(s, OpShiftLeftLogical, s->t_u32,
                                    emit1(s, OpUConvert, s->t_u32, emit2(s, OpBitwiseAnd, s->t_u64, ga, cu64(s, bm))), cu(s, 3));
                uint32_t mk = emit1(s, OpNot, s->t_u32, emit2(s, OpShiftLeftLogical, s->t_u32, cu(s, m), sh));
                uint32_t v = emit2(s, OpShiftLeftLogical, s->t_u32, emit2(s, OpBitwiseAnd, s->t_u32, rd_reg(s, d), cu(s, m)), sh);
                (void)emit_gmem_atomw(s, OpAtomicAnd, wi0, mk);
                (void)emit_gmem_atomw(s, OpAtomicOr, wi0, v);
            } else {
                for (int k = 0; k < nw; k++) {
                    uint32_t wi = k ? emit2(s, OpIAdd, s->t_u32, wi0, cu(s, (uint32_t)k)) : wi0;
                    emit_gmem_stw(s, wi, rd_reg(s, d == RZ ? RZ : d + k));
                }
            }
            emit_branch(s, lbl_done);
        }
        emit_label(s, lbl_done);
    } else if (OPM(w, 0xed00000000000000ull, 0xff00000000000000ull)) {  // ATOM
        static const uint32_t aops[9] = {OpAtomicIAdd, OpAtomicUMin, OpAtomicUMax, 0, 0, OpAtomicAnd, OpAtomicOr, OpAtomicXor, OpAtomicExchange};
        int b = (int)bits(w, 20, 8), op = (int)bits(w, 52, 4);
        bool wide = bits(w, 48, 1);
        int64_t off = sbits(w, 28, 20);
        emit_gmem_addr(s, a, wide, off, true);
        uint32_t ok = load(s, s->t_bool, s->v_gok);
        uint32_t lbl_ok = new_id(s), lbl_no = new_id(s), lbl_done = new_id(s);
        emit_sel_merge(s, lbl_done);
        emit_cond_branch(s, ok, lbl_ok, lbl_no);
        emit_label(s, lbl_ok);
        uint32_t wi = emit1(s, OpUConvert, s->t_u32, emit2(s, OpShiftRightLogical, s->t_u64, load(s, s->t_u64, s->v_ga), cu64(s, 2)));
        uint32_t av = emit_gmem_atomw(s, aops[op], wi, rd_reg(s, b));
        if (d != RZ) wr_reg(s, d, av);
        emit_branch(s, lbl_done);
        emit_label(s, lbl_no);
        if (d != RZ) wr_reg(s, d, cu(s, 0));
        emit_branch(s, lbl_done);
        emit_label(s, lbl_done);
    } else if (OPM(w, 0x50f8000000000000ull, 0xfff8000000000000ull)) {  // FSWZADD
        uint32_t swz = (uint32_t)bits(w, 28, 8);
        uint32_t fa = u2f(s, rd_reg(s, a)), fb = u2f(s, rd_reg(s, (int)bits(w, 20, 8)));
        if (s->stage != STAGE_FRAGMENT) {
            wr_reg(s, d, f2u(s, emit_f2(s, OpFAdd, s->t_f32, fa, fb)));
        } else {
            uint32_t lane = load(s, s->t_u32, s->v_lane);
            uint32_t sh = emit2(s, OpShiftLeftLogical, s->t_u32, emit2(s, OpBitwiseAnd, s->t_u32, lane, cu(s, 3)), cu(s, 1));
            uint32_t m = emit2(s, OpBitwiseAnd, s->t_u32, emit2(s, OpShiftRightLogical, s->t_u32, cu(s, swz), sh), cu(s, 3));
            uint32_t ma = emit3(s, OpSelect, s->t_f32, emit2(s, OpIEqual, s->t_bool, m, cu(s, 1)), cf(s, -1.0f),
                                emit3(s, OpSelect, s->t_f32, emit2(s, OpIEqual, s->t_bool, m, cu(s, 3)), cf(s, 0.0f), cf(s, 1.0f)));
            uint32_t mb = emit3(s, OpSelect, s->t_f32, emit2(s, OpIEqual, s->t_bool, m, cu(s, 2)), cf(s, -1.0f), cf(s, 1.0f));
            uint32_t res = emit_f2(s, OpFAdd, s->t_f32, emit_f2(s, OpFMul, s->t_f32, fa, ma), emit_f2(s, OpFMul, s->t_f32, fb, mb));
            wr_reg(s, d, f2u(s, res));
        }
    } else if (OPM(w, 0xef40000000000000ull, 0xfff8000000000000ull) ||
               OPM(w, 0xef50000000000000ull, 0xfff8000000000000ull)) {  // LDL / STL
        bool is_st = bits(w, 52, 1);
        int sz = (int)bits(w, 48, 3);
        uint32_t off = (uint32_t)(int32_t)sbits(w, 20, 24);
        int nw = sz == 5 ? 2 : sz >= 6 ? 4 : 1;
        uint32_t la = a == RZ ? cu(s, off) : (off ? emit2(s, OpIAdd, s->t_u32, rd_reg(s, a), cu(s, off)) : rd_reg(s, a));
        uint32_t wi0 = emit2(s, OpShiftRightLogical, s->t_u32, la, cu(s, 2));
        if (!is_st) {
            if (sz < 4) {
                if (d != RZ) {
                    uint32_t idx = emit2(s, OpBitwiseAnd, s->t_u32, wi0, cu(s, SPV_LMEM_WORDS - 1));
                    uint32_t v = load(s, s->t_u32, ac1(s, s->p_fn_u32, s->v_lmem, idx));
                    uint32_t sh = emit2(s, OpShiftLeftLogical, s->t_u32, emit2(s, OpBitwiseAnd, s->t_u32, la, cu(s, 3)), cu(s, 3));
                    uint32_t vs = emit2(s, OpShiftRightLogical, s->t_u32, v, sh);
                    uint32_t val;
                    if (sz == 0) val = emit2(s, OpBitwiseAnd, s->t_u32, vs, cu(s, 0xFFu));
                    else if (sz == 1) val = i2u(s, emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, vs), ci(s, 0), ci(s, 8)));
                    else if (sz == 2) val = emit2(s, OpBitwiseAnd, s->t_u32, vs, cu(s, 0xFFFFu));
                    else val = i2u(s, emit3(s, OpBitFieldSExtract, s->t_s32, u2i(s, vs), ci(s, 0), ci(s, 16)));
                    wr_reg(s, d, val);
                }
            } else {
                for (int k = 0; k < nw; k++) {
                    if (d == RZ || d + k >= RZ) continue;
                    uint32_t wi = k ? emit2(s, OpIAdd, s->t_u32, wi0, cu(s, (uint32_t)k)) : wi0;
                    uint32_t idx = emit2(s, OpBitwiseAnd, s->t_u32, wi, cu(s, SPV_LMEM_WORDS - 1));
                    wr_reg(s, d + k, load(s, s->t_u32, ac1(s, s->p_fn_u32, s->v_lmem, idx)));
                }
            }
        } else {
            if (sz < 4) {
                uint32_t m = sz < 2 ? 0xFFu : 0xFFFFu;
                uint32_t sh = emit2(s, OpShiftLeftLogical, s->t_u32, emit2(s, OpBitwiseAnd, s->t_u32, la, cu(s, 3)), cu(s, 3));
                uint32_t idx = emit2(s, OpBitwiseAnd, s->t_u32, wi0, cu(s, SPV_LMEM_WORDS - 1));
                uint32_t ptr = ac1(s, s->p_fn_u32, s->v_lmem, idx);
                uint32_t old = load(s, s->t_u32, ptr);
                uint32_t mk = emit1(s, OpNot, s->t_u32, emit2(s, OpShiftLeftLogical, s->t_u32, cu(s, m), sh));
                uint32_t ins = emit2(s, OpShiftLeftLogical, s->t_u32, emit2(s, OpBitwiseAnd, s->t_u32, rd_reg(s, d), cu(s, m)), sh);
                store(s, ptr, emit2(s, OpBitwiseOr, s->t_u32, emit2(s, OpBitwiseAnd, s->t_u32, old, mk), ins));
            } else {
                for (int k = 0; k < nw; k++) {
                    uint32_t wi = k ? emit2(s, OpIAdd, s->t_u32, wi0, cu(s, (uint32_t)k)) : wi0;
                    uint32_t idx = emit2(s, OpBitwiseAnd, s->t_u32, wi, cu(s, SPV_LMEM_WORDS - 1));
                    store(s, ac1(s, s->p_fn_u32, s->v_lmem, idx), rd_reg(s, d == RZ ? RZ : d + k));
                }
            }
        }
    } else if (OPM(w, 0xef10000000000000ull, 0xfff8000000000000ull)) {  // SHFL
        int mode = (int)bits(w, 30, 2);
        bool imm = bits(w, 28, 1);
        uint32_t lm = (uint32_t)bits(w, 20, 5);
        int pd = (int)bits(w, 48, 3);
        uint32_t av = rd_reg(s, a);
        if (s->stage == STAGE_VERTEX) {
            wr_reg(s, d, av);
        } else {
            uint32_t sc = cu(s, ScopeSubgroup);
            uint32_t res;
            if (s->quad_shfl && mode == 3 && imm && lm >= 1 && lm <= 3) {
                uint32_t dir = cu(s, lm - 1);
                res = emit3(s, OpGroupNonUniformQuadSwap, s->t_u32, sc, av, dir);
            } else {
                static const uint32_t sops[4] = {
                    OpGroupNonUniformShuffle, OpGroupNonUniformShuffleUp,
                    OpGroupNonUniformShuffleDown, OpGroupNonUniformShuffleXor};
                uint32_t bv = imm ? cu(s, lm) : rd_reg(s, (int)bits(w, 20, 8));
                res = emit3(s, sops[mode], s->t_u32, sc, av, bv);
            }
            wr_reg(s, d, res);
        }
        if (pd != PT) wr_pred(s, pd, s->c_true);
    } else {
        emit_half_spv(s, w);
    }

    if (guarded) {
        if (!s->blk_terminated) emit_branch(s, lbl_after);
        emit_label(s, lbl_after);
    }
}

// ------------------------------------------------------------------ Declarations & module assembly
static uint32_t def_type_ptr(Spv *s, uint32_t sc, uint32_t pointee) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_types, OpTypePointer, 4); wb_push(&s->sec_types, id); wb_push(&s->sec_types, sc); wb_push(&s->sec_types, pointee);
    return id;
}
static uint32_t def_global(Spv *s, uint32_t pty, uint32_t sc) {
    uint32_t id = new_id(s);
    wb_op(&s->sec_vars, OpVariable, 4); wb_push(&s->sec_vars, pty); wb_push(&s->sec_vars, id); wb_push(&s->sec_vars, sc);
    add_iface(s, id);
    return id;
}

static void init_types_and_globals(Spv *s) {
    s->t_void = new_id(s); wb_op(&s->sec_types, OpTypeVoid, 2); wb_push(&s->sec_types, s->t_void);
    s->t_bool = new_id(s); wb_op(&s->sec_types, OpTypeBool, 2); wb_push(&s->sec_types, s->t_bool);
    s->t_u32  = new_id(s); wb_op(&s->sec_types, OpTypeInt, 4); wb_push(&s->sec_types, s->t_u32); wb_push(&s->sec_types, 32); wb_push(&s->sec_types, 0);
    s->t_s32  = new_id(s); wb_op(&s->sec_types, OpTypeInt, 4); wb_push(&s->sec_types, s->t_s32); wb_push(&s->sec_types, 32); wb_push(&s->sec_types, 1);
    s->t_f32  = new_id(s); wb_op(&s->sec_types, OpTypeFloat, 3); wb_push(&s->sec_types, s->t_f32); wb_push(&s->sec_types, 32);
    if (s->info->uses_gmem) {
        s->t_u64 = new_id(s); wb_op(&s->sec_types, OpTypeInt, 4); wb_push(&s->sec_types, s->t_u64); wb_push(&s->sec_types, 64); wb_push(&s->sec_types, 0);
    }

    s->t_v2f = new_id(s); wb_op(&s->sec_types, OpTypeVector, 4); wb_push(&s->sec_types, s->t_v2f); wb_push(&s->sec_types, s->t_f32); wb_push(&s->sec_types, 2);
    s->t_v3f = new_id(s); wb_op(&s->sec_types, OpTypeVector, 4); wb_push(&s->sec_types, s->t_v3f); wb_push(&s->sec_types, s->t_f32); wb_push(&s->sec_types, 3);
    s->t_v4f = new_id(s); wb_op(&s->sec_types, OpTypeVector, 4); wb_push(&s->sec_types, s->t_v4f); wb_push(&s->sec_types, s->t_f32); wb_push(&s->sec_types, 4);
    s->t_v2u = new_id(s); wb_op(&s->sec_types, OpTypeVector, 4); wb_push(&s->sec_types, s->t_v2u); wb_push(&s->sec_types, s->t_u32); wb_push(&s->sec_types, 2);
    s->t_v3u = new_id(s); wb_op(&s->sec_types, OpTypeVector, 4); wb_push(&s->sec_types, s->t_v3u); wb_push(&s->sec_types, s->t_u32); wb_push(&s->sec_types, 3);
    s->t_v4u = new_id(s); wb_op(&s->sec_types, OpTypeVector, 4); wb_push(&s->sec_types, s->t_v4u); wb_push(&s->sec_types, s->t_u32); wb_push(&s->sec_types, 4);
    s->t_v2i = new_id(s); wb_op(&s->sec_types, OpTypeVector, 4); wb_push(&s->sec_types, s->t_v2i); wb_push(&s->sec_types, s->t_s32); wb_push(&s->sec_types, 2);
    s->t_v3i = new_id(s); wb_op(&s->sec_types, OpTypeVector, 4); wb_push(&s->sec_types, s->t_v3i); wb_push(&s->sec_types, s->t_s32); wb_push(&s->sec_types, 3);
    s->t_v4i = new_id(s); wb_op(&s->sec_types, OpTypeVector, 4); wb_push(&s->sec_types, s->t_v4i); wb_push(&s->sec_types, s->t_s32); wb_push(&s->sec_types, 4);

    s->t_struct_uu = new_id(s); wb_op(&s->sec_types, OpTypeStruct, 4); wb_push(&s->sec_types, s->t_struct_uu); wb_push(&s->sec_types, s->t_u32); wb_push(&s->sec_types, s->t_u32);
    s->t_struct_ii = new_id(s); wb_op(&s->sec_types, OpTypeStruct, 4); wb_push(&s->sec_types, s->t_struct_ii); wb_push(&s->sec_types, s->t_s32); wb_push(&s->sec_types, s->t_s32);

    s->p_fn_u32  = def_type_ptr(s, ScFunction, s->t_u32);
    s->p_fn_bool = def_type_ptr(s, ScFunction, s->t_bool);
    if (s->info->uses_gmem) s->p_fn_u64 = def_type_ptr(s, ScFunction, s->t_u64);
    s->p_in_f32  = def_type_ptr(s, ScInput, s->t_f32);
    s->p_in_s32  = def_type_ptr(s, ScInput, s->t_s32);
    if (s->uses_lane) s->p_in_u32 = def_type_ptr(s, ScInput, s->t_u32);
    s->p_in_bool = def_type_ptr(s, ScInput, s->t_bool);
    s->p_in_v2f  = def_type_ptr(s, ScInput, s->t_v2f);
    s->p_in_v4f  = def_type_ptr(s, ScInput, s->t_v4f);
    s->p_in_v4i  = def_type_ptr(s, ScInput, s->t_v4i);
    s->p_in_v4u  = def_type_ptr(s, ScInput, s->t_v4u);
    s->p_out_f32 = def_type_ptr(s, ScOutput, s->t_f32);
    s->p_out_v4f = def_type_ptr(s, ScOutput, s->t_v4f);
    s->p_out_v4i = def_type_ptr(s, ScOutput, s->t_v4i);
    s->p_out_v4u = def_type_ptr(s, ScOutput, s->t_v4u);
    s->p_uni_u32 = def_type_ptr(s, ScUniform, s->t_u32);
    s->p_uni_f32 = def_type_ptr(s, ScUniform, s->t_f32);
    s->p_uni_v4u = def_type_ptr(s, ScUniform, s->t_v4u);
    s->p_uni_v4f = def_type_ptr(s, ScUniform, s->t_v4f);

    if (s->uses_local) {
        uint32_t clm = cu(s, SPV_LMEM_WORDS);
        s->t_lmem_arr = new_id(s);
        wb_op(&s->sec_types, OpTypeArray, 4); wb_push(&s->sec_types, s->t_lmem_arr); wb_push(&s->sec_types, s->t_u32); wb_push(&s->sec_types, clm);
        s->c_lmem_null = new_id(s);
        wb_op(&s->sec_types, OpConstantNull, 3); wb_push(&s->sec_types, s->t_lmem_arr); wb_push(&s->sec_types, s->c_lmem_null);
        s->p_fn_lmem = def_type_ptr(s, ScFunction, s->t_lmem_arr);
    }

    s->c_false = new_id(s); wb_op(&s->sec_types, OpConstantFalse, 3); wb_push(&s->sec_types, s->t_bool); wb_push(&s->sec_types, s->c_false);
    s->c_true  = new_id(s); wb_op(&s->sec_types, OpConstantTrue, 3);  wb_push(&s->sec_types, s->t_bool); wb_push(&s->sec_types, s->c_true);

    uint32_t set = s->stage == STAGE_FRAGMENT ? 1u : 0u;

    // Uniform blocks: CB0..CB17
    for (int i = 0; i < 18; i++) {
        if (!(s->info->cbuf_mask & (1u << i))) continue;
        uint32_t n = s->info->cbuf_size[i] / 16;
        if (s->cbuf_need[i] > n) n = s->cbuf_need[i];
        if ((s->info->cbuf_dynamic & (1u << i)) && n < 4096) n = 4096;
        if (!n) n = 1;
        s->cbuf_arr_len[i] = n;
        uint32_t cn = cu(s, n);
        uint32_t t_arr = new_id(s);
        wb_op(&s->sec_types, OpTypeArray, 4); wb_push(&s->sec_types, t_arr); wb_push(&s->sec_types, s->t_v4u); wb_push(&s->sec_types, cn);
        dec1(s, t_arr, DecArrayStride, 16);
        uint32_t t_blk = new_id(s);
        wb_op(&s->sec_types, OpTypeStruct, 3); wb_push(&s->sec_types, t_blk); wb_push(&s->sec_types, t_arr);
        dec0(s, t_blk, DecBlock);
        mdec1(s, t_blk, 0, DecOffset, 0);
        uint32_t p_blk = def_type_ptr(s, ScUniform, t_blk);
        s->v_cb[i] = def_global(s, p_blk, ScUniform);
        dec1(s, s->v_cb[i], DecDescriptorSet, set);
        dec1(s, s->v_cb[i], DecBinding, (uint32_t)i);
    }

    // Global memory SSBOs: gmem_ (17) + optional paged pool pmem_ (23), pmemh_ (25), ptab_ (24)
    if (s->info->uses_gmem) {
        uint32_t t_rta = new_id(s);
        wb_op(&s->sec_types, OpTypeRuntimeArray, 3); wb_push(&s->sec_types, t_rta); wb_push(&s->sec_types, s->t_u32);
        dec1(s, t_rta, DecArrayStride, 4);
        s->p_sb_u32 = def_type_ptr(s, ScStorageBuffer, s->t_u32);

        uint32_t t_gblk = new_id(s);
        wb_op(&s->sec_types, OpTypeStruct, 3); wb_push(&s->sec_types, t_gblk); wb_push(&s->sec_types, t_rta);
        dec0(s, t_gblk, DecBlock); mdec1(s, t_gblk, 0, DecOffset, 0);
        if (!s->info->uses_gmem_store) mdec0(s, t_gblk, 0, DecNonWritable);
        uint32_t p_gblk = def_type_ptr(s, ScStorageBuffer, t_gblk);

        s->v_gmem = def_global(s, p_gblk, ScStorageBuffer);
        dec1(s, s->v_gmem, DecDescriptorSet, set);
        dec1(s, s->v_gmem, DecBinding, GMEM_BUFFER_INDEX);
        if (s->uses_pmem) {
            s->v_pmem = def_global(s, p_gblk, ScStorageBuffer);
            dec1(s, s->v_pmem, DecDescriptorSet, set);
            dec1(s, s->v_pmem, DecBinding, PMEM_BUFFER_INDEX);
            s->v_pmemh = def_global(s, p_gblk, ScStorageBuffer);
            dec1(s, s->v_pmemh, DecDescriptorSet, set);
            dec1(s, s->v_pmemh, DecBinding, PMEM_HI_BUFFER_INDEX);
            uint32_t p_ptblk = p_gblk;
            if (s->info->uses_gmem_store) {
                uint32_t t_ro = new_id(s);
                wb_op(&s->sec_types, OpTypeStruct, 3); wb_push(&s->sec_types, t_ro); wb_push(&s->sec_types, t_rta);
                dec0(s, t_ro, DecBlock); mdec1(s, t_ro, 0, DecOffset, 0); mdec0(s, t_ro, 0, DecNonWritable);
                p_ptblk = def_type_ptr(s, ScStorageBuffer, t_ro);
            }
            s->v_ptab = def_global(s, p_ptblk, ScStorageBuffer);
            dec1(s, s->v_ptab, DecDescriptorSet, set);
            dec1(s, s->v_ptab, DecBinding, PTAB_BUFFER_INDEX);
        }
    }

    // RScale (binding 18)
    if (s->uses_rscale && s->stage != STAGE_VERTEX) {
        s->info->uses_rscale = true;
        uint32_t n = (s->key && s->key->flip_x) ? 66u : 65u;
        uint32_t cn = cu(s, n);
        uint32_t t_arr = new_id(s);
        wb_op(&s->sec_types, OpTypeArray, 4); wb_push(&s->sec_types, t_arr); wb_push(&s->sec_types, s->t_v4f); wb_push(&s->sec_types, cn);
        dec1(s, t_arr, DecArrayStride, 16);
        uint32_t t_blk = new_id(s);
        wb_op(&s->sec_types, OpTypeStruct, 3); wb_push(&s->sec_types, t_blk); wb_push(&s->sec_types, t_arr);
        dec0(s, t_blk, DecBlock); mdec1(s, t_blk, 0, DecOffset, 0);
        s->v_rscale = def_global(s, def_type_ptr(s, ScUniform, t_blk), ScUniform);
        dec1(s, s->v_rscale, DecDescriptorSet, set);
        dec1(s, s->v_rscale, DecBinding, 18);
    }

    // Tint (binding 19)
    if (s->uses_tint) {
        s->info->uses_tint = true;
        uint32_t c2 = cu(s, 2);
        uint32_t t_arr = new_id(s);
        wb_op(&s->sec_types, OpTypeArray, 4); wb_push(&s->sec_types, t_arr); wb_push(&s->sec_types, s->t_v4f); wb_push(&s->sec_types, c2);
        dec1(s, t_arr, DecArrayStride, 16);
        uint32_t t_blk = new_id(s);
        wb_op(&s->sec_types, OpTypeStruct, 3); wb_push(&s->sec_types, t_blk); wb_push(&s->sec_types, t_arr);
        dec0(s, t_blk, DecBlock); mdec1(s, t_blk, 0, DecOffset, 0);
        s->v_tint = def_global(s, def_type_ptr(s, ScUniform, t_blk), ScUniform);
        dec1(s, s->v_tint, DecDescriptorSet, set);
        dec1(s, s->v_tint, DecBinding, 19);
    }

    // Combined image samplers (binding 32 + i), deduplicating non-aggregate Image/SampledImage/Pointer types
    struct { uint32_t key, t_img, t_samp, p_samp; } img_cache[32];
    int n_img_cache = 0;
    for (int i = 0; i < s->info->ntex; i++) {
        ShaderTexture *t = &s->info->tex[i];
        uint32_t dim = Dim2D, arr = 0;
        switch (t->type) {
        case TEXTYPE_1D: dim = Dim1D; arr = 0; break;
        case TEXTYPE_1D_ARRAY: dim = Dim1D; arr = 1; break;
        case TEXTYPE_2D: dim = Dim2D; arr = 0; break;
        case TEXTYPE_2D_ARRAY: dim = Dim2D; arr = 1; break;
        case TEXTYPE_3D: dim = Dim3D; arr = 0; break;
        case TEXTYPE_CUBE: dim = DimCube; arr = 0; break;
        case TEXTYPE_CUBE_ARRAY: dim = DimCube; arr = 1; break;
        default: dim = Dim2D; arr = 0; break;
        }
        bool is_u = t->is_uint && !t->shadow;
        uint32_t sty = is_u ? s->t_u32 : s->t_f32;
        uint32_t ikey = (dim << 4) | (arr << 2) | ((t->shadow ? 1u : 0u) << 1) | (is_u ? 1u : 0u);
        uint32_t p_samp = 0;
        for (int k = 0; k < n_img_cache; k++) {
            if (img_cache[k].key == ikey) {
                s->t_img[i] = img_cache[k].t_img;
                s->t_samp[i] = img_cache[k].t_samp;
                p_samp = img_cache[k].p_samp;
                break;
            }
        }
        if (!p_samp) {
            s->t_img[i] = new_id(s);
            wb_op(&s->sec_types, OpTypeImage, 9);
            wb_push(&s->sec_types, s->t_img[i]); wb_push(&s->sec_types, sty); wb_push(&s->sec_types, dim);
            wb_push(&s->sec_types, t->shadow ? 1u : 0u); wb_push(&s->sec_types, arr); wb_push(&s->sec_types, 0u);
            wb_push(&s->sec_types, 1u); wb_push(&s->sec_types, 0u);
            s->t_samp[i] = new_id(s);
            wb_op(&s->sec_types, OpTypeSampledImage, 3); wb_push(&s->sec_types, s->t_samp[i]); wb_push(&s->sec_types, s->t_img[i]);
            p_samp = def_type_ptr(s, ScUniformConstant, s->t_samp[i]);
            if (n_img_cache < 32) {
                img_cache[n_img_cache].key = ikey;
                img_cache[n_img_cache].t_img = s->t_img[i];
                img_cache[n_img_cache].t_samp = s->t_samp[i];
                img_cache[n_img_cache].p_samp = p_samp;
                n_img_cache++;
            }
        }
        s->v_tex[i] = def_global(s, p_samp, ScUniformConstant);
        dec1(s, s->v_tex[i], DecDescriptorSet, set);
        dec1(s, s->v_tex[i], DecBinding, (uint32_t)(32 + i));
    }

    // Storage images (binding 100 + i) for SULD / SUST / SUATOM
    uint32_t t_simg_unk = 0, p_simg_unk = 0, t_simg_r32 = 0, p_simg_r32 = 0;
    for (int i = 0; i < s->info->ntex; i++) {
        ShaderTexture *t = &s->info->tex[i];
        if (!t->is_image) continue;
        uint32_t ti, pi;
        if (t->image_atomic) {
            if (!t_simg_r32) {
                t_simg_r32 = new_id(s);
                wb_op(&s->sec_types, OpTypeImage, 9);
                wb_push(&s->sec_types, t_simg_r32); wb_push(&s->sec_types, s->t_u32); wb_push(&s->sec_types, Dim2D);
                wb_push(&s->sec_types, 0u); wb_push(&s->sec_types, 0u); wb_push(&s->sec_types, 0u);
                wb_push(&s->sec_types, 2u); wb_push(&s->sec_types, ImgFmtR32ui);
                p_simg_r32 = def_type_ptr(s, ScUniformConstant, t_simg_r32);
                if (!s->p_img_u32) s->p_img_u32 = def_type_ptr(s, ScImage, s->t_u32);
            }
            ti = t_simg_r32; pi = p_simg_r32;
        } else {
            if (!t_simg_unk) {
                t_simg_unk = new_id(s);
                wb_op(&s->sec_types, OpTypeImage, 9);
                wb_push(&s->sec_types, t_simg_unk); wb_push(&s->sec_types, s->t_u32); wb_push(&s->sec_types, Dim2D);
                wb_push(&s->sec_types, 0u); wb_push(&s->sec_types, 0u); wb_push(&s->sec_types, 0u);
                wb_push(&s->sec_types, 2u); wb_push(&s->sec_types, ImgFmtUnknown);
                p_simg_unk = def_type_ptr(s, ScUniformConstant, t_simg_unk);
            }
            ti = t_simg_unk; pi = p_simg_unk;
        }
        s->t_simg[i] = ti;
        s->v_simg[i] = def_global(s, pi, ScUniformConstant);
        dec1(s, s->v_simg[i], DecDescriptorSet, set);
        dec1(s, s->v_simg[i], DecBinding, (uint32_t)(100 + i));
    }

    // Stage I/O and built-ins
    if (s->stage == STAGE_VERTEX) {
        for (int i = 0; i < 32; i++) {
            if (!(s->in_attr_mask & (1u << i))) continue;
            int kind = s->key ? s->key->attr_kind[i] : 0;
            uint32_t pty = kind == 1 ? s->p_in_v4i : kind == 2 ? s->p_in_v4u : s->p_in_v4f;
            s->v_in[i] = def_global(s, pty, ScInput);
            dec1(s, s->v_in[i], DecLocation, (uint32_t)i);
        }
        if (s->uses_vid) { s->v_vid = def_global(s, s->p_in_s32, ScInput); dec1(s, s->v_vid, DecBuiltIn, BiVertexIndex); }
        if (s->uses_iid) { s->v_iid = def_global(s, s->p_in_s32, ScInput); dec1(s, s->v_iid, DecBuiltIn, BiInstanceIndex); }
        // gl_PerVertex { vec4 Position; float PointSize; }
        uint32_t t_pv = new_id(s);
        wb_op(&s->sec_types, OpTypeStruct, 4); wb_push(&s->sec_types, t_pv); wb_push(&s->sec_types, s->t_v4f); wb_push(&s->sec_types, s->t_f32);
        dec0(s, t_pv, DecBlock);
        mdec1(s, t_pv, 0, DecBuiltIn, BiPosition);
        mdec0(s, t_pv, 0, DecInvariant);
        mdec1(s, t_pv, 1, DecBuiltIn, BiPointSize);
        s->v_pervertex = def_global(s, def_type_ptr(s, ScOutput, t_pv), ScOutput);
        for (int i = 0; i < 32; i++) {
            if (!(s->out_attr_mask & (1u << i))) continue;
            s->v_out[i] = def_global(s, s->p_out_v4f, ScOutput);
            dec1(s, s->v_out[i], DecLocation, (uint32_t)i);
        }
    } else {
        if (s->uses_frag_coord || s->omap_depth) {
            s->v_fragcoord = def_global(s, s->p_in_v4f, ScInput);
            dec1(s, s->v_fragcoord, DecBuiltIn, BiFragCoord);
        }
        if (s->uses_front_facing) {
            s->v_frontfacing = def_global(s, s->p_in_bool, ScInput);
            dec1(s, s->v_frontfacing, DecBuiltIn, BiFrontFacing);
        }
        if (s->uses_point_coord) {
            s->v_pointcoord = def_global(s, s->p_in_v2f, ScInput);
            dec1(s, s->v_pointcoord, DecBuiltIn, BiPointCoord);
        }
        if (s->uses_lane) {
            s->v_lane = def_global(s, s->p_in_u32, ScInput);
            dec1(s, s->v_lane, DecBuiltIn, BiSubgroupLocalInvocationId);
            dec0(s, s->v_lane, DecFlat);
        }
        for (int i = 0; i < 32; i++) {
            if (!(s->in_attr_mask & (1u << i))) continue;
            s->v_in[i] = def_global(s, s->p_in_v4f, ScInput);
            dec1(s, s->v_in[i], DecLocation, (uint32_t)i);
            if (s->flat_mask & (1u << i)) dec0(s, s->v_in[i], DecFlat);
            else if (s->noperspective_mask & (1u << i)) dec0(s, s->v_in[i], DecNoPerspective);
        }
        for (int rt = 0; rt < 8; rt++) {
            if (!((s->omap_target >> (rt * 4)) & 0xF)) continue;
            int kind = s->key ? s->key->rt_kind[rt] : 0;
            uint32_t pty = kind == 1 ? s->p_out_v4i : kind == 2 ? s->p_out_v4u : s->p_out_v4f;
            s->v_out[rt] = def_global(s, pty, ScOutput);
            dec1(s, s->v_out[rt], DecLocation, (uint32_t)rt);
        }
        if (s->omap_depth) {
            s->info->writes_depth = true;
            s->v_fragdepth = def_global(s, s->p_out_f32, ScOutput);
            dec1(s, s->v_fragdepth, DecBuiltIn, BiFragDepth);
        }
    }
}

ShaderInfo *maxwell_translate_spirv(const uint8_t *blob, size_t max_size, size_t const_size, int stage, const ShaderKey *key) {
    (void)const_size;
    if (!key || !key->glsl) return NULL;
    if (stage != STAGE_VERTEX && stage != STAGE_FRAGMENT) return NULL;
    if (stage == STAGE_VERTEX && key->xfb_n) return NULL;
    if (maxwell_dbg_code || maxwell_dbg_vs) return NULL;

    static int d16 = -1, rsc_pix = -1, nodisc = -1, fb_log = -1, q_shfl = -1, e_fs = -1;
    if (d16 < 0) { const char *e = getenv("XCDE_D16_REF"); d16 = !(e && *e == '0'); }
    if (rsc_pix < 0) { const char *e = getenv("XCDE_RSC_PIXEL"); rsc_pix = !(e && *e == '0'); }
    if (nodisc < 0) nodisc = getenv("LARECOMP_NO_DISCARD") != NULL;
    if (fb_log < 0) fb_log = getenv("XC3_SPV_FALLBACK_LOG") != NULL;
    if (q_shfl < 0) { const char *e = getenv("XC3_QUAD_SHFL"); q_shfl = !(e && *e == '0'); }
    if (e_fs < 0) e_fs = !getenv("XCDE_EARLY_FS") || atoi(getenv("XCDE_EARLY_FS")) > 0;

    const uint8_t *sph = blob + 0x30;
    const uint64_t *code = (const uint64_t *)(blob + 0x80);
    if (max_size <= 0x80) return NULL;
    size_t n = (max_size - 0x80) / 8;
    if (n > 65536) n = 65536;
    size_t end = 0;
    for (size_t i = 0; i < n; i++) {
        if (i % 4 == 0) continue;
        uint64_t w = code[i];
        if (OPM(w, 0xe240000000000000ull, 0xfff0000000000000ull) && sbits(w, 20, 24) == -8 && end) break;
        if (w == 0 && i > 4) break;
        end = i + 1;
    }
    if (!end) return NULL;

    // Collect branch targets; fall back on BRX or unresolvable SSY/PBK/PCNT stacks.
    uint8_t *label = calloc(end + 4, 1);
    label[1] = 1;
    bool any_branch = false;
    for (size_t i = 0; i < end; i++) {
        if (i % 4 == 0) continue;
        uint64_t w = code[i];
        uint32_t pc = (uint32_t)(i * 8);
        if (OPM(w, 0xe250000000000000ull, 0xfff0000000000000ull)) {
            if (fb_log) fprintf(stderr, "[spv-fb] st=%d reason=BRX w=%016llx\n", stage, (unsigned long long)w);
            free(label); return NULL;
        }
        bool is_flow = OPM(w, 0xe240000000000000ull, 0xfff0000000000020ull) ||
                       OPM(w, 0xe290000000000000ull, 0xfff0000000000020ull) ||
                       OPM(w, 0xe2a0000000000000ull, 0xfff0000000000020ull) ||
                       OPM(w, 0xe2b0000000000000ull, 0xfff0000000000020ull);
        if (is_flow) {
            int64_t t = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
            if (t >= 0 && (size_t)(t / 8) < end) label[t / 8] = 1;
            any_branch = true;
        }
        if (OPM(w, 0xf0f8000000000000ull, 0xfff8000000000000ull) ||
            OPM(w, 0xe340000000000000ull, 0xfff0000000000000ull) ||
            OPM(w, 0xe350000000000000ull, 0xfff0000000000000ull))
            any_branch = true;
    }
    uint32_t *stk_tgt = NULL;
    if (any_branch) {
        stk_tgt = spv_resolve_stacks(code, end);
        if (!stk_tgt) {
            if (fb_log) fprintf(stderr, "[spv-fb] st=%d reason=stk_unresolved w=0\n", stage);
            free(label); return NULL;
        }
    }

    Spv *s = calloc(1, sizeof *s);
    ShaderInfo *info = calloc(1, sizeof *info);
    info->stage = stage;
    s->stage = stage;
    s->key = key;
    s->info = info;
    s->code = code;
    s->end = end;
    s->branchy = any_branch;
    s->stk_tgt = stk_tgt;
    s->label = label;
    s->no_contract = (stage == STAGE_VERTEX);
    s->d16_ref = d16 != 0;
    s->rsc_pixel = rsc_pix != 0;
    s->no_discard = nodisc != 0;
    s->quad_shfl = q_shfl != 0;
    s->early_fs = e_fs != 0;
    if (stage == STAGE_FRAGMENT) {
        s->omap_target = *(const uint32_t *)(sph + 0x48);
        s->omap_depth = (*(const uint32_t *)(sph + 0x4C) >> 1) & 1;
        s->uses_tint = ((s->omap_target & 0x7) == 0x7) && (key->rt_kind[0] == 0);
    }

    // Pass 1: pre-scan all instructions
    for (size_t i = 0; i < end && !s->unsupported; i++) {
        if (i % 4 == 0) continue;
        scan_insn(s, code[i]);
        info->num_insts++;
    }
    if (s->unsupported) goto bail;
    if (info->uses_gmem && (info->cbuf_mask & (1u << GMEM_BUFFER_INDEX))) { mark_unsupported(s, "gmem_cb17", 0); goto bail; }
    for (int i = 0; i < info->ntex; i++)
        if (info->tex[i].shadow && info->tex[i].type == TEXTYPE_3D) { mark_unsupported(s, "tex_3d_sh", 0); goto bail; }
    if (s->uses_local) info->local_mem = SPV_LMEM_WORDS * 4;

    // Pass 2: emit SPIR-V sections
    s->next_id = 1;
    s->glsl_ext = new_id(s);
    init_types_and_globals(s);

    uint32_t t_fn = new_id(s);
    wb_op(&s->sec_types, OpTypeFunction, 3); wb_push(&s->sec_types, t_fn); wb_push(&s->sec_types, s->t_void);
    uint32_t fn_main = new_id(s);

    wb_op(&s->sec_fn, OpFunction, 5);
    wb_push(&s->sec_fn, s->t_void); wb_push(&s->sec_fn, fn_main); wb_push(&s->sec_fn, 0); wb_push(&s->sec_fn, t_fn);
    emit_label(s, new_id(s));

    // All Function-scope OpVariable declarations MUST precede any non-OpVariable instruction in entry block
    for (int i = 0; i < 256; i++)
        if (s->reg_used[i]) {
            s->v_reg[i] = new_id(s);
            wb_op(&s->sec_fn, OpVariable, 4); wb_push(&s->sec_fn, s->p_fn_u32); wb_push(&s->sec_fn, s->v_reg[i]); wb_push(&s->sec_fn, ScFunction);
        }
    for (int i = 0; i < 7; i++)
        if (s->pred_used[i]) {
            s->v_pred[i] = new_id(s);
            wb_op(&s->sec_fn, OpVariable, 4); wb_push(&s->sec_fn, s->p_fn_bool); wb_push(&s->sec_fn, s->v_pred[i]); wb_push(&s->sec_fn, ScFunction);
        }
    if (s->uses_cc) {
        s->v_cc  = new_id(s); wb_op(&s->sec_fn, OpVariable, 4); wb_push(&s->sec_fn, s->p_fn_u32);  wb_push(&s->sec_fn, s->v_cc);  wb_push(&s->sec_fn, ScFunction);
        s->v_ccz = new_id(s); wb_op(&s->sec_fn, OpVariable, 4); wb_push(&s->sec_fn, s->p_fn_bool); wb_push(&s->sec_fn, s->v_ccz); wb_push(&s->sec_fn, ScFunction);
        s->v_ccs = new_id(s); wb_op(&s->sec_fn, OpVariable, 4); wb_push(&s->sec_fn, s->p_fn_bool); wb_push(&s->sec_fn, s->v_ccs); wb_push(&s->sec_fn, ScFunction);
    }
    if (s->branchy) {
        s->v_pc    = new_id(s); wb_op(&s->sec_fn, OpVariable, 4); wb_push(&s->sec_fn, s->p_fn_u32); wb_push(&s->sec_fn, s->v_pc);    wb_push(&s->sec_fn, ScFunction);
        s->v_guard = new_id(s); wb_op(&s->sec_fn, OpVariable, 4); wb_push(&s->sec_fn, s->p_fn_u32); wb_push(&s->sec_fn, s->v_guard); wb_push(&s->sec_fn, ScFunction);
    }
    if (info->uses_gmem) {
        s->v_ga  = new_id(s); wb_op(&s->sec_fn, OpVariable, 4); wb_push(&s->sec_fn, s->p_fn_u64);  wb_push(&s->sec_fn, s->v_ga);  wb_push(&s->sec_fn, ScFunction);
        s->v_gok = new_id(s); wb_op(&s->sec_fn, OpVariable, 4); wb_push(&s->sec_fn, s->p_fn_bool); wb_push(&s->sec_fn, s->v_gok); wb_push(&s->sec_fn, ScFunction);
        if (s->uses_pmem) {
            s->v_gpm  = new_id(s); wb_op(&s->sec_fn, OpVariable, 4); wb_push(&s->sec_fn, s->p_fn_bool); wb_push(&s->sec_fn, s->v_gpm);  wb_push(&s->sec_fn, ScFunction);
            s->v_gph  = new_id(s); wb_op(&s->sec_fn, OpVariable, 4); wb_push(&s->sec_fn, s->p_fn_bool); wb_push(&s->sec_fn, s->v_gph);  wb_push(&s->sec_fn, ScFunction);
            s->v_gtmp = new_id(s); wb_op(&s->sec_fn, OpVariable, 4); wb_push(&s->sec_fn, s->p_fn_u32);  wb_push(&s->sec_fn, s->v_gtmp); wb_push(&s->sec_fn, ScFunction);
        }
    }
    if (s->uses_local) {
        s->v_lmem = new_id(s);
        wb_op(&s->sec_fn, OpVariable, 5); wb_push(&s->sec_fn, s->p_fn_lmem); wb_push(&s->sec_fn, s->v_lmem);
        wb_push(&s->sec_fn, ScFunction); wb_push(&s->sec_fn, s->c_lmem_null);
    }

    // Zero-initialize locals & default vertex outputs
    for (int i = 0; i < 256; i++) if (s->reg_used[i]) store(s, s->v_reg[i], cu(s, 0));
    for (int i = 0; i < 7; i++) if (s->pred_used[i]) store(s, s->v_pred[i], s->c_false);
    if (s->uses_cc) { store(s, s->v_cc, cu(s, 0)); store(s, s->v_ccz, s->c_false); store(s, s->v_ccs, s->c_false); }
    if (stage == STAGE_VERTEX) {
        uint32_t v0001 = vec4(s, s->t_v4f, cf(s, 0.0f), cf(s, 0.0f), cf(s, 0.0f), cf(s, 1.0f));
        store(s, ac1(s, s->p_out_v4f, s->v_pervertex, ci(s, 0)), v0001);
        store(s, ac1(s, s->p_out_f32, s->v_pervertex, ci(s, 1)), cf(s, info->writes_point_size ? 0.0f : 1.0f));
        for (int i = 0; i < 32; i++)
            if (s->out_attr_mask & (1u << i)) store(s, s->v_out[i], v0001);
    }

    // Structured control flow around shader body
    s->lbl_epilogue = new_id(s);
    s->lbl_loop_cont = new_id(s);
    if (!s->branchy) {
        uint32_t lbl_hdr = new_id(s), lbl_body = new_id(s);
        emit_branch(s, lbl_hdr);
        emit_label(s, lbl_hdr);
        emit_loop_merge(s, s->lbl_epilogue, s->lbl_loop_cont);
        emit_branch(s, lbl_body);
        emit_label(s, lbl_body);
        for (size_t i = 0; i < end; i++) {
            if (i % 4 == 0) continue;
            emit_insn_spv(s, code[i], (uint32_t)(i * 8));
        }
        if (!s->blk_terminated) emit_branch(s, s->lbl_loop_cont);
        emit_label(s, s->lbl_loop_cont);
        emit_cond_branch(s, s->c_false, lbl_hdr, s->lbl_epilogue);
    } else {
        store(s, s->v_pc, cu(s, 8));
        store(s, s->v_guard, cu(s, 0));
        // Assign a SPIR-V label ID to each marked basic block start
        uint32_t *blk_lbl = calloc(end + 4, sizeof(uint32_t));
        uint32_t *case_pc = malloc((end + 4) * sizeof(uint32_t));
        uint32_t *case_lbl = malloc((end + 4) * sizeof(uint32_t));
        uint32_t ncases = 0;
        for (size_t i = 0; i < end; i++) {
            if (i % 4 == 0 || !label[i]) continue;
            uint32_t lid = new_id(s);
            blk_lbl[i] = lid;
            case_pc[ncases] = (uint32_t)(i * 8);
            case_lbl[ncases] = lid;
            ncases++;
        }
        uint32_t lbl_hdr = new_id(s), lbl_cond = new_id(s), lbl_body = new_id(s);
        uint32_t lbl_sw_def = new_id(s), lbl_sw_merge = new_id(s);
        emit_branch(s, lbl_hdr);
        emit_label(s, lbl_hdr);
        emit_loop_merge(s, s->lbl_epilogue, s->lbl_loop_cont);
        emit_branch(s, lbl_cond);
        emit_label(s, lbl_cond);
        uint32_t g = load(s, s->t_u32, s->v_guard);
        emit_cond_branch(s, emit2(s, OpULessThan, s->t_bool, g, cu(s, 100000)), lbl_body, s->lbl_epilogue);
        emit_label(s, lbl_body);
        uint32_t pcv = load(s, s->t_u32, s->v_pc);
        emit_sel_merge(s, lbl_sw_merge);
        wb_op(&s->sec_fn, OpSwitch, 3 + ncases * 2);
        wb_push(&s->sec_fn, pcv); wb_push(&s->sec_fn, lbl_sw_def);
        for (uint32_t c = 0; c < ncases; c++) { wb_push(&s->sec_fn, case_pc[c]); wb_push(&s->sec_fn, case_lbl[c]); }
        s->blk_terminated = true;

        bool opened = false;
        for (size_t i = 0; i < end; i++) {
            if (i % 4 == 0) continue;
            if (blk_lbl[i]) {
                if (opened && !s->blk_terminated) emit_branch(s, blk_lbl[i]);  // structured fallthrough to next case
                emit_label(s, blk_lbl[i]);
                opened = true;
            }
            if (opened) emit_insn_spv(s, code[i], (uint32_t)(i * 8));
        }
        if (opened && !s->blk_terminated) emit_branch(s, s->lbl_epilogue);
        emit_label(s, lbl_sw_def);
        emit_branch(s, s->lbl_epilogue);
        emit_label(s, lbl_sw_merge);
        emit_branch(s, s->lbl_epilogue);
        emit_label(s, s->lbl_loop_cont);
        uint32_t g2 = emit2(s, OpIAdd, s->t_u32, load(s, s->t_u32, s->v_guard), cu(s, 1));
        store(s, s->v_guard, g2);
        emit_branch(s, lbl_hdr);
        free(blk_lbl); free(case_pc); free(case_lbl);
    }

    // Epilogue
    emit_label(s, s->lbl_epilogue);
    if (stage == STAGE_VERTEX) {
        if (key->flip_y) {
            uint32_t py = ac2(s, s->p_out_f32, s->v_pervertex, ci(s, 0), ci(s, 1));
            store(s, py, emit_f1(s, OpFNegate, s->t_f32, load(s, s->t_f32, py)));
        }
        if (key->flip_x) {
            uint32_t px = ac2(s, s->p_out_f32, s->v_pervertex, ci(s, 0), ci(s, 0));
            store(s, px, emit_f1(s, OpFNegate, s->t_f32, load(s, s->t_f32, px)));
        }
        if (key->depth_minus_one) {
            uint32_t pz = ac2(s, s->p_out_f32, s->v_pervertex, ci(s, 0), ci(s, 2));
            uint32_t pw = ac2(s, s->p_out_f32, s->v_pervertex, ci(s, 0), ci(s, 3));
            uint32_t nz = emit_f2(s, OpFMul, s->t_f32, emit_f2(s, OpFAdd, s->t_f32, load(s, s->t_f32, pz), load(s, s->t_f32, pw)), cf(s, 0.5f));
            store(s, pz, nz);
        }
    } else {
        int r = 0;
        uint32_t c0_val = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (s->omap_target >> (rt * 4)) & 0xF;
            if (!m) continue;
            int kind = key->rt_kind[rt];
            uint32_t comps[4];
            for (int c = 0; c < 4; c++) {
                if (m & (1u << c)) {
                    uint32_t rv = (r < 256 && s->reg_used[r]) ? rd_reg(s, r) : cu(s, 0);
                    comps[c] = kind == 0 ? u2f(s, rv) : kind == 1 ? u2i(s, rv) : rv;
                    r++;
                } else {
                    comps[c] = kind == 0 ? cf(s, 0.0f) : kind == 1 ? ci(s, 0) : cu(s, 0);
                }
            }
            uint32_t vty = kind == 1 ? s->t_v4i : kind == 2 ? s->t_v4u : s->t_v4f;
            uint32_t cv = vec4(s, vty, comps[0], comps[1], comps[2], comps[3]);
            if (rt == 0 && kind == 0) c0_val = cv;
            store(s, s->v_out[rt], cv);
        }
        // NVN alpha test (T191)
        if (key->alpha_func && key->alpha_func != 8 && (s->omap_target & 8) && key->rt_kind[0] == 0 && !s->no_discard) {
            if (key->alpha_func == 1) {
                wb_op(&s->sec_fn, OpKill, 1);
                s->blk_terminated = true;
            } else if (key->alpha_func < 8 && c0_val) {
                uint32_t a0 = extract(s, s->t_f32, c0_val, 3);
                uint32_t ref = extract(s, s->t_f32, glsl1(s, GlslUnpackHalf2x16, s->t_v2f, cu(s, key->alpha_ref)), 0);
                static const uint32_t aop[8] = {0, 0, OpFOrdLessThan, OpFOrdEqual, OpFOrdLessThanEqual, OpFOrdGreaterThan, OpFOrdNotEqual, OpFOrdGreaterThanEqual};
                uint32_t pass = emit2(s, aop[key->alpha_func], s->t_bool, a0, ref);
                uint32_t lbl_kill = new_id(s), lbl_ok = new_id(s);
                emit_sel_merge(s, lbl_ok);
                emit_cond_branch(s, pass, lbl_ok, lbl_kill);
                emit_label(s, lbl_kill);
                wb_op(&s->sec_fn, OpKill, 1);
                emit_label(s, lbl_ok);
            }
        }
        if (!s->blk_terminated && s->uses_tint && c0_val) {
            uint32_t t0 = load(s, s->t_v4f, ac2(s, s->p_uni_v4f, s->v_tint, ci(s, 0), ci(s, 0)));
            uint32_t w0 = extract(s, s->t_f32, t0, 3);
            uint32_t gt = emit2(s, OpFOrdGreaterThan, s->t_bool, w0, cf(s, 0.0f));
            uint32_t lbl_do = new_id(s), lbl_done = new_id(s);
            emit_sel_merge(s, lbl_done);
            emit_cond_branch(s, gt, lbl_do, lbl_done);
            emit_label(s, lbl_do);
            uint32_t t1 = load(s, s->t_v4f, ac2(s, s->p_uni_v4f, s->v_tint, ci(s, 0), ci(s, 1)));
            uint32_t g3 = vec3(s, s->t_v3f, extract(s, s->t_f32, c0_val, 0), extract(s, s->t_f32, c0_val, 1), extract(s, s->t_f32, c0_val, 2));
            uint32_t lum = emit2(s, OpDot, s->t_f32, g3, vec3(s, s->t_v3f, cf(s, 0.299f), cf(s, 0.587f), cf(s, 0.114f)));
            uint32_t t0rgb = vec3(s, s->t_v3f, extract(s, s->t_f32, t0, 0), extract(s, s->t_f32, t0, 1), extract(s, s->t_f32, t0, 2));
            uint32_t t1rgb = vec3(s, s->t_v3f, extract(s, s->t_f32, t1, 0), extract(s, s->t_f32, t1, 1), extract(s, s->t_f32, t1, 2));
            uint32_t pw = glsl2(s, GlslPow, s->t_f32, lum, extract(s, s->t_f32, t1, 3));
            uint32_t tgt = emit2(s, OpFAdd, s->t_v3f, emit2(s, OpVectorTimesScalar, s->t_v3f, t0rgb, pw),
                                 emit2(s, OpVectorTimesScalar, s->t_v3f, t1rgb, lum));
            uint32_t mx = glsl3(s, GlslFMix, s->t_v3f, g3, tgt, vec3(s, s->t_v3f, w0, w0, w0));
            uint32_t nc0 = vec4(s, s->t_v4f, extract(s, s->t_f32, mx, 0), extract(s, s->t_f32, mx, 1),
                                extract(s, s->t_f32, mx, 2), extract(s, s->t_f32, c0_val, 3));
            store(s, s->v_out[0], nc0);
            emit_branch(s, lbl_done);
            emit_label(s, lbl_done);
        }
        if (!s->blk_terminated && s->omap_depth) {
            uint32_t dv = (r + 1 < 256 && s->reg_used[r + 1]) ? u2f(s, rd_reg(s, r + 1))
                        : extract(s, s->t_f32, load(s, s->t_v4f, s->v_fragcoord), 2);
            store(s, s->v_fragdepth, dv);
        }
    }
    if (!s->blk_terminated) wb_op(&s->sec_fn, OpReturn, 1);
    wb_op(&s->sec_fn, OpFunctionEnd, 1);

    // Assemble final SPIR-V binary in spec-required section order
    WBuf mod = {0};
    wb_push(&mod, SPV_MAGIC);
    wb_push(&mod, SPV_VERSION_1_5);
    wb_push(&mod, 0);
    wb_push(&mod, s->next_id);
    wb_push(&mod, 0);

    // Capabilities
    wb_op(&mod, OpCapability, 2); wb_push(&mod, CapShader);
    if (info->uses_gmem) { wb_op(&mod, OpCapability, 2); wb_push(&mod, CapInt64); }
    if (s->uses_1d) { wb_op(&mod, OpCapability, 2); wb_push(&mod, CapSampled1D); }
    if (s->uses_cube_arr) { wb_op(&mod, OpCapability, 2); wb_push(&mod, CapSampledCubeArray); }
    if (s->uses_img_query) { wb_op(&mod, OpCapability, 2); wb_push(&mod, CapImageQuery); }
    if (s->uses_simg_read) { wb_op(&mod, OpCapability, 2); wb_push(&mod, CapStorageImageReadWithoutFormat); }
    if (s->uses_simg_write) { wb_op(&mod, OpCapability, 2); wb_push(&mod, CapStorageImageWriteWithoutFormat); }
    if (s->uses_lane || s->uses_quad || s->uses_shfl || s->uses_shfl_rel) {
        wb_op(&mod, OpCapability, 2); wb_push(&mod, CapGroupNonUniform);
    }
    if (s->uses_shfl) { wb_op(&mod, OpCapability, 2); wb_push(&mod, CapGroupNonUniformShuffle); }
    if (s->uses_shfl_rel) { wb_op(&mod, OpCapability, 2); wb_push(&mod, CapGroupNonUniformShuffleRelative); }
    if (s->uses_quad) { wb_op(&mod, OpCapability, 2); wb_push(&mod, CapGroupNonUniformQuad); }

    // ExtInstImport "GLSL.std.450" (13 bytes -> 4 words)
    wb_op(&mod, OpExtInstImport, 6); wb_push(&mod, s->glsl_ext); wb_str(&mod, "GLSL.std.450");

    // MemoryModel
    wb_op(&mod, OpMemoryModel, 3); wb_push(&mod, AddrLogical); wb_push(&mod, MemGLSL450);

    // EntryPoint
    wb_op(&mod, OpEntryPoint, (uint32_t)(5 + s->n_iface));
    wb_push(&mod, stage == STAGE_VERTEX ? ExecVertex : ExecFragment);
    wb_push(&mod, fn_main);
    wb_str(&mod, "main");  // 2 words
    for (int i = 0; i < s->n_iface; i++) wb_push(&mod, s->iface[i]);

    // ExecutionMode
    if (stage == STAGE_FRAGMENT) {
        wb_op(&mod, OpExecutionMode, 3); wb_push(&mod, fn_main); wb_push(&mod, ModeOriginUpperLeft);
        if (s->omap_depth) { wb_op(&mod, OpExecutionMode, 3); wb_push(&mod, fn_main); wb_push(&mod, ModeDepthReplacing); }
        if (s->early_fs && s->uses_sust && !s->omap_depth && !info->uses_discard && !(key && key->alpha_func && key->alpha_func != 8)) {
            wb_op(&mod, OpExecutionMode, 3); wb_push(&mod, fn_main); wb_push(&mod, ModeEarlyFragmentTests);
        }
    }

    // Decorations, Types+Constants, Globals, Function
    size_t total = mod.n + s->sec_dec.n + s->sec_types.n + s->sec_vars.n + s->sec_fn.n;
    uint32_t *out = malloc(total * sizeof(uint32_t));
    size_t pos = 0;
    memcpy(out + pos, mod.w, mod.n * 4); pos += mod.n;
    if (s->sec_dec.n)   { memcpy(out + pos, s->sec_dec.w,   s->sec_dec.n * 4);   pos += s->sec_dec.n; }
    if (s->sec_types.n) { memcpy(out + pos, s->sec_types.w, s->sec_types.n * 4); pos += s->sec_types.n; }
    if (s->sec_vars.n)  { memcpy(out + pos, s->sec_vars.w,  s->sec_vars.n * 4);  pos += s->sec_vars.n; }
    if (s->sec_fn.n)    { memcpy(out + pos, s->sec_fn.w,    s->sec_fn.n * 4);    pos += s->sec_fn.n; }

    info->spv = out;
    info->spv_words = pos;
    info->uses_vid = s->uses_vid;
    info->attr_mask = s->in_attr_mask;
    info->out_mask = stage == STAGE_FRAGMENT ? s->omap_target : s->out_attr_mask;

    free(mod.w);
    free(s->sec_dec.w); free(s->sec_types.w); free(s->sec_vars.w); free(s->sec_fn.w);
    free(s->cu32); free(s->cs32); free(s->cf32); free(s->cu64_tab);
    free(stk_tgt); free(label); free(s);
    return info;

bail:
    if (fb_log) fprintf(stderr, "[spv-fb] st=%d reason=%s w=%016llx\n", stage, s->bail_reason ? s->bail_reason : "other", (unsigned long long)s->bail_word);
    free(s->sec_dec.w); free(s->sec_types.w); free(s->sec_vars.w); free(s->sec_fn.w);
    free(s->cu32); free(s->cs32); free(s->cf32); free(s->cu64_tab);
    free(stk_tgt); free(label); free(s);
    maxwell_free(info);
    return NULL;
}
