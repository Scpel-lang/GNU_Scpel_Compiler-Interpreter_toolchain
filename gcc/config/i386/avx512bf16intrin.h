/* Please review: $(src-dir)/SPL-README for Licencing info. */

#ifndef _IMMINTRIN_H_INCLUDED
#error "Never use <avx512bf16intrin.h> directly; include <immintrin.h> instead."
#endif

#ifndef _AVX512BF16INTRIN_H_INCLUDED
#define _AVX512BF16INTRIN_H_INCLUDED

#ifndef __AVX512BF16__
#pragma GCC push_options
#pragma GCC target("avx512bf16")
#define __DISABLE_AVX512BF16__
#endif /* __AVX512BF16__ */

/* Internal data types for implementing the intrinsics.  */
typedef short __v32bh __attribute__ ((__vector_size__ (64)));

/* The Intel API is flexible enough that we must allow aliasing with other
   vector types, and their scalar components.  */
typedef short __m512bh __attribute__ ((__vector_size__ (64), __may_alias__));

/* Convert One BF16 Data to One Single Float Data.  */
extern __inline float
__attribute__ ((__gnu_inline__, __always_inline__, __artificial__))
_mm_cvtsbh_ss (__bfloat16 __A)
{
  union{ float a; unsigned int b;} __tmp;
  __tmp.b = ((unsigned int)(__A)) << 16;
  return __tmp.a;
}

/* vcvtne2ps2bf16 */

extern __inline __m512bh
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm512_cvtne2ps_pbh (__m512 __A, __m512 __B)
{
  return (__m512bh)__builtin_ia32_cvtne2ps2bf16_v32hi(__A, __B);
}

extern __inline __m512bh
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm512_mask_cvtne2ps_pbh (__m512bh __A, __mmask32 __B, __m512 __C, __m512 __D)
{
  return (__m512bh)__builtin_ia32_cvtne2ps2bf16_v32hi_mask(__C, __D, __A, __B);
}

extern __inline __m512bh
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm512_maskz_cvtne2ps_pbh (__mmask32 __A, __m512 __B, __m512 __C)
{
  return (__m512bh)__builtin_ia32_cvtne2ps2bf16_v32hi_maskz(__B, __C, __A);
}

/* vcvtneps2bf16 */

extern __inline __m256bh
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm512_cvtneps_pbh (__m512 __A)
{
  return (__m256bh)__builtin_ia32_cvtneps2bf16_v16sf(__A);
}

extern __inline __m256bh
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm512_mask_cvtneps_pbh (__m256bh __A, __mmask16 __B, __m512 __C)
{
  return (__m256bh)__builtin_ia32_cvtneps2bf16_v16sf_mask(__C, __A, __B);
}

extern __inline __m256bh
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm512_maskz_cvtneps_pbh (__mmask16 __A, __m512 __B)
{
  return (__m256bh)__builtin_ia32_cvtneps2bf16_v16sf_maskz(__B, __A);
}

/* vdpbf16ps */

extern __inline __m512
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm512_dpbf16_ps (__m512 __A, __m512bh __B, __m512bh __C)
{
  return (__m512)__builtin_ia32_dpbf16ps_v16sf(__A, __B, __C);
}

extern __inline __m512
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm512_mask_dpbf16_ps (__m512 __A, __mmask16 __B, __m512bh __C, __m512bh __D)
{
  return (__m512)__builtin_ia32_dpbf16ps_v16sf_mask(__A, __C, __D, __B);
}

extern __inline __m512
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm512_maskz_dpbf16_ps (__mmask16 __A, __m512 __B, __m512bh __C, __m512bh __D)
{
  return (__m512)__builtin_ia32_dpbf16ps_v16sf_maskz(__B, __C, __D, __A);
}

extern __inline __m512
__attribute__ ((__gnu_inline__, __always_inline__, __artificial__))
_mm512_cvtpbh_ps (__m256bh __A)
{
  return (__m512)_mm512_castsi512_ps ((__m512i)_mm512_slli_epi32 (
	 (__m512i)_mm512_cvtepi16_epi32 ((__m256i)__A), 16));
}

extern __inline __m512
__attribute__ ((__gnu_inline__, __always_inline__, __artificial__))
_mm512_maskz_cvtpbh_ps (__mmask16 __U, __m256bh __A)
{
  return (__m512)_mm512_castsi512_ps ((__m512i) _mm512_slli_epi32 (
	 (__m512i)_mm512_maskz_cvtepi16_epi32 (
	 (__mmask16)__U, (__m256i)__A), 16));
}

extern __inline __m512
__attribute__ ((__gnu_inline__, __always_inline__, __artificial__))
_mm512_mask_cvtpbh_ps (__m512 __S, __mmask16 __U, __m256bh __A)
{
  return (__m512)_mm512_castsi512_ps ((__m512i)(_mm512_mask_slli_epi32 (
	 (__m512i)__S, (__mmask16)__U,
	 (__m512i)_mm512_cvtepi16_epi32 ((__m256i)__A), 16)));
}

#ifdef __DISABLE_AVX512BF16__
#undef __DISABLE_AVX512BF16__
#pragma GCC pop_options
#endif /* __DISABLE_AVX512BF16__ */

#endif /* _AVX512BF16INTRIN_H_INCLUDED */
