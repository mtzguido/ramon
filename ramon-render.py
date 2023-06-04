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

    x_axis = []
    y_axis = []
    for t in loads.keys():
        x_axis.append(t)
        y_axis.append(loads[t])

    plt.plot(x_axis, y_axis)

    lasttime = -10
    lasty = -5
    for tag in marks.keys():
        time = marks[tag]
        if (lasttime + 20 >= time):
            y = lasty + 1
        else:
            y = -5
        plt.vlines(time, -5, 29, colors='C1', label=tag);
        plt.text(time, y, tag, rotation=45, size='smaller')
        lasttime = time
        lasty = y
    #plt.legend()

    plt.xlabel("Wall clock time")
    plt.ylabel("Load")
    
    plt.title("Load average across time")

    imagefn = fn + ".png"
    plt.savefig(imagefn, dpi=500)
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
