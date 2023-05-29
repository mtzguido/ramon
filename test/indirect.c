#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

int main()
{
	int pid = fork();

	if (pid) {
		const size_t sz = (size_t)1 << 31;
		char *p = malloc(sz);
		memset(p, 42, sz);
		free(p);
		waitpid(pid, NULL, 0);
	} else {
		execl("./car.exe", "car.exe", NULL);
	}
	return 0;
}
