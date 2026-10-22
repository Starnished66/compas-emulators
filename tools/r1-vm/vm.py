#!/usr/bin/env python3
"""A constrained Malta/24Kf software test guest, not an X1600 hardware model."""
import argparse, fcntl, hashlib, json, os, pathlib, re, select, shlex, shutil, socket, subprocess, sys, time, uuid
REPO=pathlib.Path(__file__).resolve().parents[2]
WORK=pathlib.Path(os.environ.get('R1_VM_WORK',REPO/'build/r1-vm')).resolve()
STATE=WORK/'vm-state.json'
def qemu_binary():
    return os.environ.get('QEMU_SYSTEM_MIPSEL') or shutil.which('qemu-system-mipsel') or str(WORK/'host/usr/bin/qemu-system-mipsel')
def identity(ram):
    result={'ram_kib':ram,'cpu_model':'24Kf'}
    for name in ['vmlinux','image-manifest.json']:
        path=WORK/name
        result[name]=hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None
    binary=pathlib.Path(qemu_binary())
    result['qemu_sha256']=hashlib.sha256(binary.read_bytes()).hexdigest() if binary.exists() else None
    result['host_machine_id']=pathlib.Path('/etc/machine-id').read_text().strip()
    return result
def state():return json.loads(STATE.read_text()) if STATE.exists() else {}
def unit_name():return 'compas-r1-vm-'+hashlib.sha256(str(WORK).encode()).hexdigest()[:10]
def active():return subprocess.run(['systemctl','--user','is-active','--quiet',unit_name()]).returncode==0
def quota(value):
    value=round(float(value),2)
    if not 1 <= value <= 100:raise ValueError('CPU quota must be 1..100 percent of one host CPU')
    subprocess.run(['systemctl','--user','set-property','--runtime',unit_name(),f'CPUQuota={value}%'],check=True)
    s=state();s['cpu_quota_percent']=value;STATE.write_text(json.dumps(s,indent=2)+'\n')
def execute(command,timeout=180):
    """One serialized command over the private serial console; no guest network service."""
    WORK.mkdir(parents=True,exist_ok=True)
    deadline=time.monotonic()+timeout
    with (WORK/'console.lock').open('w') as lock:
        while True:
            try:
                fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);break
            except BlockingIOError:
                if time.monotonic()>=deadline:raise TimeoutError('console lock timed out')
                time.sleep(.05)
        marker='__R1_VM_'+uuid.uuid4().hex+'__'
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as sock:
            sock.settimeout(min(timeout,5));sock.connect(str(WORK/'serial.sock'))
            # Drain an old prompt; commands echoed by the terminal cannot match the numeric sentinel.
            sock.sendall(b'\n')
            sock.settimeout(.15)
            drain_deadline=time.monotonic()+1
            try:
                while time.monotonic()<min(deadline,drain_deadline):
                    if not sock.recv(65536):raise RuntimeError('guest console disconnected')
            except socket.timeout:pass
            payload="stty -echo; sh -c "+shlex.quote(command)+"; _r1_rc=$?; printf '\\n"+marker+"RC=%s\\n' \"$_r1_rc\"\n"
            sock.sendall(payload.encode());data=bytearray()
            pattern=re.compile(re.escape(marker.encode())+rb'RC=([0-9]+)\r?\n')
            while time.monotonic()<deadline:
                sock.settimeout(min(1,max(.1,deadline-time.monotonic())))
                try:chunk=sock.recv(65536)
                except socket.timeout:continue
                if not chunk:raise RuntimeError('guest console disconnected before command finished')
                data.extend(chunk);match=pattern.search(data)
                if match:
                    output=bytes(data[:match.start()]).decode(errors='replace').replace('\r','')
                    # The first command after boot can be echoed before stty takes effect.
                    lines=output.splitlines()
                    lines=[x for x in lines if marker not in x]
                    return {'output':'\n'.join(lines).strip(),'exit_code':int(match[1])}
            # Stop a timed-out guest command; leave a shell usable on the next request.
            sock.sendall(b'\x03')
            raise TimeoutError(f'guest command timed out after {timeout}s')
def qmp_quit():
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as sock:
        sock.settimeout(3);sock.connect(str(WORK/'qmp.sock'));sock.recv(8192)
        sock.sendall(b'{"execute":"qmp_capabilities"}\n');sock.recv(8192)
        sock.sendall(b'{"execute":"quit"}\n')
def start(args):
    if active():raise RuntimeError('VM already running; stop it before changing its RAM or disks')
    for name in ['rootfs.img','sdcard.img','vmlinux']:
        if not (WORK/name).is_file():raise RuntimeError(f'{WORK/name} missing; run the build scripts first')
    WORK.mkdir(parents=True,exist_ok=True)
    for name in ['serial.sock','qmp.sock']:(WORK/name).unlink(missing_ok=True)
    if (WORK/'serial.log').exists():
        (WORK/'serial.log').rename(WORK/('serial-'+str(time.time_ns())+'.log'))
    profile_path=REPO/'tools/r1-vm/profiles/r1.json'
    profile=json.loads(profile_path.read_text()) if profile_path.exists() else {}
    calibrated=WORK/'calibration.json'
    calibrated_data=json.loads(calibrated.read_text()) if calibrated.exists() else {}
    memory_settings=WORK/'machine-settings.json'
    memory_data=json.loads(memory_settings.read_text()) if memory_settings.exists() else {}
    ram=args.ram_kib or memory_data.get('ram_kib',calibrated_data.get('ram_kib',65536))
    if not 32768<=ram<=262144:raise ValueError('guest physical RAM must be 32768..262144 KiB')
    if calibrated_data and calibrated_data.get('identity') != identity(ram):
        print('Saved calibration does not match these images/RAM/host; using uncalibrated default.',file=sys.stderr)
        calibrated_data={}
    pct=round(float(args.cpu_quota if args.cpu_quota is not None else calibrated_data.get('selected_quota_percent',100)),2)
    if not 1<=float(pct)<=100:raise ValueError('CPU quota must be 1..100')
    binary=qemu_binary()
    argv=[binary,'-M','malta','-cpu','24Kf','-smp','1','-m',str(ram)+'K',
          '-kernel',str(WORK/'vmlinux'),'-append',f'console=ttyS0,115200 root=/dev/vda rw rootwait init=/sbin/init mem={ram}K',
          '-drive',f'file={WORK}/rootfs.img,format=raw,if=virtio,index=0',
          '-drive',f'file={WORK}/sdcard.img,format=raw,if=virtio,index=1',
          '-vga','none','-display','none','-monitor','none','-nic','none','-no-reboot',
          '-chardev',f'socket,id=serial0,path={WORK}/serial.sock,server=on,wait=off,logfile={WORK}/serial.log',
          '-serial','chardev:serial0','-qmp',f'unix:{WORK}/qmp.sock,server=on,wait=off']
    # No icount: CPUQuota limits wall throughput without redefining the guest clock.
    env_path=str(WORK/'host/usr/lib64')
    if os.environ.get('LD_LIBRARY_PATH'):env_path+=':'+os.environ['LD_LIBRARY_PATH']
    cmd=['systemd-run','--user','--quiet','--service-type=exec','--unit='+unit_name(),
         '--property=CPUQuota='+str(pct)+'%','--property=CPUQuotaPeriodSec=10ms',
         '--property=StandardOutput=append:'+str(WORK/'qemu.log'),
         '--property=StandardError=inherit','--working-directory='+str(WORK),
         '--setenv=LD_LIBRARY_PATH='+env_path,'--']+argv
    subprocess.run(['systemctl','--user','reset-failed',unit_name()],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    subprocess.run(cmd,check=True)
    s={'unit':unit_name(),'ram_kib':ram,'cpu_quota_percent':float(pct),'cpu_model':'24Kf',
       'identity':identity(ram),'machine':'malta','vcpus':1,'swap_kib':24576,'qemu_argv':argv,'started_at':time.strftime('%Y-%m-%dT%H:%M:%S%z'),
       'calibration':'provisional' if not calibrated_data else calibrated_data.get('quality','provisional'),
       'limitations':['not an X1600 device model','CPU quota is not MHz','virtual storage and audio timing differ','24Kf is a compatible MIPS32r2 CPU, not the X1600 core']}
    STATE.write_text(json.dumps(s,indent=2)+'\n')
    deadline=time.monotonic()+args.timeout
    while time.monotonic()<deadline:
        log=WORK/'serial.log'
        if log.exists() and 'R1_VM_READY' in log.read_text(errors='replace'):
            result=execute('cat /proc/meminfo | head -5; cat /proc/swaps; uname -a; test -d /usr/data/mnt/sd_0/.emulators-baseline && grep /dev/zram0 /proc/swaps',10)
            if result['exit_code'] != 0:raise RuntimeError('guest booted but virtual SD or zram is missing: '+result['output'])
            print(result['output']);print('VM ready; CPU quota',pct,'%; RAM',ram,'KiB');return
        if not active():raise RuntimeError('QEMU exited; inspect '+str(WORK/'qemu.log'))
        time.sleep(.5)
    raise TimeoutError('guest did not reach R1_VM_READY; inspect '+str(WORK/'serial.log'))
def status():
    s=state();s['running']=active()
    if s['running']:
        props=subprocess.check_output(['systemctl','--user','show',unit_name(),'-p','MainPID','-p','ControlGroup','-p','CPUQuotaPerSecUSec']).decode()
        s['systemd']=dict(x.split('=',1) for x in props.splitlines() if '=' in x)
        try:s['guest']=execute('cat /proc/meminfo | head -5; cat /proc/swaps; cat /proc/loadavg',10)['output']
        except Exception as e:s['guest_error']=str(e)
    return s
def stop():
    if active():
        try:execute('sync; poweroff -f',5)
        except Exception:pass
        time.sleep(.5)
        if active():
            try:qmp_quit()
            except Exception:pass
            time.sleep(.5)
            if active():subprocess.run(['systemctl','--user','stop',unit_name()],check=True,timeout=15)
        if active():raise RuntimeError('VM is still running after stop')
    print('VM stopped')
def match_memory(args):
    if not active():start(argparse.Namespace(cpu_quota=100,ram_kib=None,timeout=180))
    observations=[]
    for attempt in range(4):
        r=execute("grep '^MemTotal:' /proc/meminfo",10)
        m=re.search(r'MemTotal:\s+(\d+)',r['output'])
        if not m:raise RuntimeError('cannot read guest MemTotal: '+r['output'])
        visible=int(m[1]);ram=state()['ram_kib']
        observations.append({'ram_kib':ram,'memtotal_kib':visible})
        if abs(visible-args.target_kib)<=args.tolerance_kib:
            data={'ram_kib':ram,'target_memtotal_kib':args.target_kib,'observed_memtotal_kib':visible,'observations':observations}
            (WORK/'machine-settings.json').write_text(json.dumps(data,indent=2)+'\n')
            print(json.dumps(data,indent=2));return
        if attempt==3:break
        ram=max(32768,min(262144,((ram+args.target_kib-visible+3)//4)*4))
        pct=state()['cpu_quota_percent'];stop()
        start(argparse.Namespace(cpu_quota=pct,ram_kib=ram,timeout=180))
    raise RuntimeError('memory fit did not converge: '+json.dumps(observations))

def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='action',required=True)
    a=sub.add_parser('start');a.add_argument('--cpu-quota',type=float);a.add_argument('--ram-kib',type=int);a.add_argument('--timeout',type=float,default=180)
    a=sub.add_parser('exec');a.add_argument('--timeout',type=float,default=180);a.add_argument('--json',action='store_true');a.add_argument('command')
    a=sub.add_parser('quota');a.add_argument('percent',type=float)
    a=sub.add_parser('status');a.add_argument('--json',action='store_true')
    a=sub.add_parser('match-memory');a.add_argument('--target-kib',type=int,default=58196);a.add_argument('--tolerance-kib',type=int,default=64)
    sub.add_parser('stop');args=p.parse_args()
    try:
        if args.action=='start':start(args)
        elif args.action=='exec':
            r=execute(args.command,args.timeout);print(json.dumps(r) if args.json else r['output']);return 0 if args.json else r['exit_code']
        elif args.action=='quota':quota(args.percent);print('CPU quota:',args.percent,'% of one host CPU')
        elif args.action=='status':print(json.dumps(status(),indent=2))
        elif args.action=='match-memory':match_memory(args)
        else:stop()
        return 0
    except Exception as e:print('r1-vm:',e,file=sys.stderr);return 1
if __name__=='__main__':sys.exit(main())
