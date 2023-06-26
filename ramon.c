#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "opts.h"

#define VAR_RAMONROOT "RAMONROOT"
#define VAR_RAMONSOCK "RAMONSOCK"

void help(const char *progname);
void help_cb(void *unused __attribute__((unused)), const char *progname __attribute__((unused)))
{
	help("ramon");
	exit(0);
}

const char  * opt_outfile     = NULL;
bool          opt_stderr      = true;
bool          opt_tee         = false;
FILE        * opt_fout        = NULL;
bool          opt_keep        = false;
bool          opt_wait        = false;
const char  * opt_tally       = false;
int           opt_debug       = 1;
int           opt_verbosity   = 1;
bool          opt_save        = false;
long          opt_pollms      = 1000;
const char  * opt_mark        = NULL;
bool          opt_render      = false;
long          opt_maxmem      = 0;
long          opt_maxcpu      = 0;
long          opt_maxstack    = 0;
bool          opt_noclobber   = false;

struct opt ramon_opts[] = {
	OPT_STR("output", 'o', "Output to <file> instead", &opt_outfile),
	OPT_STRBOOL("tee", 0, "Tee the output to <file> as well as to stderr", &opt_outfile, &opt_tee),
	OPT_INT("poll", 'p', "Set the poll rate to <int> ms, set to 0 to disable", &opt_pollms),
	OPT_BOOL("keep", 'k', "Keep the cgroup after ramon finishes", &opt_keep),
	OPT_STR("mark", 0, "Send a timemark to an enclosing ramon invocation, and do nothing else", &opt_mark),
	OPT_BOOL("wait", 'w', "Wait for all processes in cgroup instead of just the root", &opt_wait),
	OPT_BOOL("save", 's', "Save ramon's output to a freshly created file", &opt_save),
	OPT_BOOL("noclobber", 0, "Make sure to not overwrite the output file", &opt_noclobber),
	OPT_STR("tally", 't', "Tally the resources of an existing cgroup instead", &opt_tally),
	OPT_INT("limit-mem", 0, "Limit the group's memory usage to <int> bytes", &opt_maxmem),
	OPT_INT("limit-cpu", 0, "Limit the group's CPU usage to <int> seconds", &opt_maxcpu),
	OPT_INT("limit-stack", 0, "Limit *each subprocess* stack to <int> bytes, this is done via ulimit", &opt_maxstack),
	OPT_ACTION("help", 'h', "Display help output and exit", NULL, &help_cb),
	OPT_INC(NULL, 'd', "Increase debug level", &opt_debug),
	OPT_INC(NULL, 'v', "Increase verbosity", &opt_verbosity),
	OPT_END,
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

long clk_tck;
long nproc;

struct procstat_info
{
	char execname[16]; /* TASK_COMM_LEN = 16, see `man 5 proc' */
	unsigned long utime; /* in clk_tck units */
	unsigned long stime; /* in clk_tck units */
};

/* open directory fd for our cgroup */
int cgroup_fd;
/* parent cgroup directory */
char cgroupfs_root[PATH_MAX];
/* our cgroup */
char cgroup_path[PATH_MAX];

FILE *proc_stat_f;

int child_pid;

int sock_up = -1;
int sock_down = -1;
char sock_down_path[PATH_MAX];

int gopipe[2];

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
	int _errno = errno;

	fprintf(stderr, "WARNING: ramon: ");

	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	va_end(va);
	if (_errno)
		fprintf(stderr, " (%s)", strerror(_errno));
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
		if (opt_debug >= n)	\
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

void notify_up(const char *msg, int len)
{
	int rc;
	if (sock_up < 0)
		quit("sock_up unset");

	rc = write(sock_up, msg, len);
	if (rc < len)
		warn("Writing to upstream socket failed (%i vs %i)", rc, len);
}

void help(const char *progname)
{
	/* fprintf(stderr, "%s: resource accounting and monitoring tool\n", progname); */
	fprintf(stderr, "This is ramon version %s\n", RAMON_VERSION);
	fprintf(stderr, "Usage: %s <options> [--] command <args...>\n", progname);
	fprintf(stderr, "Options:\n");
	print_opts(stderr, ramon_opts);
}

void __outf(int col, const char *key, const char *fmt, ...)
{
	va_list va;

	assert (opt_stderr || opt_fout);

	if (opt_stderr) {
		/* When printing to stderr we prepend a marker */
		if (col)
			fprintf(stderr, "\x1b[31;1m");
		fprintf(stderr, "ramon: %-20s ", key);
		va_start(va, fmt);
		vfprintf(stderr, fmt, va);
		va_end(va);
		if (col)
			fprintf(stderr, "\x1b[0m");
		fputs("\n", stderr);
	}
	if (opt_fout) {
		fprintf(opt_fout, "%-20s ",key);
		va_start(va, fmt);
		vfprintf(opt_fout, fmt, va);
		va_end(va);
		fputs("\n", opt_fout);
	}
}

void ramon_flush()
{
	fflush(stderr);
	if (opt_fout)
		fflush(opt_fout);
}

#define outf(n, ...)					\
	do {						\
		if (opt_verbosity >= n)			\
			__outf(0, __VA_ARGS__);		\
	} while(0)

#define outf_col(n, col, ...)				\
	do {						\
		if (opt_verbosity >= n)			\
			__outf(col, __VA_ARGS__);	\
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
	FILE *f;
	int rc;

	f = fopen("/proc/mounts", "r");
	if (!f)
		quit("fopen mounts");

	while (fscanf(f, "%s", buf) > 0) {
		if (strcmp(buf, "cgroup2")) {
			skipline(f);
			continue;
		}
		rc = fscanf(f, "%s", cgroupfs_root);
		if (rc != 1)
			quit("Could not read mount line?");
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
	char buf[5000];

	sprintf(buf, "%s/rootgroup", cgroup_path);

	rc = rmdir(buf);
	if (rc < 0 && errno != ENOENT)
		warn("Could not remove cgroup/rootgroup");

	rc = rmdir(cgroup_path);
	if (rc < 0 && errno != ENOENT)
		warn("Could not remove cgroup");
}

void put_in_cgroup()
{
	int rc;
	int fd;

	rc = mkdirat(cgroup_fd, "rootgroup", 0755);
	if (rc < 0)
		quit("mkdir sub");

	fd = openat(cgroup_fd, "rootgroup/cgroup.procs", O_WRONLY);
	if (fd < 0)
		quit("open cgroup");

	/* Writing "0" adds the current process to the group */
	rc = write(fd, "0", 1);
	if (rc < 1)
		quit("write into cgroup.procs");

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

bool any_in_cgroup(bool should_warn)
{
	bool ret = false;
	unsigned long pid; // type ok?
	FILE *f;

	f = fopenat(cgroup_fd, "cgroup.procs", "r");
	if (!f) {
		warn("Could not open cgroup.procs");
		/* Optimistically carry on */
		return false;
	}

	while (fscanf (f, "%lu", &pid) > 0) {
		char buf[500];
		if (!should_warn) {
			fclose(f);
			return true;
		}

		/* dirty hack, improve */
		{
			char fn[200];
			size_t i = 0;
			int c;
			sprintf(fn, "/proc/%lu/cmdline", pid);
			FILE *ff = fopen(fn, "r");
			while (i < (sizeof fn - 1) && (c = fgetc(ff)) != EOF) {
				if (!c) c = ' ';
				buf[i++] = c;
			}
			buf[i] = 0;
		}

		warn("Subprocess still alive after main finished (pid %lu, cmdline = '%s')", pid, buf);
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

int pending_sigint()
{
	sigset_t set;
	sigpending(&set);
	return sigismember(&set, SIGINT);
}

void wait_cgroup()
{
	if (opt_wait) {
		if (any_in_cgroup(false)) {
			warn("Waiting for cgroup to finish");
			while (any_in_cgroup(false) && !pending_sigint())
				usleep(5000);
		}
	} else {
		if (any_in_cgroup(true)) {
			warn("Killing remaining processes");
			kill_cgroup();
		}
	}
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
	sprintf(p, "%02.3fs", (usecs % 1000000) / 1e6);
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

int open_and_read_val(bool *nowarn, int dirfd, const char *pathname, const char *fmt, void *wo)
{
	FILE *f = fopenat(dirfd, pathname, "r");
	if (!f) {
		if (nowarn && !*nowarn) {
			warn("could not open %s", pathname);
			*nowarn = true;
		}
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

/*
 * Must be set with
 *     zero_wall_us = cur_wall_us()
 * to make cur_wall_us meaningful.
 */
long zero_wall_us = 0;
long cur_wall_us()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (1000000 * ts.tv_sec + ts.tv_nsec / 1000) - zero_wall_us;
}

bool nowarn_memorypeak = false;

void read_cgroup(struct cgroup_res_info *wo)
{
	/* TODO: change this API, we only open the same files over and over,
	 * so keep them open instead. */
	struct kvfmt cpukeys[] = {
		{ .key = "usage_usec",  .fmt = "%li", .wo = &wo->usage_usec  },
		{ .key = "user_usec",   .fmt = "%li", .wo = &wo->user_usec   },
		{ .key = "system_usec", .fmt = "%li", .wo = &wo->system_usec },
	};
	open_and_read_kvs(cgroup_fd, "cpu.stat", 3, cpukeys);

	if (open_and_read_val(&nowarn_memorypeak, cgroup_fd, "memory.peak", "%lu", &wo->mempeak) != 1)
		wo->mempeak = -1;

	if (open_and_read_val(NULL, cgroup_fd, "pids.peak", "%lu", &wo->pidpeak) != 1)
		wo->pidpeak = -1;

	if (open_and_read_val(NULL, cgroup_fd, "memory.current", "%lu", &wo->memcurr) != 1)
		wo->memcurr= -1;
}

void print_cgroup_res_info(struct cgroup_res_info *res)
{
	outf(0, "group.total", "%.3fs", res->usage_usec / 1e6);
	outf(0, "group.utime", "%.3fs", res->user_usec / 1e6);
	outf(0, "group.stime", "%.3fs", res->system_usec / 1e6);

	if (res->mempeak > 0) {
		const char *suf;
		long mempeak = humanize(res->mempeak, &suf);
		outf(0, "group.mempeak", "%lu%sB", mempeak, suf);
	}
	if (res->pidpeak > 0)
		outf(0, "group.pidpeak", "%lu", res->pidpeak);
}

int read_proc_stat(int pid, struct procstat_info *wo)
{
	int rc;

	if (!proc_stat_f) {
		char buf[64];

		sprintf(buf, "/proc/%i/stat", child_pid);
		proc_stat_f = fopen(buf, "r");
		if (!proc_stat_f) {
			warn("Could not open %s", buf);
			return -1;
		}
	} else {
		rewind(proc_stat_f);
		fflush(proc_stat_f);
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
	unsigned long lu_dummy;
	int pid_;
	char st;

	rc = fscanf(proc_stat_f, "%d   (%[^)]) %c   %*d  %*d  %*d  %*d  %*d  %*u  %lu "
		       "%lu  %lu  %lu  %lu  %lu"
			, &pid_
			, (char*)&wo->execname // warning about size, probably better to change this and be defensive
			, &st
			, &lu_dummy
			, &lu_dummy
			, &lu_dummy
			, &lu_dummy
			, &wo->utime
			, &wo->stime
			);

	if (rc != 9) {
		warn("Parsing procstat failed");
		return -1;
	}

	if (pid != pid_)
		warn("PID mismatch in stat?");
	/* if (st != 'Z') */
	/*         warn("Child is not zombie?"); */

	return 0;
}


void poll()
{
	static unsigned long last_poll_usage = 0;
	static unsigned long last_poll_us = 0;
	static unsigned long last_poll_utime = 0;

	unsigned long delta_us, wall_us;
	struct cgroup_res_info res;

	/* get absolute time */
	wall_us = cur_wall_us();

	/* relative to last measure */
	delta_us = wall_us - last_poll_us;

	/*
	 * If we are somehow woken up less than 1 microsecond
	 * after the last time, just skip this measurement.
	 */
	if (delta_us == 0)
		return;

	read_cgroup(&res);

	unsigned long utime;
	struct procstat_info stat;
	int rc = read_proc_stat(child_pid, &stat);
	if (rc < 0) {
		warn("Reading procstat during poll failed");
		utime = 0;
	} else {
		utime = stat.utime;
	}

	long clk_tck = sysconf(_SC_CLK_TCK);

	outf(0, "poll", "wall=%.3fs usage=%.3fs user=%.3fs sys=%.3fs mem=%li roottime=%.3fs load=%.2f rootload=%.2f",
			wall_us / 1e6,
			res.usage_usec / 1e6,
			res.user_usec / 1e6,
			res.system_usec / 1e6,
			res.memcurr,
			1.0 * utime / clk_tck,
			1.0 * (res.usage_usec - last_poll_usage) / delta_us,
			1000000.0 * (utime - last_poll_utime) / clk_tck / delta_us
			);
	ramon_flush();

	last_poll_usage = res.usage_usec;
	last_poll_us = wall_us;
	last_poll_utime = utime;
}

void print_exit_status(int status)
{
	outf(0, "status", wifstring(status));

	if (WIFEXITED(status)) {
		int exitcode = WEXITSTATUS(status);
		outf_col(0, exitcode, "exitcode", "%i", exitcode);
	} else if (WIFSIGNALED(status)) {
		int sig = WTERMSIG(status);
		outf_col(0, 1, "signal", "%i (SIG%s %s)", sig, signame(sig), strsignal(sig));
		outf_col(0, 0, "core dumped", "%s", WCOREDUMP(status) ? "true" : "false");
	}
}

int setup_signalfd()
{
	struct sigaction sa;
	int sfd, rc;

	/* Prevent SIGCHLDs when child is stopped, we don't care (or do we?) */
	sa.sa_handler = SIG_DFL;
	sa.sa_flags = SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);
	rc = sigaction(SIGCHLD, &sa, NULL);
	if (rc < 0)
		return rc;

	/* Set up a signal fd */
	sigset_t sigmask;
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGCHLD);
	sigaddset(&sigmask, SIGALRM); /* Used for timer */

	rc = sigprocmask(SIG_BLOCK, &sigmask, NULL);
	if (rc < 0)
		return rc;

	sfd = signalfd(-1, &sigmask, SFD_CLOEXEC);
	return sfd;
}

void restore_signals()
{
	sigset_t sigmask;
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGCHLD);
	sigaddset(&sigmask, SIGALRM);
	sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
}

int sfd, epfd;

void set_poll_timer()
{
	struct timeval t;
	struct itimerval it;
	int rc;

	/*
	 * If pollms=0, then both values are zero,
	 * and setitimer disables the timer.
	 */
	t.tv_sec = opt_pollms / 1000;
	t.tv_usec = (opt_pollms % 1000) * 1000;
	it.it_interval = t;
	it.it_value    = t;

	rc = setitimer(ITIMER_REAL, &it, NULL);
	if (rc < 0)
		warn("Could not set timer; polling may not work");
}

void epfd_add(int fd)
{
	struct epoll_event ev;
	int rc;

	ev.events = EPOLLIN;
	ev.data.fd = fd;
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
	if (rc < 0)
		quit("epoll_ctl add %i", fd);
}

void print_sysinfo()
{
	struct sysinfo info;
	int rc;

	nproc = sysconf(_SC_NPROCESSORS_ONLN);
	if (nproc < 0)
		warn("could not read nproc");
	else
		outf(1, "nproc", "%i", nproc);

	rc = sysinfo(&info);
	if (rc < 0) {
		warn("no sysinfo");
		return;
	}

	outf(1, "sys.mem",          "%i MiB", (info.mem_unit * info.totalram) >> 20);
	outf(1, "sys.freemem",      "%i MiB", (info.mem_unit * info.freeram) >> 20);
	outf(1, "sys.availablemem", "%i MiB", (info.mem_unit * (info.totalram - info.bufferram)) >> 20);
	outf(1, "sys.nprocs", "%i", info.procs);
}

void prepare_monitor()
{
	outf(1, "version", "%s", RAMON_VERSION);
	print_current_time("start");

	clk_tck = sysconf(_SC_CLK_TCK);
	if (clk_tck < 0)
		quit("clktck?");

	print_sysinfo();

	sfd = setup_signalfd();
	if (sfd < 0)
		quit("signalfd");

	/* Set zero timestamp */
	zero_wall_us = cur_wall_us();

	set_poll_timer();

	epfd = epoll_create(1);
	if (epfd < 0)
		quit("epoll_create");

	/* signalfd */
	epfd_add(sfd);

	/* sock down */
	if (sock_down >= 0)
		epfd_add(sock_down);
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

	dbg(2, "self.rusage.utime = %.3fs", utime_usec / 1e6);
	dbg(2, "self.rusage.stime = %.3fs", stime_usec / 1e6);
	dbg(2, "estimated cpu overhead = %2.5f%%", 100.0 * (utime_usec+stime_usec) / total_usec);
}

void print_zombie_stats(int pid)
{
	struct procstat_info stat;
	int rc;

	rc = read_proc_stat(pid, &stat);
	if (rc < 0) {
		warn("Reading procstat of zombie failed");
		return;
	}

	outf(0, "root.execname", "%s", stat.execname);
	outf(0, "root.utime", "%.3fs", 1.0 * stat.utime / clk_tck);
	outf(0, "root.stime", "%.3fs", 1.0 * stat.stime / clk_tck);
}

int handle_sig()
{
	struct signalfd_siginfo si;
	int rc = read (sfd, &si, sizeof si);
	if (rc != sizeof si)
		quit("read signal?");

	switch (si.ssi_signo) {
	case SIGINT:
		/* Just forward */
		kill(child_pid, SIGINT);
		return 0;
	case SIGCHLD:
		return -1;
	case SIGALRM:
		poll();
		return 0;
	default:
		warn("Unexpected signal: %i", si.ssi_signo);
		return 0;
	}
}

void wait_monitor()
{
	struct epoll_event ev;
	int rc;

	while (1) {
		rc = epoll_wait(epfd, &ev, 1, -1);

		if (rc < 0 && errno == EINTR)
			continue;

		if (rc <= 0) {
			/* really should not happen */
			warn("epoll returned %i", rc);
			continue;
		}

		/* Got a signal */
		if (ev.data.fd == sfd) {
			rc = handle_sig();
			if (rc)
				break;
			continue;
		}

		/* Child wants to connect */
		if (ev.data.fd == sock_down) {
			struct sockaddr_un cli;
			socklen_t len = sizeof cli;
			int fd = accept(sock_down, &cli, &len);
			if (fd < 0)
				warn("accept failed");

			epfd_add(fd);
			continue;
		}

		/* Got a message (hopefully... fixme) */
		{
			/* must be a client socket writing */
			if (ev.events & EPOLLIN) {
				char buf[200];
				int rc = read(ev.data.fd, buf, sizeof buf - 1);
				if (rc > 0) {
					buf[rc] = 0;
					outf(0, "mark", "str=%s wall=%.3fs", buf, cur_wall_us() / 1e6);
				}
				/* relay upwards if connected */
				if (sock_up >= 0)
					notify_up(buf, rc);
			}
			if (ev.events & EPOLLHUP)
				close(ev.data.fd);
		}
	}
}

/* Returns the exit code of pid */
int post_mortem(int pid)
{
	unsigned long wall_usec;
	int status;
	int rc;

	wait_cgroup();

	print_current_time("end");

	print_zombie_stats(pid);

	struct cgroup_res_info res;
	read_cgroup(&res);
	print_cgroup_res_info(&res);

	rc = wait4(pid, &status, WNOHANG, NULL);
	if (rc != pid)
		quit("wait4");

	print_exit_status(status);

	wall_usec = cur_wall_us();

	outf(0, "walltime", "%.3fs", wall_usec / 1e6);
	outf(0, "loadavg", "%.2f", 1.0f * res.usage_usec / wall_usec);
	print_overhead(res.usage_usec);

	if (!opt_keep)
		try_rm_cgroup();
	else
		dbg(1, "Keeping cgroup in path '%s', you should manually delete it eventually.", cgroup_path);

	return WEXITSTATUS(status);
}

void setup_sock_down()
{
	struct sockaddr_un addr;
	char *sockname;
	int s, rc;

	/* FIXME: somewhere else? */
	sockname = tempnam("/tmp", "ramon");
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
	setenv(VAR_RAMONSOCK, sockname, 1);
	sock_down = s;

	free(sockname);
}

int connect_to_upstream()
{
	const char *up_path = getenv(VAR_RAMONSOCK);
	struct sockaddr_un addr;
	int rc, s;

	if (!up_path) {
		warn("$" VAR_RAMONSOCK " unset; not a nested invocation?");
		return -1;
	}

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) {
		warn ("Could not allocate unix socket");
		return s;
	}

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, up_path, 108);

	rc = connect(s, &addr, sizeof addr);
	if (rc < 0) {
		warn("Could not connect to upstream");
		return rc;
	}

	sock_up = s;
	return 0;
}

void setup()
{
	const char *e_ramonroot = getenv(VAR_RAMONROOT);
	int rc;

	if (!e_ramonroot) {
		/*
		 * Fresh invocation: create a fresh cgroup, and open a socket
		 * to listen for subinvocations. Expose the socket via an environment
		 * variable.
		 */
		find_cgroup_fs();
		make_new_cgroup();
	} else {
		/* subinvocation, nest within the parent's cgroup and connect */
		strcpy(cgroupfs_root, e_ramonroot);
		make_sub_cgroup(e_ramonroot);
		rc = connect_to_upstream();
		if (rc < 0)
			exit(rc);
	}

	/* enable memory controller on new group */
	{
		FILE *f = fopenat(cgroup_fd, "cgroup.subtree_control", "w");
		if (!f)
			quit("cannot open subtree control");

		int rc = fprintf(f, "+memory +pids");
		if (rc != 13)
			warn("couldn't enable memory controller?");
		fclose(f);
		write(gopipe[1], "x", 1);
		close(gopipe[0]);
	}


	if (opt_maxmem) {
		FILE *f = fopenat(cgroup_fd, "memory.max", "w");
		if (!f)
			quit("cannot limit memory");

		fprintf(f, "%li", opt_maxmem);
		fclose(f);
	}

	if (opt_maxcpu) {
		/* FILE *f = fopenat(cgroup_fd, "cpu.max", "w"); */
		/* if (!f) */
			quit("cannot limit cpu (IOU)");

		/* fprintf(f, "%li", opt_maxmem); */
		/* fclose(f); */
	}

	/*
	 * Re-set the root, even if we are subinvocation: messages are
	 * passed upwards (TODO!).
	 */
	setenv(VAR_RAMONROOT, cgroup_path, 1);
	dbg(2, "cgroup is '%s'", cgroup_path);
	setup_sock_down();
}

int spawn(int argc, char **argv)
{
	int pid;

	for (int i = 0; i < argc; i++)
		outf(1, "argv", "%i = %s", i, argv[i]);

	/* flush before forking */
	fflush(NULL);

	pid = fork();
	if (pid)
		return pid;

	/*
	 * Child just executes the given command, exit with 127
	 * (standard for 'command not found') otherwise.
	 */

	/* Put self in fresh cgroup */
	put_in_cgroup();

	/* wait for go signal */
	close(gopipe[1]);
	char x;
	read(gopipe[0], &x, 1);

	/* Child should not have signals blocked */
	restore_signals();

	/* Maybe limit stack */
	if (opt_maxstack)
		limit_own_stack(opt_maxstack);

	/*
	 * Close outfile if we opened one. All other files which remain
	 * were opened with O_CLOEXEC.
	 */
	if (opt_outfile)
		fclose(opt_fout);

	/* TODO: does this really drop privileges? */
	dbg(2, "getuid() = %i", getuid());
	setuid(getuid());

	/* Execute given command */
	execvp(argv[0], argv);

	/* exec() failed if we reach here */
	perror(argv[0]);
	exit(127);
}

int exec_and_monitor(int argc, char **argv)
{
	int rc;

	prepare_monitor();

	child_pid = spawn(argc, argv);
	outf(1, "childpid", "%lu", child_pid);

	wait_monitor();

	rc = post_mortem(child_pid);

	if (opt_outfile)
		fclose(opt_fout);

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
	int optind;

	optind = parse_opts(argc, argv, false, ramon_opts);
	if (optind < 0) {
		fprintf(stderr, "Use '-h' to see the list of options.\n");
		exit(1);
	}

	if (opt_outfile && !opt_tee)
		opt_stderr = false;

	if (opt_render && !opt_outfile)
		quit("An output file is needed to use --render");

	/* Maybe redirect output */
	if (opt_outfile) {
		int flags = O_WRONLY | O_CREAT | O_TRUNC | (opt_noclobber ? O_EXCL : 0);
		int fd = open(opt_outfile, flags, 0644);
		int ctr=0;
		if (opt_noclobber) {
			while (fd < 0 && errno == EEXIST) {
				char buf[200];
				sprintf(buf, "%s%i", opt_outfile, ctr++);
				fd = open(buf, flags, 0644);
			}
		}
		opt_fout = fdopen(fd, "w");
		if (!opt_fout)
			quit(opt_outfile);
	} else if (opt_save) {
		char temp[] = "XXXXXX.ramon";
		opt_fout = fmkstemps(temp, 6);
		if (!opt_fout)
			quit("could not create save file %s", temp);
		dbg(1, "Saving output to %s", temp);
	}

	/* Tally mode: just parse a cgroup dir and exit,
	 * no running anything. */
	if (opt_tally) {
		cgroup_fd = open(opt_tally, O_DIRECTORY | O_CLOEXEC);
		if (cgroup_fd < 0)
			quit("open cgroup dir");
		struct cgroup_res_info res;
		read_cgroup(&res);
		print_cgroup_res_info(&res);
		return 0;
	}

	if (opt_mark) {
		/* If we cannot connect, just warn and exit successfully */
		rc = connect_to_upstream();
		if (rc < 0)
			return 0;
		notify_up(opt_mark, strlen(opt_mark));
		return 0;
	}

	if (optind == argc) {
		fprintf(stderr, "%s: no command given\n", argv[0]);
		fprintf(stderr, "Use '-h' to see the list of options.\n");
		return 1;
	}

	pipe(gopipe);

	setup();

	rc = exec_and_monitor(argc - optind, argv + optind);

	if (opt_render) {
		assert(opt_outfile);
		char cmd[500];
		int rc;
		snprintf(cmd, sizeof cmd, "ramon-render %s", opt_outfile);
		rc = system(cmd);
		if (rc)
			warn("ramon-render failed");
	}

	return rc;
}
