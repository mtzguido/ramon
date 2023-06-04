#!/usr/bin/env python3

from parse import *
import matplotlib

def load_file(fn):
    loads = {}
    with open(fn) as f:
        for line in f:
            if not (search("poll" , line)):
                continue
            wall = search("wall={:g}", line).fixed
            usage = search("usage={:g}", line).fixed
            load = search("load={:g}", line).fixed
            loads[wall] = load
    return loads, 1

def plot(fn, load):
    import matplotlib.pyplot as plt
    import numpy as np

    x_axis = []
    y_axis = []
    for t in load.keys():
        x_axis.append(t)
        y_axis.append(load[t])

    plt.plot(x_axis, y_axis)

    plt.xlabel("Wall clock time")
    plt.ylabel("Load")

    plt.title("Load average across time")

    plt.savefig(fn + ".png", dpi=500)

def main():
    import sys
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("file", help="ramon output with --poll")
    args = parser.parse_args()

    file = args.file

    load, totaltime = load_file(file)

    plot(file, load)

main()
