[![Makefile CI](https://github.com/mtzguido/ramon/actions/workflows/ci.yml/badge.svg)](https://github.com/mtzguido/ramon/actions/workflows/ci.yml)
# ramon
Resource monitoring

TODO:
- Use cgroups
- That will require `CAP_SYS_ADMIN`
- Report recursive (group) and individual (getrusage/wait4, or proc, kinda broken now)
- OPTIONAL polling to show a graph of CPU/memory usage

NOTE: if the invoked process has several threads,
they be accounte for together even in nonrecusive mode,
as they belong to the same process.
