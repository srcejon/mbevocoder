#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Steady-state sweep on the AMBE-3000R to find the flattest index per family (others at a reference)."""
"""Rank every index of every family by the flatness of the AMBE-3000R's steady-state shape, the
others held at a reference (to choose the probe reference for dstar_codebook.py).

  python tools/quality/dstar_flatsweep.py dstar|ambe2450 b3,b4,b5,b6,b7,b8 <tag> [analyse-only]"""
import sys, subprocess, os, json
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
ROOT = Path(__file__).resolve().parents[2]
import numpy as np, dstar_codebook as d
from scipy.io import wavfile
from compare import find_tool
from hil_encoder import find_ambe3000
d.set_rate(sys.argv[1]); ref=tuple(int(v) for v in sys.argv[2].split(',')); tag=sys.argv[3]; b0=73; RUN=24; MEAS=8
L=int(d.R.LTABLE[b0]); f0=d.R.f0_cps(b0)
cs=[]
r=ref
for b3 in range(512): cs.append(("b3",b3,(b3,r[1],r[2],r[3],r[4],r[5])))
for b4 in range(128): cs.append(("b4",b4,(r[0],b4,r[2],r[3],r[4],r[5])))
for k,fam in enumerate(("b5","b6","b7","b8")):
    for i in d.R.hocIndices[k]:
        idx=list(r); idx[2+k]=i; cs.append((fam,i,tuple(idx)))
cs.append(("ref",0,ref))
if len(sys.argv)<5:
    lines=[]
    for _,_,idx in cs:
        for _ in range(RUN): lines.append(f"{b0} {d.R.voicedRow} 50 {idx[0]} {idx[1]} {idx[2]} {idx[3]} {idx[4]} {idx[5]}")
    open(str(ROOT / f'build/probe/flat_{tag}.txt'),'w').write("\n".join(lines)+"\n")
    tool=find_tool(); chip=find_ambe3000()
    subprocess.run([str(tool),"frames","--rate",d.R.cliRate,str(ROOT / f'build/probe/flat_{tag}.txt'),str(ROOT / f'build/probe/flat_{tag}.dvsi')],check=True,capture_output=True)
    print(len(lines),"frames; chip decoding...")
    r_=subprocess.run([str(chip),"--port","COM3","--rate",d.R.cliRate,"decode",str(ROOT / f'build/probe/flat_{tag}.dvsi'),str(ROOT / f'build/probe/flat_{tag}.hw.wav')],capture_output=True,text=True); print(r_.stderr.strip().splitlines()[-1])
fs,x=wavfile.read(str(ROOT / f'build/probe/flat_{tag}.hw.wav')); x=x.astype(float)
res={}
for n,(fam,i,idx) in enumerate(cs):
    seg=x[(n+1)*RUN*160-MEAS*160:(n+1)*RUN*160]
    amp=d.harmonic_lin(seg-seg.mean(),f0,L); lm=np.log2(np.maximum(amp,1e-9))
    top=np.sort(lm)[::-1]
    # range over the harmonics that are above the floor; a flat shape has small spread
    spread=float(np.std(lm[lm>lm.max()-60/6.02]))*6.02
    res[(fam,i)]=(spread, float((lm.max()-lm.min())*6.02), float(np.abs(seg).max()))
out={}
for fam in ("b3","b4","b5","b6","b7","b8"):
    rows=sorted([(i,v) for (f,i),v in res.items() if f==fam], key=lambda t:t[1][0])
    out[fam]=[i for i,_ in rows[:6]]
    print(fam, "flattest (index, spread dB, range dB, peak):", [(i,round(v[0],1),round(v[1],1),int(v[2])) for i,v in rows[:6]])
print("reference itself:", res[("ref",0)])
json.dump({"ref":list(ref),"best":out}, open(str(ROOT / f'build/probe/flat_{tag}.json'),'w'))
