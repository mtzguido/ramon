#!/usr/bin/env python3

from parse import *
from result import *

cache={}

def parse_time(s):
    # fix
    return search("{:g}s", s).fixed[0]

def parse_mem(s):
    r = search("{:d}{:l}", s).fixed
    mem = r[0]
    memU = r[1]
    match memU:
        case "KiB":
            mem *= 1000;
        case "MiB":
            mem *= 1000000;
        case "GiB":
            mem *= 1000000000;
    return mem

def humanize(n):
    sufs=['', 'Ki', 'Mi', 'Gi', 'Ti']
    suf=0
    if n > 10000:
        n = n / 1000
        suf = suf + 1
    if n > 10000:
        n = n / 1000
        suf = suf + 1
    if n > 10000:
        n = n / 1000
        suf = suf + 1
    if n > 10000:
        n = n / 1000
        suf = suf + 1
    return str(n) + sufs[suf]

def do_load_ramon_file(fn):
    ret = {}
    ret["fn"] = fn.removesuffix(".ramon")
    with open(fn) as f:
        for line in f:
            comps = line.split()
            if comps[0] == "group.total":
                t = parse_time(comps[1])
                ret["time"] = t
            elif comps[0] == "group.mempeak":
                m = parse_mem(comps[1])
                ret["mem"] = m
            elif comps[0] == "exitcode":
                m = int(comps[1])
                ret["rc"] = m

    if not "rc" in ret or not "time" in ret or not "mem" in ret:
        print(f"Warning: ignoring {fn} since it is incomplete")
        return Err('incomplete')
    else:
        return Ok(ret)

def pi_fn(d): return d["fn"]
def pi_time(d): return d["time"]
def pi_mem(d): return d["mem"]

def load_ramon_file(fn):
    if fn in cache:
        return cache[fn]
    else:
        d = do_load_ramon_file(fn)
        cache[fn] = d
        return d

def find(root):
    from os import listdir
    from os.path import isfile, isdir, join
    #  print("find: " + root)
    ret = []
    all = listdir(root)
    for f in all:
        f2 = join(root, f)
        if isfile(f2) and f2.endswith(".ramon"):
            ret.append(f2)
        elif isdir(f2):
            ret.extend(find(f2))
    return ret

def mkmatching(r1, r2, _ds1, _ds2):
    ret = []
    ds1=[]
    ds1.extend(_ds1)
    ds2=[]
    ds2.extend(_ds2)
    ds1.sort(key=pi_fn)
    ds2.sort(key=pi_fn)
    while ds1 and ds2:
        d1 = ds1[0]
        d2 = ds2[0]
        h1 = d1['fn']
        h2 = d2['fn']
        th1 = h1.removeprefix(r1 + '/')
        th2 = h2.removeprefix(r2 + '/')
        if th1 == th2:
            # Only match if both succeeded, we do not compare
            # failed runs, or failed vs sucessful runs
            if d1['rc'] == 0 and d2['rc'] == 0:
                #  print("got a match between " + h1 + " and " + h2)
                m={}
                m['fn'] = th1
                m['l'] = d1
                m['r'] = d2
                ret.append(m)

            ds1.pop(0)
            ds2.pop(0)
        elif th1 < th2:
            ds1.pop(0)
        else:
            ds2.pop(0)
    return ret

def m_pi_timediff(m):
    return m['r']['time'] - m['l']['time']

def m_pi_timepercdiff(m):
    return (m['r']['time'] - m['l']['time']) / m['l']['time']

def m_pi_memdiff(m):
    return m['r']['mem'] - m['l']['mem']

def m_pi_mempercdiff(m):
    return (m['r']['mem'] - m['l']['mem']) / m['l']['mem']

# This repetition sucks, fix it.
def sort_and_print_match_mem(pi, n, ms, reverse=True):
    print(f"|{'FILE':90}  |{'MEM_L':12}  |{'MEM_R':12}  |{'DIFF':12}    |{'DIFF(%)':5}|")
    print(f"|-------------|-------------:|-------------:|--------------:|------------:|")
    ms.sort(key=pi, reverse=reverse)
    for m in ms[:n]:
        fn = m["fn"]
        mem_l = m['l']["mem"]
        mem_r = m['r']["mem"]
        mdiff = round(mem_r - mem_l, 3)
        mperc = round(100 * (mdiff / mem_l), 1)
        print(f"|{fn:90}  |{humanize(mem_l):0.9}B|{humanize(mem_r):0.9}B|{humanize(mdiff):0.9}B|{mperc:4}%|")

def sort_and_print_match(pi, n, ms, reverse=True):
    print(f"|{'FILE':90}  |{'TIME_L':9}  |{'TIME_R':9}  |{'DIFF(s)':9}  |{'DIFF(%)':5}|")
    print(f"|-------------|-------------:|-------------:|--------------:|------------:|")
    ms.sort(key=pi, reverse=reverse)
    for m in ms[:n]:
        fn = m["fn"]
        time_l = m['l']["time"]
        time_r = m['r']["time"]
        tdiff = round(time_r - time_l, 3)
        tperc = round(100 * (tdiff / time_l), 1)
        print(f"|{fn:90}  |{time_l:8.3f}s  |{time_r:8.3f}s  |{tdiff:8.3f}s  |{tperc:4}%|")

def sort_and_print(pi, n, ds):
    print(f"|{'FILE':90} |{'TIME':8} |{'MEM':11}|")
    print(f"|------------|----------:|---------:|")
    #  ds = list(map(load_ramon_file, fns))
    ds.sort(key=pi, reverse=True)
    for d in ds[:n]:
        fn = d["fn"]
        time = d["time"]
        mem = d["mem"]
        print(f"|{fn:90} |{time :8.3f}s |{humanize(mem):0.8}B|")

def ok_list(it):
    ret = []
    for i in it:
        match i:
            case Ok(e):
                ret.append(e)
            #  case Err(s):
    return ret

def begin_section(hdr):
    print()
    print()
    print(f"## {hdr}")
    print()
    print(f"<details><summary>{hdr}</summary>")
    print()

def end_section():
    print("</details>")

def go (r1, r2):
    f_lhs = find(r1)
    f_rhs = find(r2)

    lhs = ok_list(map(load_ramon_file, f_lhs))
    rhs = ok_list(map(load_ramon_file, f_rhs))
    all = lhs + rhs

    # replace 20 by -1 to print all

    print()
    print()
    print("# SUMMARY")
    nl = len(lhs)
    nr = len(rhs)
    sl = len(list(filter(lambda x : x['rc'] == 0, lhs)))
    sr = len(list(filter(lambda x : x['rc'] == 0, rhs)))
    pl = 100.0 * sl / nl if nl != 0 else 0
    pr = 100.0 * sl / nl if nl != 0 else 0

    print(f"- LHS tests = {nl}")
    print(f"- RHS tests = {nr}")
    print(f"- LHS success = {sl}  ({pl}%)")
    print(f"- RHS success = {sr}  ({pr}%)")

    matches = mkmatching(r1, r2, lhs, rhs)
    ##  print(matches)
    begin_section("TOP 20 RUNTIME INCREASE")
    sort_and_print_match(m_pi_timediff, 20, matches)
    end_section()

    begin_section("TOP 20 RUNTIME INCREASE (RELATIVE)")
    sort_and_print_match(m_pi_timepercdiff, 20, matches)
    end_section()

    begin_section("TOP 20 RUNTIME DECREASE")
    sort_and_print_match(m_pi_timediff, 20, matches, reverse=False)
    end_section()

    begin_section("TOP 20 RUNTIME DECREASE (RELATIVE)")
    sort_and_print_match(m_pi_timepercdiff, 20, matches, reverse=False)
    end_section()

    begin_section("TOP 20 LHS FILES, BY RUNTIME")
    sort_and_print(pi_time, 20, lhs)
    end_section()

    begin_section("TOP 20 RHS FILES, BY RUNTIME")
    sort_and_print(pi_time, 20, rhs)
    end_section()

    begin_section("TOP 20 MEMORY INCREASE")
    sort_and_print_match_mem(m_pi_memdiff, 20, matches)
    end_section()

    begin_section("TOP 20 MEMORY INCREASE (RELATIVE)")
    sort_and_print_match_mem(m_pi_mempercdiff, 20, matches)
    end_section()

    begin_section("TOP 20 MEMORY DECREASE")
    sort_and_print_match_mem(m_pi_memdiff, 20, matches, reverse=False)
    end_section()

    begin_section("TOP 20 MEMORY DECREASE (RELATIVE)")
    sort_and_print_match_mem(m_pi_mempercdiff, 20, matches, reverse=False)
    end_section()

    begin_section("TOP 20 LHS FILES, BY PEAK MEMORY USAGE")
    sort_and_print(pi_mem, 20, lhs)
    end_section()

    begin_section("TOP 20 RHS FILES, BY PEAK MEMORY USAGE")
    sort_and_print(pi_mem, 20, rhs)
    end_section()

    begin_section("FULL COMPARISON")
    sort_and_print_match(lambda x : x['fn'], -1, matches, reverse=False)
    end_section()

def maybe_extract_url(url):
    import os
    import tempfile

    if url.find("://") != -1:
        d = tempfile.mkdtemp(suffix="ramon.compare")
        print(f"LHS is URL, downloading to {d}")
        # Download the tarball in the URL to the directory
        os.system(f"wget -O {d}/lhs.tar.gz {url}")
        # Extract
        os.system(f"tar -xzf {d}/lhs.tar.gz -C {d}")
        return d
    else:
        return url

def main():
    import os
    import sys
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument('lhs', help='directory/URL for the old run')
    parser.add_argument('rhs', help='directory/URL for the new run')
    args = parser.parse_args()

    print(f'Comparing {args.lhs} and {args.rhs}')

    lhs = maybe_extract_url(args.lhs)
    rhs = maybe_extract_url(args.rhs)

    lhs = lhs.rstrip('/')
    rhs = rhs.rstrip('/')

    go(lhs, rhs)

main()
