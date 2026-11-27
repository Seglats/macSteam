#!/usr/bin/env python3
"""Writes scripts/surround/kemar_elev0.h from MIT's KEMAR compact HRTF set: the 37 directions at
ear height (0 to 180 degrees to the right, every 5; the left side is the mirror image), 128
samples a direction and ear at 44.1 kHz, as in the set's elev0/H0e<az>a.wav files.

    curl -O https://sound.media.mit.edu/resources/KEMAR/compact.zip   # then unzip it
    python3 scripts/surround_kemar.py path/to/unzipped                # the folder with elev0/

"Copyright 1994 by the MIT Media Laboratory. It is provided free with no restrictions on use,
provided the authors are cited when the data is used in any research or commercial application."
Bill Gardner and Keith Martin, MIT Media Lab Machine Listening Group."""
from pathlib import Path
import struct
import sys
import wave

ROOT = Path(__file__).resolve().parents[1]


def main(folder):
    rows = []
    for az in range(0, 181, 5):
        with wave.open(str(Path(folder) / 'elev0' / f'H0e{az:03d}a.wav')) as w:
            assert w.getnchannels() == 2 and w.getframerate() == 44100 and w.getnframes() == 128
            values = struct.unpack('<256h', w.readframes(128))
        left, right = values[0::2], values[1::2]
        rows.append('    {{%s},\n     {%s}},' % (','.join(map(str, left)), ','.join(map(str, right))))
    out = ROOT / 'scripts/surround/kemar_elev0.h'
    out.write_text('''/* MIT KEMAR head-related impulse responses, compact set, elevation 0 (scripts/surround_kemar.py).
 * kemar_elev0[a][ear][n]: source at 5*a degrees to the right (0 front, 180 behind), ear 0 left,
 * 1 right, 128 samples at 44.1 kHz, 16-bit. A source on the left is the mirror image.
 *
 * Bill Gardner and Keith Martin, "HRTF Measurements of a KEMAR Dummy-Head Microphone", MIT Media
 * Lab Perceptual Computing Technical Report #280, 1994. "Copyright 1994 by the MIT Media
 * Laboratory. It is provided free with no restrictions on use, provided the authors are cited when
 * the data is used in any research or commercial application." */
#define KEMAR_RATE 44100
#define KEMAR_TAPS 128
#define KEMAR_STEPS 37
static const short kemar_elev0[KEMAR_STEPS][2][KEMAR_TAPS] = {
''' + '\n'.join(rows) + '\n};\n')
    print(out, out.stat().st_size, 'bytes')


if __name__ == '__main__':
    main(sys.argv[1])
