# Whistle patterns

Findings from the recorded double whistle, Windows speech, pink noise, hum, and
short excerpts from `~/Music`. The listener can still pick the whistle out of
those mixes. The detector often cannot. The whistle is a short mark on the
spectrum: a ridge about 150 Hz tall, a few FFT bins, not a band that owns the
frame.

## What the files are

`tests/audio/patterns/double-whistle.wav` and `single-whistle.wav` are 48 kHz
stereo. Ingest averages the channels and resamples to 16 kHz. The room tone is
near RMS 1. The whistles peak around 2400–3800.

Each whistle is one rising note with a 30–40 ms hole, not two notes. The pair
is two of those notes about 530 ms apart.

| Tone | Time | Mean pitch |
|---|---|---|
| First | 1220–1630 ms, about 400 ms | 2400 Hz, glide 2030→2530 |
| Second | 2210–2620 ms, about 400 ms | 2180 Hz, glide 1660→2440 |

The means are about 210 Hz apart, inside the 350 Hz pair limit. A pure 2 kHz
beep pair also matches. The rule is two narrow tones, not a voiceprint.

## What the detector does now

Each 10 ms hop, 512-point power spectrum. The tone follows a ridge. One loud
bin elsewhere is a hole, not a reason to throw the tone away.

1. The frame is active when RMS is at least `max(120, noise × 2.2)`.
2. A peak is a local maximum in 1–4 kHz. Prominence is the power in the peak
   ±2 bins, divided by the median power of the bins from ±3 to ±10, with the
   core left out. The median is at least 1e-8. Prominence has to reach 8.
   That 8 separates these recordings. It is not a general boundary: a tonal
   instrument can be prominent too.
3. Acquisition takes three hops in a row. Concentration, flatness, and the
   bass/speech band ratio are required only there. The tone’s start is the
   first of those hops.
4. Once locked, the next hop is the best peak within 180 Hz of the last
   accepted pitch, scored by prominence times how close it is. 350 Hz is the
   cap on that search, including across a hole, where the reach grows by 1 Hz
   per millisecond. It is not an allowed step on every hop.
5. A hop with no acceptable peak is a hole. The tone stays open for 80 ms.
   Good-hop time has to reach 70 ms and at least 55% of the tone. The mean
   pitch uses accepted hops only. `tone_end` is the last good hop. The pair
   gap uses that, not the later decision time.
6. A tone still going after 700 ms is rejected, and another tone cannot start
   until the ridge has been gone for the hang time.
7. Two accepted tones, gap 150–900 ms, mean pitches within 350 Hz, then a
   2.5 s cooldown.

```text
for each hop:
    if acquiring:
        require the wide-band gate and a prominent peak near the previous hop
        lock after 3 hops, keeping the original start
    else if a scored peak is inside the glide window:
        accept that hop
    else:
        count a hole
        if the hole exceeds 80 ms:
            close at the last good hop
            keep the tone when duration, good-hop time, and coverage pass
```

## Results

| Input | Result |
|---|---|
| Recorded double whistle | Match, 1210–2620 ms, gap 540 ms |
| Recorded single whistle | No match |
| Speech, pink noise, or hum alone | No match |
| Speech 18–24 dB under the whistle | Match |
| Eight-second music excerpts alone | No match. These are smoke tests, not a false-activation rate |
| Depeche Mode, 24 dB under | Match, 1220–2620 ms, gap 540 ms |
| Vitalic 24 dB under | Match, 1220–2630 ms, gap 550 ms |
| Tribe 24 dB under | Match, 1210–2600 ms, gap 550 ms |
| Winehouse 24 dB under | Match, 1220–2630 ms. The first tone runs long (590 ms), so the gap is 400 ms |
| Solo piano 18 dB under | Match, end 2580 ms |
| Orbital 18 dB under | Miss. The second whistle breaks into pieces and the pair pitch check drops them |
| In-band beep pair (2100 then 2300 Hz) | Match. This is a two-tone gesture, not one person’s whistle |
| One 2 s beep at 2100 Hz | Rejected as overlong. No second tone starts until it stops |
| Same take fed in 160, 320, 1000, or 8000 sample blocks | Same match |

Levels are the music or speech RMS against the whistle’s active RMS, not the
silence around it.

## What the −24 dB miss was

The old rule took the loudest bin in 1–4 kHz and, if that bin jumped more than
350 Hz, threw away the whole tone. On Winehouse, Vitalic, and Tribe at −24 dB
the second whistle was the right length and the right pitch, then one vocal or
bell partial won a hop and the tone was discarded. The pair limits were not
the failure.

The ridge tracker keeps that tone. A hop that leaves the ridge is a hole.
Those three mixes now match, and the clean take is still 1210–2620 ms.

The mean of the accepted hops can sit below the gray global-peak line. On the
clean take the tones average about 2070 Hz and 2140 Hz, while the loudest bin
still climbs through about 2500 Hz. A strong partial inside the 180 Hz glide
window pulls the tracked ridge for part of the first tone. The pair still
lands because both tones move together and stay inside the 350 Hz pitch limit.
At equal level the wide-band gate can still refuse to start a tone, because
acquisition still asks for band-wide contrast.

A short tone in the gap is still a pair event. A 140 ms beep at 3000 Hz
between the two whistles takes the second slot, fails the pitch check, and the
real second whistle is left without a partner. A louder partial 700 Hz off the
ridge, laid through the middle of the first tone, does not.

## Checking the measurements

`sound-trigger inspect` uses the same PFFFT frames as the detector.

```text
pm-audio sound-trigger inspect take.wav --json
pm-audio sound-trigger inspect take.wav --frames --out take.frames.jsonl
pm-audio sound-trigger inspect take.wav --plot take.svg
```

`--frames` is one `pm.sound.frame/v1` object per hop. Besides the old levels
it carries `global_peak_hz`, `tracked_peak_hz` (null when the hop matched
nothing), `predicted_peak_hz`, `tracked_prominence`, `state`
(`idle`, `acquiring`, `tracking`, `hole`, `released`), `hole_ms`,
`good_hop_fraction`, and `decision_reason`.

The plot draws the global peak in gray and the tracked ridge on top. Cyan is
acquisition. Gold is an accepted tracker hop. A ring at the top of the frame
marks the close: gold when the tone was kept, rust when it was rejected.
The dashed line is prominence 8. `whistle` on a frame means that hop was
accepted, not that the whole gesture matched.

## Training a different sound

The double whistle stays the built-in rule. A new gesture is a `template-dtw`
profile: 24 log bands, constrained DTW. Training needs 8 accepted takes of
that one gesture, plus at least one ambient file that contains an event.
Leave-one-out sets the threshold. Replaying one of the 8 takes is not a test.

One clap is not enough to enroll. `stemp/sound-trigger/isolation/clap.wav` is
a single CC0 take (Freesound 151219, OwlStorm). Copy it into eight separate
recordings, and keep a ninth out of the training set.

```text
pm-audio sound-trigger train --id clap --json --samples take1.wav take2.wav take3.wav take4.wav take5.wav take6.wav take7.wav take8.wav --ambient stemp/sound-trigger/isolation/music/vitalic.wav stemp/sound-trigger/isolation/music/piano.wav stemp/sound-trigger/isolation/speech-only.wav --out stemp/sound-trigger/clap.json
```

Stop if train prints one of these and exits 2:

```text
fewer than 8 positive takes passed signal-quality checks
positive takes have incompatible durations
positive and negative validation scores overlap
```

If it writes the profile, the held-out take must match and the ambient files
must not:

```text
pm-audio sound-trigger match held-out.wav --profile stemp/sound-trigger/clap.json --jsonl
pm-audio sound-trigger match stemp/sound-trigger/isolation/music/vitalic.wav --profile stemp/sound-trigger/clap.json --jsonl
pm-audio sound-trigger match stemp/sound-trigger/isolation/music/piano.wav --profile stemp/sound-trigger/clap.json --jsonl
pm-audio sound-trigger match stemp/sound-trigger/isolation/speech-only.wav --profile stemp/sound-trigger/clap.json --jsonl
```

A match is `pm.sound.trigger/v1` and exit 0. A reject is `pm.sound.no-match/v1`
and exit 2.

Live check of the built-in double whistle. This uses the system default mic,
runs until Ctrl+C, and prints a line each time the pair is found:

```text
pm-audio sound-trigger test
```

`--input` selects a device substring. Empty is the system default. `--source`
is `mic` unless you pass `desktop`. `--profile` replaces the built-in whistle
for that run. A camera-plus-mic mix stays on the host `sound-trigger` command.

The enrolled whistle is the eight takes in `tests/audio/patterns/1.wav`
through `8.wav`. Same ambient set as the clap example. This writes
`stemp/sound-trigger/whistle.json`:

```text
pm-audio sound-trigger train --id whistle --json --samples tests/audio/patterns/1.wav tests/audio/patterns/2.wav tests/audio/patterns/3.wav tests/audio/patterns/4.wav tests/audio/patterns/5.wav tests/audio/patterns/6.wav tests/audio/patterns/7.wav tests/audio/patterns/8.wav --ambient stemp/sound-trigger/isolation/music/vitalic.wav stemp/sound-trigger/isolation/music/piano.wav stemp/sound-trigger/isolation/speech-only.wav --out stemp/sound-trigger/whistle.json
```

Listen on the system default mic until Ctrl+C. A found line is that profile,
and the score is a confidence (higher is a closer match):

```text
pm-audio sound-trigger test --profile stemp/sound-trigger/whistle.json
```

## Phone

Settings has a Sound trigger button. It bundles `dist/shared/sound-profiles`,
including the trained whistle JSON and its feature sidecar, and runs this
detector on the open microphone. A match starts live voice. Stop is the
microphone notification. See
[handsfree-android.md](handsfree-android.md).
