#pragma once

#define FUSE_USE_VERSION 26

#define _XOPEN_SOURCE 500

#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <fuse.h>
#include <getopt.h>
#include <libssh/libssh.h>

#define BUF_SIZE 4096
#define CACHE_SIZE 1024

struct file_cache_local {
  char *remotepath;
  char *localpath;
  int access;
};

struct bb_state {
  FILE *logfile;
  char *rootdir;
  ssh_session session; // ssh session
  // caching system
  struct file_cache_local cache[CACHE_SIZE];
  int num_cache;
};

#define BB_DATA ((struct bb_state *) fuse_get_context()->private_data)
