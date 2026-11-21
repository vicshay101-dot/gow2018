#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared launch environment for run.py (Windows) and run.sh (Linux).
Extended for God of War (2018) [CUSA07408] Android Port Fork.

One source for the graphics options, the selected patches and the direct memory (DMEM) /
VBlank those patches need.
"""
import os
import sys
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent


def disable_shadows_requested(ini):
    """New positive-toggle setting wins; legacy inverse toggle remains readable."""
    if 'disable_dynamic_shadows' in ini:
        return ini['disable_dynamic_shadows'] == '1'
    return ini.get('effect_dynamic_shadows', '1') == '0'


def derive(game, config, state_path, env):
    """Plan one launch.

    game:       the game folder (eboot.bin)
    config:     gow3.ini path
    state_path: graphics_state.json path (restart confirmation)
    env:        the current environment (GOW3_PATCHES, GOW3_PATCHES_ONLY, explicit overrides)
    """
    sys.path.insert(0, str(PORT / 'scripts'))
    import graphics_options
    try:
        from patches import game_profile, game_title_id, patch_profile_status, patch_requirements, selected_patches
    except ImportError:
        def game_profile(title): return None
        def game_title_id(g): return 'CUSA07408'
        def patch_profile_status(g): return {'title_id': 'CUSA07408', 'app_version': '01.00', 'reason': 'validated'}
        def patch_requirements(*a, **k): return None, None
        def selected_patches(*a, **k): return []

    old_ini = graphics_options.read_ini(config)
    previous = old_ini.get('render_resolution')
    if previous is not None:
        canonical = graphics_options.canonical_resolution(previous)
        if canonical != previous:
            graphics_options.write_keys(config, {'render_resolution': canonical})
    graphics, reverted = graphics_options.prepare(config, state_path)
    android_port = env.get('GOW3_ANDROID_PORT') == '1'
    ini = graphics_options.read_ini(config)

    if android_port and 'frames_queued' not in ini:
        graphics_options.write_keys(config, {'frames_queued': '2'})
        ini = graphics_options.read_ini(config)
    if android_port and ini.get('upscaler') != 'off':
        graphics_options.write_keys(config, {'upscaler': 'off', 'preset': '0',
                                             'fsr1': '0', 'rcas': '0'})
        ini = graphics_options.read_ini(config)

    updates = dict(graphics_options.environment(graphics))
    if android_port:
        updates['GOW3_UPSCALER'] = 'none'
        updates['GOW3_FSR1'] = '0'
        updates['GOW3_RCAS'] = '0'
        updates['GOW3_UPSCALER_SOURCE'] = 'disabled-android'
        updates['GOW3_RESOLUTION_STATUS'] = 'native-game-patch'
    elif 'GOW3_UPSCALER' not in env:
        updates['GOW3_UPSCALER'] = 'none'
        updates['GOW3_UPSCALER_SOURCE'] = 'desktop-safe-default'

    # Honor READBACK_LINEAR override or default to 0 on Android for CUSA07408
    updates['GOW3_READBACK_LINEAR'] = env.get('GOW3_READBACK_LINEAR', '0' if android_port else '1')
    updates['GOW3_READBACK_LINEAR_SOURCE'] = (
        'override' if 'GOW3_READBACK_LINEAR' in env else 'android-fork-optimized')

    identity_check = patch_profile_status(game)
    title_id = identity_check.get('title_id') or env.get('GOW3_GAME_TITLE', 'unknown')
    is_gow2018 = (title_id == 'CUSA07408' or 'CUSA07408' in str(game))
    resolution = graphics.get('render_resolution', 'native')

    # Allow custom direct resolutions for CUSA07408 (GoW 2018 fork support).
    # GoW 2018 divides render targets by 8 (depthBy8/flagsby8/bloom8th), so map
    # presets to exact 16:9 8-aligned dimensions patched natively in eboot.elf.
    gow2018_resolutions = {
        '240p': '512x288',
        '360p': '640x360',
        '424p': '768x432',
        '480p': '896x504',
        '540p': '1024x576',
        '720p': '1024x576',
    }
    if android_port and resolution in graphics_options.DIRECT_RESOLUTIONS:
        if is_gow2018:
            pixel_res = gow2018_resolutions.get(resolution, '1024x576')
            updates['GOW3_PATCH_INPUT_RES'] = pixel_res
            updates['GOW3_RESOLUTION_STATUS'] = 'gow2018-native-eboot-patch'
            w, h = pixel_res.split('x')
            updates['GOW3_SCREEN_W'] = w
            updates['GOW3_SCREEN_H'] = h
        else:
            try:
                from patches import CUSA01715_0102_ANDROID_SHA
                if (identity_check['title_id'] == 'CUSA01715' and
                    identity_check['app_version'] == '01.02' and
                    identity_check['eboot_sha256'] == CUSA01715_0102_ANDROID_SHA and
                    identity_check['reason'] == 'validated'):
                    updates['GOW3_PATCH_INPUT_RES'] = graphics_options.DIRECT_PIXEL_RESOLUTIONS[resolution]
                    updates['GOW3_RESOLUTION_STATUS'] = 'verified-direct-game-patch'
                else:
                    graphics['render_resolution'] = 'native'
                    updates['GOW3_RESOLUTION_STATUS'] = 'unsupported-eboot-native-fallback'
            except Exception:
                graphics['render_resolution'] = 'native'
                updates['GOW3_RESOLUTION_STATUS'] = 'unsupported-eboot-native-fallback'

    names, dmem, vblank = [], None, None
    shadow_requested = disable_shadows_requested(ini) or (env.get('GOW3_SHADOWS') == '0')
    updates['GOW3_SHADOWS_REQUESTED'] = 'off' if shadow_requested else 'on'

    if is_gow2018:
        # CUSA07408 fork path
        shadows_enabled = (env.get('GOW3_SHADOWS') == '1')
        updates['GOW3_SHADOWS_STATUS'] = 'enabled' if shadows_enabled else 'disabled-by-fork'
        updates['GOW3_SHADOWS'] = '1' if shadows_enabled else '0'
        updates['GOW3_SHADOWS_REQUESTED'] = 'on' if shadows_enabled else 'off'
        updates['GOW3_GAME_TITLE'] = 'CUSA07408'
        updates['GOW3_GAME_VERSION'] = identity_check.get('app_version') or '01.00'
        updates['GOW3_PATCH_STATUS'] = 'gow2018-fork-active'
        updates['GOW3_RESOLUTION_STATUS'] = 'gow2018-native-eboot-patch'
        updates['GOW3_READBACKS'] = env.get('GOW3_READBACKS', '0')
        updates['GOW3_DEFERRED_READBACK'] = '0'
        updates['GOW3_STALE_READBACK'] = '0'
        updates['GOW3_READBACK_LINEAR'] = '0'
        pixel_res = env.get('GOW3_PATCH_INPUT_RES') or updates.get('GOW3_PATCH_INPUT_RES') or '1024x576'
        w, h = pixel_res.split('x') if 'x' in pixel_res else ('1024', '576')
        updates['GOW3_PATCH_INPUT_RES'] = pixel_res
        updates['GOW3_SCREEN_W'] = w
        updates['GOW3_SCREEN_H'] = h
        updates['GOW3_HOT_PAGES'] = env.get('GOW3_HOT_PAGES', '1')
        updates['GOW3_FAULT_WINDOW'] = '1024'
        updates['GOW3_COPY_GPU_BUFFERS'] = '1'
        updates['GOW3_ASYNC_SHADERS'] = '1'
        updates['GOW3_ASYNC_CAPTURE'] = '1'
        reason = 'gow2018-fork-active'
        dmem = 7104
        vblank = 60
    else:
        identity = identity_check
        profile = game_profile(identity['title_id'])
        reason = identity['reason'] or 'unknown'
        if identity['reason'] != 'validated':
            if profile is None or identity['eboot_matches'] is False:
                updates['GOW3_PATCH_STATUS'] = identity['reason']
        if profile and identity['reason'] == 'validated':
            try:
                names = selected_patches(profile[1], profile[2], env.get('GOW3_PATCHES', ''),
                                         env.get('GOW3_PATCHES_ONLY') == '1',
                                         startup_black_screen=graphics['patch_startup_black_screen'] != '0')
            except ValueError:
                names, reason = [], 'patch-conflict'
            if names:
                names = graphics_options.patch_names(names, graphics)
                shadow_reference = '6e72330e9092e5164049b865e5568f2d450904001a73fe0a2f4b39194c195b3c'
                if shadow_requested and identity.get('eboot_sha256') == shadow_reference:
                    names.append('Disable Dynamic Shadows')
                updates['GOW3_SHADOWS_STATUS'] = ('disabled-patch-selected'
                    if 'Disable Dynamic Shadows' in names else
                    'unsupported-eboot' if shadow_requested else 'enabled')
                expand_arena = graphics['expand_video_arena'] != '0'
                dmem, vblank = patch_requirements(profile[1], names, profile[2],
                                                  expand_video_arena=expand_arena)
        updates.update({'GOW3_GAME_TITLE': identity.get('title_id') or 'unknown',
                        'GOW3_GAME_VERSION': identity.get('app_version') or 'unknown',
                        'GOW3_EBOOT_SHA256': identity.get('eboot_sha256') or 'unavailable',
                        'GOW3_PATCH_STATUS': reason})

    return {'updates': updates, 'patch_names': names, 'dmem': dmem, 'vblank': vblank,
            'reverted': reverted, 'identity': identity_check}


def _quote(value):
    return "'" + str(value).replace("'", "'\\''") + "'"


def main(argv):
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--game', required=True, type=Path)
    p.add_argument('--config', required=True, type=Path)
    p.add_argument('--state', required=True, type=Path)
    p.add_argument('--shell', action='store_true',
                   help='print export KEY=VALUE lines (for run.sh) instead of JSON')
    a = p.parse_args(argv)
    plan = derive(a.game, a.config, a.state, dict(os.environ))
    if not a.shell:
        import json
        print(json.dumps(plan))
        return 0
    exports = dict(plan['updates'])
    if plan['patch_names'] or plan['updates'].get('GOW3_PATCH_STATUS') in ('validated', 'gow2018-fork-active'):
        if plan['patch_names']:
            exports['GOW3_PATCHES'] = ';'.join(plan['patch_names'])
            exports['GOW3_PATCHES_ONLY'] = '1'
    if plan['dmem']:
        exports['GOW3_DMEM_MB'] = str(plan['dmem'])
    if plan['vblank']:
        exports['GOW3_VBLANK_HZ'] = str(plan['vblank'])
    for key, value in exports.items():
        print(f'export {key}={_quote(str(value))}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
