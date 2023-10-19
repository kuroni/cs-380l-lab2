/*
  Big Brother File System
*/

#define HAVE_SYS_XATTR_H 1

#include "params.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif

#include "log.h"

void sys_error(const char* msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

//  All the paths are relative to the root of the mounted
//  filesystem.
static void bb_fullpath(char fpath[PATH_MAX], const char *path) {
  strcpy(fpath, BB_DATA->rootdir);
  strncat(fpath, path, PATH_MAX); // ridiculously long paths will
  log_msg("    bb_fullpath:  rootdir = \"%s\", path = \"%s\", fpath = \"%s\"\n", BB_DATA->rootdir, path, fpath);
}

/////// SSH stuff

void ssh_free_session(ssh_session session) {
  ssh_disconnect(session);
  ssh_free(session);
}

void ssh_error(ssh_session session) {
  log_msg("SSH Error: %s\n", ssh_get_error(session));
  ssh_free_session(session);
  exit(SSH_ERROR);
}

int ssh_execute(ssh_session session, char* command, char* output, int size) {
  ssh_channel channel = ssh_channel_new(session);
  if (channel == NULL) ssh_error(session);
  int rc;
  if ((rc = ssh_channel_open_session(channel)) != SSH_OK) {
    ssh_channel_free(channel);
    return SSH_ERROR;
  }
  if ((rc = ssh_channel_request_exec(channel, command)) != SSH_OK) {
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    return SSH_ERROR;
  }
  int r = 0;
  while (1) {
    int rd = ssh_channel_read(channel, output + r, size - r, 0);
    if (rd < 0) {
      ssh_channel_close(channel);
      ssh_channel_free(channel);
      return SSH_ERROR;
    } else if (rd == 0) {
      break;
    } else {
      r += rd;
    }
  }
  
  output[r] = '\0';
  ssh_channel_send_eof(channel);
  ssh_channel_close(channel);
  ssh_channel_free(channel);
  return SSH_OK;
}

char* scp_receive(ssh_session session, ssh_scp scp, int *size) {
  int rc;
  int mode;
  char *filename, *buffer;

  rc = ssh_scp_pull_request(scp);
  if (rc != SSH_SCP_REQUEST_NEWFILE) {
    fprintf(stderr,"Error receiving information about file: %s\n",
          ssh_get_error(session));
    return NULL;
  }

  *size = ssh_scp_request_get_size(scp);
  filename = strdup(ssh_scp_request_get_filename(scp));
  mode = ssh_scp_request_get_permissions(scp);
  fprintf(stderr,"Receiving file %s, size %d, permissions 0%o\n",
          filename, *size, mode);
  free(filename);

  buffer = (char *)malloc((*size + 1) * sizeof(char));
  if (buffer == NULL) {
    fprintf(stderr,"Memory allocation error\n");
    return NULL;
  }

  ssh_scp_accept_request(scp);
  for (int r = 0; r < *size; ) {
    int st = ssh_scp_read(scp, buffer + r, *size - r);
    if (rc == SSH_ERROR) {
      fprintf(stderr,"Error receiving file data: %s\n",
              ssh_get_error(session));
      free(buffer);
      return NULL;
    }
    r += st;
  }
  buffer[*size] = '\0';

  rc = ssh_scp_pull_request(scp);
  if (rc != SSH_SCP_REQUEST_EOF) {
    fprintf(stderr,"Unexpected request: %s\n",
            ssh_get_error(session));
    return NULL;
  }

  return buffer;
}

int scp_write_remote(ssh_session session, ssh_scp scp, char* fpath, char* buf, int size) {
  int rc;
  rc = ssh_scp_init(scp);
  if (rc != SSH_OK) {
    log_msg("Error initializing scp session: %s\n",
            ssh_get_error(BB_DATA->session));
    return rc;
  }
  rc = ssh_scp_push_file(scp, fpath, size, S_IRUSR |  S_IWUSR);
  if (rc != SSH_OK) {
    log_msg("Can't open remote file: %s\n",
            ssh_get_error(BB_DATA->session));
    return rc;
  }
  rc = ssh_scp_write(scp, buf, size);
  if (rc != SSH_OK) {
    log_msg("Can't write to remote file: %s\n",
            ssh_get_error(BB_DATA->session));
    return rc;
  }
  return SSH_OK;
}

/////// Local file caching system stuff

/**
 * Open remote path by caching in temp file
*/
int cache_open(const char *fpath, char localpath[]) {
  for (int i = 0; i < BB_DATA->num_cache; i++) {
    if (strcmp(BB_DATA->cache[i].remotepath, fpath) == 0) {
      BB_DATA->cache[i].access++;
      strcpy(localpath, BB_DATA->cache[i].localpath);
      log_msg("remote %s mapped to %s\n", fpath, localpath);
      return EXIT_SUCCESS;
    }
  }
  // no cached local file
  if (BB_DATA->num_cache == CACHE_SIZE) { // cache is full
    return EXIT_FAILURE;
  }
  int i = BB_DATA->num_cache++;
  BB_DATA->cache[i].remotepath = (char*)malloc(sizeof(char) * (strlen(fpath) + 1));
  strcpy(BB_DATA->cache[i].remotepath, fpath);
  BB_DATA->cache[i].localpath = tmpnam(NULL);
  BB_DATA->cache[i].access = 1;
  // pull file content from SSH to buf using SCP
  ssh_scp scp = ssh_scp_new(BB_DATA->session, SSH_SCP_READ, fpath);
  if (scp == NULL) {
    log_msg("Error allocating scp session: %s\n",
            ssh_get_error(BB_DATA->session));
    return EXIT_FAILURE;
  }
  int rc = ssh_scp_init(scp);
  if (rc != SSH_OK) {
    log_msg("Error initializing scp session: %s\n",
            ssh_get_error(BB_DATA->session));
    ssh_scp_free(scp);
    return rc;
  }
  int size;
  char* buf = scp_receive(BB_DATA->session, scp, &size);
  if (buf == NULL) {
    log_msg("error reading remote file %s\n", fpath);
    ssh_scp_close(scp);
    ssh_scp_free(scp);
    return EXIT_FAILURE;
  }
  ssh_scp_close(scp);
  ssh_scp_free(scp);
  // write file content from buf to local file
  FILE* f = fopen(BB_DATA->cache[i].localpath, "w");
  if (f == NULL) {
    log_error("fopen");
    return EXIT_FAILURE;
  }
  int nwrite = fwrite(buf, sizeof(char), size, f);
  if (nwrite < strlen(buf)) {
    log_error("fwrite");
    return EXIT_FAILURE;
  }

  strcpy(localpath, BB_DATA->cache[i].localpath);
  free(buf); fclose(f);
  log_msg("remote %s mapped to %s\n", fpath, localpath);
  return EXIT_SUCCESS;
}

/**
 * Close remote path. Flush if access to remote 
*/
int cache_close(const char *fpath) {
  for (int i = 0; i < BB_DATA->num_cache; i++) {
    if (strcmp(BB_DATA->cache[i].remotepath, fpath) == 0) {
      if (--BB_DATA->cache[i].access > 0) {
        return EXIT_SUCCESS;
      }
      // no more local access to file, time to flush to remote
      // pull file content from local to buf
      struct stat sb;
      int rc = lstat(BB_DATA->cache[i].localpath, &sb);
      if (rc != EXIT_SUCCESS) {
        log_error("lstat");
        return EXIT_FAILURE;
      }
      size_t size = sb.st_size;
      char *buf = (char*)malloc(sizeof(char) * (size + 1));
      FILE* f = fopen(BB_DATA->cache[i].localpath, "r");
      int nbytes = fread(buf, sizeof(char), size, f);
      if (nbytes < size) {
        log_error("fread");
        return EXIT_FAILURE;
      }
      fclose(f);
      // push file content from buf to remote
      ssh_scp scp = ssh_scp_new(BB_DATA->session, SSH_SCP_WRITE, fpath);
      if (scp == NULL) {
        log_msg("Error allocating scp session: %s\n",
                ssh_get_error(BB_DATA->session));
        free(buf);
        return EXIT_FAILURE;
      }
      rc = scp_write_remote(BB_DATA->session, scp, fpath, buf, size);

      ssh_scp_close(scp);
      ssh_scp_free(scp);
      free(buf);
      log_msg("mapping %s -> %s is severed\n", BB_DATA->cache[i].remotepath, BB_DATA->cache[i].localpath);
      free(BB_DATA->cache[i].localpath);
      free(BB_DATA->cache[i].remotepath);
      for (int j = i; j + 1 < BB_DATA->num_cache; j++) {
        BB_DATA->cache[j] = BB_DATA->cache[j + 1];
      }
      BB_DATA->num_cache--;
      // clean up here: remove file from cache + clean up pointers
      return rc;
    }
  }
  return EXIT_FAILURE; // no file in cache with name fpath
}

/////// BBFS stuff

/**
 * Get file attributes.
 */
int bb_getattr(const char *path, struct stat *statbuf) {
  char fpath[PATH_MAX];

  log_command("bb_getattr(path=\"%s\", statbuf=0x%08x)", path, statbuf);
  bb_fullpath(fpath, path);

  char output[BUF_SIZE], command[BUF_SIZE];

  // non-fs stats
  strcpy(command, "stat -c \"\%d \%i \%f \%h \%u \%g \%t \%s \%X \%Y \%Z \%b\" ");
  strncat(command, fpath, BUF_SIZE);
  int rc = ssh_execute(BB_DATA->session, command, output, BUF_SIZE);
  if (rc != 0) {
    log_msg("remote stat error\n");
    return EXIT_FAILURE;
  } else {
    rc = sscanf(output, "%lu %lu %x %u %u %u %lx %ld %ld %ld %ld %ld",
      &statbuf->st_dev, &statbuf->st_ino, &statbuf->st_mode, &statbuf->st_nlink,
      &statbuf->st_uid, &statbuf->st_gid, &statbuf->st_rdev, &statbuf->st_size,
      &statbuf->st_blocks, &statbuf->st_atime, &statbuf->st_mtime, &statbuf->st_ctime);
    if (rc != 12) {
      log_stat(statbuf);
      return EXIT_FAILURE;
    }
  }

  // fs stats
  strcpy(command, "stat -f -c \"\%s\" ");
  strncat(command, fpath, BUF_SIZE);
  rc = ssh_execute(BB_DATA->session, command, output, BUF_SIZE);
  if (rc != 0) {
    log_msg("remote stat error\n");
    return EXIT_FAILURE;
  } else {
    rc = sscanf(output, "%ld", &statbuf->st_blksize);
    if (rc != 1) {
      log_stat(statbuf);
      return EXIT_FAILURE;
    }
  }

  log_stat(statbuf);
}

/**
 * Read the target of a symbolic link
 */
int bb_readlink(const char *path, char *link, size_t size) {
  int retstat;
  char fpath[PATH_MAX];

  log_msg("bb_readlink(path=\"%s\", link=\"%s\", size=%d)", path, link, size);
  bb_fullpath(fpath, path);

  retstat = log_syscall("fpath", readlink(fpath, link, size - 1), 0);
  if (retstat >= 0) {
    link[retstat] = '\0';
    retstat = 0;
  }

  return retstat;
}

/**
 * Create a file node
 */
int bb_mknod(const char *path, mode_t mode, dev_t dev) {
  int retstat;
  char fpath[PATH_MAX];

  log_command("bb_mknod(path=\"%s\", mode=0%3o, dev=%lld)", path, mode, dev);
  bb_fullpath(fpath, path);
  if (S_ISREG(mode)) {
    retstat = log_syscall("open", open(fpath, O_CREAT | O_EXCL | O_WRONLY, mode), 0);
    if (retstat >= 0) {
      retstat = log_syscall("close", close(retstat), 0);
    }
  } else if (S_ISFIFO(mode)) {
    retstat = log_syscall("mkfifo", mkfifo(fpath, mode), 0);
  } else {
    retstat = log_syscall("mknod", mknod(fpath, mode, dev), 0);
  }

  return retstat;
}

/**
 * Create a directory
 */
int bb_mkdir(const char *path, mode_t mode) {
  char fpath[PATH_MAX];

  log_command("bb_mkdir(path=\"%s\", mode=0%3o)", path, mode);
  bb_fullpath(fpath, path);

  return log_syscall("mkdir", mkdir(fpath, mode), 0);
}

/**
 * Remove a file
 */
int bb_unlink(const char *path) {
  char fpath[PATH_MAX];

  log_command("bb_unlink(path=\"%s\")", path);
  bb_fullpath(fpath, path);

  return log_syscall("unlink", unlink(fpath), 0);
}

/**
 * Remove a directory
 */
int bb_rmdir(const char *path) {
  char fpath[PATH_MAX];

  log_command("bb_rmdir(path=\"%s\")", path);
  bb_fullpath(fpath, path);

  return log_syscall("rmdir", rmdir(fpath), 0);
}

/**
 * Create a symbolic link
 */
int bb_symlink(const char *path, const char *link) {
  char flink[PATH_MAX];

  log_command("bb_symlink(path=\"%s\", link=\"%s\")", path, link);
  bb_fullpath(flink, link);

  return log_syscall("symlink", symlink(path, flink), 0);
}

/**
 * Rename a file
 */
int bb_rename(const char *path, const char *newpath) {
  char fpath[PATH_MAX];
  char fnewpath[PATH_MAX];

  log_command("bb_rename(fpath=\"%s\", newpath=\"%s\")", path, newpath);
  bb_fullpath(fpath, path);
  bb_fullpath(fnewpath, newpath);

  return log_syscall("rename", rename(fpath, fnewpath), 0);
}

/**
 * Create a hard link to a file
 */
int bb_link(const char *path, const char *newpath) {
  char fpath[PATH_MAX], fnewpath[PATH_MAX];

  log_command("bb_link(path=\"%s\", newpath=\"%s\")", path, newpath);
  bb_fullpath(fpath, path);
  bb_fullpath(fnewpath, newpath);

  return log_syscall("link", link(fpath, fnewpath), 0);
}

/**
 * Change the permission bits of a file
 */
int bb_chmod(const char *path, mode_t mode) {
  char fpath[PATH_MAX];

  log_command("bb_chmod(fpath=\"%s\", mode=0%03o)", path, mode);
  bb_fullpath(fpath, path);

  return log_syscall("chmod", chmod(fpath, mode), 0);
}

/**
 * Change the owner and group of a file
 */
int bb_chown(const char *path, uid_t uid, gid_t gid) {
  char fpath[PATH_MAX];

  log_command("bb_chown(path=\"%s\", uid=%d, gid=%d)", path, uid, gid);
  bb_fullpath(fpath, path);

  return log_syscall("chown", chown(fpath, uid, gid), 0);
}

/**
 * Change the size of a file
 */
int bb_truncate(const char *path, off_t newsize) {
  char fpath[PATH_MAX];

  log_command("bb_truncate(path=\"%s\", newsize=%lld)", path, newsize);
  bb_fullpath(fpath, path);

  return log_syscall("truncate", truncate(fpath, newsize), 0);
}

/**
 * Change the access and/or modification times of a file
 */
int bb_utime(const char *path, struct utimbuf *ubuf) {
  char fpath[PATH_MAX];

  log_command("bb_utime(path=\"%s\", ubuf=0x%08x)", path, ubuf);
  bb_fullpath(fpath, path);

  return log_syscall("utime", utime(fpath, ubuf), 0);
}

/**
 * File open operation
 */
int bb_open(const char *path, struct fuse_file_info *fi) {
  int retstat = 0;
  int fd;
  char fpath[PATH_MAX];

  log_command("bb_open(path\"%s\", fi=0x%08x)",
              path, fi);
  bb_fullpath(fpath, path);

  char localpath[L_tmpnam + 1];
  int rc = cache_open(fpath, localpath);
  if (rc == EXIT_FAILURE) {
    log_msg("open failure\n");
    return rc;
  }

  fd = log_syscall("open", open(localpath, fi->flags), 0);
  if (fd < 0) {
    retstat = log_error("open");
  }

  fi->fh = fd;

  log_fi(fi);

  return retstat;
}

/**
 * Read data from an open file
 */
int bb_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
  log_command("bb_read(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)", path, buf, size, offset, fi);
  log_fi(fi);

  return log_syscall("pread", pread(fi->fh, buf, size, offset), 0);
}

/**
 * Write data to an open file
 */
int bb_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
  log_command("bb_write(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)", path, buf, size, offset, fi);
  log_fi(fi);

  return log_syscall("pwrite", pwrite(fi->fh, buf, size, offset), 0);
}

/**
 * Get file system statistics
 */
int bb_statfs(const char *path, struct statvfs *statv)
{
  int retstat = 0;
  char fpath[PATH_MAX];

  log_command("bb_statfs(path=\"%s\", statv=0x%08x)", path, statv);
  bb_fullpath(fpath, path);

  // get stats for underlying filesystem
  retstat = log_syscall("statvfs", statvfs(fpath, statv), 0);

  log_statvfs(statv);

  return retstat;
}

/**
 * Possibly flush cached data
 */
int bb_flush(const char *path, struct fuse_file_info *fi) {
  log_command("bb_flush(path=\"%s\", fi=0x%08x)", path, fi);
  log_fi(fi);

  return 0;
}

/**
 * Release an open file
 */
int bb_release(const char *path, struct fuse_file_info *fi) {
  log_command("bb_release(path=\"%s\", fi=0x%08x)", path, fi);
  log_fi(fi);

  int rc = log_syscall("close", close(fi->fh), 0);
  if (rc != EXIT_SUCCESS) {
    return rc;
  }
  char fpath[PATH_MAX];
  bb_fullpath(fpath, path);
  return cache_close(fpath);
}

/**
 * Synchronize file contents
 */
int bb_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
  log_command("bb_fsync(path=\"%s\", datasync=%d, fi=0x%08x)", path, datasync, fi);
  log_fi(fi);

  // some unix-like systems (notably freebsd) don't have a datasync call
#ifdef HAVE_FDATASYNC
  if (datasync)
      return log_syscall("fdatasync", fdatasync(fi->fh), 0);
    else
#endif
  return log_syscall("fsync", fsync(fi->fh), 0);
}

#ifdef HAVE_SYS_XATTR_H
/** Set extended attributes */
int bb_setxattr(const char *path, const char *name, const char *value, size_t size, int flags) {
  char fpath[PATH_MAX];

  log_command("bb_setxattr(path=\"%s\", name=\"%s\", value=\"%s\", size=%d, flags=0x%08x)", path, name, value, size,
              flags);
  bb_fullpath(fpath, path);

  return log_syscall("lsetxattr", lsetxattr(fpath, name, value, size, flags), 0);
}

/**
 * Get extended attributes
 */
int bb_getxattr(const char *path, const char *name, char *value, size_t size) {
  int retstat = 0;
  char fpath[PATH_MAX];

  log_command("bb_getxattr(path=\"%s\", name=\"%s\", value=0x%08x, size=%d)", path, name, value, size);
  bb_fullpath(fpath, path);

  retstat = log_syscall("lgetxattr", lgetxattr(fpath, name, value, size), 0);
  if (retstat >= 0) {
    log_msg("    value = \"%s\"\n", value);
  }

  return retstat;
}

/**
 * List extended attributes
 */
int bb_listxattr(const char *path, char *list, size_t size) {
  int retstat = 0;
  char fpath[PATH_MAX];
  char *ptr;

  log_command("bb_listxattr(path=\"%s\", list=0x%08x, size=%d)", path, list, size);
  bb_fullpath(fpath, path);

  retstat = log_syscall("llistxattr", llistxattr(fpath, list, size), 0);
  if (retstat >= 0) {
    log_msg("    returned attributes (length %d):\n", retstat);
    for (ptr = list; ptr < list + retstat; ptr += strlen(ptr)+1) {
      log_msg("    \"%s\"\n", ptr);
    }
  }

  return retstat;
}

/**
 * Remove extended attributes
 */
int bb_removexattr(const char *path, const char *name) {
  char fpath[PATH_MAX];

  log_command("bb_removexattr(path=\"%s\", name=\"%s\")", path, name);
  bb_fullpath(fpath, path);

  return log_syscall("lremovexattr", lremovexattr(fpath, name), 0);
}
#endif

/**
 * Open directory
 */
int bb_opendir(const char *path, struct fuse_file_info *fi) {
  DIR *dp;
  int retstat = 0;
  char fpath[PATH_MAX];

  log_command("bb_opendir(path=\"%s\", fi=0x%08x)", path, fi);
  bb_fullpath(fpath, path);

  dp = opendir(fpath);
  log_msg("    opendir returned 0x%p\n", dp);
  if (dp == NULL) {
    retstat = log_error("bb_opendir opendir");
  }

  fi->fh = (intptr_t) dp;

  log_fi(fi);

  return retstat;
}

/**
 * Read directory
 */

int bb_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
  int retstat = 0;
  DIR *dp;
  struct dirent *de;

  log_command("bb_readdir(path=\"%s\", buf=0x%08x, filler=0x%08x, offset=%lld, fi=0x%08x)",
              path, buf, filler, offset, fi);
  // once again, no need for fullpath -- but note that I need to cast fi->fh
  dp = (DIR *) (uintptr_t) fi->fh;

  de = readdir(dp);
  log_msg("    readdir returned 0x%p\n", de);
  if (de == 0) {
    retstat = log_error("bb_readdir readdir");
    return retstat;
  }

  do {
    if (filler(buf, de->d_name, NULL, 0) != 0) {
      log_msg("    ERROR bb_readdir filler:  buffer full");
      return -ENOMEM;
    }
  } while ((de = readdir(dp)) != NULL);

  log_fi(fi);

  return retstat;
}

/**
 * Release directory
 */
int bb_releasedir(const char *path, struct fuse_file_info *fi) {
  int retstat = 0;

  log_command("bb_releasedir(path=\"%s\", fi=0x%08x)", path, fi);
  log_fi(fi);

  closedir((DIR *) (uintptr_t) fi->fh);

  return retstat;
}

/**
 * Synchronize directory contents
 */
int bb_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi) {
  int retstat = 0;

  log_command("bb_fsyncdir(path=\"%s\", datasync=%d, fi=0x%08x)", path, datasync, fi);
  log_fi(fi);

  return retstat;
}

/**
 * Initialize filesystem
 *
 * The return value will passed in the private_data field of
 * fuse_context to all file operations and as a parameter to the
 * destroy() method.
 */
void *bb_init(struct fuse_conn_info *conn) {
  log_command("bb_init()");

  log_conn(conn);
  log_fuse_context(fuse_get_context());

  return BB_DATA;
}

/**
 * Clean up filesystem
 *
 * Called on filesystem exit.
 */
void bb_destroy(void *userdata) {
  log_command("bb_destroy(userdata=0x%08x)\n", userdata);
}

/** Check file access permissions */
int bb_access(const char *path, int mask) {
  int retstat = 0;
  char fpath[PATH_MAX];

  log_command("bb_access(path=\"%s\", mask=0%o)", path, mask);
  bb_fullpath(fpath, path);

  retstat = access(fpath, mask);

  if (retstat < 0) {
    retstat = log_error("bb_access access");
  }

  return retstat;
}

/**
 * Create and open a file
 */
// Not implemented.

/**
 * Change the size of an open file
 */
int bb_ftruncate(const char *path, off_t offset, struct fuse_file_info *fi) {
  int retstat = 0;

  log_command("bb_ftruncate(path=\"%s\", offset=%lld, fi=0x%08x)", path, offset, fi);
  log_fi(fi);

  retstat = ftruncate(fi->fh, offset);
  if (retstat < 0) {
    retstat = log_error("bb_ftruncate ftruncate");
  }

  return retstat;
}

/**
 * Get attributes from an open file
 */
int bb_fgetattr(const char *path, struct stat *statbuf, struct fuse_file_info *fi) {
  int retstat = 0;

  log_command("bb_fgetattr(path=\"%s\", statbuf=0x%08x, fi=0x%08x)", path, statbuf, fi);
  log_fi(fi);
  if (!strcmp(path, "/")) {
    return bb_getattr(path, statbuf);
  }

  retstat = fstat(fi->fh, statbuf);
  if (retstat < 0) {
    retstat = log_error("bb_fgetattr fstat");
  }

  log_stat(statbuf);

  return retstat;
}

struct fuse_operations bb_oper = {
    .getattr = bb_getattr,
    .readlink = bb_readlink,
    .getdir = NULL,
    .mknod = bb_mknod,
    .mkdir = bb_mkdir,
    .unlink = bb_unlink,
    .rmdir = bb_rmdir,
    .symlink = bb_symlink,
    .rename = bb_rename,
    .link = bb_link,
    .chmod = bb_chmod,
    .chown = bb_chown,
    .truncate = bb_truncate,
    .utime = bb_utime,
    .open = bb_open,
    .read = bb_read,
    .write = bb_write,
    .statfs = bb_statfs,
    .flush = bb_flush,
    .release = bb_release,
    .fsync = bb_fsync,

#ifdef HAVE_SYS_XATTR_H
    .setxattr = bb_setxattr,
    .getxattr = bb_getxattr,
    .listxattr = bb_listxattr,
    .removexattr = bb_removexattr,
#endif

    .opendir = bb_opendir,
    .readdir = bb_readdir,
    .releasedir = bb_releasedir,
    .fsyncdir = bb_fsyncdir,
    .init = bb_init,
    .destroy = bb_destroy,
    .access = bb_access,
    .ftruncate = bb_ftruncate,
    .fgetattr = bb_fgetattr
};

void bb_usage() {
  fprintf(stderr, "usage:  bbfs [FUSE and mount options] remoteAddress mountPoint logFile\n");
  abort();
}

int main(int argc, char *argv[]) {
  if ((getuid() == 0) || (geteuid() == 0)) {
    fprintf(stderr, "Please do not run bb as root\n");
    return EXIT_FAILURE;
  }

  if ((argc < 4) || (argv[argc - 3][0] == '-') || (argv[argc - 2][0] == '-') || (argv[argc - 1][0] == '-')) {
    bb_usage();
  }

  struct bb_state *bb_data = malloc(sizeof(struct bb_state));
  if (bb_data == NULL) {
    sys_error("malloc");
  }

  char *logFile = argv[argc - 1];
  argv[argc - 1] = NULL;
  char *remoteAddress = argv[argc - 3];
  argv[argc - 3] = argv[argc - 2];
  argv[argc - 2] = NULL;
  argc -= 2;

  bb_data->logfile = log_open(logFile);
  char user[BUF_SIZE], host[BUF_SIZE], remotepath[BUF_SIZE];
  if (sscanf(remoteAddress, "%[^@]@%[^:]:%s", user, host, remotepath) < 3) {
    fprintf(stderr, "cannot parse address");
    exit(EXIT_FAILURE);
  }
  bb_data->rootdir = remotepath;

  // intializing SSH session
  bb_data->session = ssh_new();
  if (bb_data->session == NULL) {
    fprintf(stderr, "cannot initialize ssh session");
    exit(SSH_ERROR);
  }

  ssh_options_set(bb_data->session, SSH_OPTIONS_HOST, host);
  ssh_options_set(bb_data->session, SSH_OPTIONS_USER, user);

  int rc = ssh_connect(bb_data->session);
  if (rc != SSH_OK) ssh_error(bb_data->session);

  rc = ssh_userauth_publickey_auto(bb_data->session, NULL, NULL);
  if (rc != SSH_AUTH_SUCCESS) ssh_error(bb_data->session);

  fprintf(stderr, "Connected to %s@%s\n", user, host);

  // starting fuse
  fprintf(stderr, "about to call fuse_main\n");
  int fuse_stat = fuse_main(argc, argv, &bb_oper, bb_data);
  fprintf(stderr, "fuse_main returned %d\n", fuse_stat);

  ssh_free_session(bb_data->session);
  free(bb_data);
  return fuse_stat;
}
