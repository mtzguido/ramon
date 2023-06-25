#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "opts.h"

void __opt_inc_cb(void *par, const char *arg __attribute__((unused)))
{
	int *i = par;
	(*i)++;
}

static int handle1(struct opt *o, bool negated, const char *optarg)
{
	if (o->cb)
		o->cb(o->par, optarg);
	if (o->wo_long)
		*o->wo_long = atol(optarg);
	if (o->wo_str)
		*o->wo_str = optarg;
	if (o->wo_bool)
		*o->wo_bool = !negated;
	return 0;
}

static int parse_long(int nopts, struct opt opts[], const char *optname, const char *maybearg)
{
	bool negated = false;
	int i;

	if (strlen(optname) > 3 && strncmp(optname, "no-", 3) == 0) {
		optname += 3;
		negated = true;
	}

	for (i = 0; i < nopts; i++) {
		/* TODO: handle --opt=arg */
		if (opts[i].longname && !strcmp(optname, opts[i].longname)) {
			handle1(&opts[i], negated, opts[i].has_arg == HAS_ARG_YES ? maybearg : NULL);

			if (opts[i].has_arg == HAS_ARG_YES)
				return 1;
			else
				return 0;
		}
	}

	fprintf(stderr, "unknown option: '--%s'\n", optname);
	return -1;
}

/* returns 1 iff took rest as arg */
static int parse_short(int nopts, struct opt opts[], char c, const char *maybearg)
{
	int i;
	for (i = 0; i < nopts; i++) {
		if (opts[i].shortname && c == opts[i].shortname) {
			if (opts[i].has_arg != HAS_ARG_NO && maybearg) {
				handle1(&opts[i], false, maybearg);
				return 1;
			} else {
				handle1(&opts[i], false, NULL);
				return 0;
			}
		}
	}

	fprintf(stderr, "unknown option: '-%c'\n", c);
	return -1;
}

static int parse_shorts(int nopts, struct opt opts[], const char *opt, const char *maybearg)
{
	bool bundled = opt[1] != 0;
	int rc;

	if (bundled) {
		rc = parse_short(nopts, opts, opt[0], &opt[1]);
		if (rc < 0)
			return rc;
		/* if the parser used the arg, we must anyway not advance, as it was
		 * in the same arg */
		if (rc == 1)
			return 0;
	} else if (opt[1] == 0) {
		rc = parse_short(nopts, opts, opt[0], maybearg);
		if (rc < 0 || rc > 0)
			return rc;
	}

	const char *c;
	for (c = &opt[1]; *c; c++) {
		rc = parse_short(nopts, opts, *c, NULL);
		if (rc < 0)
			return rc;
	}

	return 0;
}

int parse_opts(int argc, char **argv, int shuffle, struct opt opts[])
{
	int nopts;
	int i;

	for (nopts = 0; opts[nopts].longname || opts[nopts].shortname; nopts++)
		;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--")) {
			break;
		} else if (!strncmp(argv[i], "--", 2)) {
			int rc = parse_long(nopts, opts, argv[i]+2, i+1 < argc ? argv[i+1] : NULL);
			if (rc < 0)
				return rc;
			i += rc;
		} else if (!strncmp(argv[i], "-", 1)) {
			int rc = parse_shorts(nopts, opts, argv[i]+1, i+1 < argc ? argv[i+1] : NULL);
			if (rc < 0)
				return rc;
			i += rc;
		} else {
			/* not an option */

			if (!shuffle)
				break;
		}
	}

	return i;
}

void print_opts(FILE *f, struct opt opts[])
{
	int i;
	for (i = 0; opts[i].longname || opts[i].shortname; i++) {
		if (opts[i].longname)
			fprintf(f, "  --%s ", opts[i].longname);
		if (opts[i].longname && opts[i].negatable)
			fprintf(f, "  --no-%s ", opts[i].longname);
		if (opts[i].shortname)
			fprintf(f, "  -%c ", opts[i].shortname);

		fprintf(f, "\n      %s\n", opts[i].desc);
	}
}
