# Native ArkOS port

Upstream: https://github.com/Kyant0/AndroidLiquidGlass

Pinned source: `65ab177e90e5c1d8c62e70cf7755841982da65f6` (kmp branch,
2026-08-26). The upstream source files in this directory are unmodified references.
The Apache-2.0 license and original shader copyright notice are preserved.

`user/liquid_glass.c` is a modified native C translation of the rounded rectangle
SDF, circular refraction, normal/depth calculation, seven-color dispersion,
directional highlight, 1.5 saturation, and the example's .5/300 press spring.
`user/desktop_glass.inc` integrates the effect with the live guest compositor.
ArkOS supplies its own shape coverage, window manager, rasterizer and graphics driver.
The original Android/Compose APIs remain reference material and are not guest dependencies.

The XRGB path uses opaque samples, bilinear Q8 interpolation, a finite separable
Gaussian with integer weights, and analytic geometry cached at Q8 offsets. These
are documented numerical differences from Android/Skia's floating GPU effects.
A formula comparison is distinct from a pixel comparison of Android and ArkOS.
The CPU math port runs inside ArkOS Ring3; ordinary builds are offline.

The GPU path lowers the same lens, gradient, dispersion, highlight and vibrancy
equations into fixed TGSI in `scripts/build-glass-shaders.py`. The native
`kernel/virtio_gpu.c` PCI driver retains intermediate GPU textures and submits
four passes: source preparation, horizontal Gaussian, vertical Gaussian/tint,
and lens/coverage composition. Source preparation avoids repeating saturation
and integer shadow work for every blur tap. No Android, Compose, Linux, DRM or
Mesa runtime is linked into the guest.

GPU sampling uses floating bilinear texture filtering and an 8×8 cubic corner
coverage calculation. Fragment positions are exact half-integer pixel centers.
The finite integer Gaussian and ArkOS continuous corner shape still differ
from Android/Skia. The GPU test's independent native oracle found maximum
channel differences of two levels across seven scenes; this verifies ArkOS's
GPU equations, not pixel equality against an Android device.

The Apple M4 result is obtained by the normal virtual-GPU implementation in
UTM 4.7.5 / QEMU 10.0.2 / VirGL / ANGLE Metal. QEMU is the development target,
not an ArkOS runtime service. Final pixel presentation currently reads the
GPU material back into the guest desktop canvas; full GPU composition and
direct GPU scanout remain separate work. See `docs/GPU.md` for evidence.
