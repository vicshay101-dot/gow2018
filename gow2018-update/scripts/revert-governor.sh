#!/bin/bash
# ============================================================
# God of War (2018) Android Port — Revert Governor to Default
# Run via ADB shell or Shizuku (rish -c "bash revert-governor.sh")
# ============================================================

set -e

echo "=== Reverting CPU Governor ==="

for policy_dir in /sys/devices/system/cpu/cpufreq/policy*; do
    if [ -d "$policy_dir" ]; then
        policy=$(basename "$policy_dir")
        if echo "schedutil" > "$policy_dir/scaling_governor" 2>/dev/null; then
            echo "[$policy] → Reverted to: schedutil ✓"
        else
            echo "[$policy] → Failed to revert ✗"
        fi
    fi
done

# Re-enable thermal throttling
if [ -f /sys/class/thermal/thermal_zone0/mode ]; then
    echo "enabled" > /sys/class/thermal/thermal_zone0/mode 2>/dev/null && \
        echo "[thermal] Throttling re-enabled ✓" || \
        echo "[thermal] Cannot re-enable ✗"
fi

# Revert GPU governor
for gpu_gov in /sys/class/kgsl/kgsl-3d0/devfreq/governor \
               /sys/devices/platform/soc/*/devfreq/*/governor; do
    if [ -f "$gpu_gov" ]; then
        if echo "msm-adreno-tz" > "$gpu_gov" 2>/dev/null; then
            echo "[GPU] → Reverted to: msm-adreno-tz ✓"
        fi
        break
    fi
done

echo ""
echo "=== Revert Complete ==="
