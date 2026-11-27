# Headphone surround test sound

`ball-bounce.wav` is the sound of macSteam's headphone surround Test (Experimental › Headphone
surround, `docs/SURROUND.md`). The test repeats it at different spots around the listener
(`scripts/surround_signal.h`).

- **Source:** "bouncy ball on tile" (Pixabay id 95978), uploaded by freesound_community, from
  Pixabay as `freesound_community-bouncy-ball-on-tile-95978.mp3`.
- **Licence:** [Pixabay Content License](https://pixabay.com/service/license-summary/). It's free
  to use and to change, and no credit is needed. It may not be sold or handed out on its own,
  unchanged.
- **What was done to it:** one bounce was cut out, 0.297–0.497 s, the first hit. It was made
  mono at 48 kHz, rumble below 80 Hz was removed, it got a 1 ms fade in and a fade out over its
  last 60 %, and its peak was set to 0.6. 16-bit WAV, 0.2 s. Made with:

  ```sh
  python3 scripts/surround_render.py cut freesound_community-bouncy-ball-on-tile-95978.mp3 0.297 0.2 scripts/surround/ball-bounce.wav
  ```
