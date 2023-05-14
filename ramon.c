#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>

void quit(char *s) {
	perror(s);
	exit(1);
}

int main(int argc, char **argv)
{
	int pid;
	int rc;
	FILE *fout = stderr;

	if (argc > 1 && !strcmp(argv[1], "-o")) {
		fout = fopen(argv[2], "w");
		if (!fout)
			quit("fopen");
		argv[2] = argv[0];
		argv += 2;
		argc-=2;
	}

	pid = fork();
	if (!pid) {
		execvp(argv[1], &argv[1]);
		exit(127); /* if we couldn't exec, exit with 127 */
	} else {
		int status;
		struct rusage res;
		rc = wait4(pid, &status, 0, &res);
		if (rc < 0)
			quit("wait4");

		fprintf(fout, "CPU time = %li.%03lis\n", res.ru_utime.tv_sec, res.ru_utime.tv_usec / 1000);
		fprintf(fout, "System time = %li.%03lis\n", res.ru_stime.tv_sec, res.ru_utime.tv_usec / 1000);
		fprintf(fout, "Max RSS = %likb\n", res.ru_maxrss);

		exit(WEXITSTATUS(status));
	}
}
