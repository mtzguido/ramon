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
            mem *= 1000;
        case "MiB":
            mem *= 1000000;
        case "GiB":
            mem *= 1000000000;
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

def main():
    import os
    import sys
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("file", help="ramon output with --poll")
    parser.add_argument("--open", action="store_true", help="open the generated image")
    args = parser.parse_args()

    file = args.file

    rloads, loads, mems, marks = load_file(file)

    if args.open:
        os.system("xdg-open " + img)

def mkmatching(r1, r2, _fs1, _fs2):
    ret = []
    fs1=[]
    fs1.extend(_fs1)
    fs2=[]
    fs2.extend(_fs2)
    fs1.sort()
    fs2.sort()
    while fs1 and fs2:
        h1 = fs1[0]
        h2 = fs2[0]
        d1 = load_ramon_file(h1)
        d2 = load_ramon_file(h2)
        th1 = h1.removeprefix(r1)
        th2 = h2.removeprefix(r2)
        if th1 == th2:
            #  print("got a match between " + h1 + " and " + h2)
            m={}
            m['fn'] = th1
            m['l'] = d1
            m['r'] = d2
            ret.append(m)
            fs1.pop(0)
            fs2.pop(0)
        elif th1 < th2:
            fs1.pop(0)
        else:
            fs2.pop(0)
    return ret

def pi_time(d): return d["time"]
def pi_mem(d): return d["mem"]

def m_pi_timediff(m):
    return m['r']['time'] - m['l']['time']

def m_pi_percdiff(m):
    return (m['r']['time'] - m['l']['time']) / m['l']['time']

def sort_and_print_match(pi, n, ms, reverse=True):
    print(f"{'FILE':70}  {'TIME_L':9}  {'TIME_R':9}  {'DIFF(s)':9}  {'DIFF(%)':5}")
    ms.sort(key=pi, reverse=reverse)
    for m in ms[:n]:
        fn = m["fn"]
        time_l = m['l']["time"]
        time_r = m['r']["time"]
        tdiff = round(time_r - time_l, 3)
        tperc = round(100 * (tdiff / time_l), 1)
        print(f"{fn:70}  {time_l:8}s  {time_r:8}s  {tdiff:8}s  {tperc:4}%")

def sort_and_print(pi, n, fns):
    print(f"{'FILE':70} {'TIME':8} {'MEM':11}")
    ds = list(map(load_ramon_file, fns))
    ds.sort(key=pi, reverse=True)
    for d in ds[:n]:
        fn = d["fn"]
        time = d["time"]
        mem = d["mem"]
        mem = round(mem / 1024)
        print(f"{fn:70} {time:8}s {mem:8}KiB")

def go (r1, r2):
    lhs = find(r1)
    rhs = find(r2)
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
    sort_and_print_match(m_pi_percdiff, 20, matches)

    print()
    print()
    print("# TOP 20 RUNTIME DECREASE")
    print("#############################################")
    sort_and_print_match(m_pi_timediff, 20, matches, reverse=False)

    print()
    print()
    print("# TOP 20 RUNTIME DECREASE (RELATIVE)")
    print("#############################################")
    sort_and_print_match(m_pi_percdiff, 20, matches, reverse=False)

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
