[![Makefile CI](https://github.com/mtzguido/ramon/actions/workflows/ci.yml/badge.svg)](https://github.com/mtzguido/ramon/actions/workflows/ci.yml)
# ramon
Resource monitoring

TODO:
- Sort out cgroups1 vs cgroups2, can we support both?
- Report recursive (group) and individual (getrusage/wait4, or proc, kinda broken now)
- OPTIONAL polling to show a graph of CPU/memory usage
- make hierarchical: find current cgroup and nest within it

NOTE: if the invoked process has several threads,
they be accounte for together even in nonrecusive mode,
as they belong to the same process.
