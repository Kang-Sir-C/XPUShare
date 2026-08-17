import argparse
try:
    import inotify.adapters  # type: ignore
    import inotify.constants  # type: ignore
    _HAS_INOTIFY = True
except Exception:
    inotify = None  # type: ignore
    _HAS_INOTIFY = False

import os
import sys
import signal
import shlex
import subprocess as sp
import time

args = None
podlist = {}

def prepare_env(name, port, schd_port):
    client_env = os.environ.copy()
    client_env['SCHEDULER_IP'] = '127.0.0.1'
    client_env['SCHEDULER_PORT'] = str(schd_port)
    client_env['POD_MANAGER_IP'] = '0.0.0.0'
    client_env['POD_MANAGER_PORT'] = str(port)
    client_env['POD_NAME'] = name
    return client_env

def launch_scheduler():
    cfg_h, cfg_t = os.path.split(args.pod_list)
    if cfg_h == '':
        cfg_h = os.getcwd()

    cmd = "{} -p {} -f {} -P {} -q {} -m {} -w {}".format(
        args.schd, cfg_h, cfg_t, args.port, args.base_quota, args.min_quota, args.window
    )
    with open("/xpushare/log/xhook-scheduler.log","a") as err:
        proc = sp.Popen(shlex.split(cmd), universal_newlines=True, bufsize=1, stderr = err)
    return proc

def update_podmanager(file):
    with open(file) as f:
        lines = f.readlines()
    if not lines:
        return
    podnum = int(lines[0])
    for _, val in podlist.items():
        val[0] = False
    for i in range(1, podnum+1):
        name, port = lines[i].split()
        name_port = lines[i][:-1]
        if name_port not in podlist:
            sys.stderr.write("[launcher] pod manager id '{}' port '{}' start running\n".format(name_port, port))
            sys.stderr.flush()
            with open("/xpushare/log/xhook-pmgr.log","a") as err:
                proc = sp.Popen(
                    shlex.split(args.pmgr),
                    env=prepare_env(name, port, args.port),
                    preexec_fn=os.setpgrp,
                    stderr=err
                )
            podlist[name_port] = [True, proc]
        else:
            podlist[name_port][0] = True
    del_list = []
    for n, val in podlist.items():
        if not val[0]:
            os.killpg(os.getpgid(val[1].pid), signal.SIGKILL)
            val[1].wait()
            sys.stderr.write("[launcher] pod manager id '{}' has been deleted\n".format(n))
            sys.stderr.flush()
            del_list.append(n)
    for n in del_list:
        del podlist[n]

def main():
    global args
    parser = argparse.ArgumentParser()
    parser.add_argument('schd', help='path to scheduler executable')
    parser.add_argument('pmgr', help='path to pod-manager executable')
    parser.add_argument('gpu_uuid', help='scheduling system GPU UUID')
    parser.add_argument('pod_list', help='path to pod list file')
    parser.add_argument('pmgr_port_dir', help='path to pod port dir')
    parser.add_argument('--port', type=int, default=49901, help='base port')
    parser.add_argument('--base_quota', type=float, default=300, help='base quota (ms)')
    parser.add_argument('--min_quota', type=float, default=20, help='minimum quota (ms)')
    parser.add_argument('--window', type=float, default=10000, help='time window (ms)')
    args = parser.parse_args()

    launch_scheduler()
    sys.stderr.write(f"[launcher] scheduler started on 0.0.0.0:{args.port}\n")
    sys.stderr.flush()
    
    update_podmanager(os.path.join(args.pmgr_port_dir, args.gpu_uuid)) #first time 

    target = os.path.join(args.pmgr_port_dir, args.gpu_uuid)
    if _HAS_INOTIFY:
        ino = inotify.adapters.Inotify()
        ino.add_watch(args.pmgr_port_dir, inotify.constants.IN_CLOSE_WRITE)
        for event in ino.event_gen(yield_nones=False):
            (_, _, _, filename) = event
            try:
                if filename == args.gpu_uuid:
                    update_podmanager(target)
            except:  # file content may not correct
                sys.stderr.write("Catch exception in update_podmanager: {}\n".format(sys.exc_info()))
                sys.stderr.flush()
    else:
        sys.stderr.write("[launcher] inotify not available, falling back to polling\n")
        sys.stderr.flush()
        last_mtime = None
        while True:
            try:
                st = os.stat(target)
                mtime = st.st_mtime_ns
            except Exception:
                mtime = None
            if mtime != last_mtime and mtime is not None:
                last_mtime = mtime
                try:
                    update_podmanager(target)
                except:
                    sys.stderr.write("Catch exception in update_podmanager: {}\n".format(sys.exc_info()))
                    sys.stderr.flush()
            time.sleep(0.2)

if __name__ == '__main__':
    os.setpgrp()
    try:
        main()
    except:
        sys.stderr.write("Catch exception: {}\n".format(sys.exc_info()))
        sys.stderr.flush()
    finally:
        for _, val in podlist.items():
            os.killpg(os.getpgid(val[1].pid), signal.SIGKILL)
        os.killpg(0, signal.SIGKILL)
