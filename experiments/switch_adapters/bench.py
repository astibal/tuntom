#!/usr/bin/env python3
"""Multi-adapter IPC benchmark: fixed CPU budget, explicit port ownership."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import select
import statistics
import subprocess
import sys
import tempfile
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"switch_mt"))
from bench import physical_cpus, snapshot, stop, parse_stats, process_cpu, task_samples, host_cpu, rss_kib


def allocation(tunnels, adapters, direction, variant):
    groups = 1 if variant=='shared' else (min(2,adapters) if variant=='sharded' else adapters)
    adapter_owners = [i%groups for i in range(adapters)]
    if direction=='duplex':
        # Extra adapter threads share the SAME directional CPU, preserving
        # both adapter and tunnel hardware budgets across variants.
        rx = tx = [groups]*tunnels+adapter_owners
        rx_cpus,tx_cpus = [0]*groups+[2],[1]*groups+[3]
        arx=atx=list(range(groups)); trx=ttx=[groups]
    elif direction=='up':
        rx = [i%2 for i in range(tunnels)]+[0]*adapters
        tx = [groups]*tunnels+adapter_owners
        rx_cpus,tx_cpus = [0,1],[2+i%2 for i in range(groups)]+[2]
        arx=[]; atx=list(range(groups)); trx=[0,1]; ttx=[]
    else:
        rx = [groups]*tunnels+adapter_owners
        tx = [i%2 for i in range(tunnels)]+[0]*adapters
        rx_cpus,tx_cpus = [i%2 for i in range(groups)]+[0],[2,3]
        arx=list(range(groups)); atx=[]; trx=[]; ttx=[0,1]
    return dict(rx_owners=rx,tx_owners=tx,rx_cpus=rx_cpus,tx_cpus=tx_cpus,
                adapter_rx=arx,adapter_tx=atx,tunnel_rx=trx,tunnel_tx=ttx)


def run(args, variant, direction, size, rate, adapters, shape, cpus):
    plan = allocation(args.tunnels, adapters, direction, variant)
    worker_cpus = cpus[:4]
    source_cpus, sink_cpus = cpus[4:6], cpus[6:8]
    port_count = args.tunnels + adapters
    csv = lambda values: ",".join(map(str, values))
    with tempfile.TemporaryDirectory(prefix="tuntom-mt-bench.") as temp:
        data, ctl, final_stats = (Path(temp) / part for part in ("data", "control", "final"))
        names = [f"tunnel{i}" for i in range(args.tunnels)] + [f"adapter{i}" for i in range(adapters)]
        command = [args.draft, "--socket", str(data), "--control-socket", str(ctl),
                   "--stats-file",str(final_stats),"--pool-size",str(args.pool_size),
                   "--queue-size",str(args.queue_size),"--backpressure","retry","--idle","sleep",
                   "--rx-workers",str(len(plan['rx_cpus'])),"--tx-workers",str(len(plan['tx_cpus'])),
                   "--worker-cpus",csv([cpus[i] for i in plan['rx_cpus']+plan['tx_cpus']])]
        for name in names: command += ["--port",name]
        for i in range(adapters): command += ["--exit-port",f"adapter{i}"]
        for i in range(args.tunnels):
            a = i % adapters
            command += ["--route",f"tunnel{i}:18=adapter{a}:{1000+i}",
                        "--route",f"adapter{a}:{100+i}=tunnel{i}:{1000+args.tunnels+a}"]
        for side in ('rx','tx'):
            for name,owner in zip(names,plan[side+'_owners']):
                command += [f"--{side}-owner",f"{name}:{owner}"]
        switch = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                                  preexec_fn=lambda: os.sched_setaffinity(0, set(worker_cpus)))
        driver = None
        try:
            deadline = time.monotonic() + 10
            while not ctl.exists():
                if switch.poll() is not None:
                    raise RuntimeError("switch startup: " + switch.stderr.read())
                if time.monotonic() > deadline:
                    raise RuntimeError("control startup timeout")
                time.sleep(.01)
            driver_cmd = [args.driver,str(data),str(size),str(args.duration),str(rate),
                          csv(source_cpus),csv(sink_cpus),str(int(args.verify_all)),
                          str(args.tunnels),str(adapters),direction,shape]
            driver = subprocess.Popen(driver_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True)
            if not select.select([driver.stdout], [], [], 10)[0] or driver.stdout.readline().strip() != "READY":
                raise RuntimeError("driver startup failed")
            deadline = time.monotonic() + 10
            while True:
                stats = snapshot(ctl)
                if stats["connections_current"] == port_count and stats.get("workers_started", 1):
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError("registration timeout")
                time.sleep(.01)
            for i,name in enumerate(names):
                assert stats[f'port_{i}_name']==name
                for side in ('rx','tx'): assert stats[f'port_{i}_{side}_owner']==plan[side+'_owners'][i]
            before_cpu, before_host = process_cpu(switch.pid), host_cpu()
            before_tasks = task_samples(switch.pid)
            tasks = dict(before_tasks)
            before_driver_tasks = task_samples(driver.pid)
            driver_tasks = dict(before_driver_tasks)
            load_before = os.getloadavg()
            began = time.monotonic()
            driver.stdin.write("START\n")
            driver.stdin.close()
            driver.stdin = None
            peak_rss = 0
            while driver.poll() is None:
                if switch.poll() is not None:
                    raise RuntimeError("switch exited: " + switch.stderr.read())
                if time.monotonic() - began > args.duration + 15:
                    raise RuntimeError("driver timed out")
                tasks.update(task_samples(switch.pid))
                try:
                    driver_tasks.update(task_samples(driver.pid))
                except (FileNotFoundError, ProcessLookupError):
                    pass
                peak_rss = max(peak_rss, rss_kib(switch.pid))
                time.sleep(.1)
            output, error = driver.communicate()
            if driver.returncode:
                raise RuntimeError(f"driver failed: {output} {error}")
            row = json.loads(output)
            after_host = host_cpu()
            row.update(switch_cpu_s=process_cpu(switch.pid)-before_cpu,
                       cpu_observation_s=time.monotonic()-began, switch_peak_rss_kib=peak_rss,
                       host_loadavg_before=load_before, host_loadavg_after=os.getloadavg(),
                       affinity=dict(switch=worker_cpus, source=source_cpus, sink=sink_cpus),
                       command=command, driver_command=driver_cmd)
            row["host_cpu_busy_percent"] = {}
            for cpu, first in before_host.items():
                delta = [last-first for first, last in zip(first, after_host[cpu])]
                row["host_cpu_busy_percent"][cpu] = 100*(sum(delta)-delta[3]-delta[4])/max(1, sum(delta))
            row["switch_tasks"] = {}
            for tid, last in tasks.items():
                first = before_tasks.get(tid, {})
                row["switch_tasks"][tid] = {key: value-first.get(key, 0) if key != "name" else value
                                             for key, value in last.items()}
            row["driver_tasks"] = {}
            for tid, last in driver_tasks.items():
                first = before_driver_tasks.get(tid, {})
                row["driver_tasks"][tid] = {key: value-first.get(key, 0) if key != "name" else value
                                            for key, value in last.items()}
            stats = snapshot(ctl)
            stop(switch)
            if switch.returncode:
                raise RuntimeError("switch shutdown: " + switch.stderr.read())
            if final_stats.exists():
                stats = parse_stats(final_stats.read_text())
            row["switch_stats"] = stats
            row["allocation"] = plan
            row["sent"], row["received"] = sum(row["sent_by_port"]), sum(row["received_by_port"])
            row["switch_drops"] = row["sent"]-row["received"]
            for key in ("malformed_frames", "send_errors", "route_misses", "target_disconnected", "rx_errors", "shutdown_drops", "buffers_in_use"):
                assert stats.get(key, 0) == 0, (key, row)
            assert row["invalid"] == 0 and row["sent"] == stats["frames_rx"], row
            assert row["received"] == stats["frames_tx"], row
            assert row["switch_drops"] == stats["send_backpressure_drops"]+stats.get("queue_full_drops", 0), row
            row["pps"] = row["received"] / row["elapsed_s"]
            row["mbps"] = row["received_bytes"]*8/row["elapsed_s"]/1e6
            row["offered_loss_percent"] = 100*(row["offered"]-row["received"])/max(1, row["offered"])
            row["switch_loss_percent"] = 100*row["switch_drops"]/max(1, row["sent"])
            for who in ("switch", "source", "sink"):
                row[who+"_cpu_percent"] = 100*row[who+"_cpu_s"]/row["elapsed_s"]
            row["switch_cpu_ns_per_frame"] = row["switch_cpu_s"]*1e9/max(1,row["received"])
            row["total_cpu_ns_per_frame"] = sum(row[k+"_cpu_s"] for k in ("switch", "source", "sink"))*1e9/max(1,row["received"])
            row['actual_offered_pps'] = row['offered']/row['elapsed_s']
            row['offer_achievement_percent'] = 100*row['actual_offered_pps']/rate if rate else None
            row['source_peak_cpu_percent'] = max(row['source_cpu_by_thread'])*100/row['elapsed_s']
            row['sink_peak_cpu_percent'] = max(row['sink_cpu_by_thread'])*100/row['elapsed_s']
            row['to_adapter_pps'] = [n/row['elapsed_s'] for n in row['received_by_port'][args.tunnels:]]
            row['from_adapter_pps'] = [sum(row['received_by_port'][i] for i in range(a,args.tunnels,adapters))/row['elapsed_s'] for a in range(adapters)]
            offered = [n+b for n,b in zip(row['sent_by_port'],row['source_backpressure_by_port'])]
            row['offered_to_adapter'] = [sum(offered[i] for i in range(a,args.tunnels,adapters)) for a in range(adapters)]
            row['offered_from_adapter'] = offered[args.tunnels:]
            row['worker_cpu_s'] = {side:[stats[f'worker_{side}_{i}_cpu_s'] for i in range(len(plan[side+'_cpus']))] for side in ('rx','tx')}
            row['adapter_cpu_s'] = {side:[row['worker_cpu_s'][side][i] for i in plan['adapter_'+side]] for side in ('rx','tx')}
            row['tunnel_cpu_s'] = {side:[row['worker_cpu_s'][side][i] for i in plan['tunnel_'+side]] for side in ('rx','tx')}
            workers = [v for v in row['switch_tasks'].values() if v['name'].startswith('sw-')]
            row['worker_runqueue_cpu_equivalents'] = sum(v['runqueue_wait_s'] for v in workers)/row['elapsed_s']
            row['worker_context_switches_per_second'] = sum(v['context_switches'] for v in workers)/row['elapsed_s']
            row['wake_requests_per_frame'] = stats['wake_calls']/max(1,row['received'])
            assert row['switch_drops']==0, row
            return row
        finally:
            stop(driver)
            stop(switch)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',required=True)
    p.add_argument('--resume',action='store_true')
    p.add_argument('--adapters',default='1,2,4')
    p.add_argument('--tunnels',type=int,default=8)
    p.add_argument('--variants',default='shared,sharded,per-adapter')
    p.add_argument('--case',action='append',help='duplex|up|down:bytes:pps:equal|hot')
    p.add_argument('--duration',type=float,default=4)
    p.add_argument('--repeat',type=int,default=3)
    p.add_argument('--verify-all',action='store_true')
    p.add_argument('--pool-size',type=int,default=128)
    p.add_argument('--queue-size',type=int,default=128)
    p.add_argument('--draft',default='/tmp/tuntom-switch-adapters')
    p.add_argument('--driver',default='/tmp/tuntom-switch-adapters-load')
    args=p.parse_args()
    counts=list(map(int,args.adapters.split(',')))
    variants=args.variants.split(',')
    if args.tunnels<2 or any(a<1 or args.tunnels%a or args.tunnels+a>128 for a in counts):
        p.error('adapters must divide tunnel count; at least two tunnels; maximum 128 total ports')
    if any(v not in ('shared','sharded','per-adapter') for v in variants):p.error('unknown variant')
    if args.duration<=0 or args.repeat<1:p.error('duration/repeat must be positive')
    cases=[]
    for case in args.case or ['duplex:9000:160000:equal','duplex:9000:240000:equal',
                             'duplex:9000:320000:equal','duplex:9000:240000:hot',
                             'up:9000:0:equal','down:9000:0:equal',
                             'up:64:0:equal','down:64:0:equal']:
        d,size,rate,shape=case.split(':')
        if d not in ('duplex','up','down') or shape not in ('equal','hot'):p.error('unknown case')
        size=int(size);rate=float(rate)
        if (size!=0 and not 40<=size<=65535) or rate<0:p.error('invalid size/rate')
        cases.append((d,size,rate,shape))
    cpus=physical_cpus()
    if len(cpus)!=8:p.error('this layout requires eight physical cores')
    output=Path(args.output)
    output.parent.mkdir(parents=True,exist_ok=True)
    if output.exists()!=args.resume:p.error('existing output needs --resume; resume needs an existing output')
    root=Path(__file__).resolve().parents[2]
    files=[Path(args.draft),Path(args.driver),*root.joinpath('src').rglob('*.hpp'),
           root/'experiments/switch_workers/main.cpp',root/'experiments/switch_workers/poll_wake.hpp',
           root/'experiments/switch_mt/queues.hpp',root/'experiments/switch_mt/bench.py',
           *Path(__file__).parent.glob('*.cpp'),*Path(__file__).parent.glob('*.py'),Path(__file__).parent/'build.sh']
    meta=dict(arguments=vars(args),utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
              physical_cpus=cpus,allowed_logical_cpus=sorted(os.sched_getaffinity(0)),uname=list(platform.uname()),
              layout='Switch physical 0-3; sources4,5; sinks6,7. Duplex extra adapter threads stay on the same directional cores. One-way adapter work may use two cores; other side keeps two cores.',
              cpu_topology={str(c):{n:Path(f'/sys/devices/system/cpu/cpu{c}/topology/{n}').read_text().strip() for n in ('core_id','physical_package_id','thread_siblings_list')} for c in os.sched_getaffinity(0)},
              compiler_flags='Native: -std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -Isrc. For an explicitly selected sanitizer binary, see validation.txt.',
              sha256={str(f.resolve()):hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
              git_head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip())
    metadata_path=Path(str(output)+'.metadata.json')
    if args.resume:
        old=json.loads(metadata_path.read_text())
        for k,v in vars(args).items():
            if k!='resume' and old['arguments'][k]!=v:p.error('changed resume argument '+k)
        if old['sha256']!=meta['sha256']:p.error('resume needs identical measured source/binary files')
        metadata_path=Path(str(output)+f'.resume-{time.time_ns()}.metadata.json')
    metadata_path.write_text(json.dumps(meta,indent=2)+'\n')
    rows=json.loads(output.read_text()) if args.resume else []
    keys=('repeat','adapters','variant','direction','payload_size','offered_pps','shape')
    completed={tuple(r[k] for k in keys) for r in rows}
    began=time.monotonic()
    for repeat in range(args.repeat):
        order=counts[repeat%len(counts):]+counts[:repeat%len(counts)]
        for direction,size,rate,shape in cases:
            for index,adapters in enumerate(order):
                if shape=='hot' and adapters==1:continue
                shift=(repeat+index)%len(variants)
                for variant in variants[shift:]+variants[:shift]:
                    if adapters==1 and variant!='shared':continue
                    if variant=='sharded' and adapters<=2:continue
                    key=(repeat,adapters,variant,direction,size,rate,shape)
                    if key in completed:continue
                    row=dict(zip(keys,key))
                    row.update(run(args,variant,direction,size,rate,adapters,shape,cpus))
                    row['metadata_file']=metadata_path.name
                    rows.append(row)
                    tmp=Path(str(output)+'.tmp')
                    tmp.write_text(json.dumps(rows,indent=2)+'\n');tmp.replace(output)
                    adcpu={d:round(sum(row['adapter_cpu_s'][d])/row['elapsed_s'],2) for d in ('rx','tx')}
                    print(f"{repeat} A={adapters} {variant:11s} {direction}:{size}:{rate:g}:{shape} "
                          f"delivered={row['pps']:.0f}pps loss={row['offered_loss_percent']:.2f}% "
                          f"CPU={row['switch_cpu_percent']:.0f}% adapterCPU={adcpu} "
                          f"P99={row['latency_p99_us']:.0f}us source/sink={row['source_peak_cpu_percent']:.0f}/{row['sink_peak_cpu_percent']:.0f}%",flush=True)
    print(f'PASS: {len(rows)} runs; identities, routing, FIFO, ownership, accounting, pools; {time.monotonic()-began:.1f}s',flush=True)


if __name__=='__main__':main()
