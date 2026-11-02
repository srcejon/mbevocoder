/*
 * Project 25 IMBE Encoder/Decoder Fixed-Point implementation
 * Developed by Pavel Yazev E-mail: pyazev@gmail.com
 * Version 1.0 (c) Copyright 2009
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Licensed as the rest of this vocoder (see encode.cc) and distributed with OP25 under
 * GPL-3.0-or-later; changes marked mbevocoder (NOTICE.md).
 */
/* -*- c++ -*- */
#ifndef INCLUDED_IMBE_VOCODER_H
#define INCLUDED_IMBE_VOCODER_H

#include <stdint.h>
#include <string.h>

#include "imbe.h"
#include "dsp_sub.h"
#include "basic_op.h"
#include "math_sub.h"
#include "encode.h"
#include "decode.h"

class imbe_vocoder
{
public:
	imbe_vocoder(void);	// constructor
	~imbe_vocoder() {}	// destructor
	// imbe_encode compresses 160 samples (in unsigned int format)
	// outputs u[] vectors as frame_vector[]
	void imbe_encode(int16_t *frame_vector, int16_t *snd) {
		encode(&my_imbe_param, frame_vector, snd);
	}
	// imbe_decode decodes IMBE codewords (frame_vector),
	// outputs the resulting 160 audio samples (snd)
	void imbe_decode(int16_t *frame_vector, int16_t *snd) {
		decode(&my_imbe_param, frame_vector, snd);
	}
	// hack to enable ambe encoder read access to speech parameters
	const IMBE_PARAM* param(void) const {return &my_imbe_param;}
	void set_gain_adjust(float gain_adjust) {d_gain_adjust = gain_adjust;}
	// mbevocoder: quantise a frame from MBE model parameters instead of speech (transcoding).
	// p must carry ref_pitch (Q8.8 samples, 21..122.5 in steps of 0.5), sa[] (Q14.2 linear
	// amplitudes, analysis scale) and v_uv_dsn[] for the harmonic count that ref_pitch implies;
	// this fills num_harms/num_bands/b_vec and the frame vector. Uses the same amplitude
	// prediction state as imbe_encode.
	void imbe_encode_model(IMBE_PARAM *p, int16_t *frame_vector);
	static int16_t imbe_num_harms(int16_t ref_pitch);
	// mbevocoder experiments: when > 0, replaces the refined pitch (Q8.8 samples) of the next
	// encoded frame after pitch refinement, so voicing and amplitudes are analysed at that
	// pitch; the pitch tracker's own history is not touched. Cleared after each frame.
	void set_forced_pitch(int16_t ref_pitch) {forced_ref_pitch = ref_pitch;}
	// mbevocoder: scale the standard's voicing threshold (percent, every band; > 100 voices more).
	// band_thr holds the scaled threshold. The IMBE encoder sets it (MBEVOCODER_IMBE_VOICING_THRESHOLD,
	// docs/encoder-quality.md "Voicing threshold"); the AMBE encoders leave it at 100 and apply their
	// own per-band multipliers to band_thr.
	void set_voicing_scale(int16_t pct) {voicing_scale_pct = pct;}
	// mbevocoder: keep the voicing threshold's frame energy and its tracked maximum (the standard's
	// M function) in double precision. The encoder's input gain (rescale_input) moves the input
	// towards full scale and the maximum with it, which in 32 bits saturates both: a quiet frame
	// then looks as loud as the speech before it and loses the standard's relative threshold.
	void set_wide_energy(bool on) {wide_energy = on;}
	// mbevocoder, with set_wide_energy: scale the voicing threshold by (level - r0) / w, clamped to
	// 0..1, where level is the frame's energy above 200 Hz against its tracked maximum in dB; w = 0
	// turns it off. With the input gain the analysis voices quiet background (at the bottom of
	// the pitch range, as a comb across every band) that its lost resolution used to leave
	// unvoiced, as the AMBE-3000R's and DVSI's encoders do.
	void set_level_gate(double r0, double w) {level_gate_r0 = r0; level_gate_w = w;}
	// mbevocoder: the background-noise gate (v_uv_det.cc, noise_gate_bands): the analysis tracks the
	// background per bin over frames well under the loudest, and at the lowest pitches leaves a
	// band unvoiced whose 500 Hz column holds under twice that noise, where the voicing test
	// passes noise (mains hum in the pauses of speech came out as a voiced comb).
	void set_noise_gate(bool on) {noise_gate = on;}

	// mbevocoder: imbe_encode in three stages, so a caller can change the frame between them:
	// imbe_analyse_pitch (input buffers, pitch estimation and refinement: ref_pitch, fund_freq),
	// imbe_analyse_voicing (voicing and amplitudes at that pitch: num_harms, v_uv_dsn, sa, b_vec[0..1])
	// and imbe_quantise (amplitude quantisation and the frame vector). The object is copyable, so
	// a stage can be rerun from a copy taken before it.
	void imbe_analyse_pitch(int16_t *snd) {analyse_pitch(&my_imbe_param, snd);}
	void imbe_analyse_voicing(void);
	void imbe_quantise(int16_t *frame_vector);
	IMBE_PARAM* mutable_param(void) {return &my_imbe_param;}
	// mbevocoder (diagnostics): decode a frame vector to the model only (fundamental, voicing and
	// the amplitudes sa, Q14.2, before enhancement) into param(); the decoder state advances as
	// imbe_decode's does. For comparing this codec's own reconstruction with another decoder's.
	void imbe_decode_model(int16_t *frame_vector);
	// mbevocoder: the encoder's input gain changed by 2^shift (encoder.cpp, "Input gain"): scale the
	// stored input history and the level-dependent analysis state to match, so the analysis
	// window holds one gain throughout. The quantiser's state is on the compensated scale and
	// is not touched.
	void rescale_input(int shift);
	// Set ref_pitch (Q8.8 samples) and the fund_freq that goes with it, as pitch refinement would.
	static void imbe_set_pitch(IMBE_PARAM *imbe_param, int16_t ref_pitch);
private:
	int16_t forced_ref_pitch = 0;
	int16_t voicing_scale_pct = 100;
	bool wide_energy = false;
	double th_max_wide = 0.0, gate_max = 0.0;
	double level_gate_r0 = 0.0, level_gate_w = 0.0;
	bool noise_gate = false;
	double ng_noise[128] = {0.0}, ng_smooth[128] = {0.0}, ng_loud = 0.0;
	int ng_age = -1;   // frames since the last noise-like frame; -1: no estimate

	void noise_gate_bands(IMBE_PARAM *imbe_param, const Cmplx16 *fft_buf, const Word32 *M_num,
	                      const Word16 *harm_bins, Word16 *b1_vec, Word16 *uv_harms_cnt);
	IMBE_PARAM my_imbe_param;

	/* data items originally static (moved from individual c++ sources) */
	Word16 prev_pitch, prev_prev_pitch, prev_e_p, prev_prev_e_p;
	UWord32 seed ;
	Word16 num_harms_prev1;
	Word32 sa_prev1[NUM_HARMS_MAX + 2];
	Word16 num_harms_prev2;
	Word32 sa_prev2[NUM_HARMS_MAX + 2];
	Word16 uv_mem[105];
	UWord32 ph_mem[NUM_HARMS_MAX];
	Word16 num_harms_prev3;
	Word32 fund_freq_prev;
	Word16 vu_dsn_prev[NUM_HARMS_MAX];
	Word16 sa_prev3[NUM_HARMS_MAX];
	Word32 th_max;
	Word16 v_uv_dsn[NUM_BANDS_MAX];
	Word16 wr_array[FFTLENGTH / 2 + 1];
	Word16 wi_array[FFTLENGTH / 2 + 1];
	Word16 pitch_est_buf[PITCH_EST_BUF_SIZE];
	Word16 pitch_ref_buf[PITCH_EST_BUF_SIZE];
	Word32 dc_rmv_mem;
	Cmplx16 fft_buf[FFTLENGTH];
	Word16 pe_lpf_mem[PE_LPF_ORD];
	float d_gain_adjust;

	/* member functions */
	void idct(Word16 *in, Word16 m_lim, Word16 i_lim, Word16 *out);
	void dct(Word16 *in, Word16 m_lim, Word16 i_lim, Word16 *out);
	void fft_init(void);
	void fft(Word16 *datam1, Word16 nn, Word16 isign);
	void encode(IMBE_PARAM *imbe_param, Word16 *frame_vector, Word16 *snd);
	void analyse_pitch(IMBE_PARAM *imbe_param, Word16 *snd);
	void parse(int argc, char **argv);
	void pitch_est_init(void);
	Word32 autocorr(Word16 *sigin, Word16 shift, Word16 scale_shift);
	void e_p(Word16 *sigin, Word16 *res_buf);
	void pitch_est(IMBE_PARAM *imbe_param, Word16 *frames_buf);
	void sa_decode_init(void);
	void sa_decode(IMBE_PARAM *imbe_param);
	void sa_encode_init(void);
	void sa_encode(IMBE_PARAM *imbe_param);
	void uv_synt_init(void);
	void uv_synt(IMBE_PARAM *imbe_param, Word16 *snd);
	void v_synt_init(void);
	void v_synt(IMBE_PARAM *imbe_param, Word16 *snd);
	void pitch_ref_init(void);
	Word16 voiced_sa_calc(Word32 num, Word16 den);
	Word16 unvoiced_sa_calc(Word32 num, Word16 den);
	void v_uv_det(IMBE_PARAM *imbe_param, Cmplx16 *fft_buf);
	void decode_init(IMBE_PARAM *imbe_param);
	void decode(IMBE_PARAM *imbe_param, Word16 *frame_vector, Word16 *snd);
	void encode_init(void);
};

#endif /* INCLUDED_IMBE_VOCODER_H */
