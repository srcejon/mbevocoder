#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Voicing transitions: how does the AMBE-3000R move between a voiced and an unvoiced frame?

  python tools/quality/probe_vuv_transition.py [--port COM3]

Held frames switching between all voiced (row 0), all unvoiced (16) and half voiced (8) every
ten frames at one fundamental, decoded on the AMBE-3000R and by our decoder, aligned on level
markers; the periodicity (normalised autocorrelation at the pitch period over two periods)
and level are printed every 2.5 ms across each transition. The capture is reused when
build/probe/vuvtrans/hw2.wav exists. Findings in docs/decoder-quality.md.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, read_wav  # noqa: E402
from encoder_compare import parse_parms  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--port", default="COM3")
args = ap.parse_args()
os.chdir(ROOT)
T = str(find_tool()); C = str(find_ambe3000())
HOLD=10; b0=60; shape="308 78 8 14 13 1"; gain=20
work=Path('build/probe/vuvtrans'); work.mkdir(parents=True,exist_ok=True)
lines=[f"{b0} 0 {gain+10} {shape}"]*10 + [f"{b0} 16 {gain-10} {shape}"]*10   # level markers for alignment
for rep in range(6):
    lines += [f"{b0} 0 {gain} {shape}"]*HOLD + [f"{b0} 16 {gain} {shape}"]*HOLD
# also a partial switch: row 0 <-> row 8 (bands 0-3 voiced, 4-7 unvoiced)
for rep in range(4):
    lines += [f"{b0} 0 {gain} {shape}"]*HOLD + [f"{b0} 8 {gain} {shape}"]*HOLD
lines += [f"{b0} 16 {gain-10} {shape}"]*10 + [f"{b0} 0 {gain+10} {shape}"]*10
spec=work/'spec.txt'; spec.write_text('\n'.join(lines)+'\n')
dvsi=work/'spec2.dvsi'; hw=work/'hw2.wav'; ours=work/'ours2.wav'
subprocess.run([T,'frames','--rate','ambe2450',str(spec),str(dvsi)],check=True,capture_output=True)
subprocess.run([T,'parms','--rate','ambe2450',str(dvsi),str(dvsi)+'.parms'],check=True,capture_output=True)
if not hw.exists():
    subprocess.run([C,'--port',args.port,'--rate','ambe2450','decode',str(dvsi),str(hw)],check=True,capture_output=True)
subprocess.run([T,'decode','--rate','ambe2450',str(dvsi),str(ours)],check=True,capture_output=True)
p=parse_parms(Path(str(dvsi)+'.parms')); period=2*np.pi/p[0][1]
x=read_wav(hw); y=read_wav(ours)
# align on the level markers (first 20 frames and last 20): envelope cross-correlation there
def env(sig): return np.sqrt(np.convolve(sig**2, np.ones(16)/16, 'same'))
ex=env(x[:20*160]); ey=env(y[:20*160]); ex-=ex.mean(); ey-=ey.mean()
c=np.correlate(ex,ey,'full'); lag=np.argmax(c)-(len(ey)-1)
ex=env(x[-20*160:]); ey=env(y[-20*160:]); ex-=ex.mean(); ey-=ey.mean()
c2=np.correlate(ex,ey,'full'); lag2=np.argmax(c2)-(len(ey)-1)
print('lag chip vs ours from the start marker', lag, 'from the end marker', lag2)
xa=np.roll(x,-lag)
print(f'pitch period {period:.1f} samples')
P=int(round(period)); W=2*P
def periodicity(sig, c):
    seg=sig[c-W//2:c+W//2]
    a=seg[:-P]; b=seg[P:]
    return np.dot(a,b)/np.sqrt(np.dot(a,a)*np.dot(b,b)+1e-12)
def energy(sig,c):
    seg=sig[c-40:c+40]; return 10*np.log10(np.mean(seg**2)+1e-12)
M=20*160
for label,start in (('row 0 -> 16 (all voiced -> all unvoiced)', M+2*2*HOLD*160 + HOLD*160), ('row 16 -> 0', M+3*2*HOLD*160), ('row 0 -> 8 (upper bands off)', M+6*2*HOLD*160+HOLD*160)):
    # transition at frame boundary 'start' (samples); profile from -2 frames to +2 frames in 20-sample steps
    print(label)
    print('   t(ms)   chip periodicity / level dB   |   ours')
    for off in range(-160, 241, 20):
        c=start+off
        print(f'  {off/8:6.0f}   {periodicity(xa,c):+5.2f} {energy(xa,c):6.1f}          |  {periodicity(y,c):+5.2f} {energy(y,c):6.1f}')
