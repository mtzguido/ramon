[![Makefile CI](https://github.com/mtzguido/ramon/actions/workflows/ci.yml/badge.svg)](https://github.com/mtzguido/ramon/actions/workflows/ci.yml)
# ramon
Resource monitoring

TODO:
- Use cgroups
- That will require `CAP_SYS_ADMIN`
- Report recursive (group) and individual (getrusage/wait4)

NOTE: if the invoked process has several threads,
they be accounte for together even in nonrecusive mode,
as they bleong to the same process.
