/* encode.c: machine code the reader makes itself: override stubs (section 7.5, Appendix A) and the
 * instructions of `arm64-imm/1` (section 10.2). MIT License. */
#include "encode.h"

#include <math.h>

static size_t le32(uint8_t *out, uint32_t w)
{
    out[0] = (uint8_t)w;
    out[1] = (uint8_t)(w >> 8);
    out[2] = (uint8_t)(w >> 16);
    out[3] = (uint8_t)(w >> 24);
    return 4;
}

int mn_arch_has_stub(const char *arch, size_t n)
{
    return mn_streqz(arch, n, "arm64") || mn_streqz(arch, n, "x86-64");
}

size_t mn_stub(const char *arch, size_t n, int action, uint64_t v, uint8_t out[MN_STUB_MAX])
{
    const uint32_t ret = 0xD65F03C0u;
    size_t len = 0;
    if (mn_streqz(arch, n, "arm64")) {
        if (action == MNCF_ACTION_SKIP) {
            return le32(out, ret);
        }
        if (v <= 0xFFFF) {
            len += le32(out + len, 0xD2800000u | (uint32_t)v << 5);              /* movz x0, #v */
        } else if ((~v) <= 0xFFFF) {
            len += le32(out + len, 0x92800000u | (uint32_t)(~v) << 5);           /* movn x0, #~v */
        } else {
            int hw;
            len += le32(out + len, 0xD2800000u | (uint32_t)(v & 0xFFFF) << 5);   /* movz x0, #(bits 0-15) */
            for (hw = 1; hw <= 3; hw++) {
                uint32_t chunk = (uint32_t)(v >> (16 * hw)) & 0xFFFF;
                if (chunk != 0) {
                    len += le32(out + len, 0xF2800000u | (uint32_t)hw << 21 | chunk << 5);
                }
            }
        }
        len += le32(out + len, ret);
        return len;
    }
    if (mn_streqz(arch, n, "x86-64")) {
        int i;
        if (action == MNCF_ACTION_SKIP) {
            out[0] = 0xC3;
            return 1;
        }
        if (v <= 0xFFFFFFFFu) {
            out[len++] = 0xB8;                                    /* mov eax, imm32 */
            for (i = 0; i < 4; i++) {
                out[len++] = (uint8_t)(v >> (8 * i));
            }
        } else if (v >= 0xFFFFFFFF80000000ULL) {
            out[len++] = 0x48;                                    /* mov rax, simm32 */
            out[len++] = 0xC7;
            out[len++] = 0xC0;
            for (i = 0; i < 4; i++) {
                out[len++] = (uint8_t)(v >> (8 * i));
            }
        } else {
            out[len++] = 0x48;                                    /* mov rax, imm64 */
            out[len++] = 0xB8;
            for (i = 0; i < 8; i++) {
                out[len++] = (uint8_t)(v >> (8 * i));
            }
        }
        out[len++] = 0xC3;
        return len;
    }
    return 0;
}

int mn_insn_parse(const char *s, size_t n)
{
    if (mn_streqz(s, n, "movz")) {
        return MN_INSN_MOVZ;
    }
    if (mn_streqz(s, n, "movz-movk")) {
        return MN_INSN_MOVZ_MOVK;
    }
    if (mn_streqz(s, n, "fmov")) {
        return MN_INSN_FMOV;
    }
    return -1;
}

size_t mn_insn_size(int insn)
{
    return insn == MN_INSN_MOVZ_MOVK ? 8 : 4;
}

/* The 8-bit immediate of fmov for this value, or -1 if fmov cannot hold it exactly. */
static int fmov_imm8(const mn_num *value)
{
    double v, mantissa, m16;
    int exponent, e, sign, bcd;
    if (value->kind != MNCF_NUMBER_INT && value->kind != MNCF_NUMBER_FLOAT) {
        return -1;
    }
    v = mn_num_to_double(value);
    if (v == 0 || !isfinite(v)) {
        return -1;
    }
    sign = v < 0 ? 1 : 0;
    mantissa = frexp(fabs(v), &exponent);      /* fabs(v) = mantissa * 2^exponent, 0.5 <= mantissa < 1 */
    e = exponent - 1;
    m16 = mantissa * 32 - 16;
    if (e < -3 || e > 4 || m16 != floor(m16) || m16 < 0 || m16 > 15) {
        return -1;
    }
    bcd = e >= 1 ? e - 1 : (4 | (e + 3));
    return sign << 7 | bcd << 4 | (int)m16;
}

static int whole_in(const mn_num *v, uint64_t max, uint64_t *out)
{
    if (v->kind != MNCF_NUMBER_INT || v->huge || v->negative || v->magnitude > max) {
        return 0;
    }
    *out = v->magnitude;
    return 1;
}

size_t mn_arm64_imm(int insn, int width, int rd, const mn_num *value, uint8_t out[8])
{
    uint32_t sf = width == 64 ? 1u : 0u;
    uint64_t v;
    if (insn == MN_INSN_MOVZ) {
        if (!whole_in(value, 0xFFFF, &v)) {
            return 0;
        }
        return le32(out, sf << 31 | 0x52800000u | (uint32_t)v << 5 | (uint32_t)rd);
    }
    if (insn == MN_INSN_MOVZ_MOVK) {
        uint32_t lo, hi;
        if (!whole_in(value, 0xFFFFFFFFu, &v)) {
            return 0;
        }
        lo = (uint32_t)(v & 0xFFFF);
        hi = (uint32_t)(v >> 16);
        le32(out, sf << 31 | 0x52800000u | lo << 5 | (uint32_t)rd);
        le32(out + 4, sf << 31 | 0x72A00000u | hi << 5 | (uint32_t)rd);
        return 8;
    }
    if (insn == MN_INSN_FMOV) {
        int imm8 = fmov_imm8(value);
        uint32_t ftype = width == 64 ? 1u : 0u;
        if (imm8 < 0) {
            return 0;
        }
        return le32(out, 0x1E201000u | ftype << 22 | (uint32_t)imm8 << 13 | (uint32_t)rd);
    }
    return 0;
}
