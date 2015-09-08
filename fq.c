/*
 * fq [FILES...] - follow output of nq jobs, quitting when they are done
 *
 * To the extent possible under law,
 * Christian Neukirchen <chneukirchen@gmail.com>
 * has waived all copyright and related or neighboring rights to this work.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DELAY 250000

#ifdef USE_INOTIFY
#include <sys/inotify.h>
char ibuf[8192];
#endif

char buf[8192];

static int
islocked(int fd)
{
  if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
    return (errno == EWOULDBLOCK);
  } else {
    flock(fd, LOCK_UN);
    return 0;
  }
}

static int
alphabetic(const void *a, const void *b)
{
  return strcmp(*(char **)a, *(char **)b);
}

int
main(int argc, char *argv[])
{
  int i, fd, dirfd;
  off_t off, loff;
  ssize_t rd;
  int didsth = 0, seen_nl = 0;
  int opt = 0, aflag = 0, qflag = 0;
  char *path;

#ifdef USE_INOTIFY
  int ifd, wd;
#endif

  close(0);

  while ((opt = getopt(argc, argv, "+aq")) != -1) {
    switch (opt) {
    case 'a':
      aflag = 1;
      break;
    case 'q':
      qflag = 1;
      break;
    default:
      fputs("usage: fq [-qa] [JOBID...]\n", stderr);
      exit(1);
    }
  }

  path = getenv("NQDIR");
  if (!path)
    path = ".";

#ifndef O_DIRECTORY
  /* manually check stat(2) for directory existence on systems w/o O_DIRECTORY,
   * such as Solaris or older Linux kernels. The O_DIRECTORY flag prevents a
   * race condition by opening a path and checking if it's a directory in one
   * system call, but this isn't available on all platforms.
   */
  struct stat statbuf;
  if (stat(path, &statbuf) == -1) {
    perror("stat");
    exit(111);
  }

  if (!S_ISDIR(statbuf.st_mode)) {
    perror("S_ISDIR");
    exit(111);
  }

  dirfd = open(path, O_RDONLY);
#else
  dirfd = open(path, O_RDONLY | O_DIRECTORY);
#endif
  if (dirfd < 0) {
    perror("open dir");
    exit(111);
  }

  if (optind == argc) {  /* behave as if $NQDIR/,* was passed. */
    DIR *dir;
    struct dirent *d;
    int len = 0;

    argc = 0;
    argv = 0;
    optind = 0;

    dir = fdopendir(dirfd);
    if (!dir)
      exit(111);

    while ((d = readdir(dir))) {
      if (d->d_name[0] != ',')
        continue;
      if (argc >= len) {
        len = 2*len + 1;
        argv = realloc(argv, len * sizeof (char *));
        if (!argv)
          exit(222);
      }
      argv[argc] = strdup(d->d_name);
      if (!argv[argc])
        exit(222);
      argc++;
    }

    qsort(argv, argc, sizeof (char *), alphabetic);
  }

#ifdef USE_INOTIFY
  ifd = inotify_init();
  if (ifd < 0)
    exit(111);
#endif

  for (i = optind; i < argc; i++) {
    loff = 0;
    seen_nl = 0;

    fd = openat(dirfd, argv[i], O_RDONLY);
    if (fd < 0)
      continue;

    /* skip not running jobs, unless -a was passed, or we did not
     * output anything yet and are at the last argument.  */
    if (!aflag && !islocked(fd) && (didsth || i != argc - 1))
      continue;

    write(1, "==> ", 4);
    write(1, argv[i], strlen(argv[i]));
    write(1, qflag ? " " : "\n", 1);

    didsth = 1;

#ifdef USE_INOTIFY
    wd = inotify_add_watch(ifd, argv[i], IN_MODIFY | IN_CLOSE_WRITE);
#endif

    while (1) {
      off = lseek(fd, 0, SEEK_END);

      if (off < loff)
        loff = off;               /* file truncated */

      if (off == loff) {
        if (flock(fd, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK) {
#ifdef USE_INOTIFY
          /* any inotify event is good */
          read(ifd, ibuf, sizeof ibuf);
#else
          /* poll for size change */
          while (off == lseek(fd, 0, SEEK_END))
            usleep(DELAY);
#endif
          continue;
        } else {
          flock(fd, LOCK_UN);
          break;
        }
      }

      if (off - loff > sizeof buf)
        off = loff + sizeof buf;

      rd = pread(fd, &buf, off - loff, loff);
      if (qflag) {
        if (!seen_nl) {
          char *s;
          if ((s = memchr(buf, '\n', rd))) {
            write(1, buf, s+1-buf);
            seen_nl = 1;
          } else {
            write(1, buf, rd);
          }
        }
      } else {
        write(1, buf, rd);
      }

      loff += rd;
    }

    if (qflag && !seen_nl)
      write(1, "\n", 1);

#ifdef USE_INOTIFY
    inotify_rm_watch(ifd, wd);
#endif
    close(fd);
  }

#ifdef USE_INOTIFY
  close(ifd);
#endif
  return 0;
}
