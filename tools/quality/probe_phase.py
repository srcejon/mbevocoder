#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Harmonic phases of the AMBE-3000R's voiced synthesis on held frames, against ours.

  python tools/quality/probe_phase.py [--port COM3]

Held all-voiced frames (four shapes at four fundamentals), decoded on the AMBE-3000R and by
us; the steady part of each hold is fitted with L sinusoids at the exact harmonics and the
phase of each harmonic relative to the fundamental's (phi_l - l phi_1) printed for both.
Findings in docs/decoder-quality.md ("Regenerated phases"). The capture is reused when
build/probe/phase/hw.wav exists.
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
HOLD=20; b0s=[60,75,90,105]; shapes=["308 78 8 14 13 1","150 40 20 10 5 3","200 60 12 3 9 2","80 110 25 8 2 5"]
work=Path('build/probe/phase'); work.mkdir(parents=True,exist_ok=True)
lines=[]
for b0 in b0s:
    for sh in shapes: lines += [f"{b0} 0 20 {sh}"]*HOLD
spec=work/'spec.txt'; spec.write_text('\n'.join(lines)+'\n'); dvsi=work/'spec.dvsi'; hw=work/'hw.wav'; ours=work/'ours.wav'
subprocess.run([T,'frames','--rate','ambe2450',str(spec),str(dvsi)],check=True,capture_output=True)
subprocess.run([T,'parms','--rate','ambe2450',str(dvsi),str(dvsi)+'.parms'],check=True,capture_output=True)
if not hw.exists(): subprocess.run([C,'--port',args.port,'--rate','ambe2450','decode',str(dvsi),str(hw)],check=True,capture_output=True)
subprocess.run([T,'decode','--rate','ambe2450',str(dvsi),str(ours)],check=True,capture_output=True)
p=parse_parms(Path(str(dvsi)+'.parms')); x=read_wav(hw); y=read_wav(ours)
def harmonic_fit(seg, f0, L):
    n=np.arange(len(seg)); cols=[]
    for l in range(1,L+1):
        w=2*np.pi*l*f0; cols += [np.cos(w*n), np.sin(w*n)]
    A=np.stack(cols,1); c,*_=np.linalg.lstsq(A,seg,rcond=None)
    H=c[0::2]-1j*c[1::2]   # a cos + b sin = Re(H e^{j w n}) with H = a - jb
    resid=seg-A@c
    return H, 10*np.log10(np.sum(resid**2)/np.sum(seg**2))
def rel_phase(H):
    ph=np.angle(H); l=np.arange(1,len(H)+1)
    r=ph - l*ph[0]; return (r+np.pi)%(2*np.pi)-np.pi
np.set_printoptions(precision=2, suppress=True, linewidth=200)
idx=0
for b0 in b0s:
    for sh in shapes:
        base=idx*HOLD; w0=p[base][1]; L=p[base][2]; f0=w0/(2*np.pi)
        k0=(base+8)*160; k1=(base+18)*160
        Hc,rc=harmonic_fit(x[k0:k1],f0,L); Ho,ro=harmonic_fit(y[k0:k1],f0,L)
        pc=rel_phase(Hc); po=rel_phase(Ho)
        mag=20*np.log10(np.abs(Hc)+1e-9); keep=mag>mag.max()-40
        # circular correlation of the two phase patterns and a linear fit of chip phase on ours (mod 2pi)
        d=(pc-po+np.pi)%(2*np.pi)-np.pi
        print(f'b0 {b0} L {L} [{sh}] fit resid chip {rc:.1f} dB ours {ro:.1f} dB; |chip-ours| phase mean {np.mean(np.abs(d[keep])):.2f} rad on {keep.sum()} harmonics; chip rel phase[1..12]: {pc[:12]}; ours: {po[:12]}')
        idx+=1
