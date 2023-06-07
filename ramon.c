#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/limits.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct cfg {
	const char *outfile;
	FILE *fout;
	bool keep;
	const char *tally;
	int debug;
	int verbosity;
	bool save; /* save to a fresh file */
	int pollms;
	const char *notify;
	bool render;
	long maxmem;
	long maxcpu;
	long maxstack;
};

/* Global config state */
struct cfg cfg = {
	.outfile     = NULL,
	.fout        = NULL, /* set to stderr by main() */
	.keep        = false,
	.tally       = NULL,
	.debug       = 1,
	.verbosity   = 1,
	.pollms      = 1000,
	.notify      = NULL,
	.render      = false,
	.maxmem      = 0,
	.maxcpu      = 0,
	.maxstack    = 0,
};

struct cgroup_res_info
{
	long usage_usec;
	long user_usec;
	long system_usec;
	long mempeak;
	long pidpeak;
	long memcurr;
};

/* open directory fd for our cgroup */
int cgroup_fd;
/* parent cgroup directory */
char cgroupfs_root[PATH_MAX];
/* our cgroup */
char cgroup_path[PATH_MAX];

/* start and finish timestamps of subprocess */
struct timespec t0, t1;

int child_pid;

int sock_up = -1;
int sock_down = -1;
char sock_down_path[PATH_MAX];

void quit(const char *fmt, ...)
{
	va_list va;
	int _errno = errno;

	fprintf(stderr, "ERROR: ramon: ");

	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	va_end(va);
	fprintf(stderr, " (%s)", strerror(_errno));
	fputs("\n", stderr);
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

	fprintf(stderr, "DEBUG: ramon %i: ", getpid());

	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	va_end(va);
	fputs("\n", stderr);
}

#define dbg(n, ...)				\
	do {					\
		if (cfg.debug >= n)	\
			__dbg(__VA_ARGS__);	\
	} while(0)

FILE *fopenat(int dirfd, const char *pathname, const char *mode)
{
	int flags = (strchr(mode, 'r') ? O_RDONLY : 0)
		  | (strchr(mode, 'w') ? O_WRONLY : 0);
	int fd = openat(dirfd, pathname, flags);
	if (fd < 0)
		return NULL;
	return fdopen(fd, mode);
}

const struct option longopts[] = {
	{ .name = "output",       .has_arg = required_argument, .flag = NULL, .val = 'o' },
	{ .name = "keep-cgroup",  .has_arg = no_argument,       .flag = NULL, .val = 'k' },
	{ .name = "tally",        .has_arg = required_argument, .flag = NULL, .val = 't' },
	{ .name = "save",         .has_arg = no_argument,       .flag = NULL, .val = 's' },
	{ .name = "poll",         .has_arg = optional_argument, .flag = NULL, .val = 'p' },
	{ .name = "help",         .has_arg = no_argument,       .flag = NULL, .val = 'h' },
	{ .name = "notify",       .has_arg = required_argument, .flag = NULL, .val = 'n' },
	{ .name = "render",       .has_arg = no_argument,       .flag = NULL, .val = 'r' },
	{ .name = "limit-mem",    .has_arg = required_argument, .flag = NULL, .val = 'm' },
	{ .name = "limit-cpu",    .has_arg = required_argument, .flag = NULL, .val = 'c' },
	{ .name = "limit-stack",  .has_arg = required_argument, .flag = NULL, .val = 'a' },
	/* { .name = "debug",        .has_arg = optional_argument, .flag = NULL, .val = 'd' }, */
	{0},
};

void help(const char *progname)
{
	fprintf(stderr, "%s: IOU a manual!\n", progname);
	fprintf(stderr, "This is version %s\n", RAMON_VERSION);
}

void parse_opts(int argc, char **argv)
{
	int rc;

	while (1) {
		rc = getopt_long(argc, argv, "+o:r1kt:dqsphvm", longopts, NULL);
		/* printf("opt = '%c', optarg = %s\n", rc, optarg); */
		switch (rc) {
		case 'o':
			cfg.outfile = optarg;
			break;

		case 't':
			cfg.tally = optarg;
			break;

		case 'k':
			cfg.keep = true;
			break;

		case 'd':
			cfg.debug++;
			break;

		case 's':
			cfg.save = true;
			break;

		case 'p':
			if (optarg)
				cfg.pollms = atoi(optarg);
			else
				cfg.pollms = 1000;
			break;

		case 'h':
			help(argv[0]);
			exit(0);

		case 'n':
			cfg.notify = optarg;
			break;

		case 'r':
			cfg.render = true;
			break;

		case 'm':
			assert(optarg);
			cfg.maxmem = atoi(optarg);
			break;

		case 'c':
			assert(optarg);
			cfg.maxcpu = atoi(optarg);
			break;

		case 'a':
			assert(optarg);
			cfg.maxstack = atoi(optarg);
			break;

		case -1:
			return;

		case '?':
		default:
			help(argv[0]);
			exit(1);
		}
	}
}

void __outf(const char *key, const char *fmt, ...)
{
	va_list va;

	/* When printing to stderr we prepend a marker */
	fprintf(cfg.fout, "%s%-20s ", cfg.fout == stderr ? "ramon: " : "", key);

	va_start(va, fmt);
	vfprintf(cfg.fout, fmt, va);
	va_end(va);
	fputs("\n", cfg.fout);
}

#define outf(n, ...)				\
	do {					\
		if (cfg.verbosity >= n)		\
			__outf(__VA_ARGS__);	\
	} while(0)

const char *wifstring(int status)
{
	if (WIFEXITED(status)) return "exited";
	if (WIFSIGNALED(status)) return "terminated by signal";
	return "unknown (please file bug report)";
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

void skipline(FILE *f)
{
	int c;
	while ((c = fgetc(f)) != '\n' && c != EOF)
		;
}

/* Find root of cgroup2 mount */
void find_cgroup_fs()
{
	char buf[PATH_MAX];

	FILE *f = fopen("/proc/mounts", "r");
	if (!f)
		quit("fopen mounts");

	while (fscanf(f, "%s", buf) > 0) {
		if (strcmp(buf, "cgroup2")) {
			skipline(f);
			continue;
		}
		fscanf(f, "%s", cgroupfs_root);
		fclose(f);
		return;
	}
	quit("did not find cgroup2 mount");
}

/* Make new fresh cgroup */
void make_new_cgroup()
{
	char buf[PATH_MAX];

	strcpy(buf, cgroupfs_root);

	strcat(buf, "/ramon_run_XXXXXX");
	char *p = mkdtemp(buf);
	if (!p)
		quit("mkdtemp");

	/* Make the directory readable by group/other, as usual. */
	chmod(p, 0755);

	strcpy(cgroup_path, buf);
	dbg(2, "Created cgroup '%s'", cgroup_path);

	cgroup_fd = open(buf, O_DIRECTORY | O_CLOEXEC);
	if (cgroup_fd < 0)
		quit("open cgroup dir");
}

/* Make a cgroup under the current one */
void make_sub_cgroup(const char *ramonroot)
{
	/* char buf[PATH_MAX]; */
	char buf2[PATH_MAX];

/*         FILE *f = fopen("/proc/self/cgroup", "r"); */
/*         assert(f); */
/*         buf[0] = 0; */
/*         while (!feof(f)) { */
/*                 if (fscanf(f, "0::%s", buf) != 1) { */
/*                         skipline(f); */
/*                         continue; */
/*                 } */
/*                 goto ok; */
/*         } */
/*         quit("could not find root cgroup... is /proc/pid/cgroup ok?"); */
/* ok: */
/*         fclose(f); */

	char *q = buf2;
	/* q = stpcpy(q, cgroupfs_root); */
	/* q = stpcpy(q, buf); */
	q = stpcpy(q, ramonroot);
	q = stpcpy(q, "/ramon_XXXXXX");
	char *p = mkdtemp(buf2);
	if (!p)
		quit("mkdtemp");

	/* Make the directory readable by group/other, as usual. */
	chmod(p, 0755);

	strcpy(cgroup_path, buf2);
	dbg(2, "cgroup is '%s'", cgroup_path);

	cgroup_fd = open(buf2, O_DIRECTORY | O_CLOEXEC);
	if (cgroup_fd < 0)
		quit("open cgroup dir");
}

void try_rm_cgroup()
{
	int rc;

	rc = rmdir(cgroup_path);
	if (rc < 0 && errno != ENOENT)
		warn("Could not remove cgroup");
}

void put_in_cgroup()
{
	int rc;

	int fd = openat(cgroup_fd, "cgroup.procs", O_WRONLY);
	if (fd < 0)
		quit("open cgroup");

	/* Writing "0" adds the current process to the group */
	rc = write(fd, "0", 1);
	if (rc < 1)
		quit("write");

	close(fd);
}

/* Operates on self, must be called only by child process */
void limit_own_stack(long size)
{
	struct rlimit rlim;
	int rc;

	rlim.rlim_cur = size;
	rlim.rlim_max = size;

	rc = setrlimit(RLIMIT_STACK, &rlim);
	if (rc < 0)
		quit("cannot limit stack");
}

bool any_in_cgroup()
{
	bool ret = false;
	unsigned long n;
	FILE *f;

	f = fopenat(cgroup_fd, "cgroup.procs", "r");
	if (!f) {
		warn("Could not open cgroup.procs");
		/* Optimistically carry on */
		return false;
	}

	while (fscanf (f, "%lu", &n) > 0) {
		warn("Subprocess still alive after main finished (pid %lu)", n);
		ret = true;
	}

	fclose(f);

	return ret;
}

void kill_cgroup()
{
	int fd, rc;

	fd = openat(cgroup_fd, "cgroup.kill", O_WRONLY);
	if (fd < 0)
		warn("Could not open cgroup.kill");

	rc = write(fd, "1", 1);
	if (rc != 1)
		warn("Could not send kill signal to group");

	close(fd);

	/*
	 * There is a race between sending the kill signal and
	 * the group really being dead and able to be removed.
	 * What should really be done is wait for events
	 * in the cgroup.events file. TODO!
	 */
	usleep(1000);
}

void destroy_cgroup()
{
	if (any_in_cgroup()) {
		warn("Killing remaining processes");
		kill_cgroup();
	}

	if (!cfg.keep)
		try_rm_cgroup();
	else
		dbg(1, "Keeping cgroup in path '%s', you should manually delete it eventually.", cgroup_path);
}

/* FIXME, very heuristic */
unsigned long humanize(unsigned long x, const char **suf)
{
	static const char* sufs[] = { "", "K", "M", "G", "T", "P", NULL };
	int pow = 0;

	/* no more than 5 digits */
	while (x > 99999 && sufs[pow+1] != NULL) {
		pow++;
		x /= 1024;
	}
	*suf = sufs[pow];
	return x;
}

#define HMS_LEN (6+1+2+1+2+1+3+1+1) // HHHHHHhMMmSS.SSSs + '\0'
void t_hms(char buf[HMS_LEN], int usecs)
{
	/* int ms = (usecs / 1000) % 1000; */
	/* int s  = (usecs / 1000000) % 60; */
	int m  = (usecs / 60/1000000) % 60;
	int h  = (usecs / 60/60/1000000);

	if (h > 999999)
		warn("Um... over a million hours?");

	char *p = buf;
	if (h) p += sprintf(p, "%02dh", h);
	if (m) p += sprintf(p, "%02dm", m);
	sprintf(p, "%02.3fs", (usecs % 1000000) / 1000000.0);
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
	int rem = nk;
	int i;

	while (!feof(f) && rem > 0) {
		if (1 != fscanf(f, "%s", key))
			return n;

		for (i = 0; i < nk; i++) {
			if (strcmp(kvs[i].key, key))
				continue;

			if (1 != fscanf(f, kvs[i].fmt, kvs[i].wo)) {
				warn("scan failed for %s", key);
			}
			n++;
			rem--;
			break;
		}
		/* no match, skip line */
		if (i == nk) {
			skipline(f);
			continue;
		}
	}

	return n;
}

int open_and_read_kvs(int dirfd, const char *pathname, int nk, struct kvfmt kvs[])
{
	FILE *f = fopenat(dirfd, pathname, "r");
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

int open_and_read_val(bool nowarn, int dirfd, const char *pathname, const char *fmt, void *wo)
{
	FILE *f = fopenat(dirfd, pathname, "r");
	if (!f) {
		if (!nowarn)
			warn("could not open %s", pathname);
		return -1;
	}
	int rc = fscanf(f, fmt, wo);
	fclose(f);
	if (!nowarn && rc != 1)
		warn("could not read value");
	return rc;
}

/* returns statically allocated string in glibc */
char *str_of_current_time()
{
	struct timeval tv;
	char *date;
	int i, rc;

	rc = gettimeofday(&tv, NULL);
	if (rc < 0)
		return NULL;

	date = ctime(&tv.tv_sec);
	if (!date)
		return NULL;

	/* remove trailing newline */
	i = strlen(date);
	if (i > 0 && date[i-1] == '\n')
		date[i-1] = 0;

	return date;
}

void print_current_time(const char *key)
{
	char *date = str_of_current_time();

	if (date)
		outf(1, key, "%s", date);
	else
		outf(1, key, "unknown (%s)", strerror(errno));
}

long wall_us()
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

	return  1000000 * (ts.tv_sec - t0.tv_sec) +
		(ts.tv_nsec - t0.tv_nsec) / 1000;
}

void read_cgroup(struct cgroup_res_info *wo)
{
	struct kvfmt cpukeys[] = {
		{ .key = "usage_usec",  .fmt = "%li", .wo = &wo->usage_usec  },
		{ .key = "user_usec",   .fmt = "%li", .wo = &wo->user_usec   },
		{ .key = "system_usec", .fmt = "%li", .wo = &wo->system_usec },
	};
	open_and_read_kvs(cgroup_fd, "cpu.stat", 3, cpukeys);

	if (open_and_read_val(true, cgroup_fd, "memory.peak", "%lu", &wo->mempeak) != 1)
		wo->mempeak = -1;

	if (open_and_read_val(true, cgroup_fd, "pids.peak", "%lu", &wo->pidpeak) != 1)
		wo->pidpeak = -1;

	if (open_and_read_val(true, cgroup_fd, "memory.current", "%lu", &wo->memcurr) != 1)
		wo->memcurr= -1;
}

void print_cgroup_res_info(struct cgroup_res_info *res)
{
	outf(0, "group.total", "%.3fs", res->usage_usec / 1000000.0);
	outf(0, "group.utime", "%.3fs", res->user_usec / 1000000.0);
	outf(0, "group.stime", "%.3fs", res->system_usec / 1000000.0);

	if (res->mempeak > 0) {
		const char *suf;
		long mempeak = humanize(res->mempeak, &suf);
		outf(0, "group.mempeak", "%lu%sB", mempeak, suf);
	}
	if (res->pidpeak > 0)
		outf(0, "group.pidpeak", "%lu", res->pidpeak);
}

struct timespec last_poll_ts = {0};
void poll()
{
	static unsigned long last_poll_usage;
	struct timespec ts;
	struct cgroup_res_info res;
	unsigned long delta_us, wall_us;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

	/* absolute */
	wall_us = 1000000 * (ts.tv_sec - t0.tv_sec) +
			(ts.tv_nsec - t0.tv_nsec) / 1000;

	/* relative to last measure */
	delta_us = 1000000 * (ts.tv_sec - last_poll_ts.tv_sec) +
			(ts.tv_nsec - last_poll_ts.tv_nsec) / 1000;

	/* If we are somehow woken up less than 1 microsecond
	 * after the last time, just skip this measurement. */
	if (delta_us == 0)
		return;

	read_cgroup(&res);

	outf(0, "poll", "wall=%.3fs usage=%.3fs user=%.3fs sys=%.3fs mem=%li load=%.2f",
			wall_us / 1000000.0,
			res.usage_usec / 1000000.0,
			res.user_usec / 1000000.0,
			res.system_usec / 1000000.0,
			res.memcurr,
			1.0 * (res.usage_usec - last_poll_usage) / delta_us);
	fflush(cfg.fout);

	last_poll_usage = res.usage_usec;
	last_poll_ts = ts;
}


void print_exit_status(int status)
{
	outf(0, "status", wifstring(status));

	if (WIFEXITED(status)) {
		outf(0, "exitcode", "%i", WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		int sig = WTERMSIG(status);
		outf(0, "signal", "%i (SIG%s %s)", sig, signame(sig), strsignal(sig));
		outf(0, "core dumped", "%s", WCOREDUMP(status) ? "true" : "false");
	}
}

void sigint(int sig)
{
	/* Ignore 3 signals */
	const int lim = 3;
	static int cnt = 0;

	if (sig != SIGINT && sig != SIGTERM) {
		warn("sigint!!!");
		return;
	}

	kill(child_pid, sig);

	if (++cnt >= lim) {
		struct sigaction sa;
		sa.sa_handler = SIG_DFL;
		sa.sa_flags = 0;
		sigemptyset(&sa.sa_mask);

		sigaction(sig, &sa, NULL);
	}
}

int setup_signalfd()
{
	int sfd, rc;

	struct sigaction sa;

	/* prevent SIGCHLDs when child is stopped */
	sa.sa_handler = SIG_DFL;
	sa.sa_flags = SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);
	rc = sigaction(SIGCHLD, &sa, NULL);
	if (rc < 0)
		return rc;

	/* handle SIGINT and SIGTERM so we can survive it */
	sa.sa_handler = sigint;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	rc = sigaction(SIGINT, &sa, NULL);
	if (rc < 0)
		return rc;
	rc = sigaction(SIGTERM, &sa, NULL);
	if (rc < 0)
		return rc;

	/* set up a signal fd */
	sigset_t sigmask;
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGCHLD);

	rc = sigprocmask(SIG_BLOCK, &sigmask, NULL);
	if (rc < 0)
		return rc;

	sfd = signalfd(-1, &sigmask, SFD_CLOEXEC);
	return sfd;
}

int sfd, epfd;

void prepare_monitor()
{
	outf(1, "version", "%s", RAMON_VERSION);
	print_current_time("start");
	int rc;

	sfd = setup_signalfd();
	if (sfd < 0)
		quit("signalfd");

	/* If we will be polling, prime the first poll timestamp */
	if (cfg.pollms)
		clock_gettime(CLOCK_MONOTONIC_RAW, &last_poll_ts);

	epfd = epoll_create(1);
	if (epfd < 0)
		quit("epoll_create");

	/* signalfd */
	{
		struct epoll_event ev;
		ev.events = EPOLLIN;
		ev.data.fd = sfd;
		rc = epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev);
		if (rc < 0)
			quit("epoll_ctl signalfd");
	}

	/* sock down */
	if (sock_down >= 0)
	{
		struct epoll_event ev;
		ev.events = EPOLLIN;
		ev.data.fd = sock_down;
		rc = epoll_ctl(epfd, EPOLL_CTL_ADD, sock_down, &ev);
		if (rc < 0)
			quit("epoll_ctl sock_down");
	}
}

static inline
int timediff_ms(struct timespec *t1, struct timespec *t2)
{
	return 1000 * (t2->tv_sec - t1->tv_sec) +
		(t2->tv_nsec - t1->tv_nsec) / 1000000;
}

void print_overhead(long total_usec)
{
	struct rusage self;
	int rc;

	rc = getrusage(RUSAGE_SELF, &self);
	if (rc < 0) {
		warn("getrusage failed");
		return;
	}

	const long utime_usec = 1000000 * self.ru_utime.tv_sec + self.ru_utime.tv_usec;
	const long stime_usec = 1000000 * self.ru_stime.tv_sec + self.ru_stime.tv_usec;

	dbg(2, "self.rusage.utime = %.3fs", utime_usec / 1000000.0);
	dbg(2, "self.rusage.stime = %.3fs", stime_usec / 1000000.0);
	dbg(2, "estimated cpu overhead = %2.5f%%", 100.0 * (utime_usec+stime_usec) / total_usec);
}

void print_zombie_stats(int pid)
{
	char comm[16]; /* TASK_COMM_LEN = 16, see `man 5 proc' */
	char buf[64];
	FILE *f;
	int rc;

	sprintf(buf, "/proc/%i/stat", pid);
	f = fopen(buf, "r");
	if (!f) {
		warn("Could not open %s", buf);
		return;
	}

	/*
	 * See `man 5 proc'.
	 *
	 * The `*' marks a particular specification as assignment-supressed,
	 * so we don't need to pass an argument to receive the value. However,
	 * gcc raises a warning if it is combined with a length modifier (`l').
	 * The `l'` can be removed without functional change, but I don't see
	 * why this would warn. Keeping_ it helps understand the code,
	 * and lessens the chance of future bugs (say if we later want to actually
	 * use a field, removing the `*', but forget to add back the `l').
	 *
	 * Anyway, for those fields, just pass a dummy.
	 */
	unsigned long utime = -1, stime = -1;
	unsigned long lu_dummy;
	int pid_;
	char st;

	rc = fscanf(f, "%d   (%[^)]) %c   %*d  %*d  %*d  %*d  %*d  %*u  %lu "
		       "%lu  %lu  %lu  %lu  %lu"
			, &pid_
			, comm
			, &st
			, &lu_dummy
			, &lu_dummy
			, &lu_dummy
			, &lu_dummy
			, &utime
			, &stime
			);

	if (rc != 9) {
		warn("Parsing procstat failed");
		return;
	}

	if (pid != pid_)
		warn("PID mismatch in stat?");
	if (st != 'Z')
		warn("Child is not zombie?");
 
	long clk_tck = sysconf(_SC_CLK_TCK);

	outf(0, "executable", "%s", comm);
	outf(0, "root.utime", "%.3fs", 1.0 * utime / clk_tck);
	outf(0, "root.stime", "%.3fs", 1.0 * stime / clk_tck);
}

/* Returns the exit code of pid */
int wait_monitor(int pid)
{
	struct epoll_event ev;
	unsigned long wall_usec;
	int timeout;
	int status;
	int rc;

	struct timespec tep0, tep;

	timeout = cfg.pollms > 0 ? cfg.pollms: -1;

	dbg(2, "entering event loop");
	dbg(2, "sock_down = %i", sock_down);
	dbg(2, "sock_up = %i", sock_up);

	clock_gettime(CLOCK_MONOTONIC_RAW, &tep0);
	while (1) {
		clock_gettime(CLOCK_MONOTONIC_RAW, &tep);

		/* try to hit a multiple of cfg.pollms */
		timeout = cfg.pollms - timediff_ms(&tep0, &tep) % cfg.pollms;

		rc = epoll_wait(epfd, &ev, 1, timeout);

		if (rc < 0 && errno == EINTR) {
			continue;
		} else if (rc == 0) {
			assert(cfg.pollms);
			poll();
		} else if (ev.data.fd == sfd) {
			struct signalfd_siginfo si;
			rc = read (sfd, &si, sizeof si);
			if (rc != sizeof si)
				quit("read signal?");

			if (si.ssi_signo == SIGCHLD) {
				break;
			} else {
				assert(!"wtf signal?");
			}
		} else if (ev.data.fd == sock_down) {
			int fd = accept(sock_down, NULL, NULL);
			if (fd < 0)
				quit("accept???");

			dbg(2, "accepted conn,fd = %i", fd);

			struct epoll_event ev;
			ev.events = EPOLLIN;
			ev.data.fd = fd;
			rc = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
			if (rc < 0)
				quit("epoll_ctl client");
		} else {
			/* must be a client socket writing */
			if (ev.events & EPOLLIN) {
				char buf[200];
				int rc = read(ev.data.fd, buf, sizeof buf - 1);
				if (rc > 0) {
					buf[rc] = 0;
					outf(0, "mark", "str=%s wall=%.3fs", buf, wall_us() / 1000000.0);
				}
			}
			if (ev.events & EPOLLHUP)
				close(ev.data.fd);
		}
	}

	clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
	print_current_time("end");

	// move both of these below exit status
	print_zombie_stats(child_pid);

	struct cgroup_res_info res;
	read_cgroup(&res);
	print_cgroup_res_info(&res);

	rc = wait4(pid, &status, WNOHANG, NULL);
	if (rc != pid)
		quit("wait4");

	print_exit_status(status);

	/* TODO: is the getrusage thing useful? */
	/* struct rusage res = child; */
	/* outf("root.cpu", "%.3fs", res.ru_utime.tv_sec + res.ru_utime.tv_usec / 1000000.0); */
	/* outf("root.sys", "%.3fs", res.ru_stime.tv_sec + res.ru_stime.tv_usec / 1000000.0); */
	/* outf("root.maxrss", "%liKB", res.ru_maxrss); */

	wall_usec = 1000000 * (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1000;

	outf(0, "walltime", "%.3fs", wall_usec / 1000000.0);
	outf(0, "loadavg", "%.2f", 1.0f * res.usage_usec / wall_usec);

	print_overhead(res.usage_usec);

	destroy_cgroup();

	return WEXITSTATUS(status);
}

void setup_sock_down()
{
	char *sockname;
	int s;
	int rc;
	struct sockaddr_un addr;

	sockname = tempnam("/tmp", "ramon");
	/* FIXME: somewhere else? */
	if (!sockname)
		quit("tempnam");

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		quit("socket");

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sockname, 108);

	rc = bind(s, &addr, sizeof addr);
	if (rc < 0)
		quit("bind");

	rc = listen(s, 10);
	if (rc < 0)
		quit("listen");

	dbg(2, "sockname = %s", sockname);
	strcpy(sock_down_path, sockname);
	setenv("RAMONSOCK", sockname, 1);
	sock_down = s;

	free(sockname);
}

void connect_to_upstream()
{
	const char *up_path = getenv("RAMONSOCK");
	struct sockaddr_un addr;
	int rc;
	int s;

	if (!up_path)
		quit("RAMOMSOCK unset; not a nested invocation?");

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		quit("socket");

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, up_path, 108);

	rc = connect(s, &addr, sizeof addr);
	if (rc < 0)
		quit("connect up");

	sock_up = s;
}

void setup()
{
	const char *e_ramonroot = getenv("RAMONROOT");

	if (!e_ramonroot) {
		/*
		 * Fresh invocation: create a fresh cgroup, and open a socket
		 * to listen for subinvocations. Expose the socket via an environment
		 * variable.
		 */
		find_cgroup_fs();
		make_new_cgroup();
	} else {
		/* subinvocation, nest within the root and connect */
		strcpy(cgroupfs_root, e_ramonroot);
		make_sub_cgroup(e_ramonroot);
		connect_to_upstream();
	}

	if (cfg.maxmem) {
		FILE *f = fopenat(cgroup_fd, "memory.max", "w");
		if (!f)
			quit("cannot limit memory");

		fprintf(f, "%li", cfg.maxmem);
		fclose(f);
	}

	if (cfg.maxcpu) {
		/* FILE *f = fopenat(cgroup_fd, "cpu.max", "w"); */
		/* if (!f) */
			quit("cannot limit cpu (IOU)");

		/* fprintf(f, "%li", cfg.maxmem); */
		/* fclose(f); */
	}

	/*
	 * Re-set the root, even if we are subinvocation: messages are
	 * passed upwards (TODO!).
	 */
	setenv("RAMONROOT", cgroup_path, 1);
	dbg(2, "cgroup is '%s'", cgroup_path);
	setup_sock_down();
}

void notify_up(const char *msg)
{
	int rc;
	if (sock_up < 0)
		quit("sock_up unset");

	rc = write(sock_up, msg, strlen(msg));
	if (rc < (int)strlen(msg))
		quit("notify");
}

int exec_and_monitor(int argc, char **argv)
{
	int pid, rc;

	for (int i = 0; i < argc; i++)
		outf(1, "argv", "%i = %s", i, argv[i]);

	/* flush before forking */
	fflush(NULL);
	clock_gettime(CLOCK_MONOTONIC_RAW, &t0);

	pid = fork();
	if (!pid) {
		/*
		 * Child just executes the given command, exit with 127
		 * (standard for * 'command not found' otherwise.
		 */

		/* Put self in fresh cgroup */
		put_in_cgroup();

		/* Maybe limit stack */
		if (cfg.maxstack)
			limit_own_stack(cfg.maxstack);

		/*
		 * Close outfile if we opened one. All other files which remain
		 * were opened with O_CLOEXEC.
		 */
		if (cfg.outfile)
			fclose(cfg.fout);

		/* TODO: does this really drop privileges? */
		dbg(2, "getuid() = %i", getuid());
		setuid(getuid());

		/* Execute given command */
		execvp(argv[0], argv);

		/* exec() failed if we reach here */
		perror(argv[0]);
		exit(127);
	}

	prepare_monitor();

	child_pid = pid;
	outf(1, "childpid", "%lu", pid);

	rc = wait_monitor(pid);

	if (cfg.outfile)
		fclose(cfg.fout);

	close(sock_down);
	int x = unlink(sock_down_path);
	if (x < 0)
		quit("unlink");

	return rc;
}

FILE *fmkstemps(char *template, int suffixlen)
{
	int fd = mkstemps(template, suffixlen);
	if (fd < 0)
		return NULL;
	return fdopen(fd, "w+");
}

int main(int argc, char **argv)
{
	int rc;

	/* non-constant default configs */
	cfg.fout = stderr;

	parse_opts(argc, argv);

	dbg(3, "TMP_MAX = %i", TMP_MAX);

	if (cfg.render && !cfg.outfile)
		quit("An output file is needed to use --render");

	/* Maybe redirect output */
	if (cfg.outfile) {
		cfg.fout = fopen(cfg.outfile, "w");
		if (!cfg.fout)
			quit(cfg.outfile);
	} else if (cfg.save) {
		char temp[] = "XXXXXX.ramon";
		cfg.fout = fmkstemps(temp, 6);
		if (!cfg.fout)
			quit("could not create save file %s", temp);
		dbg(1, "Saving output to %s", temp);
	}

	/* Tally mode: just parse a cgroup dir and exit,
	 * no running anything. */
	if (cfg.tally) {
		cgroup_fd = open(cfg.tally, O_DIRECTORY | O_CLOEXEC);
		if (cgroup_fd < 0)
			quit("open cgroup dir");
		struct cgroup_res_info res;
		read_cgroup(&res);
		print_cgroup_res_info(&res);
		exit(0);
	}

	if (cfg.notify) {
		connect_to_upstream();
		notify_up(cfg.notify);
		return 0;
	}

	if (optind == argc) {
		fprintf(stderr, "%s: no command given\n", argv[0]);
		help(argv[0]);
		exit(1);
	}

	setup();

	rc = exec_and_monitor(argc - optind, argv + optind);

	if (cfg.render) {
		assert(cfg.outfile);
		char cmd[500];
		snprintf(cmd, 500, "ramon-render.py %s", cfg.outfile);
		system(cmd);
	}

	return rc;
}
