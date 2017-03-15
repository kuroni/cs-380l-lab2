#pragma once

#define FUSE_USE_VERSION 26

#define _XOPEN_SOURCE 500

#include <limits.h>
#include <stdio.h>
#include <stdint.h>

struct bb_state {
  FILE *logfile;
  char *rootdir;
};

#define BB_DATA ((struct bb_state *) fuse_get_context()->private_data)