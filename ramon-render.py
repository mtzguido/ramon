#!/usr/bin/env python3

from parse import *

def load_file(fn):
    loads = {}
    marks = {}
    mems = {}
    with open(fn) as f:
        for line in f:
            if not (search("poll" , line)):
                continue
            wall = search("wall={:g}", line).fixed[0]
            usage = search("usage={:g}", line).fixed[0]
            mem = search("mem={:d}", line).fixed[0]
            load = search("load={:g}", line).fixed[0]
            loads[wall] = load
            mems[wall] = mem
    with open(fn) as f:
        for line in f:
            if not (search("mark" , line)):
                continue
            mark = search("str={:S}", line).fixed[0]
            wall = search("wall={:g}", line).fixed[0]
            marks[mark] = wall

    return loads, mems, marks

def plot(fn, loads, mem, marks):
    import matplotlib.pyplot as plt
    import numpy as np
    from math import ceil

    plt.figure(figsize=(12, 4), dpi=400)

    x_axis = []
    y_axis = []
    y2_axis = []
    for t in loads.keys():
        x_axis.append(t)
        y_axis.append(loads[t])
        y2_axis.append(mem[t] / 1000000000)

    maxx = ceil(max(loads.keys(), default=1))
    maxy = ceil(max(loads.values(), default=1))
    nmarks = len(marks.keys())
    print("maxx = {}".format(maxx));
    print("maxy = {}".format(maxy));
    print("nmarks = {}".format(nmarks));

    plt.fill_between(x_axis, 0, y_axis)
    plt.plot(x_axis, y2_axis, color='C2')

    lasttime = -10
    lasty = 0
    marki = 0
    for tag in marks.keys():
        time = marks[tag]
        #  if (lasttime + 50 >= time):
        #      y = lasty - 2
        #  else:
        #      y = 0
        y = (nmarks - marki) / nmarks * maxy
        plt.vlines(time, 0, maxy, colors='C1', label=tag, linestyles='dashed', linewidth=0.3);
        plt.text(time, y, tag, rotation=45, size=4)
        #  lasttime = time
        #  lasty = y
        marki = 1 + marki
    #plt.legend()
    plt.hlines(range(1+maxy), 0, maxx, colors='gray', linestyles='solid', alpha=0.5, linewidth=0.3);

    plt.xlabel("Wall clock time")
    plt.ylabel("Load")
    
    plt.title("Load average across time")

    imagefn = fn + ".png"
    plt.savefig(imagefn)
    print("Saved image in {}".format(imagefn))

def main():
    import sys
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("file", help="ramon output with --poll")
    args = parser.parse_args()

    file = args.file

    loads, mem, marks = load_file(file)

    plot(file, loads, mem, marks)

main()


# TODO
# - Use fill graph
# - show subinvocation as stacked area 
# - better mark handling
# - SVG? PDF? Allow to zoom.
# - Soften?
