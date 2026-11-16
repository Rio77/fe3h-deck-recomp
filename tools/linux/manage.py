#!/usr/bin/env python3
"""Local Linux installer/launcher. No cloud account or game data is distributed.
SPDX-License-Identifier: MIT
"""
from pathlib import Path
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from install_core import Installation, InstallError, atomic_json, digest
from game_files import validate_exefs, validate_romfs, validate_dlc, files_in, verify_copy
from processes import stop_session

HERE = Path(__file__).resolve().parent
DEFAULT_ROOT = Path(os.environ.get('XDG_DATA_HOME', str(Path.home() / '.local/share'))) / 'fe3hrecomp'


def dialog(kind, text, *args):
    if not shutil.which('kdialog'):
        raise InstallError('kdialog is required for graphical installation. Use the documented command-line options instead.')
    p = subprocess.run(['kdialog', '--title', 'FE3H Linux', kind, text, *args], text=True, capture_output=True)
    return p.returncode, p.stdout.strip()


def pick(label):
    code, text = dialog('--getexistingdirectory', str(Path.home()), '--title', label)
    if code or not text:
        raise InstallError('Installation cancelled. The existing installation was preserved.')
    return Path(text)


def environment(root, profile=True):
    env = dict(os.environ)
    for key in list(env):
        if key.startswith(('LARECOMP_', 'FE3H_', 'MANGOHUD')):
            env.pop(key)
    env.pop('LD_PRELOAD', None)
    env['LARECOMP_DATA_ROOT'] = str(root)
    env['APPIMAGE_EXTRACT_AND_RUN'] = '1'
    product = Path('/sys/class/dmi/id/product_name')
    deck = product.exists() and product.read_text().strip() in ('Jupiter', 'Galileo')
    if profile and deck:
        env.update(FE3H_HANDHELD='0', FE3H_MANAGED_HEAP='1', FE3H_NATIVE_HALF='1',
                   FE3H_SPARSE_LOCAL='1', FE3H_TIMED_SYNC='1', FE3H_DOCK_TARGETS='1',
                   FE3H_COMPUTE_SCF='1', FE3H_LIGHTING_SCF='1', FE3H_EFFECT_SCALE_FIX='0',
                   FE3H_LIMITER_MARGIN_US='1000', FE3H_WARM_PROFILE='1', SDL_AUDIO_DRIVER='pulseaudio')
    return env


def assert_game_closed(root):
    for p in Path('/proc').glob('[0-9]*/comm'):
        try:
            if p.read_text().strip() == 'fe3hrecomp' and (p.parent / 'cwd').resolve() == root:
                raise InstallError('Close FE3H before changing this installation.')
        except (OSError, PermissionError):
            pass


def app_for(root):
    manifest = json.loads((root / 'current.json').read_text(encoding='utf-8'))
    sha = manifest['app_sha256']
    import re
    if not re.fullmatch('[0-9a-f]{64}', sha):
        raise InstallError('Invalid application manifest.')
    app = root / 'apps' / (sha + '.AppImage')
    if app.is_symlink() or digest(app) != sha:
        raise InstallError('Application integrity check failed. Run the installer again.')
    return app


def verify(app, root, output):
    with Path(output).open('w') as log:
        p = subprocess.run([str(app), '--verify-game'], cwd=root, env=environment(root, False),
                           stdout=log, stderr=subprocess.STDOUT, timeout=120)
    if p.returncode:
        raise InstallError(f'The compiled game could not be verified. See {output}')


def desktop_files(root, make_desktop=True):
    for name in ('manage.py', 'install_core.py', 'game_files.py', 'processes.py'):
        source = HERE / name
        target = root / name
        if source.resolve() != target.resolve():
            temporary = target.with_suffix('.tmp')
            shutil.copy2(source, temporary)
            os.replace(temporary, target)
    launcher = root / 'FE3H.sh'
    launcher.write_text('#!/bin/sh\nHERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexec python3 "$HERE/manage.py" launch --root "$HERE"\n', encoding='utf-8')
    launcher.chmod(0o755)
    icon = HERE / 'icon.png'
    if icon.exists():
        shutil.copy2(icon, root / 'icon.png')
    if not make_desktop:
        return
    desktop = subprocess.check_output(['xdg-user-dir', 'DESKTOP'], text=True).strip()
    dest = Path(desktop) / 'FE3H-Linux.desktop'
    quoted = str(launcher).replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$')
    dest.write_text(f'[Desktop Entry]\nType=Application\nName=FE3H Linux\nExec="{quoted}"\nIcon={root / "icon.png"}\nTerminal=false\nCategories=Game;\n', encoding='utf-8')
    dest.chmod(0o755)


def install(args, installation):
    root = installation.root
    assert_game_closed(root)
    package = json.loads((HERE / 'release.json').read_text(encoding='utf-8'))
    app_source = HERE / package['appimage']
    if app_source.parent.resolve() != HERE or not app_source.is_file():
        raise InstallError('The application is missing from the installer package.')
    exefs = validate_exefs(args.exefs or pick('Select your FE3H 1.2.0 exefs folder'))
    romfs = validate_romfs(args.romfs or pick('Select your merged FE3H 1.2.0 romfs folder'))
    dlc = args.dlc
    if not args.no_dlc and dlc is None and not args.headless:
        if dialog('--yesno', 'Do you want to install your extracted DLC too?')[0] == 0:
            dlc = pick('Select the folder containing the FE3H DLC packs')
    packs = validate_dlc(dlc)
    if args.check_only:
        print(f'Validated FE3H 1.2.0 and {len(packs)} DLC packs.')
        return
    required = sum(p.stat().st_size for p in files_in(romfs))
    required += sum(p.stat().st_size for pack in packs.values() for p in files_in(pack / 'romfs'))
    if shutil.disk_usage(root).free < required + app_source.stat().st_size + 4 * 1024**3:
        raise InstallError('Not enough free space to prepare a separate installation. The current installation was preserved.')
    app = installation.add_app(app_source, package['sha256'])
    token, work = installation.workspace()
    data = work / 'data'
    view = work / 'input'
    view.mkdir()
    (view / 'exefs').symlink_to(exefs, target_is_directory=True)
    (view / 'romfs').symlink_to(romfs, target_is_directory=True)
    if packs:
        (view / 'dlc').mkdir()
        for identity, p in packs.items():
            (view / 'dlc' / identity).symlink_to(p, target_is_directory=True)
    env = environment(data, False)
    env['FE3H_INSTALLER_MODE'] = '1'
    env['LARECOMP_LAUNCHER_EXTRACT' if args.headless else 'LARECOMP_LAUNCHER_SETUP'] = str(view)
    output = work / 'setup-output.log'
    proc = None
    try:
        with output.open('w') as log:
            proc = subprocess.Popen([str(app)], env=env, cwd=data, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            started = time.monotonic()
            ready_since = None
            while proc.poll() is None:
                if time.monotonic() - started > 7200:
                    raise InstallError('Preparation exceeded its time limit; the previous installation was preserved.')
                game_log = data / 'fe3hrecomp.log'
                text = game_log.read_text(errors='replace')[-12000:] if game_log.exists() else ''
                ready = '[gamebuild] done ' in text and (data / 'extracted/fe3hgame.so.id').is_file()
                if ready:
                    ready_since = ready_since or time.monotonic()
                    if not args.headless and time.monotonic() - ready_since > 3:
                        proc.send_signal(signal.SIGINT)
                        proc.wait(timeout=30)
                        break
                time.sleep(.5)
            if proc.returncode != 0:
                raise InstallError(f'Preparation did not finish. The previous installation is unchanged. See {output}')
        built = data / 'extracted'
        verify_copy(exefs, built / 'exefs')
        verify_copy(romfs, built / 'romfs')
        for identity, pack in packs.items():
            verify_copy(pack / 'romfs', built / 'dlc' / identity / 'romfs')
        validate_exefs(built / 'exefs')
        validate_romfs(built / 'romfs')
        if len(validate_dlc(built / 'dlc' if (built / 'dlc').exists() else None)) != len(packs):
            raise InstallError('The prepared DLC set is incomplete.')
        verify(app, data, work / 'verify.log')
        assert_game_closed(root)
        backup = installation.commit(token, package['sha256'], package['version'])
        if not (root / 'fe3hrecomp.ini').exists():
            (root / 'fe3hrecomp.ini').write_text('fullscreen = 1\nresolution = match\nresolution_v2 = 1\nmenu_tip_seen = 1\naspect_asked = 1\n', encoding='utf-8')
        desktop_files(root, not args.no_desktop)
        installation.cleanup_workspace(token)
        print('Installation completed:', root)
        if backup:
            print('Previous game files retained:', backup)
        if not args.headless:
            dialog('--msgbox', 'FE3H is installed. Use FE3H Linux on your desktop. Open settings with F1 or Back + Start. Your saves and settings were preserved.')
    finally:
        if proc is not None:
            stop_session(proc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['install', 'launch', 'recover', 'status'], nargs='?', default='install')
    parser.add_argument('--root', type=Path, default=DEFAULT_ROOT)
    parser.add_argument('--exefs', type=Path)
    parser.add_argument('--romfs', type=Path)
    parser.add_argument('--dlc', type=Path)
    parser.add_argument('--no-dlc', action='store_true')
    parser.add_argument('--headless', action='store_true')
    parser.add_argument('--check-only', action='store_true')
    parser.add_argument('--no-desktop', action='store_true', help='Install launchers without creating a desktop entry')
    args = parser.parse_args()
    def interrupted(signum, frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    try:
        installation = Installation(args.root)
        with installation.lock():
            result = installation.recover()
            if args.command == 'install':
                install(args, installation)
            elif args.command == 'launch':
                app = app_for(installation.root)
                env = environment(installation.root)
                profile = env.get('FE3H_MANAGED_HEAP') == '1'
                affinity = (lambda: os.sched_setaffinity(0, {2, 3, 4, 5, 6, 7})) if profile and (os.cpu_count() or 1) >= 8 else None
                proc = subprocess.Popen([str(app), '--play'], cwd=installation.root, env=env, preexec_fn=affinity, start_new_session=True)
                try:
                    return proc.wait()
                finally:
                    stop_session(proc)
            elif args.command == 'recover':
                print('Recovery:', result)
            else:
                print('Data:', installation.root)
                print('Application:', app_for(installation.root))
                print('Recovery:', result)
        return 0
    except KeyboardInterrupt:
        print('Cancelled. Confirmed saves and the previous installation were preserved.', file=sys.stderr)
        return 130
    except (OSError, ValueError, InstallError, subprocess.SubprocessError) as e:
        print(str(e), file=sys.stderr)
        if not args.headless and args.command in ('launch', 'install') and shutil.which('kdialog'):
            dialog('--error', str(e))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
