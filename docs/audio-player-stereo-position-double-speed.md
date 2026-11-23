# Stereo player position advanced at twice the playback rate

RhythmBelle used `QMediaPlayer::position()` as its beatmap clock. On the X7 ROM in
the iOS simulator, a 2.4-second test map reached its end after roughly 1.2 seconds
of host wall time. Rendering geometry was stable, and the same beatmap and MP3
advanced at the correct rate on macOS, so neither the osu! timestamp parser nor
the Qt frontend transform explained the discrepancy.

The useful measurement was the client's progress bar: it is drawn directly from
the MMF-reported player position. At 0.215, 0.479, 0.752, and 1.016 seconds after
play began, it represented approximately 0.531, 1.039, 1.600, and 2.137 seconds.
The factor stayed close to two and the source MP3 was stereo. This pointed to a
channel-count conversion, not timer jitter or video-capture cadence.

`audio_output_stream::current_frame_position()` returns PCM frames. Each frame
already includes one sample for every interleaved channel. `player_shared` treated
that value as a scalar-sample count and multiplied it by `channels_` while
converting frames to microseconds. Stereo files therefore exposed exactly twice
the real position. The TinySoundFont player repeated the same conversion.

Both paths now compute `frames * 1,000,000 / sample_rate` without a channel
multiplier. This is a shared audio contract fix rather than a RhythmBelle or iOS
special case; mono behavior is unchanged, while stereo player position once again
tracks playback time.
