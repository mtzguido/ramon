#!/usr/bin/env python3

from parse import *

def load_file(fn):
    intervals = {}

    with open(fn) as f:
        for line in f:
            if not (search("mark" , line)):
                continue
            label = search("str={:S}", line).fixed[0]
            wall = search("wall={:g}", line).fixed[0]

            if label.endswith('.0'):
                label = label.removesuffix('.0')
                if not (label in intervals):
                    intervals[label] = {}
                intervals[label]['start'] = wall
            elif label.endswith('.1'):
                label = label.removesuffix('.1')
                if not (label in intervals):
                    intervals[label] = {}
                intervals[label]['end'] = wall
            else:
              print (f"ignoring label {label}")

    return intervals

def color(intervals):
    # returns (label, start, end, color)
    intervals=list(intervals.items())
    intervals = list(filter((lambda x : 'end' in x[1]), intervals))
    intervals.sort(key=(lambda x : x[1]['start']))
    output = []
    running = {}
    maxcol = 0
    for i in intervals:
        #  print(f"i = {i}")
        start=i[1]['start']
        end=i[1]['end']
        color=0
        for color in range(0,999):
            print (f"trying {color}")
            if not (color in running) or running[color][1] <= start:
                running[color] = (i[0], end)
                break

        if color > maxcol:
            maxcol = color
        e=(i[0], start, end, color)
        output.append(e)

    return maxcol+1, output

def plot(fn, nproc, sch):
    import matplotlib.pyplot as plt

    # Horizontal bar plot with gaps
    fig, ax = plt.subplots()
    for i in range(0, nproc):
        pairs = list(map((lambda x: (x[1], x[2] - x[1])), filter (lambda x: x[3] == i, sch)))
        ax.broken_barh(pairs, (10*i + 5, 9));

    #  ax.broken_barh([(110, 30), (150, 10)], (10, 9))
    #  ax.broken_barh([(10, 50), (100, 20), (130, 10)], (20, 9))
    ax.set_ylim(0, 10 + 10 * (nproc+1))
    ax.set_xlim(0, max(map(lambda x: x[2], sch)))
    ax.set_xlabel('seconds since start')
    ax.set_yticks(list(map(lambda i: 10*i + 5, range(0,nproc))), labels=list(map(lambda i: 1+i, range(0,nproc))))

    ax.grid(True)                                       # Make grid lines visible

    #  plt.show()


    #  import pandas as pd
    #  import plotly.express as px

    #  df = pd.DataFrame(map((lambda e : dict(Task=e[0], Start=100*e[1], End=100*e[2])), sch))

    #  #  df = pd.DataFrame([
    #  #          dict(Task="1", Start='2023-03-15', End='2023-03-15'),
    #  #          dict(Task="2", Start='2023-03-03', End='2023-03-10'),
    #  #          dict(Task="3", Start='2023-03-10', End='2023-03-15'),
    #  #  ])

    #  print(df)

    #  fig = px.timeline(df, x_start="Start", x_end="End", y="Task")
    #  fig.update_yaxes()
    #  fig.update_xaxes()

    imagefn = fn + ".gantt.png"
    #  fig.write_image(imagefn)
    plt.show()

    print("Saved image in {}".format(imagefn))
    return imagefn

def main():
    import os
    import sys
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("file", help="ramon output with marks")
    parser.add_argument("--open", action="store_true", help="open the generated image")
    args = parser.parse_args()

    file = args.file

    intervals = load_file(file)

    maxcol, sch = color(intervals)

    print(sch)

    img = plot(file, maxcol, sch)

    if args.open:
        os.system("xdg-open " + img)
main()


# TODO
# - Use fill graph
# - show subinvocation as stacked area
# - better mark handling
# - SVG? PDF? Allow to zoom.
# - Soften?
