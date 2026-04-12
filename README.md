# WINQ-EMU QEMU

A fork of [QEMU](https://www.qemu.org/) (based on v11.0.0-rc3) optimized for running Linux VMs on Windows with hardware GPU acceleration.

## What's Changed

All changes are applied as a single commit on top of upstream `v11.0.0-rc3`, making it easy to rebase onto future QEMU releases.

### WHPX Host CPU Passthrough
- **`-cpu host` for WHPX**: Full CPUID passthrough from the host CPU to the guest, including AVX-512, hybrid core topology, and all modern instruction sets. Previously, WHPX only supported named CPU models.

### Venus GPU Integration
- **virtio-gpu Venus support**: Working configuration for Venus Vulkan forwarding with blob resources on Windows
- **Dynamic EDID refresh rate**: Automatically matches the host monitor's refresh rate (falls back to 120Hz)

### SDL Display Enhancements
- **USB tablet fix**: Deferred mouse mode changes to SDL thread to prevent freezes on Windows
- **EGL guard**: Prevents crash on systems without EGL
- **DPI awareness**: Per-monitor DPI aware on Windows

### Build Configuration
- Includes a build script preconfigured for MSYS2 UCRT64 with OpenGL, virglrenderer, slirp, and WHPX enabled

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

For example, to rebase from v11.0.0-rc3 to a future v11.0.0:

```bash
git rebase --onto v11.0.0 v11.0.0-rc3 HEAD
```

## Related Projects

- [winq-emu-virglrenderer](https://github.com/cmspam/winq-emu-virglrenderer) - virglrenderer fork with Windows Venus port
- [winq-emu](https://github.com/cmspam/winq-emu) - Installer, launcher, and project page

## License

GPL-2.0 (same as upstream QEMU)

## Contributing

This is an alpha release. Anyone is welcome to look at, modify, and/or merge these changes into upstream projects.
