# WINQ-EMU QEMU

A fork of [QEMU](https://www.qemu.org/) (based on v11.0.0-rc2) optimized for running Linux VMs on Windows with hardware GPU acceleration.

## What's Changed

All changes are in the `winq-emu-alpha1` branch, applied as a single commit on top of upstream `v11.0.0-rc2`.

### WHPX Host CPU Passthrough
- **`-cpu host` for WHPX**: Full CPUID passthrough from the host CPU to the guest, including AVX-512, hybrid core topology, and all modern instruction sets. Previously, WHPX only supported named CPU models.

### Venus GPU Integration
- **virtio-gpu Venus support**: Working configuration for Venus Vulkan forwarding with blob resources on Windows
- **120Hz EDID default**: Changed default virtual display refresh rate from 75Hz to 120Hz

### Build Configuration
- Includes a build script preconfigured for MSYS2 UCRT64 with OpenGL, virglrenderer, and WHPX enabled

## Building

### Requirements (MSYS2 UCRT64)

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-meson \
          mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt64-x86_64-pkg-config \
          mingw-w64-ucrt-x86_64-glib2 mingw-w64-ucrt-x86_64-pixman \
          mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-gtk3 \
          mingw-w64-ucrt-x86_64-libepoxy mingw-w64-ucrt-x86_64-angleproject \
          mingw-w64-ucrt-x86_64-curl mingw-w64-ucrt-x86_64-libssh \
          mingw-w64-ucrt-x86_64-snappy mingw-w64-ucrt-x86_64-lzo2 \
          mingw-w64-ucrt-x86_64-zstd mingw-w64-ucrt-x86_64-capstone \
          mingw-w64-ucrt-x86_64-dtc mingw-w64-ucrt-x86_64-libslirp
```

You also need [winq-emu-virglrenderer](https://github.com/cmspam/winq-emu-virglrenderer) built and installed to `/ucrt64`.

### Build

```bash
./build.sh
```

Or manually:

```bash
mkdir -p build && cd build
../configure --target-list=x86_64-softmmu --enable-whpx --enable-opengl --enable-virglrenderer --disable-docs --disable-plugins
ninja
```

The output is `build/qemu-system-x86_64.exe`.

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
  -device virtio-net-pci,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp::2223-:22
```

## Syncing with Upstream

This fork tracks [QEMU upstream](https://gitlab.com/qemu-project/qemu). To sync:

```bash
git remote add upstream https://gitlab.com/qemu-project/qemu.git
git fetch upstream
git checkout winq-emu-alpha1
git rebase upstream/master
```

## Related Projects

- [winq-emu-virglrenderer](https://github.com/cmspam/winq-emu-virglrenderer) - virglrenderer fork with Windows Venus port
- [winq-emu](https://github.com/cmspam/winq-emu) - Installer, launcher, and project page

## License

GPL-2.0 (same as upstream QEMU)

## Contributing

This is an alpha release. Anyone is welcome to look at, modify, and/or merge these changes into upstream projects.
