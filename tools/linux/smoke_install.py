#!/usr/bin/env python3
"""Private FE3H import test; FE3H_SMOKE_EXEFS/ROMFS required, DLC/SAVE/CONFIG optional."""
from pathlib import Path
import json, os, re, shutil, subprocess, sys, time, uuid
from install_core import digest
repo=Path(__file__).resolve().parents[2];app=Path(sys.argv[1]).resolve(strict=True)
out=Path(os.environ.get('FE3H_SMOKE_OUT',str(repo/'build'/('fe3h-smoke-'+uuid.uuid4().hex))))
out.mkdir(parents=True,exist_ok=False);kit=out/'kit with spaces';root=out/'data with spaces';kit.mkdir();root.mkdir()
state={'status':'preparing','started':time.time(),'app_sha256':digest(app)}
def status(**values):
    state.update(values);p=out/'status.tmp';p.write_text(json.dumps(state,indent=2));p.replace(out/'status.json')
def save_hashes():
    p=root/'save';return {f.relative_to(p).as_posix():digest(f) for f in p.rglob('*') if f.is_file()} if p.exists() else {}
try:
    for name in ('manage.py','install_core.py','game_files.py','processes.py'):shutil.copy2(Path(__file__).parent/name,kit/name)
    shutil.copy2(app,kit/app.name)
    (kit/'release.json').write_text(json.dumps({'appimage':app.name,'sha256':state['app_sha256'],'version':'0.2.3-linux-opt5'}))
    if os.environ.get('FE3H_SMOKE_SAVE'):shutil.copytree(os.environ['FE3H_SMOKE_SAVE'],root/'save',ignore=shutil.ignore_patterns('*.pending'))
    if os.environ.get('FE3H_SMOKE_CONFIG'):
        ini=Path(os.environ['FE3H_SMOKE_CONFIG']).read_text(encoding='utf-8-sig')
        for k,v in [('fullscreen','1'),('resolution','match')]:ini=re.sub(r'^'+k+r' = .*',k+' = '+v,ini,flags=re.M)
        (root/'fe3hrecomp.ini').write_text(ini,encoding='utf-8')
    before=save_hashes();configuration=digest(root/'fe3hrecomp.ini') if (root/'fe3hrecomp.ini').exists() else None
    cmd=['python3',str(kit/'manage.py'),'install','--headless','--no-desktop','--root',str(root),'--exefs',os.environ['FE3H_SMOKE_EXEFS'],'--romfs',os.environ['FE3H_SMOKE_ROMFS']]
    cmd+=['--dlc',os.environ['FE3H_SMOKE_DLC']] if os.environ.get('FE3H_SMOKE_DLC') else ['--no-dlc']
    status(status='importing',data=str(root))
    with (out/'install.log').open('w') as log:
        proc=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT);deadline=time.monotonic()+7200
        while proc.poll() is None:
            if time.monotonic()>deadline:
                proc.terminate();proc.wait(timeout=45);raise RuntimeError('Import deadline exceeded')
            files=list(root.glob('.installer-work/*/data/fe3hrecomp.log'))
            if files:
                lines=[l for l in files[-1].read_text(errors='replace')[-6000:].splitlines() if '[gamebuild]' in l or '[launcher]' in l]
                status(elapsed=time.time()-state['started'],progress=lines[-2:])
            time.sleep(2)
        assert proc.returncode==0,'Import failed; see install.log and retained workspace logs'
    assert before==save_hashes(),'Import changed saves'
    if configuration:assert digest(root/'fe3hrecomp.ini')==configuration,'Import changed configuration'
    status(status='import-verified',saves_preserved=True,settings_preserved=True)
    if before:
        (root/'build').mkdir(exist_ok=True)
        env=dict(os.environ)
        for key in list(env):
            if key.startswith(('FE3H_','LARECOMP_','MANGOHUD')):env.pop(key)
        env.pop('LD_PRELOAD',None)
        env.update(LARECOMP_DATA_ROOT=str(root),APPIMAGE_EXTRACT_AND_RUN='1',DISABLE_LSFGVK='1',FE3H_HANDHELD='0',FE3H_MANAGED_HEAP='1',FE3H_NATIVE_HALF='1',FE3H_SPARSE_LOCAL='1',FE3H_TIMED_SYNC='1',FE3H_DOCK_TARGETS='1',FE3H_COMPUTE_SCF='1',FE3H_LIGHTING_SCF='1',FE3H_EFFECT_SCALE_FIX='0',FE3H_LIMITER_MARGIN_US='1000',FE3H_WARM_PROFILE='1',SDL_VIDEO_DRIVER='wayland',SDL_AUDIO_DRIVER='pulseaudio',LARECOMP_VK_PROF='1',LARECOMP_INPUT='300:A:15,600:A:15,900:A:15,1200:A:15,1500:A:15',LARECOMP_WINDOW_SHOT='4800',LARECOMP_EXIT_AFTER_SHOT='1')
        for line in subprocess.check_output(['systemctl','--user','show-environment'],text=True).splitlines():
            if '=' in line:
                k,v=line.split('=',1)
                if k in ('DISPLAY','WAYLAND_DISPLAY','XDG_RUNTIME_DIR','DBUS_SESSION_BUS_ADDRESS','XAUTHORITY'):env[k]=v
        status(status='checking-gameplay')
        with (out/'gameplay-output.log').open('w') as log:result=subprocess.run(['taskset','-c','2-7',str(kit/app.name),'--play'],env=env,cwd=root,stdout=log,stderr=subprocess.STDOUT,timeout=300)
        text=(root/'fe3hrecomp.log').read_text(errors='replace')
        assert result.returncode==0 and not re.search(r'\*\*\* signal|VK_ERROR_DEVICE_LOST|AddressSanitizer',text)
        assert (root/'build/window.png').is_file(),'No gameplay screenshot'
        shutil.copy2(root/'build/window.png',out/'gameplay.png');shutil.copy2(root/'fe3hrecomp.log',out/'gameplay.log')
    status(status='completed-awaiting-image-review',elapsed=time.time()-state['started'],finished=time.time())
except BaseException as error:
    status(status='failed',error=str(error),finished=time.time());raise
