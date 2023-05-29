/* catch and release */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
	const int sz = 1 << 30;
	char *p = malloc(sz);
	memset(p, 42, sz);
	free(p);
	return 0;
}
