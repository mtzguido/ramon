SHELL := bash
MAKEFLAGS += -r # No builtin rules
MAKEFLAGS += --no-print-directory
.DELETE_ON_ERROR: # Delete failed targets


#.SECONDARY: # Do not delete intermediate files
# ^ This causes stupidness.
