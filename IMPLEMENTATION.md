# Implementation Notes

## Issues Investigated
- **Fragment loss inside RTP FUs** – Fragmented NAL units could lose pieces on the air link, leaving the decoder with either dropped frames or heavily damaged pictures.
- **Decoder recovery after partial access units** – Without explicit hints the Rockchip pipeline discarded any frame that arrived with missing slices.

## Fixes and Key Changes
- **Custom depayloader updates** (`src/h265_depay.c`, `include/h265_depay.h`): Reworked the RTP H.265 depayload logic so Annex-B access units are assembled in-process, with an `emit-partial-au` property that forwards salvageable slices while tagging buffers as `DISCONT`/`CORRUPTED`. The element also guarantees byte-stream caps for downstream consumers.
- **Pipeline and decoder integration** (`src/pipeline.c`, `include/video_decoder.h`, `src/video_decoder.c`): Propagated corruption/discontinuity markers from the appsink to the Rockchip MPP layer and assert `errinfo` on partial frames so intact slices stay visible until the next clean IDR. The queue remains leaky to avoid blocking the UDP reader.
- **UDP receiver buffering** (`src/udp_receiver.c`): Kept the zero-copy oriented buffer pool while tightening the main loop to focus purely on forwarding packets into `appsrc` without auxiliary diagnostics.
- **Documentation updates** (`README.md`): Recorded the zero-copy pipeline, the depayloader behaviour, and how partial AU forwarding interacts with the decoder.

## Where the Fix Came From
The core fixes live in the depayloader and video decoder. Supporting tweaks to the pipeline and UDP receiver ensure access-unit metadata survives the trip from the network socket to the hardware decoder without adding extra latency.
