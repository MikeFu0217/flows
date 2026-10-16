#!/usr/bin/env python3
"""Generate the original Flows sample fixtures; no external recordings."""
from pathlib import Path
import math
import struct
import wave

ROOT = Path(__file__).resolve().parent
RATE = 48000

def write(name, channels, frames):
    with wave.open(str(ROOT / name), 'wb') as output:
        output.setparams((channels, 2, RATE, 0, 'NONE', 'not compressed'))
        output.writeframes(b''.join(struct.pack('<h', round(max(-1, min(1, value)) * 32767))
                                   for frame in frames for value in frame))

# A short kick with a decaying pitch sweep and a smooth tail.
phase = 0.0
kick = []
for i in range(int(RATE * .6)):
    time = i / RATE
    phase += 2 * math.pi * (48 + 130 * math.exp(-time * 45)) / RATE
    attack = min(1, time / .002)
    tail = max(0, min(1, (.6 - time) / .04))
    kick.append((.8 * math.sin(phase) * attack * math.exp(-time * 8) * tail,))
write('flows-kick.wav', 1, kick)

# One second, integer-cycle harmonics: the boundary loops cleanly at native rate.
write('flows-stereo.wav', 2, ((sum(.32 / n * math.sin(2 * math.pi * 130 * n * i / RATE + offset * (n - 1))
                                  for n in (1, 2, 3, 4)) for offset in (0, .3)) for i in range(RATE)))
