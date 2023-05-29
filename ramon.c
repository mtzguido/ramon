#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>

void quit(char *s) {
	perror(s);
	exit(1);
}

struct cfg {
	const char *outfile;
	FILE *fout;
	bool recursive;
};

/* Global config state */
struct cfg cfg = {
	.outfile = NULL,
	.fout = NULL, /* set to stderr by main() */
	.recursive = false,
};

const struct option longopts[] = {
	{ .name = "output",       .has_arg = required_argument, .flag = NULL, .val = 'o' },
	{ .name = "recursive",    .has_arg = no_argument,       .flag = NULL, .val = 'r' }, // FIXME: cook up a library for this crap
	{ .name = "no-recursive", .has_arg = no_argument,       .flag = NULL, .val = '1' },
	{0},
};

void parse_opts(int argc, char **argv)
{
	int rc;

	while (1) {
		rc = getopt_long(argc, argv, "+o:", longopts, NULL);
		switch (rc) {
		case 'o':
			cfg.outfile = optarg;
			break;

		case 'r':
			cfg.recursive = true;
			break;

		case '1':
			cfg.recursive = false;
			break;

		case -1:
			return;

		case '?':
		default:
			exit(1);
		}
	}
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
	if (WIFEXITED(status)) return "normal";
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

void monitor(int pid)
{
	struct rusage self, child;
	int status;
	int rc;

	rc = waitpid(pid, &status, 0);
	if (rc < 0)
		quit("wait4");

	outf("status", wifstring(status));

	if (WIFEXITED(status))
		outf("exitcode", "%i", WEXITSTATUS(status));

	if (WIFSIGNALED(status)) {
		int sig = WTERMSIG(status);
		outf("signal", "%i (SIG%s %s)", sig, sigabbrev_np(sig), strsignal(sig));
		outf("core dumped", "%s", WCOREDUMP(status) ? "true" : "false");
	}

	getrusage(RUSAGE_CHILDREN, &child);
	/* getrusage(RUSAGE_SELF, &self); */

	/* struct rusage res = rusage_sub(child, self); */
	struct rusage res = child;

	outf("cpu", "%.3fs", res.ru_utime.tv_sec + res.ru_utime.tv_usec / 1000000.0);
	outf("sys", "%.3fs", res.ru_stime.tv_sec + res.ru_stime.tv_usec / 1000000.0);
	outf("maxrss", "%likb", res.ru_maxrss);

	exit(WEXITSTATUS(status));
}

void usage()
{
	fprintf(stderr, "rtfm!\n");
}

int main(int argc, char **argv)
{
	int pid;

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

	pid = fork();

	/* Child just executes the given command, exit with 127 (standard for
	 * 'command not found' otherwise. */
	if (!pid) {
		execvp(argv[optind], &argv[optind]);

		/* exec failed if we reach here */
		perror(argv[optind]);
		exit(127);
	}

	monitor(pid);

	/* something very bad happened if we reach here */
	quit("wat????");
	return -1;
}
