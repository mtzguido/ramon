[![Makefile CI](https://github.com/mtzguido/ramon/actions/workflows/ci.yml/badge.svg)](https://github.com/mtzguido/ramon/actions/workflows/ci.yml)
# ramon
Resource monitoring

TODO:
- Sort out cgroups1 vs cgroups2, can we support both?
- make hierarchical: find current cgroup and nest within itA
- handle SIGINT and others (check)
- wait for group option
- config verbosity?
- JSON?
- human output/input

NOTE: if the invoked process has several threads,
they be accounte for together even in nonrecusive mode,
as they belong to the same process.

- You can trigger a poll by sending a SIGALRM to ramon.
