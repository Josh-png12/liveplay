# Audio isolation and physical hardware test

Run this test manually on the intended Windows machine for 10–15 minutes.

1. Open a project with one continuous audio cue and at least two video cues.
2. In **Settings → Event Hardware Test**, choose the video monitor, press
   **Identify Displays**, then start the 15-minute harness.
3. Start continuous audio. Exercise the left/right/stereo signal checks,
   video playback, fade-in, fade-out, fade to black, crossfade, seeks and cue
   changes. Toggle Speech Mode and operate the Show Board.
4. Stop the harness and save its JSON summary. Check the recorded underruns,
   callback overruns, maximum A/V drift, corrections, video errors and media
   errors.
5. Complete the human checklist: audible glitches heard (yes/no), picture
   appeared on the selected monitor (yes/no), correct left/right routing
   (yes/no), and each transition visually completed (yes/no).

The application does not infer whether a glitch was audible. Acceptance for
the hardware run is zero operator-confirmed audible dropouts.
