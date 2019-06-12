#include "TracyEtc1.hpp"

#include <array>
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef std::array<uint16_t, 4> v4i;

#if defined __AVX__ && !defined __SSE4_1__
#  define __SSE4_1__
#endif

#ifdef __AVX2__

#ifdef _MSC_VER
#  include <intrin.h>
#  include <Windows.h>
#  define _bswap(x) _byteswap_ulong(x)
#  define VS_VECTORCALL _vectorcall
#else
#  include <x86intrin.h>
#  pragma GCC push_options
#  pragma GCC target ("avx2,fma,bmi2")
#  define VS_VECTORCALL
#endif

#ifndef _bswap
#  define _bswap(x) __builtin_bswap32(x)
#endif

namespace tracy
{

const __m128i g_table128_SIMD[2] =
{
    _mm_setr_epi16(   2*128,   5*128,   9*128,  13*128,  18*128,  24*128,  33*128,  47*128),
    _mm_setr_epi16(   8*128,  17*128,  29*128,  42*128,  60*128,  80*128, 106*128, 183*128)
};

#ifdef _MSC_VER
static inline unsigned long _bit_scan_forward( unsigned long mask )
{
    unsigned long ret;
    _BitScanForward( &ret, mask );
    return ret;
}
#endif

static __m256i VS_VECTORCALL Sum4_AVX2( const uint8_t* data) noexcept
{
    __m128i d0 = _mm_loadu_si128(((__m128i*)data) + 0);
    __m128i d1 = _mm_loadu_si128(((__m128i*)data) + 1);
    __m128i d2 = _mm_loadu_si128(((__m128i*)data) + 2);
    __m128i d3 = _mm_loadu_si128(((__m128i*)data) + 3);

    __m128i dm0 = _mm_and_si128(d0, _mm_set1_epi32(0x00FFFFFF));
    __m128i dm1 = _mm_and_si128(d1, _mm_set1_epi32(0x00FFFFFF));
    __m128i dm2 = _mm_and_si128(d2, _mm_set1_epi32(0x00FFFFFF));
    __m128i dm3 = _mm_and_si128(d3, _mm_set1_epi32(0x00FFFFFF));

    __m256i t0 = _mm256_cvtepu8_epi16(dm0);
    __m256i t1 = _mm256_cvtepu8_epi16(dm1);
    __m256i t2 = _mm256_cvtepu8_epi16(dm2);
    __m256i t3 = _mm256_cvtepu8_epi16(dm3);

    __m256i sum0 = _mm256_add_epi16(t0, t1);
    __m256i sum1 = _mm256_add_epi16(t2, t3);

    __m256i s0 = _mm256_permute2x128_si256(sum0, sum1, (0) | (3 << 4)); // 0, 0, 3, 3
    __m256i s1 = _mm256_permute2x128_si256(sum0, sum1, (1) | (2 << 4)); // 1, 1, 2, 2

    __m256i s2 = _mm256_permute4x64_epi64(s0, _MM_SHUFFLE(1, 3, 0, 2));
    __m256i s3 = _mm256_permute4x64_epi64(s0, _MM_SHUFFLE(0, 2, 1, 3));
    __m256i s4 = _mm256_permute4x64_epi64(s1, _MM_SHUFFLE(3, 1, 0, 2));
    __m256i s5 = _mm256_permute4x64_epi64(s1, _MM_SHUFFLE(2, 0, 1, 3));

    __m256i sum5 = _mm256_add_epi16(s2, s3); //   3,   0,   3,   0
    __m256i sum6 = _mm256_add_epi16(s4, s5); //   2,   1,   1,   2
    return _mm256_add_epi16(sum5, sum6);     // 3+2, 0+1, 3+1, 3+2
}

__m256i VS_VECTORCALL Average_AVX2( const __m256i data) noexcept
{
    __m256i a = _mm256_add_epi16(data, _mm256_set1_epi16(4));

    return _mm256_srli_epi16(a, 3);
}

static __m128i VS_VECTORCALL CalcErrorBlock_AVX2( const __m256i data, const v4i a[8]) noexcept
{
    //
    __m256i a0 = _mm256_load_si256((__m256i*)a[0].data());
    __m256i a1 = _mm256_load_si256((__m256i*)a[4].data());

    // err = 8 * ( sq( average[0] ) + sq( average[1] ) + sq( average[2] ) );
    __m256i a4 = _mm256_madd_epi16(a0, a0);
    __m256i a5 = _mm256_madd_epi16(a1, a1);

    __m256i a6 = _mm256_hadd_epi32(a4, a5);
    __m256i a7 = _mm256_slli_epi32(a6, 3);

    __m256i a8 = _mm256_add_epi32(a7, _mm256_set1_epi32(0x3FFFFFFF)); // Big value to prevent negative values, but small enough to prevent overflow

                                                                      // average is not swapped
                                                                      // err -= block[0] * 2 * average[0];
                                                                      // err -= block[1] * 2 * average[1];
                                                                      // err -= block[2] * 2 * average[2];
    __m256i a2 = _mm256_slli_epi16(a0, 1);
    __m256i a3 = _mm256_slli_epi16(a1, 1);
    __m256i b0 = _mm256_madd_epi16(a2, data);
    __m256i b1 = _mm256_madd_epi16(a3, data);

    __m256i b2 = _mm256_hadd_epi32(b0, b1);
    __m256i b3 = _mm256_sub_epi32(a8, b2);
    __m256i b4 = _mm256_hadd_epi32(b3, b3);

    __m256i b5 = _mm256_permutevar8x32_epi32(b4, _mm256_set_epi32(0, 0, 0, 0, 5, 1, 4, 0));

    return _mm256_castsi256_si128(b5);
}

static void VS_VECTORCALL ProcessAverages_AVX2(const __m256i d, v4i a[8] ) noexcept
{
    __m256i t = _mm256_add_epi16(_mm256_mullo_epi16(d, _mm256_set1_epi16(31)), _mm256_set1_epi16(128));

    __m256i c = _mm256_srli_epi16(_mm256_add_epi16(t, _mm256_srli_epi16(t, 8)), 8);

    __m256i c1 = _mm256_shuffle_epi32(c, _MM_SHUFFLE(3, 2, 3, 2));
    __m256i diff = _mm256_sub_epi16(c, c1);
    diff = _mm256_max_epi16(diff, _mm256_set1_epi16(-4));
    diff = _mm256_min_epi16(diff, _mm256_set1_epi16(3));

    __m256i co = _mm256_add_epi16(c1, diff);

    c = _mm256_blend_epi16(co, c, 0xF0);

    __m256i a0 = _mm256_or_si256(_mm256_slli_epi16(c, 3), _mm256_srli_epi16(c, 2));

    _mm256_store_si256((__m256i*)a[4].data(), a0);

    __m256i t0 = _mm256_add_epi16(_mm256_mullo_epi16(d, _mm256_set1_epi16(15)), _mm256_set1_epi16(128));
    __m256i t1 = _mm256_srli_epi16(_mm256_add_epi16(t0, _mm256_srli_epi16(t0, 8)), 8);

    __m256i t2 = _mm256_or_si256(t1, _mm256_slli_epi16(t1, 4));

    _mm256_store_si256((__m256i*)a[0].data(), t2);
}

static uint64_t VS_VECTORCALL EncodeAverages_AVX2( const v4i a[8], size_t idx ) noexcept
{
    uint64_t d = ( idx << 24 );
    size_t base = idx << 1;

    __m128i a0 = _mm_load_si128((const __m128i*)a[base].data());

    __m128i r0, r1;

    if( ( idx & 0x2 ) == 0 )
    {
        r0 = _mm_srli_epi16(a0, 4);

        __m128i a1 = _mm_unpackhi_epi64(r0, r0);
        r1 = _mm_slli_epi16(a1, 4);
    }
    else
    {
        __m128i a1 = _mm_and_si128(a0, _mm_set1_epi16(-8));

        r0 = _mm_unpackhi_epi64(a1, a1);
        __m128i a2 = _mm_sub_epi16(a1, r0);
        __m128i a3 = _mm_srai_epi16(a2, 3);
        r1 = _mm_and_si128(a3, _mm_set1_epi16(0x07));
    }

    __m128i r2 = _mm_or_si128(r0, r1);
    // do missing swap for average values
    __m128i r3 = _mm_shufflelo_epi16(r2, _MM_SHUFFLE(3, 0, 1, 2));
    __m128i r4 = _mm_packus_epi16(r3, _mm_setzero_si128());
    d |= _mm_cvtsi128_si32(r4);

    return d;
}

static uint64_t VS_VECTORCALL CheckSolid_AVX2( const uint8_t* src ) noexcept
{
    __m256i d0 = _mm256_loadu_si256(((__m256i*)src) + 0);
    __m256i d1 = _mm256_loadu_si256(((__m256i*)src) + 1);

    __m256i c = _mm256_broadcastd_epi32(_mm256_castsi256_si128(d0));

    __m256i c0 = _mm256_cmpeq_epi8(d0, c);
    __m256i c1 = _mm256_cmpeq_epi8(d1, c);

    __m256i m = _mm256_and_si256(c0, c1);

    if (!_mm256_testc_si256(m, _mm256_set1_epi32(-1)))
    {
        return 0;
    }

    return 0x02000000 |
        ( (unsigned int)( src[0] & 0xF8 ) << 16 ) |
        ( (unsigned int)( src[1] & 0xF8 ) << 8 ) |
        ( (unsigned int)( src[2] & 0xF8 ) );
}

static __m128i VS_VECTORCALL PrepareAverages_AVX2( v4i a[8], const uint8_t* src) noexcept
{
    __m256i sum4 = Sum4_AVX2( src );

    ProcessAverages_AVX2(Average_AVX2( sum4 ), a );

    return CalcErrorBlock_AVX2( sum4, a);
}

static void VS_VECTORCALL FindBestFit_4x2_AVX2( uint32_t terr[2][8], uint32_t tsel[8], v4i a[8], const uint32_t offset, const uint8_t* data) noexcept
{
    __m256i sel0 = _mm256_setzero_si256();
    __m256i sel1 = _mm256_setzero_si256();

    for (unsigned int j = 0; j < 2; ++j)
    {
        unsigned int bid = offset + 1 - j;

        __m256i squareErrorSum = _mm256_setzero_si256();

        __m128i a0 = _mm_loadl_epi64((const __m128i*)a[bid].data());
        __m256i a1 = _mm256_broadcastq_epi64(a0);

        // Processing one full row each iteration
        for (size_t i = 0; i < 8; i += 4)
        {
            __m128i rgb = _mm_loadu_si128((const __m128i*)(data + i * 4));

            __m256i rgb16 = _mm256_cvtepu8_epi16(rgb);
            __m256i d = _mm256_sub_epi16(a1, rgb16);

            // The scaling values are divided by two and rounded, to allow the differences to be in the range of signed int16
            // This produces slightly different results, but is significant faster
            __m256i pixel0 = _mm256_madd_epi16(d, _mm256_set_epi16(0, 38, 76, 14, 0, 38, 76, 14, 0, 38, 76, 14, 0, 38, 76, 14));
            __m256i pixel1 = _mm256_packs_epi32(pixel0, pixel0);
            __m256i pixel2 = _mm256_hadd_epi16(pixel1, pixel1);
            __m128i pixel3 = _mm256_castsi256_si128(pixel2);

            __m128i pix0 = _mm_broadcastw_epi16(pixel3);
            __m128i pix1 = _mm_broadcastw_epi16(_mm_srli_epi32(pixel3, 16));
            __m256i pixel = _mm256_insertf128_si256(_mm256_castsi128_si256(pix0), pix1, 1);

            // Processing first two pixels of the row
            {
                __m256i pix = _mm256_abs_epi16(pixel);

                // Taking the absolute value is way faster. The values are only used to sort, so the result will be the same.
                // Since the selector table is symmetrical, we need to calculate the difference only for half of the entries.
                __m256i error0 = _mm256_abs_epi16(_mm256_sub_epi16(pix, _mm256_broadcastsi128_si256(g_table128_SIMD[0])));
                __m256i error1 = _mm256_abs_epi16(_mm256_sub_epi16(pix, _mm256_broadcastsi128_si256(g_table128_SIMD[1])));

                __m256i minIndex0 = _mm256_and_si256(_mm256_cmpgt_epi16(error0, error1), _mm256_set1_epi16(1));
                __m256i minError = _mm256_min_epi16(error0, error1);

                // Exploiting symmetry of the selector table and use the sign bit
                // This produces slightly different results, but is significant faster
                __m256i minIndex1 = _mm256_srli_epi16(pixel, 15);

                // Interleaving values so madd instruction can be used
                __m256i minErrorLo = _mm256_permute4x64_epi64(minError, _MM_SHUFFLE(1, 1, 0, 0));
                __m256i minErrorHi = _mm256_permute4x64_epi64(minError, _MM_SHUFFLE(3, 3, 2, 2));

                __m256i minError2 = _mm256_unpacklo_epi16(minErrorLo, minErrorHi);
                // Squaring the minimum error to produce correct values when adding
                __m256i squareError = _mm256_madd_epi16(minError2, minError2);

                squareErrorSum = _mm256_add_epi32(squareErrorSum, squareError);

                // Packing selector bits
                __m256i minIndexLo2 = _mm256_sll_epi16(minIndex0, _mm_cvtsi64_si128(i + j * 8));
                __m256i minIndexHi2 = _mm256_sll_epi16(minIndex1, _mm_cvtsi64_si128(i + j * 8));

                sel0 = _mm256_or_si256(sel0, minIndexLo2);
                sel1 = _mm256_or_si256(sel1, minIndexHi2);
            }

            pixel3 = _mm256_extracti128_si256(pixel2, 1);
            pix0 = _mm_broadcastw_epi16(pixel3);
            pix1 = _mm_broadcastw_epi16(_mm_srli_epi32(pixel3, 16));
            pixel = _mm256_insertf128_si256(_mm256_castsi128_si256(pix0), pix1, 1);

            // Processing second two pixels of the row
            {
                __m256i pix = _mm256_abs_epi16(pixel);

                // Taking the absolute value is way faster. The values are only used to sort, so the result will be the same.
                // Since the selector table is symmetrical, we need to calculate the difference only for half of the entries.
                __m256i error0 = _mm256_abs_epi16(_mm256_sub_epi16(pix, _mm256_broadcastsi128_si256(g_table128_SIMD[0])));
                __m256i error1 = _mm256_abs_epi16(_mm256_sub_epi16(pix, _mm256_broadcastsi128_si256(g_table128_SIMD[1])));

                __m256i minIndex0 = _mm256_and_si256(_mm256_cmpgt_epi16(error0, error1), _mm256_set1_epi16(1));
                __m256i minError = _mm256_min_epi16(error0, error1);

                // Exploiting symmetry of the selector table and use the sign bit
                __m256i minIndex1 = _mm256_srli_epi16(pixel, 15);

                // Interleaving values so madd instruction can be used
                __m256i minErrorLo = _mm256_permute4x64_epi64(minError, _MM_SHUFFLE(1, 1, 0, 0));
                __m256i minErrorHi = _mm256_permute4x64_epi64(minError, _MM_SHUFFLE(3, 3, 2, 2));

                __m256i minError2 = _mm256_unpacklo_epi16(minErrorLo, minErrorHi);
                // Squaring the minimum error to produce correct values when adding
                __m256i squareError = _mm256_madd_epi16(minError2, minError2);

                squareErrorSum = _mm256_add_epi32(squareErrorSum, squareError);

                // Packing selector bits
                __m256i minIndexLo2 = _mm256_sll_epi16(minIndex0, _mm_cvtsi64_si128(i + j * 8));
                __m256i minIndexHi2 = _mm256_sll_epi16(minIndex1, _mm_cvtsi64_si128(i + j * 8));
                __m256i minIndexLo3 = _mm256_slli_epi16(minIndexLo2, 2);
                __m256i minIndexHi3 = _mm256_slli_epi16(minIndexHi2, 2);

                sel0 = _mm256_or_si256(sel0, minIndexLo3);
                sel1 = _mm256_or_si256(sel1, minIndexHi3);
            }
        }

        data += 8 * 4;

        _mm256_store_si256((__m256i*)terr[1 - j], squareErrorSum);
    }

    // Interleave selector bits
    __m256i minIndexLo0 = _mm256_unpacklo_epi16(sel0, sel1);
    __m256i minIndexHi0 = _mm256_unpackhi_epi16(sel0, sel1);

    __m256i minIndexLo1 = _mm256_permute2x128_si256(minIndexLo0, minIndexHi0, (0) | (2 << 4));
    __m256i minIndexHi1 = _mm256_permute2x128_si256(minIndexLo0, minIndexHi0, (1) | (3 << 4));

    __m256i minIndexHi2 = _mm256_slli_epi32(minIndexHi1, 1);

    __m256i sel = _mm256_or_si256(minIndexLo1, minIndexHi2);

    _mm256_store_si256((__m256i*)tsel, sel);
}

static void VS_VECTORCALL FindBestFit_2x4_AVX2( uint32_t terr[2][8], uint32_t tsel[8], v4i a[8], const uint32_t offset, const uint8_t* data) noexcept
{
    __m256i sel0 = _mm256_setzero_si256();
    __m256i sel1 = _mm256_setzero_si256();

    __m256i squareErrorSum0 = _mm256_setzero_si256();
    __m256i squareErrorSum1 = _mm256_setzero_si256();

    __m128i a0 = _mm_loadl_epi64((const __m128i*)a[offset + 1].data());
    __m128i a1 = _mm_loadl_epi64((const __m128i*)a[offset + 0].data());

    __m128i a2 = _mm_broadcastq_epi64(a0);
    __m128i a3 = _mm_broadcastq_epi64(a1);
    __m256i a4 = _mm256_insertf128_si256(_mm256_castsi128_si256(a2), a3, 1);

    // Processing one full row each iteration
    for (size_t i = 0; i < 16; i += 4)
    {
        __m128i rgb = _mm_loadu_si128((const __m128i*)(data + i * 4));

        __m256i rgb16 = _mm256_cvtepu8_epi16(rgb);
        __m256i d = _mm256_sub_epi16(a4, rgb16);

        // The scaling values are divided by two and rounded, to allow the differences to be in the range of signed int16
        // This produces slightly different results, but is significant faster
        __m256i pixel0 = _mm256_madd_epi16(d, _mm256_set_epi16(0, 38, 76, 14, 0, 38, 76, 14, 0, 38, 76, 14, 0, 38, 76, 14));
        __m256i pixel1 = _mm256_packs_epi32(pixel0, pixel0);
        __m256i pixel2 = _mm256_hadd_epi16(pixel1, pixel1);
        __m128i pixel3 = _mm256_castsi256_si128(pixel2);

        __m128i pix0 = _mm_broadcastw_epi16(pixel3);
        __m128i pix1 = _mm_broadcastw_epi16(_mm_srli_epi32(pixel3, 16));
        __m256i pixel = _mm256_insertf128_si256(_mm256_castsi128_si256(pix0), pix1, 1);

        // Processing first two pixels of the row
        {
            __m256i pix = _mm256_abs_epi16(pixel);

            // Taking the absolute value is way faster. The values are only used to sort, so the result will be the same.
            // Since the selector table is symmetrical, we need to calculate the difference only for half of the entries.
            __m256i error0 = _mm256_abs_epi16(_mm256_sub_epi16(pix, _mm256_broadcastsi128_si256(g_table128_SIMD[0])));
            __m256i error1 = _mm256_abs_epi16(_mm256_sub_epi16(pix, _mm256_broadcastsi128_si256(g_table128_SIMD[1])));

            __m256i minIndex0 = _mm256_and_si256(_mm256_cmpgt_epi16(error0, error1), _mm256_set1_epi16(1));
            __m256i minError = _mm256_min_epi16(error0, error1);

            // Exploiting symmetry of the selector table and use the sign bit
            __m256i minIndex1 = _mm256_srli_epi16(pixel, 15);

            // Interleaving values so madd instruction can be used
            __m256i minErrorLo = _mm256_permute4x64_epi64(minError, _MM_SHUFFLE(1, 1, 0, 0));
            __m256i minErrorHi = _mm256_permute4x64_epi64(minError, _MM_SHUFFLE(3, 3, 2, 2));

            __m256i minError2 = _mm256_unpacklo_epi16(minErrorLo, minErrorHi);
            // Squaring the minimum error to produce correct values when adding
            __m256i squareError = _mm256_madd_epi16(minError2, minError2);

            squareErrorSum0 = _mm256_add_epi32(squareErrorSum0, squareError);

            // Packing selector bits
            __m256i minIndexLo2 = _mm256_sll_epi16(minIndex0, _mm_cvtsi64_si128(i));
            __m256i minIndexHi2 = _mm256_sll_epi16(minIndex1, _mm_cvtsi64_si128(i));

            sel0 = _mm256_or_si256(sel0, minIndexLo2);
            sel1 = _mm256_or_si256(sel1, minIndexHi2);
        }

        pixel3 = _mm256_extracti128_si256(pixel2, 1);
        pix0 = _mm_broadcastw_epi16(pixel3);
        pix1 = _mm_broadcastw_epi16(_mm_srli_epi32(pixel3, 16));
        pixel = _mm256_insertf128_si256(_mm256_castsi128_si256(pix0), pix1, 1);

        // Processing second two pixels of the row
        {
            __m256i pix = _mm256_abs_epi16(pixel);

            // Taking the absolute value is way faster. The values are only used to sort, so the result will be the same.
            // Since the selector table is symmetrical, we need to calculate the difference only for half of the entries.
            __m256i error0 = _mm256_abs_epi16(_mm256_sub_epi16(pix, _mm256_broadcastsi128_si256(g_table128_SIMD[0])));
            __m256i error1 = _mm256_abs_epi16(_mm256_sub_epi16(pix, _mm256_broadcastsi128_si256(g_table128_SIMD[1])));

            __m256i minIndex0 = _mm256_and_si256(_mm256_cmpgt_epi16(error0, error1), _mm256_set1_epi16(1));
            __m256i minError = _mm256_min_epi16(error0, error1);

            // Exploiting symmetry of the selector table and use the sign bit
            __m256i minIndex1 = _mm256_srli_epi16(pixel, 15);

            // Interleaving values so madd instruction can be used
            __m256i minErrorLo = _mm256_permute4x64_epi64(minError, _MM_SHUFFLE(1, 1, 0, 0));
            __m256i minErrorHi = _mm256_permute4x64_epi64(minError, _MM_SHUFFLE(3, 3, 2, 2));

            __m256i minError2 = _mm256_unpacklo_epi16(minErrorLo, minErrorHi);
            // Squaring the minimum error to produce correct values when adding
            __m256i squareError = _mm256_madd_epi16(minError2, minError2);

            squareErrorSum1 = _mm256_add_epi32(squareErrorSum1, squareError);

            // Packing selector bits
            __m256i minIndexLo2 = _mm256_sll_epi16(minIndex0, _mm_cvtsi64_si128(i));
            __m256i minIndexHi2 = _mm256_sll_epi16(minIndex1, _mm_cvtsi64_si128(i));
            __m256i minIndexLo3 = _mm256_slli_epi16(minIndexLo2, 2);
            __m256i minIndexHi3 = _mm256_slli_epi16(minIndexHi2, 2);

            sel0 = _mm256_or_si256(sel0, minIndexLo3);
            sel1 = _mm256_or_si256(sel1, minIndexHi3);
        }
    }

    _mm256_store_si256((__m256i*)terr[1], squareErrorSum0);
    _mm256_store_si256((__m256i*)terr[0], squareErrorSum1);

    // Interleave selector bits
    __m256i minIndexLo0 = _mm256_unpacklo_epi16(sel0, sel1);
    __m256i minIndexHi0 = _mm256_unpackhi_epi16(sel0, sel1);

    __m256i minIndexLo1 = _mm256_permute2x128_si256(minIndexLo0, minIndexHi0, (0) | (2 << 4));
    __m256i minIndexHi1 = _mm256_permute2x128_si256(minIndexLo0, minIndexHi0, (1) | (3 << 4));

    __m256i minIndexHi2 = _mm256_slli_epi32(minIndexHi1, 1);

    __m256i sel = _mm256_or_si256(minIndexLo1, minIndexHi2);

    _mm256_store_si256((__m256i*)tsel, sel);
}

uint64_t VS_VECTORCALL EncodeSelectors_AVX2( uint64_t d, const uint32_t terr[2][8], const uint32_t tsel[8], const bool rotate) noexcept
{
    size_t tidx[2];

    // Get index of minimum error (terr[0] and terr[1])
    __m256i err0 = _mm256_load_si256((const __m256i*)terr[0]);
    __m256i err1 = _mm256_load_si256((const __m256i*)terr[1]);

    __m256i errLo = _mm256_permute2x128_si256(err0, err1, (0) | (2 << 4));
    __m256i errHi = _mm256_permute2x128_si256(err0, err1, (1) | (3 << 4));

    __m256i errMin0 = _mm256_min_epu32(errLo, errHi);

    __m256i errMin1 = _mm256_shuffle_epi32(errMin0, _MM_SHUFFLE(2, 3, 0, 1));
    __m256i errMin2 = _mm256_min_epu32(errMin0, errMin1);

    __m256i errMin3 = _mm256_shuffle_epi32(errMin2, _MM_SHUFFLE(1, 0, 3, 2));
    __m256i errMin4 = _mm256_min_epu32(errMin3, errMin2);

    __m256i errMin5 = _mm256_permute2x128_si256(errMin4, errMin4, (0) | (0 << 4));
    __m256i errMin6 = _mm256_permute2x128_si256(errMin4, errMin4, (1) | (1 << 4));

    __m256i errMask0 = _mm256_cmpeq_epi32(errMin5, err0);
    __m256i errMask1 = _mm256_cmpeq_epi32(errMin6, err1);

    uint32_t mask0 = _mm256_movemask_epi8(errMask0);
    uint32_t mask1 = _mm256_movemask_epi8(errMask1);

    tidx[0] = _bit_scan_forward(mask0) >> 2;
    tidx[1] = _bit_scan_forward(mask1) >> 2;

    d |= tidx[0] << 26;
    d |= tidx[1] << 29;

    unsigned int t0 = tsel[tidx[0]];
    unsigned int t1 = tsel[tidx[1]];

    if (!rotate)
    {
        t0 &= 0xFF00FF00;
        t1 &= 0x00FF00FF;
    }
    else
    {
        t0 &= 0xCCCCCCCC;
        t1 &= 0x33333333;
    }

    // Flip selectors from sign bit
    unsigned int t2 = (t0 | t1) ^ 0xFFFF0000;

    return d | static_cast<uint64_t>(_bswap(t2)) << 32;
}

static uint64_t ProcessRGB( const uint8_t* src )
{
    uint64_t d = CheckSolid_AVX2( src );
    if( d != 0 ) return d;

    alignas(32) v4i a[8];

    __m128i err0 = PrepareAverages_AVX2( a, src );

    // Get index of minimum error (err0)
    __m128i err1 = _mm_shuffle_epi32(err0, _MM_SHUFFLE(2, 3, 0, 1));
    __m128i errMin0 = _mm_min_epu32(err0, err1);

    __m128i errMin1 = _mm_shuffle_epi32(errMin0, _MM_SHUFFLE(1, 0, 3, 2));
    __m128i errMin2 = _mm_min_epu32(errMin1, errMin0);

    __m128i errMask = _mm_cmpeq_epi32(errMin2, err0);

    uint32_t mask = _mm_movemask_epi8(errMask);

    uint32_t idx = _bit_scan_forward(mask) >> 2;

    d |= EncodeAverages_AVX2( a, idx );

    alignas(32) uint32_t terr[2][8] = {};
    alignas(32) uint32_t tsel[8];

    if ((idx == 0) || (idx == 2))
    {
        FindBestFit_4x2_AVX2( terr, tsel, a, idx * 2, src );
    }
    else
    {
        FindBestFit_2x4_AVX2( terr, tsel, a, idx * 2, src );
    }

    return EncodeSelectors_AVX2( d, terr, tsel, (idx % 2) == 1 );
}

#else

#ifdef __SSE4_1__
#  ifdef _MSC_VER
#    include <intrin.h>
#    include <Windows.h>
#    define _bswap(x) _byteswap_ulong(x)
#  else
#    include <x86intrin.h>
#  endif
#else
#  ifndef _MSC_VER
#    ifdef __APPLE__
#      include <libkern/OSByteOrder.h>
#      ifndef _bswap
#        define _bswap(x) OSSwapInt32(x)
#      endif
#    else
#      include <byteswap.h>
#      ifndef _bswap
#        define _bswap(x) bswap_32(x)
#      endif
#    endif
#  endif
#endif

#ifndef _bswap
#  define _bswap(x) __builtin_bswap32(x)
#endif

namespace tracy
{

const uint32_t g_avg2[16] = {
    0x00,
    0x11,
    0x22,
    0x33,
    0x44,
    0x55,
    0x66,
    0x77,
    0x88,
    0x99,
    0xAA,
    0xBB,
    0xCC,
    0xDD,
    0xEE,
    0xFF
};

const int64_t g_table256[8][4] = {
    {  2*256,  8*256,   -2*256,   -8*256 },
    {  5*256, 17*256,   -5*256,  -17*256 },
    {  9*256, 29*256,   -9*256,  -29*256 },
    { 13*256, 42*256,  -13*256,  -42*256 },
    { 18*256, 60*256,  -18*256,  -60*256 },
    { 24*256, 80*256,  -24*256,  -80*256 },
    { 33*256, 106*256, -33*256, -106*256 },
    { 47*256, 183*256, -47*256, -183*256 }
};

const uint32_t g_id[4][16] = {
    { 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 3, 3, 2, 2, 3, 3, 2, 2, 3, 3, 2, 2, 3, 3, 2, 2 },
    { 5, 5, 5, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4 },
    { 7, 7, 6, 6, 7, 7, 6, 6, 7, 7, 6, 6, 7, 7, 6, 6 }
};

#ifdef __SSE4_1__
const __m128i g_table128_SIMD[2] =
{
    _mm_setr_epi16(   2*128,   5*128,   9*128,  13*128,  18*128,  24*128,  33*128,  47*128),
    _mm_setr_epi16(   8*128,  17*128,  29*128,  42*128,  60*128,  80*128, 106*128, 183*128)
};

const __m128i g_table256_SIMD[4] =
{
    _mm_setr_epi32(  2*256,   5*256,   9*256,  13*256),
    _mm_setr_epi32(  8*256,  17*256,  29*256,  42*256),
    _mm_setr_epi32( 18*256,  24*256,  33*256,  47*256),
    _mm_setr_epi32( 60*256,  80*256, 106*256, 183*256)
};
#endif

template<class T>
static inline T sq( T val )
{
    return val * val;
}

static inline int mul8bit( int a, int b )
{
    int t = a*b + 128;
    return ( t + ( t >> 8 ) ) >> 8;
}

template<class T>
static size_t GetLeastError( const T* err, size_t num )
{
    size_t idx = 0;
    for( size_t i=1; i<num; i++ )
    {
        if( err[i] < err[idx] )
        {
            idx = i;
        }
    }
    return idx;
}

static uint64_t FixByteOrder( uint64_t d )
{
    return ( ( d & 0x00000000FFFFFFFF ) ) |
           ( ( d & 0xFF00000000000000 ) >> 24 ) |
           ( ( d & 0x000000FF00000000 ) << 24 ) |
           ( ( d & 0x00FF000000000000 ) >> 8 ) |
           ( ( d & 0x0000FF0000000000 ) << 8 );
}

template<class T, class S>
static uint64_t EncodeSelectors( uint64_t d, const T terr[2][8], const S tsel[16][8], const uint32_t* id )
{
    size_t tidx[2];
    tidx[0] = GetLeastError( terr[0], 8 );
    tidx[1] = GetLeastError( terr[1], 8 );

    d |= tidx[0] << 26;
    d |= tidx[1] << 29;
    for( int i=0; i<16; i++ )
    {
        uint64_t t = tsel[i][tidx[id[i]%2]];
        d |= ( t & 0x1 ) << ( i + 32 );
        d |= ( t & 0x2 ) << ( i + 47 );
    }

    return d;
}

static void Average( const uint8_t* data, v4i* a )
{
#ifdef __SSE4_1__
    __m128i d0 = _mm_loadu_si128(((__m128i*)data) + 0);
    __m128i d1 = _mm_loadu_si128(((__m128i*)data) + 1);
    __m128i d2 = _mm_loadu_si128(((__m128i*)data) + 2);
    __m128i d3 = _mm_loadu_si128(((__m128i*)data) + 3);

    __m128i d0l = _mm_unpacklo_epi8(d0, _mm_setzero_si128());
    __m128i d0h = _mm_unpackhi_epi8(d0, _mm_setzero_si128());
    __m128i d1l = _mm_unpacklo_epi8(d1, _mm_setzero_si128());
    __m128i d1h = _mm_unpackhi_epi8(d1, _mm_setzero_si128());
    __m128i d2l = _mm_unpacklo_epi8(d2, _mm_setzero_si128());
    __m128i d2h = _mm_unpackhi_epi8(d2, _mm_setzero_si128());
    __m128i d3l = _mm_unpacklo_epi8(d3, _mm_setzero_si128());
    __m128i d3h = _mm_unpackhi_epi8(d3, _mm_setzero_si128());

    __m128i sum0 = _mm_add_epi16(d0l, d1l);
    __m128i sum1 = _mm_add_epi16(d0h, d1h);
    __m128i sum2 = _mm_add_epi16(d2l, d3l);
    __m128i sum3 = _mm_add_epi16(d2h, d3h);

    __m128i sum0l = _mm_unpacklo_epi16(sum0, _mm_setzero_si128());
    __m128i sum0h = _mm_unpackhi_epi16(sum0, _mm_setzero_si128());
    __m128i sum1l = _mm_unpacklo_epi16(sum1, _mm_setzero_si128());
    __m128i sum1h = _mm_unpackhi_epi16(sum1, _mm_setzero_si128());
    __m128i sum2l = _mm_unpacklo_epi16(sum2, _mm_setzero_si128());
    __m128i sum2h = _mm_unpackhi_epi16(sum2, _mm_setzero_si128());
    __m128i sum3l = _mm_unpacklo_epi16(sum3, _mm_setzero_si128());
    __m128i sum3h = _mm_unpackhi_epi16(sum3, _mm_setzero_si128());

    __m128i b0 = _mm_add_epi32(sum0l, sum0h);
    __m128i b1 = _mm_add_epi32(sum1l, sum1h);
    __m128i b2 = _mm_add_epi32(sum2l, sum2h);
    __m128i b3 = _mm_add_epi32(sum3l, sum3h);

    __m128i a0 = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(b2, b3), _mm_set1_epi32(4)), 3);
    __m128i a1 = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(b0, b1), _mm_set1_epi32(4)), 3);
    __m128i a2 = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(b1, b3), _mm_set1_epi32(4)), 3);
    __m128i a3 = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(b0, b2), _mm_set1_epi32(4)), 3);

    _mm_storeu_si128((__m128i*)&a[0], _mm_packus_epi32(_mm_shuffle_epi32(a0, _MM_SHUFFLE(3, 0, 1, 2)), _mm_shuffle_epi32(a1, _MM_SHUFFLE(3, 0, 1, 2))));
    _mm_storeu_si128((__m128i*)&a[2], _mm_packus_epi32(_mm_shuffle_epi32(a2, _MM_SHUFFLE(3, 0, 1, 2)), _mm_shuffle_epi32(a3, _MM_SHUFFLE(3, 0, 1, 2))));
#else
    uint32_t r[4];
    uint32_t g[4];
    uint32_t b[4];

    memset(r, 0, sizeof(r));
    memset(g, 0, sizeof(g));
    memset(b, 0, sizeof(b));

    for( int j=0; j<4; j++ )
    {
        for( int i=0; i<4; i++ )
        {
            int index = (j & 2) + (i >> 1);
            b[index] += *data++;
            g[index] += *data++;
            r[index] += *data++;
            data++;
        }
    }

    a[0] = v4i{ uint16_t( (r[2] + r[3] + 4) / 8 ), uint16_t( (g[2] + g[3] + 4) / 8 ), uint16_t( (b[2] + b[3] + 4) / 8 ), 0};
    a[1] = v4i{ uint16_t( (r[0] + r[1] + 4) / 8 ), uint16_t( (g[0] + g[1] + 4) / 8 ), uint16_t( (b[0] + b[1] + 4) / 8 ), 0};
    a[2] = v4i{ uint16_t( (r[1] + r[3] + 4) / 8 ), uint16_t( (g[1] + g[3] + 4) / 8 ), uint16_t( (b[1] + b[3] + 4) / 8 ), 0};
    a[3] = v4i{ uint16_t( (r[0] + r[2] + 4) / 8 ), uint16_t( (g[0] + g[2] + 4) / 8 ), uint16_t( (b[0] + b[2] + 4) / 8 ), 0};
#endif
}

static void CalcErrorBlock( const uint8_t* data, unsigned int err[4][4] )
{
#ifdef __SSE4_1__
    __m128i d0 = _mm_loadu_si128(((__m128i*)data) + 0);
    __m128i d1 = _mm_loadu_si128(((__m128i*)data) + 1);
    __m128i d2 = _mm_loadu_si128(((__m128i*)data) + 2);
    __m128i d3 = _mm_loadu_si128(((__m128i*)data) + 3);

    __m128i dm0 = _mm_and_si128(d0, _mm_set1_epi32(0x00FFFFFF));
    __m128i dm1 = _mm_and_si128(d1, _mm_set1_epi32(0x00FFFFFF));
    __m128i dm2 = _mm_and_si128(d2, _mm_set1_epi32(0x00FFFFFF));
    __m128i dm3 = _mm_and_si128(d3, _mm_set1_epi32(0x00FFFFFF));

    __m128i d0l = _mm_unpacklo_epi8(dm0, _mm_setzero_si128());
    __m128i d0h = _mm_unpackhi_epi8(dm0, _mm_setzero_si128());
    __m128i d1l = _mm_unpacklo_epi8(dm1, _mm_setzero_si128());
    __m128i d1h = _mm_unpackhi_epi8(dm1, _mm_setzero_si128());
    __m128i d2l = _mm_unpacklo_epi8(dm2, _mm_setzero_si128());
    __m128i d2h = _mm_unpackhi_epi8(dm2, _mm_setzero_si128());
    __m128i d3l = _mm_unpacklo_epi8(dm3, _mm_setzero_si128());
    __m128i d3h = _mm_unpackhi_epi8(dm3, _mm_setzero_si128());

    __m128i sum0 = _mm_add_epi16(d0l, d1l);
    __m128i sum1 = _mm_add_epi16(d0h, d1h);
    __m128i sum2 = _mm_add_epi16(d2l, d3l);
    __m128i sum3 = _mm_add_epi16(d2h, d3h);

    __m128i sum0l = _mm_unpacklo_epi16(sum0, _mm_setzero_si128());
    __m128i sum0h = _mm_unpackhi_epi16(sum0, _mm_setzero_si128());
    __m128i sum1l = _mm_unpacklo_epi16(sum1, _mm_setzero_si128());
    __m128i sum1h = _mm_unpackhi_epi16(sum1, _mm_setzero_si128());
    __m128i sum2l = _mm_unpacklo_epi16(sum2, _mm_setzero_si128());
    __m128i sum2h = _mm_unpackhi_epi16(sum2, _mm_setzero_si128());
    __m128i sum3l = _mm_unpacklo_epi16(sum3, _mm_setzero_si128());
    __m128i sum3h = _mm_unpackhi_epi16(sum3, _mm_setzero_si128());

    __m128i b0 = _mm_add_epi32(sum0l, sum0h);
    __m128i b1 = _mm_add_epi32(sum1l, sum1h);
    __m128i b2 = _mm_add_epi32(sum2l, sum2h);
    __m128i b3 = _mm_add_epi32(sum3l, sum3h);

    __m128i a0 = _mm_add_epi32(b2, b3);
    __m128i a1 = _mm_add_epi32(b0, b1);
    __m128i a2 = _mm_add_epi32(b1, b3);
    __m128i a3 = _mm_add_epi32(b0, b2);

    _mm_storeu_si128((__m128i*)&err[0], a0);
    _mm_storeu_si128((__m128i*)&err[1], a1);
    _mm_storeu_si128((__m128i*)&err[2], a2);
    _mm_storeu_si128((__m128i*)&err[3], a3);
#else
    unsigned int terr[4][4];

    memset(terr, 0, 16 * sizeof(unsigned int));

    for( int j=0; j<4; j++ )
    {
        for( int i=0; i<4; i++ )
        {
            int index = (j & 2) + (i >> 1);
            unsigned int d = *data++;
            terr[index][0] += d;
            d = *data++;
            terr[index][1] += d;
            d = *data++;
            terr[index][2] += d;
            data++;
        }
    }

    for( int i=0; i<3; i++ )
    {
        err[0][i] = terr[2][i] + terr[3][i];
        err[1][i] = terr[0][i] + terr[1][i];
        err[2][i] = terr[1][i] + terr[3][i];
        err[3][i] = terr[0][i] + terr[2][i];
    }
    for( int i=0; i<4; i++ )
    {
        err[i][3] = 0;
    }
#endif
}

static unsigned int CalcError( const unsigned int block[4], const v4i& average )
{
    unsigned int err = 0x3FFFFFFF; // Big value to prevent negative values, but small enough to prevent overflow
    err -= block[0] * 2 * average[2];
    err -= block[1] * 2 * average[1];
    err -= block[2] * 2 * average[0];
    err += 8 * ( sq( average[0] ) + sq( average[1] ) + sq( average[2] ) );
    return err;
}

void ProcessAverages( v4i* a )
{
#ifdef __SSE4_1__
    for( int i=0; i<2; i++ )
    {
        __m128i d = _mm_loadu_si128((__m128i*)a[i*2].data());

        __m128i t = _mm_add_epi16(_mm_mullo_epi16(d, _mm_set1_epi16(31)), _mm_set1_epi16(128));

        __m128i c = _mm_srli_epi16(_mm_add_epi16(t, _mm_srli_epi16(t, 8)), 8);

        __m128i c1 = _mm_shuffle_epi32(c, _MM_SHUFFLE(3, 2, 3, 2));
        __m128i diff = _mm_sub_epi16(c, c1);
        diff = _mm_max_epi16(diff, _mm_set1_epi16(-4));
        diff = _mm_min_epi16(diff, _mm_set1_epi16(3));

        __m128i co = _mm_add_epi16(c1, diff);

        c = _mm_blend_epi16(co, c, 0xF0);

        __m128i a0 = _mm_or_si128(_mm_slli_epi16(c, 3), _mm_srli_epi16(c, 2));

        _mm_storeu_si128((__m128i*)a[4+i*2].data(), a0);
    }

    for( int i=0; i<2; i++ )
    {
        __m128i d = _mm_loadu_si128((__m128i*)a[i*2].data());

        __m128i t0 = _mm_add_epi16(_mm_mullo_epi16(d, _mm_set1_epi16(15)), _mm_set1_epi16(128));
        __m128i t1 = _mm_srli_epi16(_mm_add_epi16(t0, _mm_srli_epi16(t0, 8)), 8);

        __m128i t2 = _mm_or_si128(t1, _mm_slli_epi16(t1, 4));

        _mm_storeu_si128((__m128i*)a[i*2].data(), t2);
    }
#else
    for( int i=0; i<2; i++ )
    {
        for( int j=0; j<3; j++ )
        {
            int32_t c1 = mul8bit( a[i*2+1][j], 31 );
            int32_t c2 = mul8bit( a[i*2][j], 31 );

            int32_t diff = c2 - c1;
            if( diff > 3 ) diff = 3;
            else if( diff < -4 ) diff = -4;

            int32_t co = c1 + diff;

            a[5+i*2][j] = ( c1 << 3 ) | ( c1 >> 2 );
            a[4+i*2][j] = ( co << 3 ) | ( co >> 2 );
        }
    }

    for( int i=0; i<4; i++ )
    {
        a[i][0] = g_avg2[mul8bit( a[i][0], 15 )];
        a[i][1] = g_avg2[mul8bit( a[i][1], 15 )];
        a[i][2] = g_avg2[mul8bit( a[i][2], 15 )];
    }
#endif
}

static void EncodeAverages( uint64_t& _d, const v4i* a, size_t idx )
{
    auto d = _d;
    d |= ( idx << 24 );
    size_t base = idx << 1;

    if( ( idx & 0x2 ) == 0 )
    {
        for( int i=0; i<3; i++ )
        {
            d |= uint64_t( a[base+0][i] >> 4 ) << ( i*8 );
            d |= uint64_t( a[base+1][i] >> 4 ) << ( i*8 + 4 );
        }
    }
    else
    {
        for( int i=0; i<3; i++ )
        {
            d |= uint64_t( a[base+1][i] & 0xF8 ) << ( i*8 );
            int32_t c = ( ( a[base+0][i] & 0xF8 ) - ( a[base+1][i] & 0xF8 ) ) >> 3;
            c &= ~0xFFFFFFF8;
            d |= ((uint64_t)c) << ( i*8 );
        }
    }
    _d = d;
}

static uint64_t CheckSolid( const uint8_t* src )
{
#ifdef __SSE4_1__
    __m128i d0 = _mm_loadu_si128(((__m128i*)src) + 0);
    __m128i d1 = _mm_loadu_si128(((__m128i*)src) + 1);
    __m128i d2 = _mm_loadu_si128(((__m128i*)src) + 2);
    __m128i d3 = _mm_loadu_si128(((__m128i*)src) + 3);

    __m128i c = _mm_shuffle_epi32(d0, _MM_SHUFFLE(0, 0, 0, 0));

    __m128i c0 = _mm_cmpeq_epi8(d0, c);
    __m128i c1 = _mm_cmpeq_epi8(d1, c);
    __m128i c2 = _mm_cmpeq_epi8(d2, c);
    __m128i c3 = _mm_cmpeq_epi8(d3, c);

    __m128i m0 = _mm_and_si128(c0, c1);
    __m128i m1 = _mm_and_si128(c2, c3);
    __m128i m = _mm_and_si128(m0, m1);

    if (!_mm_testc_si128(m, _mm_set1_epi32(-1)))
    {
        return 0;
    }
#else
    const uint8_t* ptr = src + 4;
    for( int i=1; i<16; i++ )
    {
        if( memcmp( src, ptr, 4 ) != 0 )
        {
            return 0;
        }
        ptr += 4;
    }
#endif
    return 0x02000000 |
        ( (unsigned int)( src[0] & 0xF8 ) << 16 ) |
        ( (unsigned int)( src[1] & 0xF8 ) << 8 ) |
        ( (unsigned int)( src[2] & 0xF8 ) );
}

static void PrepareAverages( v4i a[8], const uint8_t* src, unsigned int err[4] )
{
    Average( src, a );
    ProcessAverages( a );

    unsigned int errblock[4][4];
    CalcErrorBlock( src, errblock );

    for( int i=0; i<4; i++ )
    {
        err[i/2] += CalcError( errblock[i], a[i] );
        err[2+i/2] += CalcError( errblock[i], a[i+4] );
    }
}

static void FindBestFit( uint64_t terr[2][8], uint16_t tsel[16][8], v4i a[8], const uint32_t* id, const uint8_t* data )
{
    for( size_t i=0; i<16; i++ )
    {
        uint16_t* sel = tsel[i];
        unsigned int bid = id[i];
        uint64_t* ter = terr[bid%2];

        uint8_t b = *data++;
        uint8_t g = *data++;
        uint8_t r = *data++;
        data++;

        int dr = a[bid][0] - r;
        int dg = a[bid][1] - g;
        int db = a[bid][2] - b;

        int pix = dr * 77 + dg * 151 + db * 28;

        for( int t=0; t<8; t++ )
        {
            const int64_t* tab = g_table256[t];
            unsigned int idx = 0;
            uint64_t err = sq( tab[0] + pix );
            for( int j=1; j<4; j++ )
            {
                uint64_t local = sq( tab[j] + pix );
                if( local < err )
                {
                    err = local;
                    idx = j;
                }
            }
            *sel++ = idx;
            *ter++ += err;
        }
    }
}

#ifdef __SSE4_1__
// Non-reference implementation, but faster. Produces same results as the AVX2 version
static void FindBestFit( uint32_t terr[2][8], uint16_t tsel[16][8], v4i a[8], const uint32_t* id, const uint8_t* data )
{
    for( size_t i=0; i<16; i++ )
    {
        uint16_t* sel = tsel[i];
        unsigned int bid = id[i];
        uint32_t* ter = terr[bid%2];

        uint8_t b = *data++;
        uint8_t g = *data++;
        uint8_t r = *data++;
        data++;

        int dr = a[bid][0] - r;
        int dg = a[bid][1] - g;
        int db = a[bid][2] - b;

        // The scaling values are divided by two and rounded, to allow the differences to be in the range of signed int16
        // This produces slightly different results, but is significant faster
        __m128i pixel = _mm_set1_epi16(dr * 38 + dg * 76 + db * 14);
        __m128i pix = _mm_abs_epi16(pixel);

        // Taking the absolute value is way faster. The values are only used to sort, so the result will be the same.
        // Since the selector table is symmetrical, we need to calculate the difference only for half of the entries.
        __m128i error0 = _mm_abs_epi16(_mm_sub_epi16(pix, g_table128_SIMD[0]));
        __m128i error1 = _mm_abs_epi16(_mm_sub_epi16(pix, g_table128_SIMD[1]));

        __m128i index = _mm_and_si128(_mm_cmplt_epi16(error1, error0), _mm_set1_epi16(1));
        __m128i minError = _mm_min_epi16(error0, error1);

        // Exploiting symmetry of the selector table and use the sign bit
        // This produces slightly different results, but is needed to produce same results as AVX2 implementation
        __m128i indexBit = _mm_andnot_si128(_mm_srli_epi16(pixel, 15), _mm_set1_epi8(-1));
        __m128i minIndex = _mm_or_si128(index, _mm_add_epi16(indexBit, indexBit));

        // Squaring the minimum error to produce correct values when adding
        __m128i squareErrorLo = _mm_mullo_epi16(minError, minError);
        __m128i squareErrorHi = _mm_mulhi_epi16(minError, minError);

        __m128i squareErrorLow = _mm_unpacklo_epi16(squareErrorLo, squareErrorHi);
        __m128i squareErrorHigh = _mm_unpackhi_epi16(squareErrorLo, squareErrorHi);

        squareErrorLow = _mm_add_epi32(squareErrorLow, _mm_loadu_si128(((__m128i*)ter) + 0));
        _mm_storeu_si128(((__m128i*)ter) + 0, squareErrorLow);
        squareErrorHigh = _mm_add_epi32(squareErrorHigh, _mm_loadu_si128(((__m128i*)ter) + 1));
        _mm_storeu_si128(((__m128i*)ter) + 1, squareErrorHigh);

        _mm_storeu_si128((__m128i*)sel, minIndex);
    }
}
#endif

static uint64_t ProcessRGB( const uint8_t* src )
{
    uint64_t d = CheckSolid( src );
    if( d != 0 ) return d;

    v4i a[8];
    unsigned int err[4] = {};
    PrepareAverages( a, src, err );
    size_t idx = GetLeastError( err, 4 );
    EncodeAverages( d, a, idx );

#if defined __SSE4_1__
    uint32_t terr[2][8] = {};
#else
    uint64_t terr[2][8] = {};
#endif
    uint16_t tsel[16][8];
    auto id = g_id[idx];
    FindBestFit( terr, tsel, a, id, src );

    return FixByteOrder( EncodeSelectors( d, terr, tsel, id ) );
}

#endif

void CompressImageEtc1( const char* src, char* dst, int w, int h )
{
    assert( (w % 4) == 0 && (h % 4) == 0 );

    uint32_t buf[4*4];
    int i = 0;

    auto ptr = dst;
    auto blocks = w * h / 16;
    do
    {
        auto tmp = (char*)buf;
        for( int x=0; x<4; x++ )
        {
            memcpy( tmp,      src,          4 );
            memcpy( tmp + 4,  src + w * 4,  4 );
            memcpy( tmp + 8,  src + w * 8,  4 );
            memcpy( tmp + 12, src + w * 12, 4 );
            src += 4;
            tmp += 16;
        }
        if( ++i == w/4 )
        {
            src += w * 3 * 4;
            i = 0;
        }

        const auto c = ProcessRGB( (uint8_t*)buf );
        memcpy( ptr, &c, sizeof( uint64_t ) );
        ptr += sizeof( uint64_t );
    }
    while( --blocks );
}

}
