/*
 * Project 25 IMBE Encoder/Decoder Fixed-Point implementation
 * Developed by Pavel Yazev E-mail: pyazev@gmail.com
 * Version 1.0 (c) Copyright 2009
 * 
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * The software is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Boston, MA
 * 02110-1301, USA.
 */


#include "typedef.h"
#include "globals.h"
#include "imbe.h"
#include "basic_op.h"
#include "dsp_sub.h"
#include "aux_sub.h"
#include "tbls.h"
#include "encode.h"
#include "dc_rmv.h"
#include "pe_lpf.h"
#include "pitch_est.h"
#include "pitch_ref.h"
#include "v_uv_det.h"
#include "sa_encode.h"
#include "ch_encode.h"
#include "imbe_vocoder.h"
#include <math.h>




void imbe_vocoder::encode_init(void)
{
	v_zap(pitch_est_buf, PITCH_EST_BUF_SIZE);
	v_zap(pitch_ref_buf, PITCH_EST_BUF_SIZE);
	v_zap(pe_lpf_mem, PE_LPF_ORD);
	pitch_est_init();
	fft_init();
	dc_rmv_mem = 0;
	sa_encode_init();
	pitch_ref_init();
}


void imbe_vocoder::encode(IMBE_PARAM *imbe_param, Word16 *frame_vector, Word16 *snd)
{
	analyse_pitch(imbe_param, snd);
	v_uv_det(imbe_param, fft_buf);
	sa_encode(imbe_param);
	encode_frame_vector(imbe_param, frame_vector);
}

// mbevocoder: encode() in stages (imbe_vocoder.h), the same arithmetic.
void imbe_vocoder::imbe_analyse_voicing(void)
{
	v_uv_det(&my_imbe_param, fft_buf);
}

void imbe_vocoder::imbe_quantise(int16_t *frame_vector)
{
	sa_encode(&my_imbe_param);
	encode_frame_vector(&my_imbe_param, frame_vector);
}

void imbe_vocoder::imbe_set_pitch(IMBE_PARAM *imbe_param, int16_t ref_pitch)
{
	Word16 shift = norm_s(ref_pitch);
	Word16 tmp = shl(ref_pitch, shift);
	tmp = div_s(0x4000, tmp);
	imbe_param->fund_freq = L_shl(tmp, shift + 11);
	imbe_param->ref_pitch = ref_pitch;
}

void imbe_vocoder::analyse_pitch(IMBE_PARAM *imbe_param, Word16 *snd)
{
	Word16 i;
	Word16 *wr_ptr, *sig_ptr;
	
	for(i = 0; i < PITCH_EST_BUF_SIZE - FRAME; i++)
	{
		pitch_est_buf[i] = pitch_est_buf[i + FRAME];
		pitch_ref_buf[i] = pitch_ref_buf[i + FRAME];
	}
		
	dc_rmv(snd, &pitch_ref_buf[PITCH_EST_BUF_SIZE - FRAME], &dc_rmv_mem, FRAME);
	pe_lpf(&pitch_ref_buf[PITCH_EST_BUF_SIZE - FRAME], &pitch_est_buf[PITCH_EST_BUF_SIZE - FRAME], pe_lpf_mem, FRAME);

	pitch_est(imbe_param, pitch_est_buf);

    //
	// Speech windowing and FFT calculation
	//
	wr_ptr  = (Word16 *)wr;
	sig_ptr = &pitch_ref_buf[40];
	for(i = 146; i < 256; i++) 
	{
		fft_buf[i].re = mult(*sig_ptr++, *wr_ptr++); 
		fft_buf[i].im = 0;
	}
	fft_buf[0].re = *sig_ptr++;
	fft_buf[0].im = 0;
	wr_ptr--;
	for(i = 1; i < 111; i++) 
	{
		fft_buf[i].re = mult(*sig_ptr++, *wr_ptr--); 
		fft_buf[i].im = 0;
	}
	for(i = 111; i < 146; i++) 
		fft_buf[i].re = fft_buf[i].im = 0;

	fft((Word16 *)&fft_buf, FFTLENGTH, 1);

	pitch_ref(imbe_param, fft_buf);
	if (forced_ref_pitch > 0)
	{
		// mbevocoder experiments: analyse at an externally supplied pitch (fund_freq as pitch_ref)
		imbe_set_pitch(imbe_param, forced_ref_pitch);
		forced_ref_pitch = 0;
	}
}

void imbe_vocoder::rescale_input(int shift)
{
	if (shift == 0)
		return;
	Word16 i;
	Word16 s = (Word16)(shift > 0 ? shift : -shift);
	for (i = 0; i < PITCH_EST_BUF_SIZE; i++) {
		pitch_est_buf[i] = shift > 0 ? shl(pitch_est_buf[i], s) : shr(pitch_est_buf[i], s);
		pitch_ref_buf[i] = shift > 0 ? shl(pitch_ref_buf[i], s) : shr(pitch_ref_buf[i], s);
	}
	for (i = 0; i < PE_LPF_ORD; i++)
		pe_lpf_mem[i] = shift > 0 ? shl(pe_lpf_mem[i], s) : shr(pe_lpf_mem[i], s);
	dc_rmv_mem = shift > 0 ? L_shl(dc_rmv_mem, s) : L_shr(dc_rmv_mem, s);
	th_max = shift > 0 ? L_shl(th_max, (Word16)(2 * s)) : L_shr(th_max, (Word16)(2 * s));   // an energy
	th_max_wide = ldexp(th_max_wide, 2 * shift);
	gate_max = ldexp(gate_max, 2 * shift);
	for (i = 0; i < 128; i++) {   // the noise gate's powers
		ng_noise[i] = ldexp(ng_noise[i], 2 * shift);
		ng_smooth[i] = ldexp(ng_smooth[i], 2 * shift);
	}
	ng_loud = ldexp(ng_loud, 2 * shift);
}

// mbevocoder additions: model-domain quantisation for the parameter transcoder.
Word16 imbe_vocoder::imbe_num_harms(Word16 ref_pitch)
{
	// As v_uv_det: fix(0.9254 * fix(pitch / 2 + 0.5)), clamped to the harmonic range.
	Word16 tmp = shr(add(shr(ref_pitch, 1), CNST_0_25_Q8_8), 8);
	Word16 num_harms = extract_h((UWord32)CNST_0_9254_Q0_16 * tmp);
	if (num_harms < NUM_HARMS_MIN) num_harms = NUM_HARMS_MIN;
	else if (num_harms > NUM_HARMS_MAX) num_harms = NUM_HARMS_MAX;
	return num_harms;
}

void imbe_vocoder::imbe_encode_model(IMBE_PARAM *p, Word16 *frame_vector)
{
	Word16 num_harms = imbe_num_harms(p->ref_pitch);
	Word16 num_bands;
	if (num_harms <= 36)
		num_bands = extract_h((UWord32)(num_harms + 2) * CNST_0_33_Q0_16);   // fix((L+2)/3)
	else
		num_bands = NUM_BANDS_MAX;
	p->num_harms = num_harms;
	p->num_bands = num_bands;
	// Voicing word: one bit per band, first band in the MSB; a band is voiced when its
	// first harmonic is (the caller marks whole bands).
	Word16 b1_vec = 0;
	Word16 uv = 0;
	for (Word16 band = 0; band < num_bands; band++) {
		Word16 first = band * 3;
		Word16 v = p->v_uv_dsn[first] ? 1 : 0;
		b1_vec = (Word16)((b1_vec << 1) | v);
		Word16 last = (band == num_bands - 1) ? (Word16)(num_harms - 1) : (Word16)(first + 2);
		for (Word16 i = first; i <= last && i < num_harms; i++) {
			p->v_uv_dsn[i] = v;
			if (!v) uv++;
		}
	}
	p->l_uv = uv;
	p->b_vec[1] = b1_vec;
	p->b_vec[0] = shr(sub(p->ref_pitch, 0x1380), 7);   // fix(2*pitch - 39)
	p->e_p = 0;
	sa_encode(p);
	encode_frame_vector(p, frame_vector);
}
