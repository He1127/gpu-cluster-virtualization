"""Build-free orchestration of the real C/Winsock programs; Python does not do the network work."""
from pathlib import Path
import argparse
import csv
from datetime import datetime
import hashlib
import json
import os
import platform
import re
import socket
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT/'bin/socket_lab.exe'
HIDDEN = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def choose_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1',0))
        return s.getsockname()[1]

def execute(folder, server_options, *, worker_options=(), fault=False):
    folder.mkdir(parents=True)
    port = choose_port()
    commands = [[str(EXE),'--role','server','--port',str(port),'--workers','3','--threads','2','--timeout','20',*map(str,server_options)]]
    processes=[]; streams=[]
    started=datetime.now().astimezone().isoformat()
    begin=time.perf_counter(); killed=False
    try:
        for i in range(4):
            if i:
                commands.append([str(EXE),'--role','worker','--id',str(i),'--port',str(port),'--threads','2','--timeout','20',*map(str,worker_options)])
            stream=(folder/('server.log' if not i else f'worker{i}.log')).open('wb')
            streams.append(stream)
            processes.append(subprocess.Popen(commands[i],cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT,creationflags=HIDDEN))
            if not i:
                for _ in range(100):
                    if processes[0].poll() is not None: break
                    if 'LISTEN ' in (folder/'server.log').read_text(encoding='utf-8',errors='replace'): break
                    time.sleep(.02)
        if fault:
            for _ in range(200):
                if 'TASK_SPLIT ' in (folder/'server.log').read_text(encoding='utf-8',errors='replace'): break
                if processes[0].poll() is not None: break
                time.sleep(.01)
            processes[1].kill(); killed=True
        for process in processes:
            process.wait(timeout=max(1,45-(time.perf_counter()-begin)))
    finally:
        for process in processes:
            if process.poll() is None: process.kill(); process.wait()
        for stream in streams: stream.close()
    codes=[p.returncode for p in processes]
    text=(folder/'server.log').read_text(encoding='utf-8',errors='strict')
    if fault:
        ok=killed and codes[0]!=0 and 'RESULT=FAIL' in text and time.perf_counter()-begin<45
    else:
        ok=all(c==0 for c in codes) and 'RESULT=PASS role=server' in text and all('RESULT=PASS role=worker' in (folder/f'worker{i}.log').read_text(encoding='utf-8') for i in (1,2,3))
    session={'started':started,'finished':datetime.now().astimezone().isoformat(),'commands':commands,
             'pids':[p.pid for p in processes],'exit_codes':codes,'executable_sha256':sha(EXE),
             'elapsed_wall_s':time.perf_counter()-begin,'fault_worker1_killed':killed,
             'expected_outcome':'server detects failure' if fault else 'all four processes pass',
             'check':'PASS' if ok else 'FAIL'}
    (folder/'session.json').write_text(json.dumps(session,ensure_ascii=False,indent=2),encoding='utf-8')
    if not ok: raise RuntimeError(f'Case failed: {folder.name}, exits={codes}')
    rows=[]
    for line in text.splitlines():
        if line.startswith('CSV,'):
            values=next(csv.reader([line]))
            rows.append(dict(kind=values[1],link_or_count=int(values[2]),bytes=int(values[3]),
                             elapsed_s=float(values[4]),GBps=float(values[5]),Gbps=float(values[6])))
    if rows:
        aggregate=[r for r in rows if r['kind']=='aggregate'][0]
        links=[r for r in rows if r['kind']=='link']
        assert aggregate['bytes']==sum(r['bytes'] for r in links)
        assert abs(aggregate['elapsed_s']-max(r['elapsed_s'] for r in links))<2e-8
        assert abs(aggregate['GBps']-aggregate['bytes']/aggregate['elapsed_s']/1e9)<1e-6
    return session,text,rows

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',required=True,help='New batch directory relative to lab root, or absolute path')
    parser.add_argument('--full',action='store_true',help='72 measured cases, 512 MiB/link, plus six warmups')
    args=parser.parse_args()
    out=Path(args.out); out=out if out.is_absolute() else ROOT/out
    if out.exists(): raise FileExistsError('Use a NEW --out directory; evidence will not be overwritten.')
    if not EXE.is_file(): raise FileNotFoundError('Build first: scripts/build.cmd')
    out.mkdir(parents=True); (out/'screenshots').mkdir(); (out/'views').mkdir()
    environment={'date':datetime.now().astimezone().isoformat(),'platform':platform.platform(),
                 'python':platform.python_version(),'logical_processors':os.cpu_count(),
                 'executable_sha256':sha(EXE),'source_sha256':sha(ROOT/'src/socket_lab.c'),
                 'mode':'full' if args.full else 'quick','address':'127.0.0.1'}
    (out/'environment.json').write_text(json.dumps(environment,ensure_ascii=False,indent=2),encoding='utf-8')
    checks=[]
    def report(message): print(message,flush=True)
    session,text,_=execute(out/'01-task',['--mode','task','--elements','1000003'])
    parts=re.findall(r'TASK_RESULT worker=(\d+) offset=(\d+) count=(\d+) partial=(\d+) expected=(\d+)',text)
    assert len(parts)==3
    parts=sorted([[int(v) for v in row] for row in parts])
    assert parts[0][1]==0 and sum(p[2] for p in parts)==1000003
    assert all(parts[i][1]+parts[i][2]==parts[i+1][1] for i in (0,1))
    expected=sum((i*17+23)%1009 for i in range(1000003))
    assert sum(p[3] for p in parts)==expected
    checks.append({'test':'task_split_merge','check':'PASS','elements':1000003,'reference':expected,'parts':parts})
    report('TASK split/merge PASS')
    execute(out/'02-fragmented',['--mode','task','--elements','1003','--io-slice','7'],worker_options=['--io-slice','7'])
    checks.append({'test':'seven_byte_io_slices','check':'PASS','elements':1003})
    for direction in ('down','up'):
        execute(out/f'03-tail-{direction}',['--mode','bench','--direction',direction,'--active','0','--bytes','1048580','--chunk','65536'])
    checks.append({'test':'unaligned_final_chunk_both_directions','check':'PASS','payload_bytes_per_link':1048580})
    execute(out/'04-disconnect',['--mode','task','--elements','1003'],worker_options=['--delay-ms','3000'],fault=True)
    checks.append({'test':'worker1_disconnect_detected','check':'PASS','expected_application_outcome':'FAIL/nonzero'})
    report('FRAMING / TAIL / DISCONNECT checks PASS')
    measured=[]
    chunks=(16384,65536,262144) if args.full else (65536,)
    repeats=3 if args.full else 1
    mib=512 if args.full else 32
    actives=(1,2,3,0) if args.full else (1,0)
    for direction in ('down','up'):
        for chunk in chunks:
            execute(out/f'warmup-{direction}-c{chunk}',['--mode','bench','--direction',direction,'--active','0','--mib','32','--chunk',str(chunk)])
            for active in actives:
                for repeat in range(1,repeats+1):
                    name=f'bench-{direction}-c{chunk}-a{active}-r{repeat}'
                    _,_,rows=execute(out/name,['--mode','bench','--direction',direction,'--active',str(active),'--mib',str(mib),'--chunk',str(chunk)])
                    for row in rows:
                        measured.append(dict(case=name,direction=direction,chunk_bytes=chunk,active=active,repeat=repeat,**row))
                    agg=next(r for r in rows if r['kind']=='aggregate')
                    report(f'{name}: {agg["GBps"]:.6f} GB/s, application verified')
    with (out/'measurements.csv').open('w',encoding='utf-8',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=list(measured[0])); writer.writeheader(); writer.writerows(measured)
    statistics_rows=[]
    for direction in ('down','up'):
        for chunk in chunks:
            for active in actives:
                selected=[r for r in measured if r['direction']==direction and r['chunk_bytes']==chunk and r['active']==active and r['kind']=='aggregate']
                rates=[r['GBps'] for r in selected]
                statistics_rows.append({'direction':direction,'chunk_bytes':chunk,'active':active,'repeats':len(rates),
                                        'min_GBps':min(rates),'median_GBps':statistics.median(rates),'max_GBps':max(rates)})
    summary={'environment':environment,'checks':checks,'task_parts':parts,'task_reference':expected,
             'payload_mib_per_link':mib,'repeats':repeats,'block_bytes':list(chunks),
             'measured_cases':len(set(r['case'] for r in measured)),'warmup_cases':len(chunks)*2,
             'statistics':statistics_rows,'result':'PASS',
             'scope':'Local loopback application payload throughput including pattern generation/check, framing, scheduling and application acknowledgements; not NIC bandwidth or a universal maximum.'}
    (out/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
    lines=['SOCKET LAB 03 - REAL RUN SUMMARY',f'Address=127.0.0.1  Processes=1 coordinator + 3 workers',
           f'Data I/O=Winsock OVERLAPPED + IOCP, 2 completion threads/process',
           f'Task: N=1000003, merged={expected}, reference={expected}, PASS',
           'Framed 7-byte I/O / final partial block / disconnect detection: PASS',
           f'Payload={mib} MiB/link, repeats={repeats}; rates below are application GB/s',
           'direction  chunk_KiB  active  min_GB/s  median_GB/s  max_GB/s']
    for row in statistics_rows:
        lines.append(f'{row["direction"]:9} {row["chunk_bytes"]//1024:9} {row["active"]:7} {row["min_GBps"]:9.4f} {row["median_GBps"]:12.4f} {row["max_GBps"]:10.4f}')
    lines+=['active=0 means THREE concurrent links; active=1/2/3 means one link.',
            'Aggregate=sum(payload bytes)/(common start to last application DONE).',
            'Loopback only; CPU/check/copy costs included. No physical NIC limit claimed.', 'RESULT=PASS']
    (out/'summary.log').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    report(f'ALL PASS; saved {out}')

if __name__=='__main__': main()
