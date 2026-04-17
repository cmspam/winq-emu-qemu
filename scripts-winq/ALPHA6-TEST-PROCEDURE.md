# Alpha 6 end-to-end test procedure

Everything in the Alpha 6 checkpoint is built and committed. Two features
still need end-to-end verification in the guest VM, but the host WHPX state
is currently dirty (known bug #3: "WHPX error when rebooting a VM"). A
clean host reboot is the fastest path to testing.

## After host reboot

Open an MSYS2 UCRT64 shell and run:

```bash
cd /c/Users/charlesmiller/Development/claude/winq-emu-qemu
```

### 1. Sanity: minimal (headless) boot

Confirms the binary and 9p/VA-API patches haven't broken anything basic.

```bash
MODE=minimal scripts-winq/test-vm.sh -- 'echo alive'
```

Expected: `[test-vm] ssh ready` then `alive`, exit 0.

### 2. 9p folder sharing (should already work — smoke-test only)

Already tested end-to-end pre-reboot and passes. Re-confirm:

```bash
mkdir -p /c/Users/charlesmiller/Documents/winq-share
echo hello-host > /c/Users/charlesmiller/Documents/winq-share/hello.txt

EXTRA_QEMU_ARGS="-fsdev local,id=p9,path=C:\\Users\\charlesmiller\\Documents\\winq-share,security_model=mapped-xattr -device virtio-9p-pci,fsdev=p9,mount_tag=winqshare" \
  scripts-winq/test-vm.sh -- \
  "sudo mkdir -p /mnt/host && sudo mount -t 9p -o trans=virtio,version=9p2000.L winqshare /mnt/host && cat /mnt/host/hello.txt && echo from-guest | sudo tee /mnt/host/roundtrip.txt"
```

Expected: `hello-host` and `from-guest` echoed back, then on the host
`cat /c/Users/charlesmiller/Documents/winq-share/roundtrip.txt` shows
`from-guest`.

### 3. Host D3D11 Video infrastructure probe

Independent of the VM — just checks the host.

```bash
gcc -O2 -Wall -o scripts-winq/d3d11-video-probe.exe \
    scripts-winq/d3d11-video-probe.c -ld3d11 -ldxgi -ldxguid -luuid
./scripts-winq/d3d11-video-probe.exe | head -30
```

Expected (seen on Intel Arc B390 pre-reboot): "73 decoder profile(s)"
including **H.264 VLD, H.265 Main/Main10, VP9 Profile 0/2, AV1 Profile 0**
plus "CreateVideoDecoder succeeded — backend init path works."

### 4. VA-API in the guest — the critical test

This is the one blocked pre-reboot. Run in **video mode** (virtio-vga-gl
without venus; venus stays off because after a successful run here we
can separately try full mode with venus on):

```bash
MODE=video BOOT_TIMEOUT=90 scripts-winq/test-vm.sh -- \
  'vainfo 2>&1 | head -40'
```

**Expected change vs Alpha 5 baseline:** previously `vainfo` reported only
`VAProfileNone : VAEntrypointVideoProc`. Now it should additionally list:

```
VAProfileH264Baseline           :       VAEntrypointVLD
VAProfileH264Main               :       VAEntrypointVLD
VAProfileH264High               :       VAEntrypointVLD
...
```

### 5. Actual decode with ffmpeg

With the VM running (leave the previous `test-vm.sh` command running in
another shell OR re-run with the `sleep` pattern shown in test-vm.sh to
keep the VM alive):

```bash
ssh -p 2223 -i ~/.ssh/id_rsa charlesmiller@localhost '
  # Download a small test clip
  curl -sLo /tmp/bbb.mp4 https://download.blender.org/peach/bigbuckbunny_movies/BigBuckBunny_320x180.mp4
  # Force VA-API decode, discard output
  ffmpeg -hide_banner -hwaccel vaapi -hwaccel_output_format vaapi \
    -i /tmp/bbb.mp4 -f null - 2>&1 | head -20
'
```

**Expected**: ffmpeg log contains `[h264 @ 0x...] Format vaapi_vld chosen`
and decodes to EOF without software-fallback warnings.

### 6. Visual confirmation — the thing I can't programmatically verify

Inside the VM, open a GUI app and play a video. Recommended options:

```bash
# mpv with forced vaapi hwdec (most reliable for visual confirmation)
mpv --hwdec=vaapi --gpu-context=x11 /tmp/bbb.mp4

# or
vlc /tmp/bbb.mp4

# or Haruna / Brave — if they use vaapi they'll render the video on screen
```

**Visual check**: video renders on the SDL display, not just black frames.
A black video would mean decode succeeded but output buffer handoff
to the guest GL context didn't land — in which case the CPU readback
path (currently used on Windows) has a bug.

## Debug commands if something is off

Inside the guest:
```bash
# Did the virgl VA-API driver load?
vainfo 2>&1 | grep -i driver

# What's in the env?
env | grep -i libva

# Force driver selection if multiple are available
LIBVA_DRIVER_NAME=gallium-virgl vainfo
LIBVA_TRACE=/tmp/va.log vainfo  # detailed trace

# Detailed ffmpeg log
ffmpeg -loglevel debug -hwaccel vaapi -i bbb.mp4 -f null - 2>&1 | tail -100
```

On the host, QEMU's stderr is captured by `test-vm.sh` in `$LOG` (a
`/tmp/winq-test-XXXXXX.log` file — the path is printed if boot fails).
Look for `virgl_video:` prefixed messages from the Windows backend.

## What to do if step 4 fails

1. If `vainfo` still only shows `VAProfileNone`, the virgl_video host-side
   path isn't reaching the backend. Check:
   - QEMU is actually the Alpha 6 binary: `./build/qemu-system-x86_64.exe --version` should show a post-rebase build date.
   - `libvirglrenderer-1.dll` in `C:\msys64\ucrt64\bin\` is the
     Alpha-6-built one (~2.9 MB with the D3D11 backend).
   - `-DVIRGL_RENDERER_UNSTABLE_APIS` was set at build time (check
     `build/compile_commands.json` for `virtio-gpu-virgl.c`).

2. If `vainfo` lists H.264 profiles but `ffmpeg -hwaccel vaapi` fails,
   the init succeeded but decode submission is wrong. Debug with
   `LIBVA_TRACE` and compare to a native Linux vainfo+ffmpeg run.

3. If video plays but shows artifacts / wrong colors, the NV12 upload
   path (CPU readback → glTexSubImage2D) has a pitch/stride bug. Check
   that `GL_UNPACK_ROW_LENGTH` matches the D3D11 `MappedSubresource.RowPitch`.

## Rollback

Everything is on branch `winq-emu-alpha6` in each repo, tagged
`alpha6-checkpoint`. To revert to Alpha 5 cleanly:

```bash
cd /c/Users/charlesmiller/Development/claude/winq-emu-qemu        && git checkout winq-emu-alpha5
cd /c/Users/charlesmiller/Development/claude/winq-emu-virglrenderer && git checkout winq-emu-alpha1
cd /c/Users/charlesmiller/Development/claude/winq-emu-src          && git checkout main~1
# Rebuild virglrenderer + qemu
```
