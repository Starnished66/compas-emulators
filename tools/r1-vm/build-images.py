#!/usr/bin/env python3
"""Build disposable VM disks without mounts, root privileges or device access."""
import argparse, hashlib, json, os, pathlib, shutil, subprocess
REPO = pathlib.Path(__file__).resolve().parents[2]
def copy_tree(source, dest):
    if source.exists(): shutil.copytree(source, dest, symlinks=True, dirs_exist_ok=True)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work',type=pathlib.Path,default=pathlib.Path(os.environ.get('R1_VM_WORK',REPO/'build/r1-vm')))
    p.add_argument('--firmware-root',type=pathlib.Path,default=REPO.parent/'Hiby-R1-Mod/unpack_pack/squashfs-root')
    p.add_argument('--force',action='store_true',help='replace existing VM disks; stop the VM first')
    args=p.parse_args(); work=args.work.resolve(); work.mkdir(parents=True,exist_ok=True)
    busybox=work/'busybox'
    if not busybox.is_file():p.error('build-busybox.sh must complete first')
    if (work/'vm-state.json').exists():
        state=json.loads((work/'vm-state.json').read_text())
        unit=state.get('unit')
        if unit and subprocess.run(['systemctl','--user','is-active','--quiet',unit]).returncode==0:
            p.error('VM is running; stop it before rebuilding disks')
    for name in ['rootfs.img','sdcard.img']:
        if (work/name).exists() and not args.force:p.error(f'{name} exists; use --force to replace it')
    root=work/'rootfs'; sd=work/'sdcard'
    for d in [root,sd]:
        if d.exists():shutil.rmtree(d)
        d.mkdir()
    for d in ['bin','sbin','etc/init.d','proc','sys','dev/pts','tmp','run','usr/bin','usr/sbin','usr/data/mnt/sd_0','opt','lib']:
        (root/d).mkdir(parents=True,exist_ok=True)
    shutil.copy2(busybox,root/'bin/busybox');(root/'bin/busybox').chmod(0o755)
    output=subprocess.check_output(['qemu-mipsel','-cpu','XBurstR2',str(busybox),'--list']).decode()
    for name in output.splitlines():
        target=root/'bin'/name
        if not target.exists():target.symlink_to('busybox')
    (root/'sbin/init').symlink_to('../bin/busybox')
    (root/'bin/sh').unlink(missing_ok=True);(root/'bin/sh').symlink_to('busybox')
    (root/'data').symlink_to('usr/data')
    copy_tree(REPO/'tools/r1-vm/guest',root)
    (root/'etc/init.d/rcS').chmod(0o755)
    (root/'etc/passwd').write_text('root:x:0:0:R1 VM:/root:/bin/sh\n')
    (root/'etc/group').write_text('root:x:0:\n')
    (root/'etc/profile').write_text("export PATH=/bin:/sbin:/usr/bin:/usr/sbin\nexport PS1='r1-vm# '\nexport HOME=/root\n")
    (root/'root').mkdir()
    firmware=args.firmware_root.resolve()
    if firmware.is_dir():
        # Preserve the reference firmware tree without running its X1600 init.
        copy_tree(firmware,root/'opt/hiby-root')
        for name in ['lib','usr/lib']:
            copy_tree(firmware/name,root/name)
        aliases={'ld.so.1':'ld-2.22.so','libc.so.6':'libc-2.22.so','libm.so.6':'libm-2.22.so',
                 'libpthread.so.0':'libpthread-2.22.so','libdl.so.2':'libdl-2.22.so',
                 'librt.so.1':'librt-2.22.so','libresolv.so.2':'libresolv-2.22.so',
                 'libutil.so.1':'libutil-2.22.so','libnsl.so.1':'libnsl-2.22.so'}
        for name,target in aliases.items():
            link=root/'lib'/name
            if not link.exists() and (root/'lib'/target).exists():link.symlink_to(target)
        for f in (root/'lib').glob('*.so*'):
            if not f.is_symlink():f.chmod(0o755)
    core_ids=['quicknes','gpsp','gearsystem','gearboy','wswan','pce','prboom']
    manifest={'firmware_source':str(firmware) if firmware.is_dir() else None,'binaries':{}}
    for core in core_ids:
        for kind,source in [('optimized',REPO/'build/target'/core/core),('baseline',REPO/'build/stage'/core/core)]:
            if not source.is_file():p.error(f'missing {kind} executable: {source}')
            folder=sd/('.emulators' if kind=='optimized' else '.emulators-baseline')/core
            folder.mkdir(parents=True,exist_ok=True)
            shutil.copy2(source,folder/core);(folder/core).chmod(0o755)
            shutil.copy2(REPO/'emulators'/core/'emulator.json',folder/'emulator.json')
            if core=='prboom':
                shutil.copy2(REPO/'build/stage/prboom/prboom.wad',folder/'prboom.wad')
                copy_tree(REPO/'build/stage/prboom/games',folder/'games')
            manifest['binaries'][kind+'/'+core]=hashlib.sha256(source.read_bytes()).hexdigest()
    fixtures={'GBA/240pee_mb.gba':'240pee_mb.gba','GB/gb240p.gb':'gb240p.gb',
              'MasterSystem/test.sms':'test.sms','GameGear/test.gg':'test.gg','SG-1000/test.sg':'test.sg',
              'WonderSwan/test.ws':'test.ws','WonderSwanColor/test.wsc':'test.wsc',
              'PCEngine/test.pce':'test.pce','NES/test.nes':'test.nes'}
    manifest['fixtures']={}
    for dest,source in fixtures.items():
        src=REPO/'tests/roms'/source; dst=sd/'Games'/dest
        dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)
        manifest['fixtures'][dest]=hashlib.sha256(src.read_bytes()).hexdigest()
    heavy=REPO.parent/'compas-player/build/emulator-performance/long/rom-downloads/roms'
    for name,folder in [('kirby.nes','NES'),('battletoads.nes','NES'),('doom2.gba','GBA'),('shantae.gbc','GBC'),
                        ('aerial_assault.sms','MasterSystem'),('halley_wars.gg','GameGear'),
                        ('judgement_silversword.wsc','WonderSwanColor'),('street_fighter_ii.pce','PCEngine')]:
        if (heavy/name).is_file():
            d=sd/'Games'/folder;d.mkdir(parents=True,exist_ok=True);shutil.copy2(heavy/name,d/name)
    (sd/'vm-logs').mkdir();(sd/'vm-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (work/'image-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    for name,size,tree,label in [('rootfs.img',256,root,'R1_VM_ROOT'),('sdcard.img',512,sd,'R1_VM_SD')]:
        image=work/name
        with image.open('wb') as f:f.truncate(size*1024*1024)
        subprocess.run(['mke2fs','-q','-t','ext4','-F','-L',label,'-O','^metadata_csum,^64bit','-d',str(tree),str(image)],check=True)
    print('Built persistent root and virtual SD images in',work)
if __name__=='__main__':main()
