# WINQ-EMU QEMU

A fork of [QEMU](https://www.qemu.org/) (based on v11.0.0) optimized for running Linux VMs on Windows with hardware GPU acceleration, hardware video decode, and host folder sharing.

## What's Changed

All changes are applied as a series of commits on top of upstream `v11.0.0`, making it easy to rebase onto future QEMU releases.

### WHPX Host CPU Passthrough
- **`-cpu host` for WHPX**: Full CPUID passthrough from the host CPU to the guest, including AVX-512, hybrid core topology, and all modern instruction sets. Previously, WHPX only supported named CPU models.

### WHPX Improvements (Alpha 10)
- **`IA32_PAT` MSR sync**: PAT MSR is now synchronised between QEMU and the WHPX partition. Without this, Linux's MTRR/PAT cache-type computation could fall back to UC for memory that should be Write-Combining — a real perf cliff for virtio-gpu / Venus shared mappings.
- **5 ms inner exit loop deadline**: Caps the time `whpx_vcpu_run` can spend handling cheap exits (MMIO / portio / CPUID / MSR) before yielding back to the cpu-loop. Smoother frame pacing under Vulkan / Venus submit storms.
- **`FastHypercallOutput` synthetic feature on x86**: brings the x86 path in line with ARM. Removes one mapping fault per Hyper-V hypercall.
- **`UnimplementedMsrAction = IgnoreWriteReadZero` (Windows 11 24H2+)**: hypervisor handles Linux's boot-time MSR probing in-kernel instead of trapping to userspace.
- **`WHvAdviseGpaRange(Pin)` for ≥256 MiB regions (Windows 11 24H2+)**: pins SLAT entries for the base RAM and the Venus blob hostmem region so the hypervisor doesn't demote large pages or evict under host memory pressure.

### Venus GPU Integration
- **virtio-gpu Venus support**: Working configuration for Venus Vulkan forwarding with blob resources on Windows
- **Dynamic EDID refresh rate**: Automatically matches the host monitor's refresh rate (falls back to 120Hz)

### SDL Display Enhancements
- **USB tablet fix**: Deferred mouse mode changes to SDL thread to prevent freezes on Windows
- **EGL guard**: Prevents crash on systems without EGL
- **DPI awareness**: Per-monitor DPI aware on Windows

### virtio-9p Folder Sharing on Windows
- **Windows 9pfs port**: Enables `-virtfs local,...` on Windows hosts so the GUI launcher's Folder Sharing tab can map Windows folders into the Linux guest. Based on the v4 patches by Bin Meng, with audit-cited correctness fixes.

### VA-API Hardware Video Decode
- **virgl_video acceleration**: `virtio-gpu-virgl` is built with `virgl_video` enabled so the guest's Mesa Gallium VA driver routes decode requests to the host's D3D11 video decoder (in the matching `winq-emu-virglrenderer`).

### Build Configuration
- Includes a build script preconfigured for MSYS2 UCRT64 with OpenGL, virglrenderer, slirp, virtio-9p, and WHPX enabled

## Building

### Requirements (MSYS2 UCRT64)

All builds must be done from the MSYS2 UCRT64 shell (not MINGW64 or MSYS).

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-meson \
          mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkg-config \
          mingw-w64-ucrt-x86_64-glib2 mingw-w64-ucrt-x86_64-pixman \
          mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-libepoxy \
          mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-dtc \
          mingw-w64-ucrt-x86_64-zstd mingw-w64-ucrt-x86_64-libslirp \
          git diffutils
```

**Important**: The `git` and `diffutils` packages (from the MSYS2 base, not the mingw-w64 variants) are required — QEMU's meson build needs `git` to fetch subprojects and `diff` to run its test schema checks. Without these, configure will fail.

You also need [winq-emu-virglrenderer](https://github.com/cmspam/winq-emu-virglrenderer) built and installed to `/ucrt64` before building QEMU.

### Build

```bash
./build.sh
```

Or manually:

```bash
mkdir -p build && cd build
../configure --target-list=x86_64-softmmu --prefix=/ucrt64 --enable-whpx --enable-opengl --enable-virglrenderer --enable-slirp --disable-docs --disable-plugins
ninja
```

The output is `build/qemu-system-x86_64.exe` and `build/qemu-img.exe`.

### Running

```bash
qemu-system-x86_64.exe \
  -machine q35,accel=whpx \
  -cpu host \
  -m 8G -smp 8 \
  -drive file=disk.qcow2,format=qcow2,if=virtio \
  -device virtio-vga-gl,blob=on,hostmem=4G,venus=on \
  -display sdl,gl=on \
  -device virtio-sound-pci \
  -usb -device usb-tablet \
  -device virtio-net-pci,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp::2223-:22
```

## Syncing with Upstream

This fork tracks [QEMU upstream](https://gitlab.com/qemu-project/qemu). All custom changes are in a single commit on top of the upstream tag, so rebasing is straightforward:

```bash
git remote add upstream https://gitlab.com/qemu-project/qemu.git
git fetch upstream --tags
git rebase --onto <new-tag> <old-tag> HEAD
```

For example, to rebase from v11.0.0 onto a future v11.1.0:

```bash
git rebase --onto v11.1.0 v11.0.0 HEAD
```

## Related Projects

- [winq-emu-virglrenderer](https://github.com/cmspam/winq-emu-virglrenderer) - virglrenderer fork with Windows Venus port
- [winq-emu](https://github.com/cmspam/winq-emu) - Installer, launcher, and project page

## License

GPL-2.0 (same as upstream QEMU)

## Contributing

This is an alpha release. Anyone is welcome to look at, modify, and/or merge these changes into upstream projects.
