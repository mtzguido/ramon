#ifndef __OPTS_H
#define __OPTS_H 1

#include <stdio.h>

enum hasarg {
	HAS_ARG_NO = 0,
	HAS_ARG_OPTIONAL = 1,
	HAS_ARG_YES = 2,
};

struct opt {
	const char *longname;
	char shortname;
	bool negatable;
	enum hasarg has_arg;
	const char *desc;

	void *par;
	void (*cb)(void *par, const char *arg);
	bool *wo_bool;
	long *wo_long;
	const char **wo_str;
};

int parse_opts(int argc, char **argv, int shuffle, struct opt opts[]);
void print_opts(FILE *f, struct opt opts[]);

#define OPT_BOOL(_longname, _shortname, _desc, wo)			\
	{ .longname = _longname, .shortname = _shortname,		\
	  .negatable = true, .has_arg = HAS_ARG_NO, .desc = _desc,	\
	  .par = NULL, .cb = NULL, .wo_bool = wo, .wo_long = NULL,	\
	  .wo_str = NULL }

#define OPT_INT(_longname, _shortname, _desc, wo)			\
	{ .longname = _longname, .shortname = _shortname,		\
	  .negatable = false, .has_arg = HAS_ARG_YES, .desc = _desc,	\
	  .par = NULL, .cb = NULL, .wo_bool = NULL, .wo_long = wo,	\
	  .wo_str = NULL }

#define OPT_STR(_longname, _shortname, _desc, wo)			\
	{ .longname = _longname, .shortname = _shortname,		\
	  .negatable = false, .has_arg = HAS_ARG_YES, .desc = _desc,	\
	  .par = NULL, .cb = NULL, .wo_bool = NULL, .wo_long = NULL,	\
	  .wo_str = wo }

#define OPT_STRBOOL(_longname, _shortname, _desc, wo, _wo_bool)		\
	{ .longname = _longname, .shortname = _shortname,		\
	  .negatable = false, .has_arg = HAS_ARG_YES, .desc = _desc,	\
	  .par = NULL, .cb = NULL, .wo_bool = _wo_bool, .wo_long = NULL,	\
	  .wo_str = wo }

#define OPT_ACTION(_longname, _shortname, _desc, _par, _cb)		\
	{ .longname = _longname, .shortname = _shortname,		\
	  .negatable = false, .has_arg = HAS_ARG_NO, .desc = _desc,	\
	  .par = _par, .cb = _cb, .wo_bool = NULL, .wo_long = NULL,	\
	  .wo_str = NULL }

void __opt_inc_cb(void *par, const char *arg);

#define OPT_INC(_longname, _shortname, _desc, wo)			\
	{ .longname = _longname, .shortname = _shortname,		\
	  .negatable = false, .has_arg = HAS_ARG_NO, .desc = _desc,	\
	  .par = wo, .cb = __opt_inc_cb, .wo_bool = NULL,		\
	  .wo_long = NULL, .wo_str = NULL }

#define OPT_END		{ .longname = NULL, .shortname = 0 }

#endif
