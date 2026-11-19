"""Embed the checked-in mono PCM tracks without a runtime decoder."""
import struct
import sys
import wave
from pathlib import Path

TRACKS = ('lanterns-on-the-belt-compact', 'admission', 'plate-pickup', 'bite',
          'stomach-red', 'stomach-recover', 'card-flip', 'card-reveal', 'closing')


def main():
    paths = {Path(arg).stem: Path(arg) for arg in sys.argv[2:]}
    with Path(sys.argv[1]).open('w', encoding='ascii') as out:
        print('#include "projects/buffet/app/src/buffet_audio.h"', file=out)
        sizes = []
        for i, track in enumerate(TRACKS):
            with wave.open(str(paths[track]), 'rb') as wav:
                if (wav.getframerate(), wav.getsampwidth(), wav.getnchannels(), wav.getcomptype()) != (16000, 2, 1, 'NONE'):
                    raise ValueError(f'incompatible track: {track}')
                sizes.append(wav.getnframes())
                print(f'static const int16_t pcm_{i}[] = {{', file=out)
                while data := wav.readframes(48):
                    print(','.join(str(v[0]) for v in struct.iter_unpack('<h', data))+',', file=out)
                print('};', file=out)
        print('const bf_clip_t bf_tracks[BF_SOUND_COUNT] = {',file=out)
        for i, size in enumerate(sizes):
            print(f'{{pcm_{i},{size}}},',file=out)
        print('};',file=out)


if __name__ == '__main__':
    main()
