#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""CLI integration: gain is linear; IMBE erasures reach concealment and report flags."""
import array
import math
import subprocess
import sys
import tempfile
import wave
from pathlib import Path


def read(path):
    with wave.open(str(path), 'rb') as f:
        assert f.getframerate() == 8000 and f.getnchannels() == 1 and f.getsampwidth() == 2
        pcm = array.array('h', f.readframes(f.getnframes()))
    if sys.byteorder != 'little':
        pcm.byteswap()
    return pcm


def main():
    tool = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='mbe-cli-test-') as tmp:
        work = Path(tmp)
        source = work/'speech.wav'
        pcm = array.array('h', (round(1800 * sum(math.sin(2*math.pi*150*h*n/8000)/h
                              for h in range(1, 12))) for n in range(12800)))
        if sys.byteorder != 'little':
            pcm.byteswap()
        with wave.open(str(source), 'wb') as f:
            f.setparams((1, 2, 8000, 0, 'NONE', 'not compressed')); f.writeframes(pcm.tobytes())
        lost = work/'lost.txt'; lost.write_text('\n'.join(map(str, range(20, 28))))

        def call(*args):
            return subprocess.run([tool, *map(str, args)], capture_output=True, text=True, check=True)

        for rate in ['imbe4400', 'imbe7200']:
            frames = work/f'{rate}.frames'
            call('encode', '--rate', rate, source, frames)
            high, low, missing = (work/f'{rate}.{suffix}.wav' for suffix in ['high', 'low', 'missing'])
            clean = call('decode', '--rate', rate, '--errors', '--gain', '1', frames, high)   # the default
            call('decode', '--rate', rate, '--gain', '0.5', frames, low)
            damaged = call('decode', '--rate', rate, '--errors', '--lost', lost, frames, missing)
            a, b, c = read(high), read(low), read(missing)
            assert len(a) == len(b) == len(c) == 84*160
            ratio = math.sqrt(sum(x*x for x in a)/sum(x*x for x in b))
            assert abs(ratio-2) < .005, (rate, 'gain applied more than once', ratio)
            assert a[:20*160] == c[:20*160], (rate, 'loss changed earlier audio')
            assert a[20*160:28*160] != c[20*160:28*160], (rate, '--lost was ignored')
            clean_lines = clean.stdout.splitlines(); lines = damaged.stdout.splitlines()
            assert len(clean_lines) == len(lines) == 84, (rate, '--errors was ignored')
            for frame in range(20, 28):
                assert int(lines[frame].split()[0]) == frame
                assert 'R' in lines[frame], (rate, frame, lines[frame])
            assert 'M' in lines[27], (rate, 'long loss did not mute')
            assert 'R' not in lines[28] and 'M' not in lines[28], (rate, 'next good frame did not resume')
            rejected = subprocess.run([tool, 'decode', '--rate', rate, '--lost', str(work/'absent.txt'),
                                       str(frames), str(missing)], capture_output=True)
            assert rejected.returncode != 0, 'missing loss file silently ignored'
            print(f'{rate}: linear gain, loss flags, mute and recovery passed')


if __name__ == '__main__':
    main()
