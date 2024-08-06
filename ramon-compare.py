#!/usr/bin/env python3

from parse import *

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
            mem *= 1;
        case "MiB":
            mem *= 1000;
        case "GiB":
            mem *= 1000000;
    return mem

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
    return ret

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
        th1 = h1.removeprefix(r1)
        th2 = h2.removeprefix(r2)
        if th1 == th2:
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
    print(f"{'FILE':90}  {'MEM_L':12}  {'MEM_R':12}  {'DIFF':12}  {'DIFF(%)':5}")
    ms.sort(key=pi, reverse=reverse)
    for m in ms[:n]:
        fn = m["fn"]
        time_l = m['l']["mem"]
        time_r = m['r']["mem"]
        tdiff = round(time_r - time_l, 3)
        tperc = round(100 * (tdiff / time_l), 1)
        print(f"{fn:90}  {time_l:9}KiB  {time_r:9}KiB  {tdiff:9}KiB  {tperc:4}%")

def sort_and_print_match(pi, n, ms, reverse=True):
    print(f"{'FILE':90}  {'TIME_L':9}  {'TIME_R':9}  {'DIFF(s)':9}  {'DIFF(%)':5}")
    ms.sort(key=pi, reverse=reverse)
    for m in ms[:n]:
        fn = m["fn"]
        time_l = m['l']["time"]
        time_r = m['r']["time"]
        tdiff = round(time_r - time_l, 3)
        tperc = round(100 * (tdiff / time_l), 1)
        print(f"{fn:90}  {time_l:8}s  {time_r:8}s  {tdiff:8}s  {tperc:4}%")

def sort_and_print(pi, n, ds):
    print(f"{'FILE':90} {'TIME':8} {'MEM':11}")
    #  ds = list(map(load_ramon_file, fns))
    ds.sort(key=pi, reverse=True)
    for d in ds[:n]:
        fn = d["fn"]
        time = d["time"]
        mem = d["mem"]
        mem = round(mem / 1024)
        print(f"{fn:90} {time:8}s {mem:8}KiB")

def go (r1, r2):
    f_lhs = find(r1)
    f_rhs = find(r2)

    lhs = list(map(load_ramon_file, f_lhs))
    rhs = list(map(load_ramon_file, f_rhs))
    all = lhs + rhs

    # replace 20 by -1 to print all

    matches = mkmatching(r1, r2, lhs, rhs)
    #  print(matches)
    print()
    print()
    print("# TOP 20 RUNTIME INCREASE")
    print("#############################################")
    sort_and_print_match(m_pi_timediff, 20, matches)

    print()
    print()
    print("# TOP 20 RUNTIME INCREASE (RELATIVE)")
    print("#############################################")
    sort_and_print_match(m_pi_timepercdiff, 20, matches)

    print()
    print()
    print("# TOP 20 RUNTIME DECREASE")
    print("#############################################")
    sort_and_print_match(m_pi_timediff, 20, matches, reverse=False)

    print()
    print()
    print("# TOP 20 RUNTIME DECREASE (RELATIVE)")
    print("#############################################")
    sort_and_print_match(m_pi_timepercdiff, 20, matches, reverse=False)

    print()
    print()
    print("# TOP 20 LHS FILES, BY RUNTIME")
    print("#############################################")
    sort_and_print(pi_time, 20, lhs)

    print()
    print()
    print("# TOP 20 RHS FILES, BY RUNTIME")
    print("#############################################")
    sort_and_print(pi_time, 20, rhs)

    print()
    print()
    print("# TOP 20 MEMORY INCREASE")
    print("#############################################")
    sort_and_print_match_mem(m_pi_memdiff, 20, matches)

    print()
    print()
    print("# TOP 20 MEMORY INCREASE (RELATIVE)")
    print("#############################################")
    sort_and_print_match_mem(m_pi_mempercdiff, 20, matches)

    print()
    print()
    print("# TOP 20 MEMORY DECREASE")
    print("#############################################")
    sort_and_print_match_mem(m_pi_memdiff, 20, matches, reverse=False)

    print()
    print()
    print("# TOP 20 MEMORY DECREASE (RELATIVE)")
    print("#############################################")
    sort_and_print_match_mem(m_pi_mempercdiff, 20, matches, reverse=False)

    print()
    print()
    print("# TOP 20 LHS FILES, BY PEAK MEMORY USAGE")
    print("#############################################")
    sort_and_print(pi_mem, 20, lhs)

    print()
    print()
    print("# TOP 20 RHS FILES, BY PEAK MEMORY USAGE")
    print("#############################################")
    sort_and_print(pi_mem, 20, rhs)

def main():
    import os
    import sys
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument('lhs', help='directory for the old run')
    parser.add_argument('rhs', help='directory for the new run')
    args = parser.parse_args()

    print(f'Comparing {args.lhs} and {args.rhs}')

    lhs = args.lhs.rstrip('/')
    rhs = args.rhs.rstrip('/')

    go(lhs, rhs)

main()
