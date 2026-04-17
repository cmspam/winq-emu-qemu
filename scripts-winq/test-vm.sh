#!/bin/bash
# WINQ-EMU test harness. Starts the freshly-built qemu against the CachyOS test
# VM, probes SSH with liveness-checking, runs optional test commands, and
# cleans up. Designed for non-interactive CI-ish use — fast crash detection,
# no long blind sleeps.
#
# Usage:
#   scripts-winq/test-vm.sh                      # just boot, exit 0 if SSH works
#   scripts-winq/test-vm.sh -- 'vainfo'          # run command over SSH
#   MODE=minimal scripts-winq/test-vm.sh -- '…'  # no venus/virtio-gl (for 9p iter)
#   MODE=full scripts-winq/test-vm.sh  -- '…'    # venus + SDL (for video tests)
#   EXTRA_QEMU_ARGS='-virtfs ...' scripts-winq/test-vm.sh -- 'mount ...'
#
# Env knobs:
#   MODE         minimal | full            (default: minimal)
#   QCOW2        path to disk image        (default: CachyOS path)
#   QEMU_BIN     path to qemu binary       (default: repo build/)
#   SSH_PORT     host port forward         (default: 2223)
#   BOOT_TIMEOUT seconds to wait for SSH   (default: 60)
#   SNAPSHOT     on | off — COW to tmp     (default: on, keeps image pristine)
set -u
export MSYS_NO_PATHCONV=1

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

MODE="${MODE:-minimal}"
QCOW2="${QCOW2:-C:/Users/charlesmiller/Documents/VMs/CachyOS/cachyos.qcow2}"
QEMU_BIN="${QEMU_BIN:-$REPO/build/qemu-system-x86_64.exe}"
SSH_PORT="${SSH_PORT:-2223}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
SNAPSHOT="${SNAPSHOT:-on}"
EXTRA_QEMU_ARGS="${EXTRA_QEMU_ARGS:-}"
SSH_KEY="${SSH_KEY:-/c/Users/charlesmiller/.ssh/id_rsa}"
SSH_USER="${SSH_USER:-charlesmiller}"

LOG="$(mktemp -t winq-test-XXXXXX.log)"

REMOTE_CMD=""
if [[ "${1:-}" == "--" ]]; then shift; REMOTE_CMD="$*"; fi

# Build mode-specific args.
GRAPHICS_ARGS=()
case "$MODE" in
  minimal)
    # Headless, no venus — doesn't trigger the virglrenderer/WHPX state leak.
    GRAPHICS_ARGS=(
      -display none
      -vga std
    )
    ;;
  full)
    GRAPHICS_ARGS=(
      -display sdl,gl=on
      -device 'virtio-vga-gl,blob=on,hostmem=4G,venus=on'
      -device virtio-sound-pci
      -usb -device usb-tablet
    )
    ;;
  video)
    # For VA-API testing: virgl 3D context (needed for the virgl_video path)
    # but WITHOUT venus to avoid the known alpha-5 state-leak after force-kill.
    GRAPHICS_ARGS=(
      -display sdl,gl=on
      -device 'virtio-vga-gl,blob=on,hostmem=4G'
      -usb -device usb-tablet
    )
    ;;
  *)
    echo "[test-vm] !! unknown MODE=$MODE" >&2; exit 2;;
esac

DRIVE_ARG="file=${QCOW2},format=qcow2,if=virtio"
[[ "$SNAPSHOT" == "on" ]] && DRIVE_ARG="${DRIVE_ARG},snapshot=on"

cleanup() {
  local code=$?
  if [[ -n "${QEMU_PID:-}" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
    taskkill //F //PID "$QEMU_PID" >/dev/null 2>&1 || kill -9 "$QEMU_PID" 2>/dev/null || true
  fi
  if [[ -f "$LOG" && $code -ne 0 ]]; then
    echo "---- qemu log (tail) ----" >&2
    tail -40 "$LOG" >&2
  fi
  rm -f "$LOG"
  exit "$code"
}
trap cleanup EXIT INT TERM

echo "[test-vm] mode=$MODE snapshot=$SNAPSHOT"
echo "[test-vm] qemu=$QEMU_BIN"

"$QEMU_BIN" \
  -machine q35,accel=whpx \
  -cpu host \
  -m 8G \
  -smp 8 \
  -drive "$DRIVE_ARG" \
  "${GRAPHICS_ARGS[@]}" \
  -device virtio-net-pci,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp::${SSH_PORT}-:22 \
  $EXTRA_QEMU_ARGS \
  >"$LOG" 2>&1 &
QEMU_PID=$!
echo "[test-vm] qemu pid=$QEMU_PID"

deadline=$(( $(date +%s) + BOOT_TIMEOUT ))
ssh_ready=0
while (( $(date +%s) < deadline )); do
  if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    echo "[test-vm] !! qemu died before SSH ready" >&2
    exit 2
  fi
  if ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
         -o ConnectTimeout=3 -o BatchMode=yes \
         -p "$SSH_PORT" -i "$SSH_KEY" "$SSH_USER@localhost" true 2>/dev/null; then
    ssh_ready=1; break
  fi
  sleep 2
done

if (( ssh_ready == 0 )); then
  echo "[test-vm] !! ssh never came up within ${BOOT_TIMEOUT}s" >&2
  exit 3
fi

echo "[test-vm] ssh ready"

RC=0
if [[ -n "$REMOTE_CMD" ]]; then
  echo "[test-vm] running: $REMOTE_CMD"
  ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
      -p "$SSH_PORT" -i "$SSH_KEY" "$SSH_USER@localhost" "$REMOTE_CMD" || RC=$?
fi

# Always clean-shutdown the guest so WHPX state is released cleanly.
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=3 -o BatchMode=yes \
    -p "$SSH_PORT" -i "$SSH_KEY" "$SSH_USER@localhost" \
    'sudo -n systemctl poweroff' >/dev/null 2>&1 &
# Give it 10s to power down; then cleanup forces kill.
for i in 1 2 3 4 5 6 7 8 9 10; do
  kill -0 "$QEMU_PID" 2>/dev/null || break
  sleep 1
done

exit $RC
