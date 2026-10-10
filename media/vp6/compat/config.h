/*
 * Build configuration for the vendored FFmpeg VP6 decoder. Replaces the
 * file FFmpeg's configure script generates; it selects the C paths plus the
 * ARM assembly for the NEON targets.
 */

#ifndef A11_VP6_CONFIG_H
#define A11_VP6_CONFIG_H

#define FFMPEG_CONFIGURATION "a11-vp6"
#define FFMPEG_LICENSE "LGPL version 2.1 or later"
#define CC_IDENT "clang"
#define OS_NAME android
#define av_restrict restrict
#define EXTERN_PREFIX ""
#define EXTERN_ASM
#define CONFIG_THUMB 0
#define SWS_MAX_FILTER_SIZE 256

#define ARCH_X86 0
#define ARCH_X86_32 0
#define ARCH_X86_64 0
#define ARCH_AARCH64 0
#define ARCH_PPC 0
#define ARCH_MIPS 0
#define ARCH_RISCV 0
#define ARCH_LOONGARCH 0
#define ARCH_AARCH64 0
#define HAVE_X86ASM 0
#define HAVE_INLINE_ASM 1
#define HAVE_BIGENDIAN 0
#define HAVE_FAST_UNALIGNED 1
#define HAVE_FAST_64BIT 0
#define HAVE_FAST_CLZ 1
#define HAVE_FAST_CMOV 0
#define HAVE_FAST_UNALIGNED 1
#define HAVE_SIMD_ALIGN_16 1
#define HAVE_SIMD_ALIGN_32 0
#define HAVE_SIMD_ALIGN_64 0
#define HAVE_MM_EMPTY 0
#define HAVE_MMX 0
#define HAVE_MMXEXT 0
#define HAVE_SSE 0
#define HAVE_SSE2 0
#define HAVE_SSSE3 0
#define HAVE_AVX 0
#define HAVE_AVX2 0
#define HAVE_INTRINSICS_NEON 0
#define CONFIG_SMALL 0
#define CONFIG_GRAY 0
#define CONFIG_VP3DSP 1
#define CONFIG_VP6_DECODER 1
#define CONFIG_VP6A_DECODER 1
#define CONFIG_VP6F_DECODER 1
#define CONFIG_HUFFMAN 1
#define ASSERT_LEVEL 0
#define CONFIG_FTRAPV 0

#if defined(__arm__)
#define ARCH_ARM 1
#define HAVE_ARMV5TE 1
#define HAVE_ARMV6 1
#define HAVE_ARMV6T2 1
#define HAVE_VFP 1
#define HAVE_VFPV3 1
#define HAVE_SETEND 0
#if defined(__ARM_NEON)
#define HAVE_NEON 1
#else
#define HAVE_NEON 0
#endif
#define HAVE_AS_FUNC 0
#define HAVE_AS_ARCH_DIRECTIVE 1
#define AS_ARCH_LEVEL armv7-a
#define HAVE_AS_OBJECT_ARCH 1
#define HAVE_AS_FPU_DIRECTIVE 1
#define AS_FPU neon
#define HAVE_INTRINSICS_NEON 0
#else
#define ARCH_ARM 0
#define HAVE_ARMV5TE 0
#define HAVE_ARMV6 0
#define HAVE_ARMV6T2 0
#define HAVE_VFP 0
#define HAVE_VFPV3 0
#define HAVE_SETEND 0
#define HAVE_NEON 0
#endif

/* libm.h fallbacks: the C library supplies all of these. */
#define HAVE_ATAN2F 1
#define HAVE_ATANF 1
#define HAVE_CBRT 1
#define HAVE_CBRTF 1
#define HAVE_COPYSIGN 1
#define HAVE_COSF 1
#define HAVE_ERF 1
#define HAVE_EXP2 1
#define HAVE_EXP2F 1
#define HAVE_EXPF 1
#define HAVE_HYPOT 1
#define HAVE_ISFINITE 1
#define HAVE_ISINF 1
#define HAVE_ISNAN 1
#define HAVE_LDEXPF 1
#define HAVE_LLRINT 1
#define HAVE_LLRINTF 1
#define HAVE_LOG10F 1
#define HAVE_LOG2 1
#define HAVE_LOG2F 1
#define HAVE_LRINT 1
#define HAVE_LRINTF 1
#define HAVE_MIPSFPU 0
#define HAVE_POWF 1
#define HAVE_RINT 1
#define HAVE_ROUND 1
#define HAVE_ROUNDF 1
#define HAVE_SINF 1
#define HAVE_TRUNC 1
#define HAVE_TRUNCF 1

#endif /* A11_VP6_CONFIG_H */
