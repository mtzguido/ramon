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
	bool recursive;
	bool keep;
	const char *tally;
	int debug_level;
	int verbosity;
	bool save; /* save to a fresh file */
	int pollms;
	char *notify;
	bool render;
};

/* Global config state */
struct cfg cfg = {
	.outfile = NULL,
	.fout = NULL, /* set to stderr by main() */
	.recursive = false,
	.keep = false,
	.tally = NULL,
	.debug_level = 1,
	.verbosity = 1, /* TODO: choose defaults. */
	.pollms = 1000,
	.notify = NULL,
	.render = false,
};

struct cgroup_res_info
{
	long usage_usec;
	long user_usec;
	long system_usec;
	long mempeak;
	long pidpeak;
};

/* open directory fd for our cgroup */
int cgroup_fd;
/* parent cgroup directory */
char cgroupfs_root[PATH_MAX];
/* our cgroup */
char cgroup_path[PATH_MAX];

/* start and finish timestamps of subprocess */
struct timespec t0, t1;

int childpid;

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
		if (cfg.debug_level >= n)	\
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
	/* { .name = "recursive",    .has_arg = no_argument,       .flag = NULL, .val = 'r' }, // FIXME: cook up a library for this crap */
	/* { .name = "no-recursive", .has_arg = no_argument,       .flag = NULL, .val = '1' }, */
	{ .name = "keep-cgroup",  .has_arg = no_argument,       .flag = NULL, .val = 'k' },
	{ .name = "tally",        .has_arg = required_argument, .flag = NULL, .val = 't' },
	{ .name = "save",         .has_arg = no_argument,       .flag = NULL, .val = 's' },
	{ .name = "poll",         .has_arg = optional_argument, .flag = NULL, .val = 'p' },
	{ .name = "help",         .has_arg = no_argument,       .flag = NULL, .val = 'h' },
	{ .name = "notify",       .has_arg = required_argument, .flag = NULL, .val = 'n' },
	{ .name = "render",       .has_arg = no_argument,       .flag = NULL, .val = 'r' },
	/* { .name = "debug",        .has_arg = optional_argument, .flag = NULL, .val = 'd' }, */
	{0},
};

void help(const char *progname)
{
	fprintf(stderr, "%s: IOU a manual!\n", progname);
}

void parse_opts(int argc, char **argv)
{
	int rc;

	while (1) {
		rc = getopt_long(argc, argv, "+o:r1kt:dqsphv", longopts, NULL);
		/* printf("opt = '%c', optarg = %s\n", rc, optarg); */
		switch (rc) {
		case 'o':
			cfg.outfile = optarg;
			break;

		case 't':
			cfg.tally = optarg;
			break;

		/* case 'r': */
		/*         warn("ignored"); */
		/*         cfg.recursive = true; */
		/*         break; */

		/* case '1': */
		/*         warn("ignored"); */
		/*         cfg.recursive = false; */
		/*         break; */

		case 'k':
			cfg.keep = true;
			break;

		case 'd':
			cfg.debug_level++;
			break;

		case 'v':
			cfg.verbosity++;
			break;

		case 'q':
			if (cfg.debug_level > 0)
				cfg.debug_level--;
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

		case -1:
			return;

		case '?':
		default:
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
		quit("rmdir");
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

bool any_in_cgroup()
{
	bool ret = false;
	unsigned long n;
	FILE *f;

	f = fopenat(cgroup_fd, "cgroup.procs", "r");
	if (!f)
		quit("open cgroup");

	while (fscanf (f, "%lu", &n) > 0) {
		dbg(1, "A subprocess is still alive after main process finished (pid = %lu)", n);
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
		quit("open cgroup");

	rc = write(fd, "1", 1);
	if (rc != 1)
		quit("write cgroup.kill");

	close(fd);
}

void destroy_cgroup()
{
	if (any_in_cgroup()) {
		dbg(1, "Killing remaining processes");
		kill_cgroup();
	}

	/* FIXME: There is a race here between killing the group
	 * and being able to remove it. */

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
}

void print_cgroup_res_info(struct cgroup_res_info *res)
{
	outf(0, "group.usage", "%.3fs", res->usage_usec / 1000000.0);
	outf(0, "group.user", "%.3fs", res->user_usec / 1000000.0);
	outf(0, "group.system", "%.3fs", res->system_usec / 1000000.0);

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

	outf(0, "poll", "wall=%.3fs usage=%.3fs user=%.3fs sys=%.3fs load=%.2f",
			wall_us / 1000000.0,
			res.usage_usec / 1000000.0,
			res.user_usec / 1000000.0,
			res.system_usec / 1000000.0,
			1.0 * (res.usage_usec - last_poll_usage) / delta_us);
	fflush(cfg.fout);

	last_poll_usage = res.usage_usec;
	last_poll_ts = ts;
}


void print_exit_status(int status)
{
	int lvl = WIFEXITED(status) ? 1 : 0;
	outf(lvl, "status", wifstring(status));

	if (WIFEXITED(status)) {
		outf(lvl, "exitcode", "%i", WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		int sig = WTERMSIG(status);
		outf(lvl, "signal", "%i (SIG%s %s)", sig, signame(sig), strsignal(sig));
		outf(lvl, "core dumped", "%s", WCOREDUMP(status) ? "true" : "false");
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

	kill(childpid, sig);

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

/* Returns the exit code of pid */
int wait_monitor(int pid)
{
	struct epoll_event ev;
	unsigned long wall_usec;
	int timeout;
	int status;
	int rc;

	timeout = cfg.pollms > 0 ? cfg.pollms: -1;

	dbg(2, "entering event loop");
	dbg(2, "sock_down = %i", sock_down);
	dbg(2, "sock_up = %i", sock_up);

	while (1) {
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
				rc = wait4(pid, &status, WNOHANG, NULL);
				if (rc != pid)
					quit("wait4");
				break;
			} else {
				assert(!"wtf signal?");
			}
		} else if (ev.data.fd == sock_down) {
			int fd = accept(sock_down, NULL, NULL);
			if (fd < 0)
				quit("accept???");

			dbg(0, "accepted conn,fd = %i", fd);

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

	print_exit_status(status);

	struct cgroup_res_info res;
	read_cgroup(&res);
	print_cgroup_res_info(&res);

	/* TODO: is the getrusage thing useful? */
	/* struct rusage res = child; */
	/* outf("root.cpu", "%.3fs", res.ru_utime.tv_sec + res.ru_utime.tv_usec / 1000000.0); */
	/* outf("root.sys", "%.3fs", res.ru_stime.tv_sec + res.ru_stime.tv_usec / 1000000.0); */
	/* outf("root.maxrss", "%liKB", res.ru_maxrss); */

	wall_usec = 1000000 * (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1000;

	outf(0, "walltime", "%.3fs", wall_usec / 1000000.0);
	outf(0, "loadavg", "%.2f", 1.0f * res.usage_usec / wall_usec);

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
	/*
	 * Re-set the root, even if we are subinvocation: messages are
	 * passed upwards.
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

	clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
	pid = fork();
	if (!pid) {
		/*
		 * Child just executes the given command, exit with 127
		 * (standard for * 'command not found' otherwise.
		 */

		/* Put self in fresh cgroup */
		put_in_cgroup();

		/*
		 * Close outfile if we opened one, we did not use
		 * O_CLOEXEC for it.
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

	childpid = pid;
	outf(1, "childpid", "%lu", pid);

	rc = wait_monitor(pid);

	if (cfg.outfile)
		fclose(cfg.fout);

	close(sock_down);
	dbg(2, "sock_down_path = %s", sock_down_path);
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
