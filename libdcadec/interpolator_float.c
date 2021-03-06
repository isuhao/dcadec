/*
 * This file is part of libdcadec.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "common.h"
#include "interpolator.h"
#include "idct.h"
#include "fixed_math.h"
#include "fir_float.h"

static const double lfe_iir_scale = 0.001985816114019982;

static const double lfe_iir[12] = {
    -0.9698530866436986,  1.9696040724997900,  0.9999999999999996,
    -1.9643358221499630, -0.9787938538720836,  1.9785545764679620,
     1.0000000000000020, -1.9951847249255470, -0.9925096137076496,
     1.9922787089263100,  1.0000000000000000, -1.9974180593495760
};

static inline int convert(double a)
{
    return clip23(lrint(a));
}

static void interpolate_lfe(int *pcm_samples, int *lfe_samples, int npcmblocks,
                            const double *filter_coeff, bool dec_select)
{
    // Select decimation factor
    int factor = 64 << dec_select;
    int ncoeffs = 8 >> dec_select;
    int nlfesamples = npcmblocks >> (dec_select + 1);

    // Interpolation
    for (int i = 0; i < nlfesamples; i++) {
        int *src = lfe_samples + MAX_LFE_HISTORY + i;

        // One decimated sample generates 64 or 128 interpolated ones
        for (int j = 0; j < factor / 2; j++) {
            // Clear accumulation
            double res1 = 0.0;
            double res2 = 0.0;

            // Accumulate
            for (int k = 0; k < ncoeffs; k++) {
                res1 += filter_coeff[      j * ncoeffs + k] * src[-k];
                res2 += filter_coeff[255 - j * ncoeffs - k] * src[-k];
            }

            // Save interpolated samples
            pcm_samples[             j] = convert(res1);
            pcm_samples[factor / 2 + j] = convert(res2);
        }

        // Advance output pointer
        pcm_samples += factor;
    }

    // Update history
    for (int n = MAX_LFE_HISTORY - 1; n >= MAX_LFE_HISTORY - 8; n--)
        lfe_samples[n] = lfe_samples[nlfesamples + n];
}

INTERPOLATE_LFE(lfe_float_fir)
{
    (void)dec_select;
    assert(!dec_select);

    interpolate_lfe(pcm_samples, lfe_samples, npcmblocks, lfe_fir_64, false);
}

INTERPOLATE_LFE(lfe_float_fir_2x)
{
    (void)dec_select;
    assert(dec_select);

    interpolate_lfe(pcm_samples, lfe_samples, npcmblocks, lfe_fir_128, true);
}

INTERPOLATE_LFE(lfe_float_iir)
{
    // Select decimation factor
    int factor = 64 << dec_select;
    int nlfesamples = npcmblocks >> (dec_select + 1);

    // Load history
    double lfe_history[6];
    for (int i = 0; i < 6; i++)
        lfe_history[i] = ((double *)lfe_samples)[i];

    // Interpolation
    for (int i = 0; i < nlfesamples; i++) {
        double res1 = lfe_samples[MAX_LFE_HISTORY + i] * lfe_iir_scale;
        double res2;

        // One decimated sample generates 64 or 128 interpolated ones
        for (int j = 0; j < factor; j++) {
            // Filter
            for (int k = 0; k < 3; k++) {
                double tmp1 = lfe_history[k * 2 + 0];
                double tmp2 = lfe_history[k * 2 + 1];

                res2 = tmp1 * lfe_iir[k * 4 + 0] + tmp2 * lfe_iir[k * 4 + 1] + res1;
                res1 = tmp1 * lfe_iir[k * 4 + 2] + tmp2 * lfe_iir[k * 4 + 3] + res2;

                lfe_history[k * 2 + 0] = tmp2;
                lfe_history[k * 2 + 1] = res2;
            }

            // Save interpolated samples
            *pcm_samples++ = convert(res1);
            res1 = 0.0;
        }
    }

    // Store history
    for (int i = 0; i < 6; i++)
        ((double *)lfe_samples)[i] = lfe_history[i];
}

INTERPOLATE_SUB(sub32_float)
{
    (void)subband_samples_hi;
    assert(!subband_samples_hi);

    // Get history pointer
    double *history = dsp->history;

    // Select filter
    const double *filter_coeff = perfect ? band_fir_perfect : band_fir_nonperfect;

    // Interpolation begins
    for (int sample = 0; sample < nsamples; sample++) {
        int i, j, k;

        // Load in one sample from each subband
        double input[32];
        for (i = 0; i < 32; i++)
            input[i] = subband_samples_lo[i][sample];

        // Inverse DCT
        double output[32];
        idct_fast(dsp->idct, input, output);

        // Store history
        for (i = 0, k = 31; i < 16; i++, k--) {
            history[     i] = output[i] - output[k];
            history[16 + i] = output[i] + output[k];
        }

        // One subband sample generates 32 interpolated ones
        for (i = 0, k = 15; i < 16; i++, k--) {
            // Clear accumulation
            double res1 = 0.0;
            double res2 = 0.0;

            // Accumulate
            for (j = 0; j < 512; j += 64) {
                res1 += history[i + j] * filter_coeff[     i + j];
                res2 += history[k + j] * filter_coeff[16 + i + j];
            }

            for (j = 32; j < 512; j += 64) {
                res1 += history[16 + i + j] * filter_coeff[     i + j];
                res2 += history[16 + k + j] * filter_coeff[16 + i + j];
            }

            // Save interpolated samples
            pcm_samples[     i] = convert(res1);
            pcm_samples[16 + i] = convert(res2);
        }

        // Advance output pointer
        pcm_samples += 32;

        // Shift history
        for (i = 511; i >= 32; i--)
            history[i] = history[i - 32];
    }
}

INTERPOLATE_SUB(sub64_float)
{
    (void)perfect;

    // Get history pointer
    double *history = dsp->history;

    // Interpolation begins
    for (int sample = 0; sample < nsamples; sample++) {
        int i, j, k;

        // Load in one sample from each subband
        double input[64];
        if (subband_samples_hi) {
            // Full 64 subbands, first 32 are residual coded
            for (i =  0; i < 32; i++)
                input[i] = subband_samples_lo[i][sample] + subband_samples_hi[i][sample];
            for (i = 32; i < 64; i++)
                input[i] = subband_samples_hi[i][sample];
        } else {
            // Only first 32 subbands
            for (i =  0; i < 32; i++)
                input[i] = subband_samples_lo[i][sample];
            for (i = 32; i < 64; i++)
                input[i] = 0;
        }

        // Inverse DCT
        double output[64];
        idct_fast(dsp->idct, input, output);

        // Store history
        for (i = 0, k = 63; i < 32; i++, k--) {
            history[     i] = output[i] - output[k];
            history[32 + i] = output[i] + output[k];
        }

        // One subband sample generates 64 interpolated ones
        for (i = 0, k = 31; i < 32; i++, k--) {
            // Clear accumulation
            double res1 = 0.0;
            double res2 = 0.0;

            // Accumulate
            for (j = 0; j < 1024; j += 128) {
                res1 += history[i + j] * band_fir_x96[     i + j];
                res2 += history[k + j] * band_fir_x96[32 + i + j];
            }

            for (j = 64; j < 1024; j += 128) {
                res1 += history[32 + i + j] * band_fir_x96[     i + j];
                res2 += history[32 + k + j] * band_fir_x96[32 + i + j];
            }

            // Save interpolated samples
            pcm_samples[     i] = convert(res1);
            pcm_samples[32 + i] = convert(res2);
        }

        // Advance output pointer
        pcm_samples += 64;

        // Shift history
        for (i = 1023; i >= 64; i--)
            history[i] = history[i - 64];
    }
}
