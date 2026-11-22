#!/system/bin/sh
# Experimental Android/Bionic runtime. No Debian, X server or Linux loader.
# Fork: God of War (2018) [CUSA07408] & God of War III [CUSA01715] Performance Optimization
set -eu
files=${FXD_FILES:?FXD_FILES must identify the app private files directory}
base="$files/gow3port"
runtime="$base/arm64"
python="$runtime/python/bin/python3.13"
export HOME="$files/home/gow3port" TMPDIR="$base/tmp" XDG_RUNTIME_DIR="$base/tmp"
export PATH=/system/bin
export LD_LIBRARY_PATH="$runtime/lib:$runtime/python/lib"
export PYTHONHOME="$runtime/python" PYTHONPATH="$base/scripts" PYTHONUTF8=1
export FEX_APP_CONFIG_LOCATION="$HOME/.fex-emu/" FEX_APP_DATA_LOCATION="$HOME/.fex-emu/" FEX_APP_CACHE_LOCATION="$HOME/.cache/fex-emu/"
export GOW3_BIONIC_RUNNER="$base/run-bionic.sh"
export GOW3_CONFIG="$base/gow3.ini"
mkdir -p "$TMPDIR" "$HOME/.fex-emu" "$HOME/.cache/fex-emu" "$base/user" "$base/logs"

# Ensure gow3.ini exists or copy optimized default
[ -f "$GOW3_CONFIG" ] || cp "$base/gow3.default.ini" "$GOW3_CONFIG"
setting() { sed -n "s/^$1=//p" "$GOW3_CONFIG" | tail -n 1; }

# Source gow3.env overrides if present (critical for custom tunings in fork)
[ -f "$base/gow3.env" ] && . "$base/gow3.env"
# User-editable overrides (no rebuild needed), e.g. `export TU_DEBUG=sysmem`
[ -r /storage/emulated/0/Download/Logs/gow3.env ] && . /storage/emulated/0/Download/Logs/gow3.env
echo "User env: TU_DEBUG=${TU_DEBUG:-} FD_MESA_DEBUG=${FD_MESA_DEBUG:-}"

chip=$("$runtime/bin/kgsl-chip-id" 2>/dev/null || true)
turnip=${GOW3_BIONIC_DRIVER:-auto}
case "$turnip" in
 auto) case "$chip" in 0x44??????) turnip=gen8 ;; 0x07??????|0x43020[01]00|0x43030b00|0x43050a01|0x43050b00|0x43051401) turnip=adreno7xx ;; *) turnip=system ;; esac ;;
 gen8|adreno7xx|system|custom) ;;
 *) echo "Invalid Bionic driver selection: $turnip" >&2; exit 2 ;;
esac
unset VK_DRIVER_FILES VK_ICD_FILENAMES VK_LAYER_PATH GOW3_LSFG LSFG_CONFIG GOW3_LSFG_STATE_FILE GOW3_LSFG_LIB
if [ "$turnip" = gen8 ] || [ "$turnip" = adreno7xx ]; then
 export GOW3_VULKAN_DRIVER="$runtime/drivers/$turnip/libvulkan_freedreno.so"
 [ -f "$GOW3_VULKAN_DRIVER" ] || { echo "Bundled $turnip driver is missing" >&2; exit 2; }
elif [ "$turnip" = custom ]; then
 custom=${GOW3_CUSTOM_VULKAN_DRIVER:-}
 case "$custom" in "$files"/gpu-drivers/*/*.so) ;; *) echo 'Invalid custom driver path' >&2; exit 2 ;; esac
 [ -f "$custom" ] || { echo 'Selected custom driver is missing; select a driver on Home' >&2; exit 2; }
 export GOW3_VULKAN_DRIVER="$custom"
 # Companion libraries supplied in the package resolve beside the selected driver.
 export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:${custom%/*}"
else unset GOW3_VULKAN_DRIVER; fi
echo "Bionic Vulkan selection: $turnip${GOW3_VULKAN_DRIVER:+ ($GOW3_VULKAN_DRIVER)}"

# Android input & bridge pipes
export GOW3_ANDROID_INPUT=1 GOW3_PAD_FILE="$base/android-pad.state" GOW3_PAD_QUIET=1
export GOW3_IME_FILE="$base/ime.request" GOW3_AUDIO_FIFO="$base/audio.fifo"
export GOW3_PRESENT_SOCKET="$base/present.sock"

# ==============================================================================
# CIRÚRGICO: CORREÇÃO DOS PRINCIPAIS GARGALOS DE FPS PARA CUSA07408 (GOW 2018)
# ==============================================================================
# 1. READBACK_LINEAR: Desativa downloads síncronos de texturas lineares CPU<-GPU.
#    Antes forçava 1, gastando 150-200ms de host copies / readbacks a cada frame!
export GOW3_READBACK_LINEAR=${GOW3_READBACK_LINEAR:-0}

# 2. FAULT_WINDOW: Aumenta a janela de dirty tracking de 4 KiB para 1024 KiB (1 MiB).
#    Antes com 4 KiB gerava 46.000+ page faults e centenas de mprotect por frame!
#    Com 1024 KiB, uma única falta resolve até 256 páginas, cortando write faults em 80-90%.
export GOW3_FAULT_WINDOW=${GOW3_FAULT_WINDOW:-1024}

# 3. Pipelines de cópia e uploads de buffer
export GOW3_COPY_GPU_BUFFERS=${GOW3_COPY_GPU_BUFFERS:-1}
export GOW3_UFFD=${GOW3_UFFD:-1}
export GOW3_DEFERRED_READBACK=${GOW3_DEFERRED_READBACK:-0}
export GOW3_STALE_READBACK=${GOW3_STALE_READBACK:-0}
export GOW3_ASYNC_SHADERS=${GOW3_ASYNC_SHADERS:-1}
export GOW3_ASYNC_CAPTURE=${GOW3_ASYNC_CAPTURE:-1}
export GOW3_OBJECT_MOTION=${GOW3_OBJECT_MOTION:-0}
export GOW3_SHADOWS=${GOW3_SHADOWS:-0}

# 4. Cache persistente de shaders Mesa Turnip no disco (evita re-compilar 100+ shaders no load)
export MESA_SHADER_CACHE_DIR="$HOME/.cache/mesa_shader_cache"
export MESA_SHADER_CACHE_MAX_SIZE=1G
mkdir -p "$MESA_SHADER_CACHE_DIR"

# 5. Resolução nativa 1024x576 (16:9 exato, múltiplo de 64, 3.5x menos pixels que 1080p) via patch de eboot
#    Sem SceneTargets proxy (quebrava Z-prepass -> tela azul) e sem stream cached (piorava host copies no Turnip)
unset GOW3_RENDER_RES GOW3_SCENE_SCALE GOW3_STREAM_MEMORY
export GOW3_READBACKS=${GOW3_READBACKS:-1}
export GOW3_PREP_WORKERS=${GOW3_PREP_WORKERS:-2}
export GOW3_PERF_DIAG=${GOW3_PERF_DIAG:-1}

# Performance report every 5 s in the log
if [ -e /sdcard/Download/gow2018_no_frame_stats ]; then unset GOW3_FRAME_STATS
else export GOW3_FRAME_STATS=${GOW3_FRAME_STATS:-1}; fi

probe="$runtime/bin/gow3-probe"
case ${1:-game} in vulkan) exec "$probe" --vulkan-only ;; esac
out="$base/out"
mkdir -p "$out"

# GOW3_GAME_DIR: the game folder
game=${GOW3_GAME_DIR:-$base/game}
[ -f "$game/eboot.bin" ] || { echo "Game missing: $game (no eboot.bin)" >&2; exit 2; }

# GOW3_PATCH_DIR: update overlay
update_view=
if [ -n "${GOW3_PATCH_DIR:-}" ]; then
    [ -f "$GOW3_PATCH_DIR/eboot.bin" ] || { echo "Update missing: $GOW3_PATCH_DIR (no eboot.bin)" >&2; exit 2; }
    if ! update_view=$("$python" "$base/scripts/game_update.py" view "$game" "$GOW3_PATCH_DIR" "$out"); then
        echo "Game update: cannot combine the game and its update (see above)" >&2
        exit 2
    fi
    game=$update_view
    echo "Game and update combined from $GOW3_PATCH_DIR" >&2
    trap 'if [ -n "${update_view:-}" ]; then "$python" "$base/scripts/game_update.py" remove "$update_view" 2>/dev/null || true; fi' EXIT
fi

vulkan_status=ok
"$probe" --vulkan-only || { echo 'Bionic Vulkan driver initialization failed' >&2; exit 4; }
"$runtime/bin/gow3-prepare" "$game" "$out"

# Direct memory / VBlank
mem_kb=$(awk '/^MemTotal:/{print $2}' /proc/meminfo 2>/dev/null || true)
[ -n "$mem_kb" ] && [ "$mem_kb" -gt 0 ] && export GOW3_DEVICE_MEM_MB=${GOW3_DEVICE_MEM_MB:-$((mem_kb/1024))}

export GOW3_ANDROID_PORT=1
unset GOW3_ANDROID_FSR_FIXED_OUTPUT
export GOW3_UPSCALER=none
export GOW3_FSR1=0
export GOW3_RCAS=0

# Execute launch environment calculation
eval "$("$python" "$base/scripts/launch_env.py" --game "$game" --config "$GOW3_CONFIG" --state "$out/graphics_state.json" --shell)"

# Explicit post-eval guarantee for God of War 2018 (CUSA07408)
if [ "${GOW3_GAME_TITLE:-}" = "CUSA07408" ] || [ "${GOW3_PATCH_STATUS:-}" = "gow2018-fork-active" ]; then
    unset GOW3_RENDER_RES GOW3_SCENE_SCALE GOW3_STREAM_MEMORY
    export GOW3_GOW2018_EBOOT_RES=${GOW3_GOW2018_EBOOT_RES:-1}
    export GOW3_PATCH_INPUT_RES=${GOW3_PATCH_INPUT_RES:-640x360}
    export GOW3_SCREEN_W=${GOW3_SCREEN_W:-640}
    export GOW3_SCREEN_H=${GOW3_SCREEN_H:-360}
    export GOW3_READBACKS=1
    export GOW3_ASYNC_BUFFER_READBACK=${GOW3_ASYNC_BUFFER_READBACK:-1}
    export GOW3_ADPF=${GOW3_ADPF:-1}
    export GOW3_DRAW_PIPE=${GOW3_DRAW_PIPE:-0}
    export GOW3_COALESCE_COMPUTE_BARRIERS=${GOW3_COALESCE_COMPUTE_BARRIERS:-0}
    export GOW3_READBACK_LINEAR=0
    export GOW3_DEFERRED_READBACK=0
    export GOW3_STALE_READBACK=0
    export GOW3_HOT_PAGES=0
    export GOW3_PREP_WORKERS=${GOW3_PREP_WORKERS:-2}
    export GOW3_PERF_DIAG=${GOW3_PERF_DIAG:-1}
    export GOW3_FAULT_WINDOW=1024
    export GOW3_COPY_GPU_BUFFERS=1
    export GOW3_ASYNC_SHADERS=1
    export GOW3_ASYNC_CAPTURE=1
    export GOW3_SHADOWS=0
fi

echo "GOW3_NATIVE_RESOLUTION: status=${GOW3_RESOLUTION_STATUS:-unknown} eboot_res=${GOW3_GOW2018_EBOOT_RES:-0}(${GOW3_PATCH_INPUT_RES:-native}) screen=${GOW3_SCREEN_W:-0}x${GOW3_SCREEN_H:-0} readbacks=${GOW3_READBACKS:-1} async_buf=${GOW3_ASYNC_BUFFER_READBACK:-0} draw_pipe=${GOW3_DRAW_PIPE:-0} cs_coalesce=${GOW3_COALESCE_COMPUTE_BARRIERS:-0} hot_pages=${GOW3_HOT_PAGES:-0} prep=${GOW3_PREP_WORKERS:-0}"
echo "Game language: system_param=${GOW3_LANGUAGE:-1} (1=English, 7=Portuguese, 17=Portuguese-Brazil)"

# Memory pool & refresh
export GOW3_DMEM_MB=${GOW3_DMEM_MB:-0}; [ "${GOW3_DMEM_MB:-0}" -ge 5056 ] 2>/dev/null || export GOW3_DMEM_MB=7104
export GOW3_VBLANK_HZ=${GOW3_VBLANK_HZ:-0}; [ "${GOW3_VBLANK_HZ:-0}" -ge 1 ] 2>/dev/null || export GOW3_VBLANK_HZ=60
echo "Texture integrity: title=${GOW3_GAME_TITLE:-unknown} version=${GOW3_GAME_VERSION:-unknown} patch=${GOW3_PATCH_STATUS:-unknown} shadows=${GOW3_SHADOWS_REQUESTED:-unknown}(${GOW3_SHADOWS_STATUS:-unknown}) readback_linear=${GOW3_READBACK_LINEAR:-0}(${GOW3_READBACK_LINEAR_SOURCE:-unknown}) deferred=${GOW3_DEFERRED_READBACK:-0} stale=${GOW3_STALE_READBACK:-0} dmem=${GOW3_DMEM_MB}MiB vblank=${GOW3_VBLANK_HZ}Hz turnip=${turnip}"

profile_file="$out/texture-profile.json"
profile_supported=false
[ "${GOW3_PATCH_STATUS:-}" = validated ] || [ "${GOW3_PATCH_STATUS:-}" = gow2018-fork-active ] && profile_supported=true
printf '%s\n' "{\"title_id\":\"${GOW3_GAME_TITLE:-unknown}\",\"app_version\":\"${GOW3_GAME_VERSION:-unknown}\",\"eboot_sha256\":\"${GOW3_EBOOT_SHA256:-unavailable}\",\"supported\":${profile_supported},\"reason\":\"${GOW3_PATCH_STATUS:-unknown}\"}" > "$profile_file"
"$python" "$base/scripts/texture_diagnostics.py" --out "$out/texture_integrity.json" --profile "$profile_file" --turnip "${turnip}" --dmem "${GOW3_DMEM_MB}" --vblank "${GOW3_VBLANK_HZ}" --vulkan-status "${vulkan_status}"
"$python" "$base/scripts/patches.py" --out "$out" --extra "${GOW3_PATCHES:-}" \
    --game-dir "$game" --patches-dir "${GOW3_PATCHES_DIR:-$base/patches}" \
    --patches-config "${GOW3_PATCHES_CONFIG:-$base/patches.json}"

cd "$base"
game_name="God of War (2018) [CUSA07408]"
[ "${GOW3_GAME_TITLE:-}" = "CUSA01715" ] && game_name="God of War III"
echo "Bionic: driver=$turnip chip=$chip; starting $game_name"
"$probe" "$out/boot-linked.bin" --content-profile "$out/content.bin" --patches "$out/patches.bin" \
 --app0 "$game" --user "$base/user" --timeout "${GOW3_TIMEOUT:-0}"
