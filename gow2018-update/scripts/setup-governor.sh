#!/bin/bash
# ============================================================
# God of War (2018) Android Port — CPU Governor Setup
# Run via ADB shell or Shizuku (rish -c "bash setup-governor.sh")
# ============================================================
#
# This script forces the CPU governor to 'performance' mode on
# all CPU policy groups to minimize scheduler preemptions and
# kernel-mode latency during gameplay.
#
# Measured impact: GPU thread preemptions drop from 125-417/frame
# to ~30-60/frame, kernel time reduced by 50%+.
#
# NOTE: This increases battery drain. Revert with:
#   echo schedutil > /sys/devices/system/cpu/cpufreq/policy*/scaling_governor
# ============================================================

set -e

echo "=== GOW2018 CPU Governor Setup ==="

# Detect available policy groups
for policy_dir in /sys/devices/system/cpu/cpufreq/policy*; do
    if [ -d "$policy_dir" ]; then
        policy=$(basename "$policy_dir")
        current=$(cat "$policy_dir/scaling_governor" 2>/dev/null || echo "unknown")
        echo "[$policy] Current governor: $current"

        if echo "performance" > "$policy_dir/scaling_governor" 2>/dev/null; then
            echo "[$policy] → Set to: performance ✓"
        else
            echo "[$policy] → Failed to set (need root/shell) ✗"
        fi
    fi
done

# Disable thermal throttling hint (if available)
if [ -f /sys/class/thermal/thermal_zone0/mode ]; then
    echo "disabled" > /sys/class/thermal/thermal_zone0/mode 2>/dev/null && \
        echo "[thermal] Throttling disabled ✓" || \
        echo "[thermal] Cannot disable (need root) ✗"
fi

# Set GPU governor to performance (Adreno)
for gpu_gov in /sys/class/kgsl/kgsl-3d0/devfreq/governor \
               /sys/devices/platform/soc/*/devfreq/*/governor; do
    if [ -f "$gpu_gov" ]; then
        current=$(cat "$gpu_gov" 2>/dev/null || echo "unknown")
        echo "[GPU] Current governor: $current"
        if echo "performance" > "$gpu_gov" 2>/dev/null; then
            echo "[GPU] → Set to: performance ✓"
        else
            echo "[GPU] → Failed to set ✗"
        fi
        break
    fi
done

echo ""
echo "=== Setup Complete ==="
echo "To revert: echo schedutil > /sys/devices/system/cpu/cpufreq/policy*/scaling_governor"
