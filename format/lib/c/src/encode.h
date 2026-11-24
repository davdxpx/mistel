/* encode.h: override stubs and arm64-imm/1 instructions. Not part of the API. MIT License. */
#ifndef MNCF_ENCODE_H
#define MNCF_ENCODE_H

#include "internal.h"

#define MN_STUB_MAX 20

enum { MN_INSN_MOVZ = 0, MN_INSN_MOVZ_MOVK = 1, MN_INSN_FMOV = 2 };

/* Whether Appendix A gives a stub for the architecture. */
int mn_arch_has_stub(const char *arch, size_t n);
/* The stub for an action and a value (taken as 64-bit two's complement); its length, 0 for none. */
size_t mn_stub(const char *arch, size_t n, int action, uint64_t value, uint8_t out[MN_STUB_MAX]);
int mn_insn_parse(const char *s, size_t n);
size_t mn_insn_size(int insn);
/* The bytes of an encode table; their length, or 0 where the value does not fit the instruction. */
size_t mn_arm64_imm(int insn, int width, int rd, const mn_num *value, uint8_t out[8]);

#endif /* MNCF_ENCODE_H */
