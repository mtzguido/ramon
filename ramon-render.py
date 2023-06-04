#!/usr/bin/env python3

from parse import *

def load_file(fn):
    loads = {}
    marks = {}
    with open(fn) as f:
        for line in f:
            if not (search("poll" , line)):
                continue
            wall = search("wall={:g}", line).fixed[0]
            usage = search("usage={:g}", line).fixed[0]
            load = search("load={:g}", line).fixed[0]
            loads[wall] = load
    with open(fn) as f:
        for line in f:
            if not (search("mark" , line)):
                continue
            mark = search("str={:S}", line).fixed[0]
            wall = search("wall={:g}", line).fixed[0]
            marks[mark] = wall

    return loads, marks

def plot(fn, loads, marks):
    import matplotlib.pyplot as plt
    import numpy as np
    from math import ceil

    x_axis = []
    y_axis = []
    for t in loads.keys():
        x_axis.append(t)
        y_axis.append(loads[t])

    maxx = ceil(max(loads.keys()))
    maxy = ceil(max(loads.values()))
    nmarks = len(marks.keys())
    print("maxx = {}".format(maxx));
    print("maxy = {}".format(maxy));
    print("nmarks = {}".format(nmarks));

    plt.fill_between(x_axis, 0, y_axis)

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
    plt.savefig(imagefn, dpi=600)
    print("Saved image in {}".format(imagefn))

def main():
    import sys
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("file", help="ramon output with --poll")
    args = parser.parse_args()

    file = args.file

    loads, marks = load_file(file)

    plot(file, loads, marks)

main()


# TODO
# - Use fill graph
# - show subinvocation as stacked area 
# - better mark handling
# - SVG? PDF? Allow to zoom.
# - Soften?
