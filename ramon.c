#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdarg.h>
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
};

/* Global config state */
struct cfg cfg = {
	.outfile = NULL,
	.fout = NULL, /* set to stderr by main() */
};

const struct option longopts[] = {
	{ .name = "output", .has_arg = required_argument, .flag = NULL, .val = 'o' },
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

	fputs(key, cfg.fout);
	fputs("\t", cfg.fout);

	va_start(va, fmt);
	vfprintf(cfg.fout, fmt, va);
	va_end(va);
	fputs("\n", cfg.fout);
}

void monitor(int pid)
{
	struct rusage res;
	int status;
	int rc;

	rc = wait4(pid, &status, 0, &res);

	if (rc < 0)
		quit("wait4");

	outf("rc", "%i", WIFEXITED(status) ? WEXITSTATUS(status) : 9999);
	outf("cpu", "%.3fs", res.ru_utime.tv_sec + res.ru_utime.tv_usec / 1000000.0);
	outf("sys", "%.3fs", res.ru_stime.tv_sec + res.ru_stime.tv_usec / 1000000.0);
	outf("maxrss", "%likb", res.ru_maxrss);

	exit(WEXITSTATUS(status));
}

int main(int argc, char **argv)
{
	int pid;

	/* non-constant defaults */
	cfg.fout = stderr;

	parse_opts(argc, argv);

	if (cfg.outfile) {
		cfg.fout = fopen(cfg.outfile, "w");
		if (!cfg.fout)
			quit("fopen");
	}

	pid = fork();

	/* Child just executes the given command, exit with 127 (standard for
	 * 'command not found' otherwise. */
	if (!pid) {
		execvp(argv[optind], &argv[optind]);
		perror(argv[optind]);
		exit(127); // command not found
	}

	monitor(pid);

	/* Something went wrong if we reach this. */
	return -1;
}
