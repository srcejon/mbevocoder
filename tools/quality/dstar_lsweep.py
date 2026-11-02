#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Count the AMBE-3000R's harmonics per fundamental index (steady all-voiced reference frames).

  python tools/quality/dstar_lsweep.py dstar|ambe2450 [analyse-only]

Writes build/probe/lsweep_<rate>_chipL.npy and prints the AMBE-3000R's count against mbelib's table
and the floor(0.9254 pi / w0) rule."""
import sys, subprocess
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
ROOT = Path(__file__).resolve().parents[2]
import numpy as np, dstar_codebook as d
from scipy.io import wavfile
from compare import find_tool
from hil_encoder import find_ambe3000
rate=sys.argv[1]; d.set_rate(rate); RUN=24; ref=d.R.REF; nb0=120
tag=f'lsweep_{rate}'
if len(sys.argv)<3:
    lines=[]
    for b0 in range(nb0):
        for _ in range(RUN): lines.append(f"{b0} {d.R.voicedRow} {50 if rate == 'dstar' else 26} {ref[0]} {ref[1]} {ref[2]} {ref[3]} {ref[4]} {ref[5]}")
    open(str(ROOT / f'build/probe/{tag}.txt'),'w').write("\n".join(lines)+"\n")
    tool=find_tool(); chip=find_ambe3000()
    subprocess.run([str(tool),"frames","--rate",d.R.cliRate,str(ROOT / f'build/probe/{tag}.txt'),str(ROOT / f'build/probe/{tag}.dvsi')],check=True,capture_output=True)
    subprocess.run([str(tool),"decode","--rate",d.R.cliRate,"--gain","0.65",str(ROOT / f'build/probe/{tag}.dvsi'),str(ROOT / f'build/probe/{tag}.ours.wav')],check=True,capture_output=True)
    r_=subprocess.run([str(chip),"--port","COM3","--rate",d.R.cliRate,"decode",str(ROOT / f'build/probe/{tag}.dvsi'),str(ROOT / f'build/probe/{tag}.hw.wav')],capture_output=True,text=True); print(r_.stderr.strip().splitlines()[-1])
def count(path):
    fs,x=wavfile.read(path); x=x.astype(float); out=[]
    for b0 in range(nb0):
        seg=x[(b0+1)*RUN*160-8*160:(b0+1)*RUN*160]; f0=d.R.f0_cps(b0); Lmax=min(60,int(0.5/f0))
        amp=d.harmonic_lin(seg-seg.mean(),f0,Lmax); lm=20*np.log10(amp/amp.max())
        present=np.where(lm>-45)[0]; out.append(int(present.max())+1 if len(present) else 0)
    return np.array(out)
chip=count(str(ROOT / f'build/probe/{tag}.hw.wav')); ours=count(str(ROOT / f'build/probe/{tag}.ours.wav')); tab=d.R.LTABLE[:nb0]
formula=np.array([int(0.9254*int(np.pi/(2*np.pi*d.R.f0_cps(b0))+0.25)) for b0 in range(nb0)])
print("ours measured == table:", np.sum(ours==tab), "/120 (method check)")
print("chip == table:", np.sum(chip==tab), " chip == formula:", np.sum(chip==formula), " chip-table hist:", dict(zip(*np.unique(chip-tab,return_counts=True))), " chip-formula hist:", dict(zip(*np.unique(chip-formula,return_counts=True))))
print("b0 chip table formula:"); print([(b0,int(chip[b0]),int(tab[b0]),int(formula[b0])) for b0 in range(0,120,4)])
np.save(str(ROOT / f'build/probe/{tag}_chipL.npy'), chip)
