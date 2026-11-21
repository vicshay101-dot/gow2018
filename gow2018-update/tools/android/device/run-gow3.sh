#!/bin/sh
# Launch script for the God of War (2018) [CUSA07408] & GoW III port on Android.
set -eu
files=${FXD_FILES:-/data/user/0/com.gow3.android/files}
host="$files/rootfs"
base="$files/gow3port"
arch="$base/arm64"
loader="$host/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
probe="$arch/bin/gow3-probe"
python="$host/usr/bin/python3"
export FXD_FILES="$files" FXD_ROOT="$host"
export PATH="$host/usr/bin:$host/bin:/system/bin"
export HOME="$files/home/gow3port"
export TMPDIR="$host/tmp" XDG_RUNTIME_DIR="$host/tmp"
export LD_LIBRARY_PATH="$arch/lib:$host/usr/lib/aarch64-linux-gnu:$host/usr/lib/aarch64-linux-gnu/pulseaudio:$host/usr/lib"

# Turnip selection
turnip=${GOW3_TURNIP:-}
if [ -z "$turnip" ]; then
    case $("$loader" "$arch/bin/kgsl-chip-id" 2>/dev/null || true) in
        0x44*) turnip=gen8 ;;
        *) turnip=main ;;
    esac
fi
icd="$host/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json"
[ "$turnip" = gen8 ] && [ -f "$host/usr/share/vulkan/icd.d/freedreno_gen8_icd.aarch64.json" ] &&
    icd="$host/usr/share/vulkan/icd.d/freedreno_gen8_icd.aarch64.json"
echo "Turnip: ${icd##*/}"
export VK_DRIVER_FILES="$icd"
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
export GLIBC_TUNABLES='glibc.cpu.hwcaps=-AVX2,-AVX,-AVX_Fast_Unaligned_Load,-AVX2_Usable,-AVX_Usable'

# Config and env overrides
export GOW3_CONFIG="$base/gow3.ini"
[ -f "$GOW3_CONFIG" ] || cp "$base/gow3.default.ini" "$GOW3_CONFIG"
[ -f "$base/gow3.env" ] && . "$base/gow3.env"

# Critical performance defaults for CUSA07408
export GOW3_READBACK_LINEAR=${GOW3_READBACK_LINEAR:-0}
export GOW3_FAULT_WINDOW=${GOW3_FAULT_WINDOW:-1024}
export GOW3_COPY_GPU_BUFFERS=${GOW3_COPY_GPU_BUFFERS:-1}
export GOW3_UFFD=${GOW3_UFFD:-1}
export GOW3_DEFERRED_READBACK=${GOW3_DEFERRED_READBACK:-0}
export GOW3_STALE_READBACK=${GOW3_STALE_READBACK:-0}
export GOW3_ASYNC_SHADERS=${GOW3_ASYNC_SHADERS:-1}
export GOW3_ASYNC_CAPTURE=${GOW3_ASYNC_CAPTURE:-1}
export GOW3_OBJECT_MOTION=${GOW3_OBJECT_MOTION:-0}
export GOW3_SHADOWS=${GOW3_SHADOWS:-0}
export GOW3_SCREEN_W=${GOW3_SCREEN_W:-960}
export GOW3_SCREEN_H=${GOW3_SCREEN_H:-540}
export GOW3_RENDER_RES=${GOW3_RENDER_RES:-540p}

export MESA_SHADER_CACHE_DIR="$HOME/.cache/mesa_shader_cache"
export MESA_SHADER_CACHE_MAX_SIZE=1G
mkdir -p "$MESA_SHADER_CACHE_DIR"

out="$base/out"
mkdir -p "$out"
game=${GOW3_GAME_DIR:-$base/game}
[ -f "$game/eboot.bin" ] || { echo "Game missing: $game (no eboot.bin)" >&2; exit 2; }

PYTHONHOME="$host/usr" "$loader" "$python" "$base/scripts/link_libc.py" "$game" --out "$out"
PYTHONHOME="$host/usr" "$loader" "$python" "$base/scripts/link_modules.py" "$game" --out "$out"
PYTHONHOME="$host/usr" "$loader" "$python" "$base/scripts/content_profile.py" "$game" --out "$out" --sku "${GOW3_CONTENT_SKU:-full}"

mem_kb=$(awk '/^MemTotal:/{print $2}' /proc/meminfo 2>/dev/null || true)
[ -n "$mem_kb" ] && [ "$mem_kb" -gt 0 ] && export GOW3_DEVICE_MEM_MB=${GOW3_DEVICE_MEM_MB:-$((mem_kb/1024))}

export GOW3_ANDROID_PORT=1
unset GOW3_ANDROID_FSR_FIXED_OUTPUT
export GOW3_UPSCALER=none
export GOW3_FSR1=0
export GOW3_RCAS=0

eval "$(PYTHONHOME="$host/usr" "$loader" "$python" "$base/scripts/launch_env.py" --game "$game" --config "$GOW3_CONFIG" --state "$out/graphics_state.json" --shell)"

echo "GOW3_NATIVE_RESOLUTION: status=${GOW3_RESOLUTION_STATUS:-unknown} patch=${GOW3_PATCH_INPUT_RES:-standard}"
echo "Game language: system_param=${GOW3_LANGUAGE:-1} (1=English, 17=Portuguese-Brazil)"

export GOW3_DMEM_MB=${GOW3_DMEM_MB:-0}; [ "${GOW3_DMEM_MB:-0}" -ge 5056 ] 2>/dev/null || export GOW3_DMEM_MB=7104
export GOW3_VBLANK_HZ=${GOW3_VBLANK_HZ:-0}; [ "${GOW3_VBLANK_HZ:-0}" -ge 1 ] 2>/dev/null || export GOW3_VBLANK_HZ=60
echo "Texture integrity: title=${GOW3_GAME_TITLE:-unknown} version=${GOW3_GAME_VERSION:-unknown} patch=${GOW3_PATCH_STATUS:-unknown} shadows=${GOW3_SHADOWS_REQUESTED:-unknown}(${GOW3_SHADOWS_STATUS:-unknown}) readback_linear=${GOW3_READBACK_LINEAR:-0}(${GOW3_READBACK_LINEAR_SOURCE:-unknown}) deferred=${GOW3_DEFERRED_READBACK:-0} stale=${GOW3_STALE_READBACK:-0} dmem=${GOW3_DMEM_MB}MiB vblank=${GOW3_VBLANK_HZ}Hz turnip=${turnip}"

profile_file="$out/texture-profile.json"
profile_supported=false
[ "${GOW3_PATCH_STATUS:-}" = validated ] || [ "${GOW3_PATCH_STATUS:-}" = gow2018-fork-active ] && profile_supported=true
printf '%s\n' "{\"title_id\":\"${GOW3_GAME_TITLE:-unknown}\",\"app_version\":\"${GOW3_GAME_VERSION:-unknown}\",\"eboot_sha256\":\"${GOW3_EBOOT_SHA256:-unavailable}\",\"supported\":${profile_supported},\"reason\":\"${GOW3_PATCH_STATUS:-unknown}\"}" > "$profile_file"
PYTHONHOME="$host/usr" "$loader" "$python" "$base/scripts/texture_diagnostics.py" --out "$out/texture_integrity.json" --profile "$profile_file" --turnip "${turnip}" --dmem "${GOW3_DMEM_MB}" --vblank "${GOW3_VBLANK_HZ}" --vulkan-status "ok"
PYTHONHOME="$host/usr" "$loader" "$python" "$base/scripts/patches.py" --out "$out" --extra "${GOW3_PATCHES:-}" \
    --game-dir "$game" --patches-dir "${GOW3_PATCHES_DIR:-$base/patches}" \
    --patches-config "${GOW3_PATCHES_CONFIG:-$base/patches.json}"

cd "$base"
game_name="God of War (2018) [CUSA07408]"
[ "${GOW3_GAME_TITLE:-}" = "CUSA01715" ] && game_name="God of War III"
echo "Bionic: starting $game_name (DMEM ${GOW3_DMEM_MB} MiB, vblank ${GOW3_VBLANK_HZ} Hz)"
"$loader" "$probe" "$out/boot-linked.bin" --content-profile "$out/content.bin" --patches "$out/patches.bin" \
 --app0 "$game" --user "$base/user" --timeout "${GOW3_TIMEOUT:-0}"
