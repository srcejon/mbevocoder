// SPDX-License-Identifier: GPL-3.0-or-later
// D-STAR floating-point analysis: see dstar_analysis.hpp. The constants follow the offline
// reference it was ported from and checked against (tools/quality/dstar_pitch.py and the
// candidate recorded in docs/encoder-quality.md), frozen on 2026-09-23.
#include "dstar_analysis.hpp"

#include "analysis/imbe_vocoder/typedef.h"

#include <cmath>
#include <cstring>
#include <utility>

extern const Word16 wr[];   // the IMBE analysis window's first half (src/analysis/imbe_vocoder/tbls.cc)

namespace mbe {

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr int kFir = 129;                  // 1200 Hz low-pass, Hamming window (scipy firwin(129, 1200, fs=8000))
constexpr int kFirHalf = 64;
constexpr int kCorrHalf = 160;             // correlation over 321 Hann-weighted sample pairs
constexpr int kLowHalf = 256;              // low-passed window around the analysis centre
constexpr int kWin = 221;                  // band-energy window (the IMBE analysis window)
constexpr int kFft = 2048;
// Frame positions relative to the input samples received so far (N = one past the newest) and
// the encoder's input alignment A: the analysis centre is N - 471 - A and the band-energy window
// starts at N - 581 - A (D-STAR, A = 98: N - 569 and N - 679, the reference's (i + 1) 160 - 519
// and - 629 on the 48-sample framing it was written for, plus the D-STAR framing's 50).
constexpr int64_t kCentreBack = 471;
constexpr int64_t kWindowBack = 581;
// Selection: octave-distance penalties from the quantiser's pitch and from the previous frame's
// choice (capped at an octave), and the gates for a change.
constexpr double kPenaltyOriginal = 0.02;
constexpr double kPenaltyContinuity = 0.03;
constexpr double kMinCorrelation = 0.7;
constexpr double kMinGain = 0.035;

struct Tables {
    double fir[kFir];
    double hann[2 * kCorrHalf + 1];
    double window[kWin];
    double windowEnergy;
    double cosTab[kFft], sinTab[kFft];
    Tables()
    {
        double sum = 0.0;
        for (int n = 0; n < kFir; ++n) {
            const double m = n - kFirHalf, c = 0.3;   // cutoff 1200 / 4000
            const double x = c * m;
            const double sinc = m == 0 ? 1.0 : std::sin(3.141592653589793 * x) / (3.141592653589793 * x);
            fir[n] = c * sinc * (0.54 - 0.46 * std::cos(kTwoPi * n / (kFir - 1)));
            sum += fir[n];
        }
        for (double& v : fir) v /= sum;
        for (int n = 0; n <= 2 * kCorrHalf; ++n) hann[n] = 0.5 - 0.5 * std::cos(kTwoPi * n / (2 * kCorrHalf));
        for (int n = 0; n < 110; ++n) {
            window[n] = static_cast<double>(wr[n]) / 32768.0;
            window[kWin - 1 - n] = window[n];
        }
        window[110] = 1.0;
        windowEnergy = 0.0;
        for (double w : window) windowEnergy += w * w;
        for (int k = 0; k < kFft; ++k) { cosTab[k] = std::cos(kTwoPi * k / kFft); sinTab[k] = std::sin(kTwoPi * k / kFft); }
    }
};

const Tables& tables()
{
    static const Tables t;
    return t;
}

} // namespace

DstarAnalysis::DstarAnalysis() { tables(); reset(); }

void DstarAnalysis::reset()
{
    std::memset(m_signal, 0, sizeof m_signal);
    std::memset(m_low, 0, sizeof m_low);
    m_pushed = 0;
    m_lowDone = 0;
    m_dcX = m_dcY = 0.0;
    m_last = -1;
}

void DstarAnalysis::advanceSampleCountForTest(int64_t samples)
{
    if (samples % kHistory != 0) return;
    m_pushed += samples;
    m_lowDone += samples;
}

double DstarAnalysis::signal(int64_t n) const
{
    if (n < 0 || n >= m_pushed || n < m_pushed - kHistory) return 0.0;
    return m_signal[n % kHistory];
}

double DstarAnalysis::low(int64_t n) const
{
    if (n < 0 || n >= m_lowDone || n < m_lowDone - kHistory) return 0.0;
    return m_low[n % kHistory];
}

void DstarAnalysis::push(const short pcm[160])
{
    const Tables& t = tables();
    for (int i = 0; i < 160; ++i) {
        // DC blocker y[n] = x[n] - x[n-1] + 0.99 y[n-1] (scipy lfilter([1, -1], [1, -0.99]))
        const double x = static_cast<double>(pcm[i]);
        const double y = x - m_dcX + 0.99 * m_dcY;
        m_dcX = x;
        m_dcY = y;
        m_signal[m_pushed % kHistory] = y;
        ++m_pushed;
    }
    // the centred low-pass is known up to 64 samples before the newest input
    while (m_lowDone + kFirHalf < m_pushed) {
        double acc = 0.0;
        for (int j = 0; j < kFir; ++j) acc += t.fir[j] * signal(m_lowDone + kFirHalf - j);
        m_low[m_lowDone % kHistory] = acc;
        ++m_lowDone;
    }
}

void DstarAnalysis::energyAmplitudes(int64_t start, double w0, int L, double amp[56]) const
{
    const Tables& t = tables();
    double frame[kWin];
    for (int n = 0; n < kWin; ++n) frame[n] = signal(start + n) * t.window[n];
    // |DFT|^2 at bins 0..1024 of the zero-padded 2048-point transform, and its running sum. A
    // radix-2 FFT in double precision with the same twiddle tables (the direct sum it replaced
    // took about 226,000 multiply-adds a call, most of the floating-point analysis' cost on every
    // rate; the two agree to about 1e-13).
    static thread_local double re[kFft], im[kFft];
    for (int n = 0; n < kFft; ++n) { re[n] = n < kWin ? frame[n] : 0.0; im[n] = 0.0; }
    for (int i = 1, j = 0; i < kFft; ++i) {   // bit reversal
        int bit = kFft >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= kFft; len <<= 1) {
        const int half = len / 2, stride = kFft / len;
        for (int i = 0; i < kFft; i += len) {
            for (int k = 0; k < half; ++k) {
                const double cr = t.cosTab[k * stride], ci = -t.sinTab[k * stride];   // e^{-2 pi i k / len}
                const int u = i + k, v = u + half;
                const double tr = re[v] * cr - im[v] * ci, ti = re[v] * ci + im[v] * cr;
                re[v] = re[u] - tr; im[v] = im[u] - ti;
                re[u] += tr; im[u] += ti;
            }
        }
    }
    double power[kFft / 2 + 1];
    double integral[kFft / 2 + 2];
    integral[0] = 0.0;
    for (int k = 0; k <= kFft / 2; ++k) {
        power[k] = re[k] * re[k] + im[k] * im[k];
        integral[k + 1] = integral[k] + power[k];
    }
    // bin-centred power integrated between the harmonic's band edges, fractional at the edges
    // (numpy.interp over edges -0.5 .. 1024.5, clamped outside)
    auto at = [&](double x) {
        if (x <= -0.5) return integral[0];
        if (x >= kFft / 2 + 0.5) return integral[kFft / 2 + 1];
        const int k = static_cast<int>(std::floor(x + 0.5));
        return integral[k] + (x - (k - 0.5)) * (integral[k + 1] - integral[k]);
    };
    const double scale = static_cast<double>(kFft) / kTwoPi * w0;
    for (int l = 1; l <= L; ++l) {
        const double e = at((l + 0.5) * scale) - at((l - 0.5) * scale);
        amp[l - 1] = 2.0 * std::sqrt((e > 0.0 ? e : 0.0) / (kFft * t.windowEnergy));
    }
}

int DstarAnalysis::frame(const AmbeRateTables& tab, int b0, int b1, int L, const float* lsa, float target[56])
{
    double w0s[kMaxCandidates];
    int Ls[kMaxCandidates];
    const int count = tab.b0Count;
    for (int i = 0; i < count; ++i) {
        w0s[i] = static_cast<double>(tab.f0(i) * 6.28318530717959f);
        Ls[i] = static_cast<int>(tab.Ltable[i]);
    }
    bool voiced = false;
    for (int k = 0; k < 8; ++k) voiced = voiced || tab.vuv[b1][k] != 0;
    return select(w0s, Ls, count, b0, voiced, L, lsa, target);
}

void DstarAnalysis::lowWindow(double* win) const
{
    const int64_t centre = m_pushed - kCentreBack - m_align;
    for (int n = 0; n <= 2 * kLowHalf; ++n) win[n] = low(centre - kLowHalf + n);
}

double DstarAnalysis::correlationIn(const double* lowWin, double w0)
{
    const Tables& t = tables();
    const double tau = kTwoPi / w0;
    double ab = 0.0, e = 0.0;
    // k is an integer, so the two read positions k + kLowHalf -+ tau / 2 have the same
    // fractional part all along the window: interpolate with constant weights
    const double ca = kLowHalf - tau / 2.0, cb = kLowHalf + tau / 2.0;
    const int fa = static_cast<int>(std::floor(ca)), fb = static_cast<int>(std::floor(cb));
    const double af = ca - fa, bf = cb - fb;
    for (int k = -kCorrHalf; k <= kCorrHalf; ++k) {
        const int ai = k + fa, bi = k + fb;
        const double a = lowWin[ai] * (1.0 - af) + lowWin[ai + 1] * af;
        const double b = lowWin[bi] * (1.0 - bf) + lowWin[bi + 1] * bf;
        const double w = t.hann[k + kCorrHalf];
        ab += w * a * b;
        e += w * (a * a + b * b);
    }
    return 2.0 * ab / (e > 1e-9 ? e : 1e-9);
}

int DstarAnalysis::select(const double* w0s, const int* Ls, int count, int b0, bool voiced, int L, const float* lsa, float* target)
{
    const int64_t N = m_pushed;
    if (m_last < 0 || m_last >= count) m_last = b0;

    // --- pitch: normalised correlation of the low-passed input at every candidate period
    double lowWin[2 * kLowHalf + 1];
    lowWindow(lowWin);
    double corr[kMaxCandidates];
    for (int i = 0; i < count; ++i) corr[i] = correlationIn(lowWin, w0s[i]);
    const double w0Old = w0s[b0], w0Last = w0s[m_last];
    int tracked = 0;
    double best = 0.0;
    for (int i = 0; i < count; ++i) {
        const double distance = std::fabs(std::log2(w0s[i] / w0Old));
        double continuity = std::fabs(std::log2(w0s[i] / w0Last));
        if (continuity > 1.0) continuity = 1.0;
        const double s = corr[i] - kPenaltyOriginal * distance - kPenaltyContinuity * continuity;
        if (i == 0 || s > best) { best = s; tracked = i; }
    }
    if (!(voiced && corr[tracked] > kMinCorrelation && corr[tracked] - corr[b0] > kMinGain)) tracked = b0;
    m_last = tracked;
    if (!target) return tracked;   // pitch only (IMBE keeps the vocoder's own amplitudes)

    // --- amplitudes: band energies on the coded grid, mean kept at the caller's target mean
    const int64_t start = N - kWindowBack - m_align;
    double amp[56], shape[56];
    double lsaMean = 0.0;
    for (int l = 0; l < L; ++l) lsaMean += static_cast<double>(lsa[l]);
    lsaMean /= L;
    energyAmplitudes(start, w0Old, L, amp);
    double mean = 0.0;
    for (int l = 0; l < L; ++l) { shape[l] = std::log2(amp[l] > 1e-6 ? amp[l] : 1e-6); mean += shape[l]; }
    mean /= L;
    for (int l = 0; l < L; ++l) shape[l] = shape[l] - mean + lsaMean;
    if (tracked == b0) {
        for (int l = 0; l < L; ++l) target[l] = static_cast<float>(shape[l]);
        return b0;
    }
    double oldMean = 0.0;
    for (int l = 0; l < L; ++l) oldMean += shape[l];
    oldMean /= L;
    const int Ln = Ls[tracked];
    energyAmplitudes(start, w0s[tracked], Ln, amp);
    mean = 0.0;
    for (int l = 0; l < Ln; ++l) { shape[l] = std::log2(amp[l] > 1e-6 ? amp[l] : 1e-6); mean += shape[l]; }
    mean /= Ln;
    for (int l = 0; l < Ln; ++l) target[l] = static_cast<float>(shape[l] + (oldMean - mean));
    return tracked;
}

} // namespace mbe
