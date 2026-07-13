# MPEG-TS non-fast seek strategy

## Goal

For seekable MPEG-TS sources that do not support fast seeking, determine the
stream duration with boundary probes and complete each time seek with one
approximate byte seek. Do not perform the repeated timestamp search used for
fast-seekable sources.

## Duration probing

`ProbeStart()` and `ProbeEnd()` run for seekable sources, including sources
where `STREAM_CAN_FASTSEEK` is false. The first PCR, or first DTS when PCR is
not available, and the last DTS establish the duration returned by
`DEMUX_GET_LENGTH`.

Boundary probing is separate from user-initiated seeking. It may issue more
than one stream seek while locating the end timestamp and restoring the
original read position.

## Non-fast time seek

For a non-fastseek source, `SeekToTime()` converts the requested timestamp to
an absolute stream position using the probed time range:

```text
start    = first PCR, or first DTS
end      = last DTS + PCR offset
fraction = clamp((target - start) / (end - start), 0, 1)
byte     = first timestamp byte
         + (last timestamp byte - first timestamp byte) * fraction
```

It then calls `vlc_stream_Seek()` once. No PCR scan or binary timestamp search
is performed after that seek. The result is intentionally approximate and can
differ from the requested time for variable-bitrate streams.

Both timestamp probes retain their corresponding byte offsets. This keeps the
time and byte ranges on the same origin when the selected program starts after
leading TS data. Stream filters installed after initial PMT parsing also adopt
the current source offset before probing, so a logical seek to byte zero is not
mistaken for the source's current position.

## Fast seek and reported position

Fast-seekable MPEG-TS sources retain the existing timestamp-based binary
search. Player position reporting also uses the normal VLC time/duration
calculation; byte offsets are not propagated through frames, ES output, or
the player timer.
