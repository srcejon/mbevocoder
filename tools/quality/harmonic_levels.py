#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-harmonic level of our decoder against the AMBE-3000R's on its own AMBE+2 frames.

  python tools/quality/harmonic_levels.py [NAME ...]

Steady frames (same voicing index as both neighbours, harmonic count within 2) are analysed
at their centre with a 512-point window on both decoders' output; per frame class (voiced,
unvoiced, mixed) the mean-removed per-harmonic levels are compared (slope, correlation, rms),
and for harmonics at a given depth below the frame's maximum in our decode, the depth the
AMBE-3000R renders. Needs corpus/golden/ambe2450/NAME.dvsi and testdata/golden/ambe2450/NAME.hw.wav. Results in docs/decoder-quality.md.
"""
import os
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, read_wav, align  # noqa: E402
from encoder_compare import parse_parms  # noqa: E402

os.chdir(ROOT)
T = str(find_tool())
files=sys.argv[1:] or ['OSR_uk_000_0020','OSR_us_000_0010','arctic_slt_arctic_a0009','arctic_bdl_arctic_a0003','libri_1089-134686-0001','libri_672-122797-0001']
N=512; fr=np.fft.rfftfreq(N,1/8000); win=np.hanning(N)
pts={'voiced':[], 'unvoiced':[], 'mixed':[]}
Path('build/probe/harm').mkdir(parents=True,exist_ok=True)
for name in files:
    g=f'corpus/golden/ambe2450/{name}.dvsi'
    o=f'build/probe/harm/{name}.ours.wav'
    subprocess.run([T,'decode','--rate','ambe2450',g,o],check=True,capture_output=True)
    pp=f'build/probe/harm/{name}.parms'
    subprocess.run([T,'parms','--rate','ambe2450',g,pp],check=True,capture_output=True)
    a=parse_parms(Path(pp))
    hw=read_wav(f'testdata/golden/ambe2450/{name}.hw.wav'); ours=read_wav(o)
    xa,lag=align(hw,ours)[:2]
    for k in range(2,min(len(a),len(hw)//160)-2):
        b,w0,L,gamma,tone,lm,v=a[k]
        if tone or a[k-1][4] or b[1] in (20,24,25,28): continue
        # steady frame: same L as neighbours, same voicing
        if a[k-1][0][1]!=b[1] or a[k+1][0][1]!=b[1] or abs(a[k-1][2]-L)>2: continue
        c=160*k+80; s0=c-N//2
        if s0<0 or s0+N>len(xa) or s0+N>len(ours): continue
        Sx=np.abs(np.fft.rfft(xa[s0:s0+N]*win))**2; So=np.abs(np.fft.rfft(ours[s0:s0+N]*win))**2
        f0=w0/(2*np.pi)
        hx=[];ho=[]
        for l in range(1,L+1):
            m=(fr>=(l*f0*8000-f0*2000))&(fr<(l*f0*8000+f0*2000))
            if not m.any(): continue
            hx.append(Sx[m].sum()); ho.append(So[m].sum())
        hx=10*np.log10(np.array(hx)+1e-12); ho=10*np.log10(np.array(ho)+1e-12)
        if hx.max()<-50: continue
        cls='voiced' if v.all() else ('unvoiced' if not v.any() else 'mixed')
        pts[cls].append(np.stack([hx-hx.mean(), ho-ho.mean(), hx-hx.max(), ho-ho.max()],axis=1))
for cls in pts:
    P=np.concatenate(pts[cls]); 
    sl=np.polyfit(P[:,0],P[:,1],1)[0]; corr=np.corrcoef(P[:,0],P[:,1])[0,1]
    print(f'{cls:9s} {len(pts[cls])} frames, {len(P)} harmonics: ours vs chip mean-removed dB: slope {sl:.2f} corr {corr:.2f}, rms diff {np.sqrt(np.mean((P[:,1]-P[:,0])**2)):.1f} dB')
    # binned: for harmonics at a given depth below the frame max in OUR decode, what depth does the chip render?
    for lo,hi in ((0,6),(6,12),(12,18),(18,24),(24,30),(30,40),(40,60)):
        m=(P[:,3]<=-lo)&(P[:,3]>-hi)
        if m.sum()>50: print(f'    ours {lo:2d}-{hi:2d} dB below frame max: chip {-P[m,2].mean():5.1f} dB below its max (n={m.sum()})')
