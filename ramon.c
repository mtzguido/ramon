#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <linux/limits.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>
#include <assert.h>

struct cfg {
	const char *outfile;
	FILE *fout;
	bool recursive;
	bool keep;
};

int cgroup_fd = 0;
char cgroup_path[PATH_MAX];

void quit(char *s)
{
	perror(s);
	exit(1);
}

/* Warn into stderr */
void warn(const char *fmt, ...)
{
	va_list va;

	fprintf(stderr, "WARNING: ramon: ");

	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	va_end(va);
	fputs("\n", stderr);
}

void __dbg(const char *fmt, ...)
{
	va_list va;

	fprintf(stderr, "DEBUG: ramon: ");

	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	va_end(va);
	fputs("\n", stderr);
}

#if 1
#define dbg(...) __dbg(__VA_ARGS__)
#else
#define dbg(...)
#endif

/* Global config state */
struct cfg cfg = {
	.outfile = NULL,
	.fout = NULL, /* set to stderr by main() */
	.recursive = false,
	.keep = false,
};

const struct option longopts[] = {
	{ .name = "output",       .has_arg = required_argument, .flag = NULL, .val = 'o' },
	{ .name = "recursive",    .has_arg = no_argument,       .flag = NULL, .val = 'r' }, // FIXME: cook up a library for this crap
	{ .name = "no-recursive", .has_arg = no_argument,       .flag = NULL, .val = '1' },
	{ .name = "keep-cgroup",  .has_arg = no_argument,       .flag = NULL, .val = 'k' },
	{0},
};

void parse_opts(int argc, char **argv)
{
	int rc;

	while (1) {
		rc = getopt_long(argc, argv, "+o:r1k", longopts, NULL);
		switch (rc) {
		case 'o':
			cfg.outfile = optarg;
			break;

		case 'r':
			warn("ignored");
			cfg.recursive = true;
			break;

		case '1':
			warn("ignored");
			cfg.recursive = false;
			break;

		case 'k':
			cfg.keep = true;
			break;

		case -1:
			return;

		case '?':
		default:
			exit(1);
		}
	}
}

void usage()
{
	fprintf(stderr, "rtfm!\n");
}

void outf(const char *key, const char *fmt, ...)
{
	va_list va;

	if (cfg.fout == stderr)
		fprintf(cfg.fout, "ramon: ");

	fprintf(cfg.fout, "%-16s", key);

	va_start(va, fmt);
	vfprintf(cfg.fout, fmt, va);
	va_end(va);
	fputs("\n", cfg.fout);
}

const char *wifstring(int status)
{
	if (WIFEXITED(status)) return "exited";
	if (WIFSIGNALED(status)) return "terminated by signal";
	return "unknown (please file bug report)";
}

struct rusage rusage_comb(const struct rusage r1, const struct rusage r2, int k)
{
	struct rusage ret = {0};

#define C1(f) {ret.f = r1.f + k * r2.f;}

	C1(ru_utime.tv_sec);
	C1(ru_utime.tv_usec);
	C1(ru_maxrss);

#undef C1

	return ret;
}

struct rusage rusage_add(const struct rusage r1, const struct rusage r2)
{
	return rusage_comb(r1, r2, 1);
}
struct rusage rusage_sub(const struct rusage r1, const struct rusage r2)
{
	return rusage_comb(r1, r2, -1);
}

static const char *signame(int sig)
{
	/* ideally use sigabbrev_np, but seems to be rather recent */
#if __GLIBC >= 3 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 32)
	return sigabbrev_np(sig);
#else
	return sys_siglist[sig] + 3; /* skip "SIG" */
#endif
}

void find_cgroup_fs()
{
	char buf[PATH_MAX];

	FILE *f = fopen("/proc/mounts", "r");
	if (!f)
		quit("fopen mounts");

	while (fscanf(f, "%s", buf) > 0) {
		if (strcmp(buf, "cgroup2")) {
			/* skip line */
			int c;
			while ((c = fgetc(f)) != '\n' && c != EOF)
				;
			continue;
		}

		fscanf(f, "%s", buf);
		// FIXME: using /ramon/XXX mempeak is not found, wtf?
		strcat(buf, "/ramon_run_XXXXXX");
		char *p = mkdtemp(buf);
		if (!p)
			quit("mkdtemp");

		/* Make the directory readable by group/other, as usual. */
		chmod(p, 0755);

		strcpy(cgroup_path, buf);
		dbg("cgroup is '%s'", cgroup_path);

		cgroup_fd = open(buf, O_DIRECTORY);
		if (cgroup_fd < 0)
			quit("open cgroup dir");
		return;
	}
	quit("did not find cgroup2 mount");
}

void try_rm_cgroup()
{
	int rc;

	rc = rmdir(cgroup_path);
	if (rc < 0 && errno != ENOENT)
		quit("rmdir");
}

void put_in_cgroup(int child_pid)
{
	int rc;
	char pidbuf[100];

	int fd = openat(cgroup_fd, "cgroup.procs", O_WRONLY);
	if (fd < 0)
		quit("open cgroup");

	sprintf(pidbuf, "%i", child_pid);
	rc = write(fd, pidbuf, strlen(pidbuf));
	if (rc < (ssize_t)strlen(pidbuf))
		quit("write");
	close(fd);
}

FILE *fopenat(int dirfd, const char *pathname, int flags)
{
	int fd = openat(dirfd, pathname, flags);
	if (fd < 0)
		return NULL;
	// FIXME: mode
	return fdopen(fd, flags == O_RDONLY ? "r" : "w");
}

void destroy_cgroup()
{
	unsigned long n;
	bool kill = false;
	FILE *f;

	f = fopenat(cgroup_fd, "cgroup.procs", O_RDONLY);
	if (!f)
		quit("open cgroup");

	while (fscanf (f, "%lu", &n) > 0) {
		warn("A subprocess is still alive after main process finished (pid = %lu)", n);
		kill = true;
	}
	fclose(f);

	if (kill) {
		warn("Killing remaining processes");
		f = fopenat(cgroup_fd, "cgroup.kill", O_WRONLY);
		if (!f)
			quit("open cgroup");
		fwrite("1", 1, 1, f);
		fclose(f);
	}

	if (!cfg.keep)
		try_rm_cgroup();
	else
		warn("Keeping cgroup in path '%s', you should manually delete it eventually.", cgroup_path);
}

/* FIXME, very heuristic */
unsigned long humanize(unsigned long x, const char **suf)
{
	static const char* sufs[] = { "", "K", "M", "G", "T", "P", NULL };
	int pow = 0;

	while (x > 102400 && sufs[pow+1] != NULL) {
		pow++;
		x /= 1024;
	}
	*suf = sufs[pow];
	return x;
}

struct kvfmt {
	const char *key;
	const char *fmt;
	void *wo;
};

int read_kvs(FILE *f, int nk, struct kvfmt kvs[])
{
	char key[256]; // review
	int n = 0;
	int i;

	while (!feof(f)) {
		if (1 != fscanf(f, "%s", key))
			return n;

		for (i = 0; i < nk; i++) {
			if (strcmp(kvs[i].key, key))
				continue;

			if (1 != fscanf(f, kvs[i].fmt, kvs[i].wo)) {
				warn("scan failed for %s", key);
			}
			n++;
			break;
		}
		/* no match, skip line */
		if (i == nk) {
			/* skip line */
			int c;
			while ((c = fgetc(f)) != '\n' && c != EOF)
				;
			continue;
		}
	}

	return n;
}

int open_and_read_kvs(int dirfd, const char *pathname, int nk, struct kvfmt kvs[])
{
	FILE *f = fopenat(dirfd, pathname, O_RDONLY);
	if (!f) {
		warn("could not open %s", pathname);
		return -1;
	}
	int n = read_kvs(f, nk, kvs);
	fclose(f);
	if (n < nk)
		warn("could not read everything");
	return n;
}

int open_and_read_val(int dirfd, const char *pathname, const char *fmt, void *wo)
{
	FILE *f = fopenat(dirfd, pathname, O_RDONLY);
	if (!f) {
		warn("could not open %s", pathname);
		return -1;
	}
	int rc = fscanf(f, fmt, wo);
	fclose(f);
	if (rc != 1)
		warn("could not read value");
	return rc;
}

void read_cgroup()
{
	{
		unsigned long usage, user, system;
		struct kvfmt cpukeys[] = {
			{ .key = "usage_usec",  .fmt = "%lu", .wo = &usage  },
			{ .key = "user_usec",   .fmt = "%lu", .wo = &user   },
			{ .key = "system_usec", .fmt = "%lu", .wo = &system },
		};

		open_and_read_kvs(cgroup_fd, "cpu.stat", 3, cpukeys);

		outf("cgroup.usage", "%.3fs", usage / 1000000.0);
		outf("cgroup.user", "%.3fs", user / 1000000.0);
		outf("cgroup.system", "%.3fs", system/ 1000000.0);
	}

	{
		unsigned long mempeak;
		const char *suf;

		if (open_and_read_val(cgroup_fd, "memory.peak", "%lu", &mempeak) > 0) {
			mempeak = humanize(mempeak, &suf);
			outf("cgroup.mempeak", "%lu%sB", mempeak, suf);
		}
	}
	{
		unsigned long pidpeak;
		if (open_and_read_val(cgroup_fd, "pids.peak", "%lu", &pidpeak) > 0)
			outf("cgroup.pidpeak", "%lu", pidpeak);
	}

}

void monitor(struct timespec *t0, int pid)
{
	struct timespec t1;
	/* struct rusage self; */
	struct rusage child;
	int status;
	int rc;
	unsigned long rt_usec;
	unsigned long total_usec;

	rc = wait4(pid, &status, 0, &child);
	if (rc < 0)
		quit("wait4");

	clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

	outf("status", wifstring(status));

	if (WIFEXITED(status)) {
		outf("exitcode", "%i", WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		int sig = WTERMSIG(status);
		outf("signal", "%i (SIG%s %s)", sig, signame(sig), strsignal(sig));
		outf("core dumped", "%s", WCOREDUMP(status) ? "true" : "false");
	}

	/* getrusage(RUSAGE_SELF, &self); */
	/* struct rusage res = rusage_sub(child, self); */
	struct rusage res = child;

	outf("root.cpu", "%.3fs", res.ru_utime.tv_sec + res.ru_utime.tv_usec / 1000000.0);
	outf("root.sys", "%.3fs", res.ru_stime.tv_sec + res.ru_stime.tv_usec / 1000000.0);
	outf("root.maxrss", "%liKB", res.ru_maxrss);

	total_usec  = res.ru_utime.tv_sec * 1000000 + res.ru_utime.tv_usec;
	total_usec += res.ru_stime.tv_sec * 1000000 + res.ru_stime.tv_usec;

	rt_usec = 1000000 * (t1.tv_sec - t0->tv_sec) + (t1.tv_nsec - t0->tv_nsec) / 1000;
	outf("walltime", "%.3fs", rt_usec / 1000000.0);
	outf("parallelism", "%.2f", (float)total_usec / rt_usec);

	read_cgroup();
	destroy_cgroup();

	exit(WEXITSTATUS(status));
}

int main(int argc, char **argv)
{
	int pid;
	struct timespec t0;

	/* non-constant default configs */
	cfg.fout = stderr;

	parse_opts(argc, argv);

	if (cfg.outfile) {
		cfg.fout = fopen(cfg.outfile, "w");
		if (!cfg.fout)
			quit("fopen");
	}

	if (optind == argc) {
		usage();
		exit(1);
	}

	find_cgroup_fs(); // also make

	clock_gettime(CLOCK_MONOTONIC_RAW, &t0);

	pid = fork();
	/* Child just executes the given command, exit with 127 (standard for
	 * 'command not found' otherwise. */
	if (!pid) {
		/* Put self in fresh cgroup */
		put_in_cgroup(getpid());

		/* TODO: drop privileges */
		warn("getuid() = %i", getuid());
		setuid(getuid());

		/* Execute given command */
		execvp(argv[optind], &argv[optind]);

		/* exec failed if we reach here */
		perror(argv[optind]);
		exit(127);
	}

	monitor(&t0, pid);

	/* something very bad happened if we reach here */
	quit("wat????");
	return -1;
}
