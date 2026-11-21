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
from game_files import validate_exefs, validate_romfs, validate_dlc, files_in, verify_copy, romfs_hash_warnings
from processes import stop_session

HERE = Path(__file__).resolve().parent
DEFAULT_ROOT = Path(os.environ.get('XDG_DATA_HOME', str(Path.home() / '.local/share'))) / 'fe3hrecomp'


def dialog(kind, text, *args):
    if not shutil.which('kdialog'):
        raise InstallError('kdialog is required for graphical installation. Use the documented command-line options instead.')
    p = subprocess.run(['kdialog', '--title', 'FE3H Deck Recomp', kind, text, *args], text=True, capture_output=True)
    return p.returncode, p.stdout.strip()


def pick(label):
    code, text = dialog('--getexistingdirectory', str(Path.home()), '--title', label)
    if code or not text:
        raise InstallError('Installation cancelled. The existing installation was preserved.')
    return Path(text)


def choose_destination(args):
    if args.root is not None:
        return args.root.expanduser()
    if (HERE / 'current.json').is_file():
        return HERE
    if (Path.cwd() / 'current.json').is_file():
        return Path.cwd().resolve()
    if args.command != 'install' or args.headless:
        return DEFAULT_ROOT
    recommended = Path.home() / 'Games/FE3H-Deck-Recomp'
    code, _ = dialog('--yesnocancel', f'Install into:\n{recommended}',
                     '--yes-label', 'Use recommended folder', '--no-label', 'Choose another folder')
    if code == 0:
        return recommended
    if code == 1:
        return pick('Choose or create the exact installation folder')
    raise InstallError('Installation cancelled. No installation was created.')


def validate_destination(path, command):
    path = Path(path).expanduser()
    if path.is_symlink():
        raise InstallError('Choose a real installation folder, not a symbolic link.')
    root = path.resolve()
    if command != 'install':
        if not root.is_dir() or not (root / 'current.json').is_file():
            raise InstallError('Installation unavailable. Check its path and mount the storage device first.')
        return root
    if root in (Path('/'), Path.home().resolve()) or root == HERE or root.is_relative_to(HERE):
        raise InstallError('Choose a dedicated folder outside the downloaded installer.')
    if root.exists() and any(root.iterdir()):
        recognized = (root / 'current.json').is_file()
        if not recognized and (root / '.installer-work').is_dir():
            recognized = any(p.is_dir() and (p / '.owned').is_file() and (p / '.owned').read_text().strip() == p.name for p in (root / '.installer-work').iterdir())
        if not recognized and (root / 'extracted').is_dir():
            validate_exefs(root / 'extracted/exefs')
            validate_romfs(root / 'extracted/romfs')
            recognized = True
        if not recognized:
            raise InstallError('The selected folder contains unrelated files. Choose an empty folder or an existing FE3H installation.')
    return root


def probe_destination(root):
    import tempfile
    parent = root
    while not parent.exists():
        parent = parent.parent
    try:
        with tempfile.TemporaryDirectory(prefix='.fe3h-check-', dir=parent) as temporary:
            base = Path(temporary)
            script = base / 'run.sh'
            script.write_text('#!/bin/sh\nexit 0\n')
            script.chmod(0o700)
            (base / 'link').symlink_to(script)
            subprocess.run([str(script)], check=True, timeout=10)
    except (OSError, subprocess.SubprocessError) as e:
        raise InstallError('This destination must support writing, symbolic links and executable files: ' + str(e)) from e


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
    settings = root / 'Settings.sh'
    settings.write_text('#!/bin/sh\nHERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexec python3 "$HERE/manage.py" settings --root "$HERE"\n', encoding='utf-8')
    settings.chmod(0o755)
    # Kits ship icon.png; source-based installation/recovery uses docs/icon.
    # An installed launcher may also refresh itself: do not copy a file onto itself.
    icon = next((p for p in (HERE / 'icon.png', HERE.parent.parent / 'docs/icon/icon_1024.png') if p.is_file()), None)
    if icon is None:
        raise InstallError('The application icon is missing. Keep icon.png with the installer.')
    target_icon = root / 'icon.png'
    if icon.resolve() != target_icon.resolve():
        temporary = root / 'icon.png.tmp'
        shutil.copy2(icon, temporary)
        os.replace(temporary, target_icon)
    quoted_settings = str(settings).replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$')
    settings_entry = f'[Desktop Entry]\nType=Application\nName=Settings\nComment=Configure FE3H Deck Recomp and mods\nExec="{quoted_settings}"\nIcon={root / "icon.png"}\nTerminal=false\nCategories=Game;\n'
    (root / 'Settings.desktop').write_text(settings_entry, encoding='utf-8')
    (root / 'Settings.desktop').chmod(0o755)
    if not make_desktop:
        return
    desktop = subprocess.check_output(['xdg-user-dir', 'DESKTOP'], text=True).strip()
    dest = Path(desktop) / 'FE3H-Deck-Recomp.desktop'
    quoted = str(launcher).replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$')
    dest.write_text(f'[Desktop Entry]\nType=Application\nName=FE3H Deck Recomp\nExec="{quoted}"\nIcon={root / "icon.png"}\nTerminal=false\nCategories=Game;\n', encoding='utf-8')
    dest.chmod(0o755)
    settings_dest = Path(desktop) / 'FE3H-Deck-Recomp-Settings.desktop'
    settings_dest.write_text(settings_entry, encoding='utf-8')
    settings_dest.chmod(0o755)


def wait_for_rebuild(proc, root, output, *, timeout=7200):
    """Wait for a --build-game invocation triggered by the 'rebuild' command."""
    started = time.monotonic()
    last_log_check = started
    while proc.poll() is None:
        if time.monotonic() - started > timeout:
            stop_session(proc)
            raise InstallError('Rebuild exceeded its time limit.')
        time.sleep(1)
        if time.monotonic() - last_log_check >= 15:
            last_log_check = time.monotonic()
            elapsed = int(last_log_check - started)
            print(f'[rebuild] compiling in progress... ({elapsed}s elapsed)', flush=True)
    with output.open('a', encoding='utf-8') as log:
        log.write(f'\n[installer] rebuild exit={proc.returncode}\n')
    if proc.returncode != 0:
        raise InstallError(f'Rebuild did not finish (exit {proc.returncode}). See {output}')


def rebuild(args, installation):
    """Rebuild the game code library with the cheats.txt now in the data folder.

    This is equivalent to pressing 'Rebuild game code' in the start-screen
    Settings > Game Files menu, but runs from the terminal so it can be
    scripted and so the user does not have to navigate the UI.

    Protections preserved
    ---------------------
    * The game must be closed before running (assert_game_closed).
    * The new .so is built into a temporary .gamebuild/ folder and only
      renamed into place after a successful link (gamebuild.c).
    * If the build fails, gamebuild.c leaves the existing fe3hgame.so
      untouched (it renames the .so away only after the new tmp is ready
      and the rename succeeds).
    * cheats.txt is never modified; a missing cheats.txt is fine — the
      rebuild then produces an empty cheat_variants list, same as before.
    """
    root = installation.root
    assert_game_closed(root)
    app = app_for(root)
    # Pass LARECOMP_DATA_ROOT so rt_data_root() finds root (and therefore
    # root/cheats.txt) even when the AppImage is not launched via manage.py.
    env = environment(root)
    # Explicitly set LARECOMP_CHEATS so gamebuild.c's setenv() call in the
    # child matches what the user has in their data folder.  gamebuild.c
    # already does this internally, but exporting it here makes it visible
    # to any wrapper or test harness that inspects the environment.
    cheats_path = root / 'cheats.txt'
    if cheats_path.is_file():
        env['LARECOMP_CHEATS'] = str(cheats_path)
        print(f'[rebuild] cheats.txt found: {cheats_path}')
    else:
        env.pop('LARECOMP_CHEATS', None)
        print('[rebuild] no cheats.txt found; rebuilding without instruction patches')
    output = root / 'rebuild.log'
    print(f'[rebuild] starting; log: {output}')
    proc = None
    try:
        with output.open('w') as log:
            proc = subprocess.Popen(
                [str(app), '--build-game'],
                cwd=root, env=env,
                stdout=log, stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            wait_for_rebuild(proc, root, output)
        cheats_file = root / 'extracted' / 'fe3hgame.so.cheats'
        variants = 0
        if cheats_file.is_file() and cheats_file.stat().st_size > 0:
            variants = sum(1 for _ in cheats_file.open())
        print(f'[rebuild] done — {variants} instruction variant(s) compiled in')
        if variants == 0 and cheats_path.is_file():
            print('[rebuild] note: 0 variants means cheats.txt has no instruction-patching cheats (opcode 04), only memory cheats — that is normal')
        print('[rebuild] restart the game to pick up the new library')
    finally:
        if proc is not None:
            stop_session(proc)


def wait_for_preparation(proc, data, headless, output, *, timeout=7200, settle=3):
    """Close a completed graphical import without treating our SIGINT as a build failure."""
    started = time.monotonic()
    ready_since = None
    requested_close = False
    while proc.poll() is None:
        if time.monotonic() - started > timeout:
            raise InstallError('Preparation exceeded its time limit; the previous installation was preserved.')
        game_log = data / 'fe3hrecomp.log'
        text = game_log.read_text(errors='replace')[-12000:] if game_log.exists() else ''
        ready = '[gamebuild] done ' in text and (data / 'extracted/fe3hgame.so.id').is_file()
        if ready:
            ready_since = ready_since or time.monotonic()
            if not headless and time.monotonic() - ready_since >= settle:
                requested_close = True
                stop_session(proc)
                break
        time.sleep(.1)
    # Only an interruption explicitly requested after the completed-build marker
    # is expected. An external interruption or a crash remains an error. The
    # caller must still validate every input copy and load the compiled library.
    accepted_close = requested_close and proc.returncode in (-signal.SIGINT, 128 + signal.SIGINT)
    with output.open('a', encoding='utf-8') as log:
        log.write(f'\n[installer] setup exit={proc.returncode}; completed-build close requested={requested_close}\n')
    if proc.returncode != 0 and not accepted_close:
        raise InstallError(f'Preparation did not finish (exit {proc.returncode}). The previous installation is unchanged. See {output}')


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
    warnings = romfs_hash_warnings(romfs)
    if warnings:
        warning = 'Not a clean 1.2.0 copy:\n' + '\n'.join(warnings) + '\nThis is a warning only; installation may continue.'
        print(warning, file=sys.stderr)
        if not args.headless:
            dialog('--sorry', warning)
    packs = validate_dlc(dlc)
    for original in [exefs, romfs, *packs.values()]:
        if root == original.resolve() or root.is_relative_to(original.resolve()):
            raise InstallError('The installation destination cannot be inside the original game files.')
    if args.check_only:
        print(f'Validated FE3H 1.2.0 and {len(packs)} DLC packs.')
        return
    required = sum(p.stat().st_size for p in files_in(exefs)) + sum(p.stat().st_size for p in files_in(romfs))
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
            wait_for_preparation(proc, data, args.headless, output)
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
            dialog('--msgbox', 'FE3H is installed. Use FE3H Deck Recomp on your desktop. Open settings with F1 or Back + Start. Your saves and settings were preserved.')
    finally:
        if proc is not None:
            stop_session(proc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['install', 'launch', 'settings', 'rebuild', 'recover', 'status'], nargs='?', default='install')
    parser.add_argument('--root', type=Path, default=None)
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
        args.root = validate_destination(choose_destination(args), args.command)
        if args.command == 'install':
            probe_destination(args.root)
            if not args.headless and dialog('--yesno', f'Prepare FE3H in:\n{args.root}\nSaves and settings will be kept in this folder.')[0] != 0:
                raise InstallError('Installation cancelled. No installation was created.')
        installation = Installation(args.root)
        with installation.lock():
            result = installation.recover()
            if args.command == 'install':
                install(args, installation)
            elif args.command in ('launch', 'settings'):
                app = app_for(installation.root)
                env = environment(installation.root)
                profile = env.get('FE3H_MANAGED_HEAP') == '1'
                affinity = (lambda: os.sched_setaffinity(0, {2, 3, 4, 5, 6, 7})) if profile and (os.cpu_count() or 1) >= 8 else None
                command = [str(app)] + (['--play'] if args.command == 'launch' else [])
                proc = subprocess.Popen(command, cwd=installation.root, env=env, preexec_fn=affinity, start_new_session=True)
                try:
                    return proc.wait()
                finally:
                    stop_session(proc)
            elif args.command == 'rebuild':
                rebuild(args, installation)
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
        if not args.headless and args.command in ('launch', 'settings', 'install') and shutil.which('kdialog'):
            dialog('--error', str(e))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
