#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What does the AMBE-3000R's tone detector measure? A 1000 Hz -20 dBFS tone under maskers.

  python tools/quality/probe_tone_masking.py [--port COM3]

White, low-pass (500 Hz), high-pass (2 kHz), band-pass (800-1200 Hz) and pink noise at 20 to
0 dB tone-to-noise ratio, then speech at +10 to -12 dB tone-to-speech ratio; the AMBE-3000R's
tone frames per 0.6 s section (30 frames) against our detector's. The capture is reused when
build/probe/tone_mask.dvsi exists. Findings in docs/frontend.md.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, lfilter

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402
from probe_tone_detect import tone  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--port", default="COM3")
args = ap.parse_args()
os.chdir(ROOT)
T = str(find_tool()); C = str(find_ambe3000())
FS=8000; rng=np.random.default_rng(3)
def noise(kind, level_db, n):
    w=rng.normal(0,1,n)
    if kind=='white': y=w
    elif kind=='low500': y=lfilter(*butter(4,500/(FS/2)),w)
    elif kind=='high2k': y=lfilter(*butter(4,2000/(FS/2),'high'),w)
    elif kind=='band800_1200': y=lfilter(*butter(4,[800/(FS/2),1200/(FS/2)],'band'),w)
    elif kind=='pink':
        Y=np.fft.rfft(w); f=np.fft.rfftfreq(n,1/FS); Y[1:]/=np.sqrt(f[1:]); Y[0]=0; y=np.fft.irfft(Y,n)
    y=y/np.sqrt(np.mean(y**2))*10**(level_db/20)*32767
    return y
sections=[]
t=tone(1000,0,0.6,-20)
for kind in ('white','low500','high2k','band800_1200','pink'):
    for snr in (20,15,10,5,0):
        sections.append((f'{kind:13s} snr {snr:2d}', t+noise(kind,-20-snr,len(t))))
sp=wavfile.read('testdata/corpus/OSR_uk_000_0020.wav')[1].astype(float)[8000:8000+int(0.6*FS)]
sp=sp/np.sqrt(np.mean(sp**2))*10**(-20/20)*32767   # speech at -20 dBFS rms
for tsr in (10,6,0,-6,-12):
    sections.append((f'speech tone-to-speech {tsr:+3d}', t*10**((tsr-0)/20)*0+tone(1000,0,0.6,-20+0)+sp*10**(-tsr/20)))
sections.append(('speech only -20', sp))
gap=np.zeros(int(0.3*FS))
x=np.concatenate([np.concatenate([s[1],gap]) for s in sections])
work=Path('build/probe'); wav=work/'tone_mask.wav'; dvsi=work/'tone_mask.dvsi'
wavfile.write(wav,FS,np.clip(x,-32768,32767).astype(np.int16))
if not dvsi.exists():
    r=subprocess.run([C,'--port',args.port,'--rate','ambe2450','--ecmode','0x1000','encode',str(wav),str(dvsi)],capture_output=True,text=True)
    if r.returncode: sys.exit(r.stderr)
subprocess.run([T,'parms','--rate','ambe2450',str(dvsi),str(dvsi)+'.parms'],check=True,capture_output=True)
subprocess.run([T,'encode','--rate','ambe2450','--tone-detect',str(wav),str(work/'tone_mask.ours.dvsi')],check=True,capture_output=True)
subprocess.run([T,'parms','--rate','ambe2450',str(work/'tone_mask.ours.dvsi'),str(work/'tone_mask.ours.parms')],check=True,capture_output=True)
def tones(p): return [int(l.split()[-2]) for l in Path(p).read_text().splitlines() if l.strip()]
ch=tones(str(dvsi)+'.parms'); ou=tones(work/'tone_mask.ours.parms')
pos=0; delay=2
for label,s in sections:
    f0=pos//160+delay; f1=(pos+len(s))//160+delay
    c=[v for v in ch[f0:f1] if v>=0]; o=[v for v in ou[f0:f1] if v>=0]
    print(f'{label:32s} chip {len(c):2d}/{f1-f0} tone frames (ids {sorted(set(c))[:3]})   ours {len(o):2d}')
    pos+=len(s)+len(gap)
