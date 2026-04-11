# T14s Camera Notes

## Scope

This directory contains the current serious work for the internal front camera on the Lenovo ThinkPad T14s Gen 6 Snapdragon/X1E.

Relevant local copies:

- Kernel sensor driver copy:
  - [`ov02c10.c`](/home/lak/camera-re-serious/kernel/ov02c10.c)
- Serious libcamera tree:
  - [`libcamera-src`](/home/lak/camera-re-serious/libcamera-src)

The camera sensor is `ov02c10`. The current Linux path is:

- `ov02c10` sensor
- Qualcomm CAMSS media graph
- `libcamera` `simple` pipeline
- `ipa/simple` userspace processing

## What We Implemented

### Kernel

Patched source is copied here:

- [`ov02c10.c`](/home/lak/camera-re-serious/kernel/ov02c10.c)

The kernel-side changes we are keeping are:

- Add `get_selection()` support for crop/default/bounds.
- Use a sane exposure default instead of relying on poor startup defaults.

Reason:

- Without `get_selection()`, `libcamera` warns that crop/pixel-array rectangles are missing and falls back to guessed sensor geometry.
- The exposure default change reduces bad startup behavior and matches the single supported native mode better.

We explicitly reverted the experimental extra `1920x1080` sensor mode. The driver is back to the single native Bayer mode.

## Serious libcamera Changes

Implemented files:

- [`camera_sensor_properties.cpp`](/home/lak/camera-re-serious/libcamera-src/src/libcamera/sensor/camera_sensor_properties.cpp)
- [`camera_sensor_helper.cpp`](/home/lak/camera-re-serious/libcamera-src/src/ipa/libipa/camera_sensor_helper.cpp)
- [`awb.cpp`](/home/lak/camera-re-serious/libcamera-src/src/ipa/simple/algorithms/awb.cpp)
- [`awb.h`](/home/lak/camera-re-serious/libcamera-src/src/ipa/simple/algorithms/awb.h)
- [`ov02c10.yaml`](/home/lak/camera-re-serious/libcamera-src/src/ipa/simple/data/ov02c10.yaml)
- [`meson.build`](/home/lak/camera-re-serious/libcamera-src/src/ipa/simple/data/meson.build)

### 1. Sensor properties

Added an `ov02c10` entry in:

- [`camera_sensor_properties.cpp`](/home/lak/camera-re-serious/libcamera-src/src/libcamera/sensor/camera_sensor_properties.cpp)

What it provides:

- unit cell size
- test pattern mapping
- sensor delays

Reason:

- Stock `libcamera` had no static properties for `ov02c10`.
- That caused warnings and fallback behavior.

### 2. Sensor helper

Added an `ov02c10` helper in:

- [`camera_sensor_helper.cpp`](/home/lak/camera-re-serious/libcamera-src/src/ipa/libipa/camera_sensor_helper.cpp)

What it provides:

- black level `4096`
- analogue gain model `AnalogueGainLinear{ 1, 0, 0, 16 }`

Reason:

- Stock `libcamera` had no sensor helper for `ov02c10`.
- The helper gives `simple` a sensor-specific gain and black-level model instead of generic fallback behavior.

### 3. Sensor-specific tuning file

Added:

- [`ov02c10.yaml`](/home/lak/camera-re-serious/libcamera-src/src/ipa/simple/data/ov02c10.yaml)

And installed it via:

- [`meson.build`](/home/lak/camera-re-serious/libcamera-src/src/ipa/simple/data/meson.build)

What it currently defines:

- `BlackLevel.blackLevel: 4096`
- `Awb` decision-locus and finite warm-start parameters
- `Ccm` matrices
- `Adjust` gamma/contrast/saturation

Reason:

- Stock `simple` used generic `uncalibrated.yaml`, which was not good enough for this sensor/module.

### 4. AWB decision-locus support

Added sensor-scoped AWB logic in:

- [`awb.cpp`](/home/lak/camera-re-serious/libcamera-src/src/ipa/simple/algorithms/awb.cpp)
- [`awb.h`](/home/lak/camera-re-serious/libcamera-src/src/ipa/simple/algorithms/awb.h)

What it does:

- parses an optional `decisionLocus` from YAML
- projects the measured AWB decision onto that locus
- blends toward it using `locusBlend`
- supports finite warm-start with:
  - `warmStartBlend`
  - `warmStartFrames`
  - `warmStartRgRange`
  - `warmStartBgRange`

Reason:

- This is the strongest Windows-derived AWB signal we extracted that we can map into `simple` without changing global behavior for other cameras.

## What We Explicitly Did Not Keep

These were tried in the experimental tree and are not part of the serious tree:

- no routing patch in `simple.cpp`
- no startup exposure/gain hardcoding in AGC or `soft_simple`
- no `cam` debug app file-output fix
- no debug scripts
- no AWB prior curve / `priorBlend`

Reason:

- They were either debug-only, not proven necessary, or not supported strongly enough by the evidence.

## What We Extracted From Windows And Used

Windows RE sources:

- [`OV02C10_RE_NOTES.md`](/home/lak/camera-re/windows-collection/OV02C10_RE_NOTES.md)

Useful front-camera artifacts:

- `com.qti.sensormodule.ov02c10.bin`
- `com.qti.tuned.lvi_ov02c10.bin`

Relevant Windows-side findings:

- Windows uses a full Qualcomm ISP/tuning stack for this camera, not just raw sensor config.
- The tuned blob contains separate AWB and color-correction stages.
- There is a shared 10-point `rg/bg` decision locus that appears to be part of the AWB decision space.
- The `LVI` and `Chicony` blobs differ only slightly; most tuning appears common.
- Windows uses warm-start and post-processed AWB decision logic.

What we used from that:

- the shared 10-point AWB decision locus in [`ov02c10.yaml`](/home/lak/camera-re-serious/libcamera-src/src/ipa/simple/data/ov02c10.yaml)
- the idea of bounded warm-start for AWB startup behavior

What we did not use:

- speculative startup exposure/gain values
- the more weakly supported 2-point prior curve
- any unverified interpretation of the deeper Windows AWB/AE logic

## Current Status

This serious tree now contains the minimum set of changes that are currently defensible and useful:

- sensor geometry and delay knowledge
- sensor helper
- sensor-specific tuning file
- Windows-derived AWB decision-locus guidance
- finite warm-start
- sensor-specific CCM and image adjustment defaults

The serious tree was rebuilt with:

- `simple` pipeline enabled
- `simple` IPA enabled
- `cam` app enabled

## What Is Still Missing

The remaining quality gap is not basic bring-up anymore. It is startup and overall ISP behavior.

Still missing for better images:

- a robust cold-start policy for AE/AGC
  - current cold-start behavior is still unstable
  - early bad frames and early stats appear to poison startup control decisions
- better startup stats/control sequencing
  - the current problem is not explained by AWB alone
- more accurate Windows-equivalent AWB/AE logic
  - especially the deeper `Decision_AfterTC` / illuminance-aware behavior
- more of the Qualcomm tuning stack
  - lens shading
  - denoise
  - richer tone/gamma behavior
  - more exact CCM/WB stage behavior
- likely a better end-state than `simple` if the goal is genuinely good laptop webcam quality

## Known Limitation

The current `simple`-based path is good enough to validate sensor support and improve image quality materially, but it still does not behave like a mature platform-specific laptop camera stack. The biggest remaining issue is true cold-start behavior after sensor reprobe or reboot.
