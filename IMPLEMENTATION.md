# Implementation Notes

## Issues Investigated
- **Fragment loss inside RTP FUs** – Logs and on-air tests showed frequent "fu-missing-start" drops and partial AUs caused by sequence gaps while fragmented units were still being assembled.
- **Upstream transport uncertainty** – It was unclear whether corruption originated on the RF link, the kernel UDP queue, or the in-process buffering layer, making root-cause analysis difficult.
- **Queue signal configuration mismatch** – The queue between `appsrc` and the custom depayloader was supposed to surface overrun/underrun events, but the property name used to enable signalling was incorrect, leading to warnings and missing diagnostics.

## Fixes and Key Changes
- **Custom depayloader diagnostics** (`src/h265_depay.c`, `include/h265_depay.h`): Added RTP sequence extension, detailed loss tracking, and reason-coded logging when closing or forwarding damaged access units. Partial AU support is exposed through a property so the pipeline can forward salvageable slices with the appropriate GST buffer flags. Caps negotiation now guarantees AU-aligned byte-stream output.
- **UDP receiver hardening** (`src/udp_receiver.c`): Switched to `recvmsg()` to inspect `SO_RXQ_OVFL`, count kernel-level drops, and detect truncated datagrams. Introduced counters for delivered packets and truncations so remaining artefacts can be correlated with transport issues.
- **Pipeline wiring and metadata propagation** (`src/pipeline.c`, `include/pipeline.h`): Enabled queue overrun/underrun callbacks, tracked their counters with atomic increments, and forwarded buffer corruption/discontinuity flags to the Rockchip decoder. Added an environment-driven knob for depayloader statistics when building the pipeline.
- **Decoder integration** (`src/video_decoder.c`, `include/video_decoder.h`): Extended the feed path to honour corruption hints and, when available, set Rockchip MPP `errinfo` so damaged frames still render intact slices.
- **Documentation updates** (`README.md`): Documented the augmented diagnostics workflow, including environment variables and logging guidance for field operators.

## Where the Fix Came From
The most impactful behavioural changes reside in `src/h265_depay.c` and `src/udp_receiver.c`. Supporting updates in `src/pipeline.c`/`include/pipeline.h` and `src/video_decoder.c` were required to propagate diagnostics and corruption flags end-to-end. Documentation changes ensure the new tooling is discoverable.
