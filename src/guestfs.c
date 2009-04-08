/* libguestfs
 * Copyright (C) 2009 Red Hat Inc. 
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <config.h>

#define _BSD_SOURCE /* for mkdtemp, usleep */
#define _GNU_SOURCE /* for vasprintf, GNU strerror_r, strchrnul */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <sys/select.h>
#include <rpc/types.h>
#include <rpc/xdr.h>

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#include "guestfs.h"
#include "guestfs_protocol.h"

static void error (guestfs_h *g, const char *fs, ...);
static void perrorf (guestfs_h *g, const char *fs, ...);
static void *safe_malloc (guestfs_h *g, size_t nbytes);
static void *safe_realloc (guestfs_h *g, void *ptr, int nbytes);
static char *safe_strdup (guestfs_h *g, const char *str);
static void *safe_memdup (guestfs_h *g, void *ptr, size_t size);

static void default_error_cb (guestfs_h *g, void *data, const char *msg);
static void stdout_event (void *data, int watch, int fd, int events);
static void sock_read_event (void *data, int watch, int fd, int events);
static void sock_write_event (void *data, int watch, int fd, int events);

static void close_handles (void);

static int select_add_handle (guestfs_h *g, int fd, int events, guestfs_handle_event_cb cb, void *data);
static int select_remove_handle (guestfs_h *g, int watch);
static int select_add_timeout (guestfs_h *g, int interval, guestfs_handle_timeout_cb cb, void *data);
static int select_remove_timeout (guestfs_h *g, int timer);
static void select_main_loop_run (guestfs_h *g);
static void select_main_loop_quit (guestfs_h *g);

#define UNIX_PATH_MAX 108

/* Also in guestfsd.c */
#define VMCHANNEL_PORT 6666
#define VMCHANNEL_ADDR "10.0.2.4"

/* Current main loop. */
static guestfs_main_loop main_loop = {
  .add_handle = select_add_handle,
  .remove_handle = select_remove_handle,
  .add_timeout = select_add_timeout,
  .remove_timeout = select_remove_timeout,
  .main_loop_run = select_main_loop_run,
  .main_loop_quit = select_main_loop_quit,
};

/* GuestFS handle and connection. */
enum state { CONFIG, LAUNCHING, READY, BUSY, NO_HANDLE };

struct guestfs_h
{
  struct guestfs_h *next;	/* Linked list of open handles. */

  /* State: see the state machine diagram in the man page guestfs(3). */
  enum state state;

  int fd[2];			/* Stdin/stdout of qemu. */
  int sock;			/* Daemon communications socket. */
  int pid;			/* Qemu PID. */
  time_t start_t;		/* The time when we started qemu. */

  int stdout_watch;		/* Watches qemu stdout for log messages. */
  int sock_watch;		/* Watches daemon comm socket. */

  char *tmpdir;			/* Temporary directory containing socket. */

  char **cmdline;		/* Qemu command line. */
  int cmdline_size;

  int verbose;
  int autosync;

  const char *path;

  /* Callbacks. */
  guestfs_abort_cb           abort_cb;
  guestfs_error_handler_cb   error_cb;
  void *                     error_cb_data;
  guestfs_reply_cb           reply_cb;
  void *                     reply_cb_data;
  guestfs_log_message_cb     log_message_cb;
  void *                     log_message_cb_data;
  guestfs_subprocess_quit_cb subprocess_quit_cb;
  void *                     subprocess_quit_cb_data;
  guestfs_launch_done_cb     launch_done_cb;
  void *                     launch_done_cb_data;

  /* These callbacks are called before reply_cb and launch_done_cb,
   * and are used to implement the high-level API without needing to
   * interfere with callbacks that the user might have set.
   */
  guestfs_reply_cb           reply_cb_internal;
  void *                     reply_cb_internal_data;
  guestfs_launch_done_cb     launch_done_cb_internal;
  void *                     launch_done_cb_internal_data;

  /* Messages sent and received from the daemon. */
  char *msg_in;
  int msg_in_size, msg_in_allocated;
  char *msg_out;
  int msg_out_size, msg_out_pos;

  int msg_next_serial;
};

static guestfs_h *handles = NULL;
static int atexit_handler_set = 0;

guestfs_h *
guestfs_create (void)
{
  guestfs_h *g;
  const char *str;

  g = malloc (sizeof (*g));
  if (!g) return NULL;

  memset (g, 0, sizeof (*g));

  g->state = CONFIG;

  g->fd[0] = -1;
  g->fd[1] = -1;
  g->sock = -1;
  g->stdout_watch = -1;
  g->sock_watch = -1;

  g->abort_cb = abort;
  g->error_cb = default_error_cb;
  g->error_cb_data = NULL;

  str = getenv ("LIBGUESTFS_DEBUG");
  g->verbose = str != NULL && strcmp (str, "1") == 0;

  str = getenv ("LIBGUESTFS_PATH");
  g->path = str != NULL ? str : GUESTFS_DEFAULT_PATH;
  /* XXX We should probably make QEMU configurable as well. */

  /* Start with large serial numbers so they are easy to spot
   * inside the protocol.
   */
  g->msg_next_serial = 0x00123400;

  /* Link the handles onto a global list.  This is the one area
   * where the library needs to be made thread-safe. (XXX)
   */
  /* acquire mutex (XXX) */
  g->next = handles;
  handles = g;
  if (!atexit_handler_set) {
    atexit (close_handles);
    atexit_handler_set = 1;
  }
  /* release mutex (XXX) */

  if (g->verbose)
    fprintf (stderr, "new guestfs handle %p\n", g);

  return g;
}

void
guestfs_close (guestfs_h *g)
{
  int i;
  char filename[256];
  guestfs_h *gg;

  if (g->state == NO_HANDLE) {
    /* Not safe to call 'error' here, so ... */
    fprintf (stderr, "guestfs_close: called twice on the same handle\n");
    return;
  }

  if (g->verbose)
    fprintf (stderr, "closing guestfs handle %p (state %d)\n", g, g->state);

  /* Try to sync if autosync flag is set. */
  if (g->autosync && g->state == READY)
    guestfs_sync (g);

  /* Remove any handlers that might be called back before we kill the
   * subprocess.
   */
  g->log_message_cb = NULL;

  if (g->state != CONFIG)
    guestfs_kill_subprocess (g);

  if (g->tmpdir) {
    snprintf (filename, sizeof filename, "%s/sock", g->tmpdir);
    unlink (filename);

    rmdir (g->tmpdir);

    free (g->tmpdir);
  }

  if (g->cmdline) {
    for (i = 0; i < g->cmdline_size; ++i)
      free (g->cmdline[i]);
    free (g->cmdline);
  }

  /* Mark the handle as dead before freeing it. */
  g->state = NO_HANDLE;

  /* acquire mutex (XXX) */
  if (handles == g)
    handles = g->next;
  else {
    for (gg = handles; gg->next != g; gg = gg->next)
      ;
    gg->next = g->next;
  }
  /* release mutex (XXX) */

  free (g);
}

/* Close all open handles (called from atexit(3)). */
static void
close_handles (void)
{
  while (handles) guestfs_close (handles);
}

static void
default_error_cb (guestfs_h *g, void *data, const char *msg)
{
  fprintf (stderr, "libguestfs: error: %s\n", msg);
}

static void
error (guestfs_h *g, const char *fs, ...)
{
  va_list args;
  char *msg;

  if (!g->error_cb) return;

  va_start (args, fs);
  vasprintf (&msg, fs, args);
  va_end (args);

  g->error_cb (g, g->error_cb_data, msg);

  free (msg);
}

static void
perrorf (guestfs_h *g, const char *fs, ...)
{
  va_list args;
  char *msg;
  int err = errno;

  if (!g->error_cb) return;

  va_start (args, fs);
  vasprintf (&msg, fs, args);
  va_end (args);

#ifndef _GNU_SOURCE
  char buf[256];
  strerror_r (err, buf, sizeof buf);
#else
  char _buf[256];
  char *buf;
  buf = strerror_r (err, _buf, sizeof _buf);
#endif

  msg = safe_realloc (g, msg, strlen (msg) + 2 + strlen (buf) + 1);
  strcat (msg, ": ");
  strcat (msg, buf);

  g->error_cb (g, g->error_cb_data, msg);

  free (msg);
}

static void *
safe_malloc (guestfs_h *g, size_t nbytes)
{
  void *ptr = malloc (nbytes);
  if (!ptr) g->abort_cb ();
  return ptr;
}

static void *
safe_realloc (guestfs_h *g, void *ptr, int nbytes)
{
  void *p = realloc (ptr, nbytes);
  if (!p) g->abort_cb ();
  return p;
}

static char *
safe_strdup (guestfs_h *g, const char *str)
{
  char *s = strdup (str);
  if (!s) g->abort_cb ();
  return s;
}

static void *
safe_memdup (guestfs_h *g, void *ptr, size_t size)
{
  void *p = malloc (size);
  if (!p) g->abort_cb ();
  memcpy (p, ptr, size);
  return p;
}

void
guestfs_set_out_of_memory_handler (guestfs_h *g, guestfs_abort_cb cb)
{
  g->abort_cb = cb;
}

guestfs_abort_cb
guestfs_get_out_of_memory_handler (guestfs_h *g)
{
  return g->abort_cb;
}

void
guestfs_set_error_handler (guestfs_h *g, guestfs_error_handler_cb cb, void *data)
{
  g->error_cb = cb;
  g->error_cb_data = data;
}

guestfs_error_handler_cb
guestfs_get_error_handler (guestfs_h *g, void **data_rtn)
{
  if (data_rtn) *data_rtn = g->error_cb_data;
  return g->error_cb;
}

int
guestfs_set_verbose (guestfs_h *g, int v)
{
  g->verbose = !!v;
  return 0;
}

int
guestfs_get_verbose (guestfs_h *g)
{
  return g->verbose;
}

int
guestfs_set_autosync (guestfs_h *g, int a)
{
  g->autosync = !!a;
  return 0;
}

int
guestfs_get_autosync (guestfs_h *g)
{
  return g->autosync;
}

int
guestfs_set_path (guestfs_h *g, const char *path)
{
  if (path == NULL)
    g->path = GUESTFS_DEFAULT_PATH;
  else
    g->path = path;
  return 0;
}

const char *
guestfs_get_path (guestfs_h *g)
{
  return g->path;
}

/* Add a string to the current command line. */
static void
incr_cmdline_size (guestfs_h *g)
{
  if (g->cmdline == NULL) {
    /* g->cmdline[0] is reserved for argv[0], set in guestfs_launch. */
    g->cmdline_size = 1;
    g->cmdline = safe_malloc (g, sizeof (char *));
    g->cmdline[0] = NULL;
  }

  g->cmdline_size++;
  g->cmdline = safe_realloc (g, g->cmdline, sizeof (char *) * g->cmdline_size);
}

static int
add_cmdline (guestfs_h *g, const char *str)
{
  if (g->state != CONFIG) {
    error (g, "command line cannot be altered after qemu subprocess launched");
    return -1;
  }

  incr_cmdline_size (g);
  g->cmdline[g->cmdline_size-1] = safe_strdup (g, str);
  return 0;
}

int
guestfs_config (guestfs_h *g,
		const char *qemu_param, const char *qemu_value)
{
  if (qemu_param[0] != '-') {
    error (g, "guestfs_config: parameter must begin with '-' character");
    return -1;
  }

  /* A bit fascist, but the user will probably break the extra
   * parameters that we add if they try to set any of these.
   */
  if (strcmp (qemu_param, "-kernel") == 0 ||
      strcmp (qemu_param, "-initrd") == 0 ||
      strcmp (qemu_param, "-nographic") == 0 ||
      strcmp (qemu_param, "-serial") == 0 ||
      strcmp (qemu_param, "-vnc") == 0 ||
      strcmp (qemu_param, "-full-screen") == 0 ||
      strcmp (qemu_param, "-std-vga") == 0 ||
      strcmp (qemu_param, "-vnc") == 0) {
    error (g, "guestfs_config: parameter '%s' isn't allowed", qemu_param);
    return -1;
  }

  if (add_cmdline (g, qemu_param) != 0) return -1;

  if (qemu_value != NULL) {
    if (add_cmdline (g, qemu_value) != 0) return -1;
  }

  return 0;
}

int
guestfs_add_drive (guestfs_h *g, const char *filename)
{
  int len = strlen (filename) + 64;
  char buf[len];

  if (strchr (filename, ',') != NULL) {
    error (g, "filename cannot contain ',' (comma) character");
    return -1;
  }

  if (access (filename, F_OK) == -1) {
    perrorf (g, "%s", filename);
    return -1;
  }

  snprintf (buf, len, "file=%s", filename);

  return guestfs_config (g, "-drive", buf);
}

int
guestfs_add_cdrom (guestfs_h *g, const char *filename)
{
  if (strchr (filename, ',') != NULL) {
    error (g, "filename cannot contain ',' (comma) character");
    return -1;
  }

  if (access (filename, F_OK) == -1) {
    perrorf (g, "%s", filename);
    return -1;
  }

  return guestfs_config (g, "-cdrom", filename);
}

int
guestfs_launch (guestfs_h *g)
{
  static const char *dir_template = "/tmp/libguestfsXXXXXX";
  int r, i, len, pmore;
  int wfd[2], rfd[2];
  int tries;
  const char *kernel_name = "vmlinuz." REPO "." host_cpu;
  const char *initrd_name = "initramfs." REPO "." host_cpu ".img";
  char *path, *pelem, *pend;
  char *kernel = NULL, *initrd = NULL;
  char unixsock[256];
  struct sockaddr_un addr;

  /* Configured? */
  if (!g->cmdline) {
    error (g, "you must call guestfs_add_drive before guestfs_launch");
    return -1;
  }

  if (g->state != CONFIG) {
    error (g, "qemu has already been launched");
    return -1;
  }

  /* Search g->path for the kernel and initrd. */
  pelem = path = safe_strdup (g, g->path);
  do {
    pend = strchrnul (pelem, ':');
    pmore = *pend == ':';
    *pend = '\0';
    len = pend - pelem;

    /* Empty element or "." means cwd. */
    if (len == 0 || (len == 1 && *pelem == '.')) {
      if (g->verbose)
	fprintf (stderr,
		 "looking for kernel and initrd in current directory\n");
      if (access (kernel_name, F_OK) == 0 && access (initrd_name, F_OK) == 0) {
	kernel = safe_strdup (g, kernel_name);
	initrd = safe_strdup (g, initrd_name);
	break;
      }
    }
    /* Look at <path>/kernel etc. */
    else {
      kernel = safe_malloc (g, len + strlen (kernel_name) + 2);
      initrd = safe_malloc (g, len + strlen (initrd_name) + 2);
      sprintf (kernel, "%s/%s", pelem, kernel_name);
      sprintf (initrd, "%s/%s", pelem, initrd_name);

      if (g->verbose)
	fprintf (stderr, "looking for %s and %s\n", kernel, initrd);

      if (access (kernel, F_OK) == 0 && access (initrd, F_OK) == 0)
	break;
      free (kernel);
      free (initrd);
      kernel = initrd = NULL;
    }

    pelem = pend + 1;
  } while (pmore);

  free (path);

  if (kernel == NULL || initrd == NULL) {
    error (g, "cannot find %s or %s on LIBGUESTFS_PATH (current path = %s)",
	   kernel_name, initrd_name, g->path);
    goto cleanup0;
  }

  /* Make the temporary directory containing the socket. */
  if (!g->tmpdir) {
    g->tmpdir = safe_strdup (g, dir_template);
    if (mkdtemp (g->tmpdir) == NULL) {
      perrorf (g, "%s: cannot create temporary directory", dir_template);
      goto cleanup0;
    }
  }

  snprintf (unixsock, sizeof unixsock, "%s/sock", g->tmpdir);
  unlink (unixsock);

  if (pipe (wfd) == -1 || pipe (rfd) == -1) {
    perrorf (g, "pipe");
    goto cleanup0;
  }

  r = fork ();
  if (r == -1) {
    perrorf (g, "fork");
    close (wfd[0]);
    close (wfd[1]);
    close (rfd[0]);
    close (rfd[1]);
    goto cleanup0;
  }

  if (r == 0) {			/* Child (qemu). */
    char vmchannel[256];
    char append[256];

    /* Set up the full command line.  Do this in the subprocess so we
     * don't need to worry about cleaning up.
     */
    g->cmdline[0] = (char *) QEMU;

    /* Construct the -net channel parameter for qemu. */
    snprintf (vmchannel, sizeof vmchannel,
	      "channel,%d:unix:%s,server,nowait",
	      VMCHANNEL_PORT, unixsock);

    /* Linux kernel command line. */
    snprintf (append, sizeof append,
	      "console=ttyS0 guestfs=%s:%d", VMCHANNEL_ADDR, VMCHANNEL_PORT);

    add_cmdline (g, "-m");
    add_cmdline (g, "384");	  /* XXX Choose best size. */
    add_cmdline (g, "-no-kqemu"); /* Avoids a warning. */
    add_cmdline (g, "-kernel");
    add_cmdline (g, (char *) kernel);
    add_cmdline (g, "-initrd");
    add_cmdline (g, (char *) initrd);
    add_cmdline (g, "-append");
    add_cmdline (g, append);
    add_cmdline (g, "-nographic");
    add_cmdline (g, "-serial");
    add_cmdline (g, "stdio");
    add_cmdline (g, "-net");
    add_cmdline (g, vmchannel);
    add_cmdline (g, "-net");
    add_cmdline (g, "user,vlan=0");
    add_cmdline (g, "-net");
    add_cmdline (g, "nic,vlan=0");
    incr_cmdline_size (g);
    g->cmdline[g->cmdline_size-1] = NULL;

    if (g->verbose) {
      fprintf (stderr, "%s", QEMU);
      for (i = 0; g->cmdline[i]; ++i)
	fprintf (stderr, " %s", g->cmdline[i]);
      fprintf (stderr, "\n");
    }

    /* Set up stdin, stdout. */
    close (0);
    close (1);
    close (wfd[1]);
    close (rfd[0]);
    dup (wfd[0]);
    dup (rfd[1]);
    close (wfd[0]);
    close (rfd[1]);

#if 0
    /* Set up a new process group, so we can signal this process
     * and all subprocesses (eg. if qemu is really a shell script).
     */
    setpgid (0, 0);
#endif

    execv (QEMU, g->cmdline);	/* Run qemu. */
    perror (QEMU);
    _exit (1);
  }

  /* Parent (library). */
  g->pid = r;

  /* Start the clock ... */
  time (&g->start_t);

  /* Close the other ends of the pipe. */
  close (wfd[0]);
  close (rfd[1]);

  if (fcntl (wfd[1], F_SETFL, O_NONBLOCK) == -1 ||
      fcntl (rfd[0], F_SETFL, O_NONBLOCK) == -1) {
    perrorf (g, "fcntl");
    goto cleanup1;
  }

  g->fd[0] = wfd[1];		/* stdin of child */
  g->fd[1] = rfd[0];		/* stdout of child */

  /* Open the Unix socket.  The vmchannel implementation that got
   * merged with qemu sucks in a number of ways.  Both ends do
   * connect(2), which means that no one knows what, if anything, is
   * connected to the other end, or if it becomes disconnected.  Even
   * worse, we have to wait some indeterminate time for qemu to create
   * the socket and connect to it (which happens very early in qemu's
   * start-up), so any code that uses vmchannel is inherently racy.
   * Hence this silly loop.
   */
  g->sock = socket (AF_UNIX, SOCK_STREAM, 0);
  if (g->sock == -1) {
    perrorf (g, "socket");
    goto cleanup1;
  }

  if (fcntl (g->sock, F_SETFL, O_NONBLOCK) == -1) {
    perrorf (g, "fcntl");
    goto cleanup2;
  }

  addr.sun_family = AF_UNIX;
  strncpy (addr.sun_path, unixsock, UNIX_PATH_MAX);
  addr.sun_path[UNIX_PATH_MAX-1] = '\0';

  tries = 100;
  while (tries > 0) {
    /* Always sleep at least once to give qemu a small chance to start up. */
    usleep (10000);

    r = connect (g->sock, (struct sockaddr *) &addr, sizeof addr);
    if ((r == -1 && errno == EINPROGRESS) || r == 0)
      goto connected;

    if (errno != ENOENT)
      perrorf (g, "connect");
    tries--;
  }

  error (g, "failed to connect to vmchannel socket");
  goto cleanup2;

 connected:
  /* Watch the file descriptors. */
  free (g->msg_in);
  g->msg_in = NULL;
  g->msg_in_size = g->msg_in_allocated = 0;

  free (g->msg_out);
  g->msg_out = NULL;
  g->msg_out_size = 0;
  g->msg_out_pos = 0;

  g->stdout_watch =
    main_loop.add_handle (g, g->fd[1],
			  GUESTFS_HANDLE_READABLE,
			  stdout_event, g);
  if (g->stdout_watch == -1) {
    error (g, "could not watch qemu stdout");
    goto cleanup3;
  }

  g->sock_watch =
    main_loop.add_handle (g, g->sock,
			  GUESTFS_HANDLE_READABLE,
			  sock_read_event, g);
  if (g->sock_watch == -1) {
    error (g, "could not watch daemon communications socket");
    goto cleanup3;
  }

  g->state = LAUNCHING;
  return 0;

 cleanup3:
  if (g->stdout_watch >= 0)
    main_loop.remove_handle (g, g->stdout_watch);
  if (g->sock_watch >= 0)
    main_loop.remove_handle (g, g->sock_watch);

 cleanup2:
  close (g->sock);

 cleanup1:
  close (wfd[1]);
  close (rfd[0]);
  kill (g->pid, 9);
  waitpid (g->pid, NULL, 0);
  g->fd[0] = -1;
  g->fd[1] = -1;
  g->sock = -1;
  g->pid = 0;
  g->start_t = 0;
  g->stdout_watch = -1;
  g->sock_watch = -1;

 cleanup0:
  free (kernel);
  free (initrd);
  return -1;
}

static void
finish_wait_ready (guestfs_h *g, void *vp)
{
  *((int *)vp) = 1;
  main_loop.main_loop_quit (g);
}

int
guestfs_wait_ready (guestfs_h *g)
{
  int r = 0;

  if (g->state == READY) return 0;

  if (g->state == BUSY) {
    error (g, "qemu has finished launching already");
    return -1;
  }

  if (g->state != LAUNCHING) {
    error (g, "qemu has not been launched yet");
    return -1;
  }

  g->launch_done_cb_internal = finish_wait_ready;
  g->launch_done_cb_internal_data = &r;
  main_loop.main_loop_run (g);
  g->launch_done_cb_internal = NULL;
  g->launch_done_cb_internal_data = NULL;

  if (r != 1) {
    error (g, "guestfs_wait_ready failed, see earlier error messages");
    return -1;
  }

  /* This is possible in some really strange situations, such as
   * guestfsd starts up OK but then qemu immediately exits.  Check for
   * it because the caller is probably expecting to be able to send
   * commands after this function returns.
   */
  if (g->state != READY) {
    error (g, "qemu launched and contacted daemon, but state != READY");
    return -1;
  }

  return 0;
}

int
guestfs_kill_subprocess (guestfs_h *g)
{
  if (g->state == CONFIG) {
    error (g, "no subprocess to kill");
    return -1;
  }

  if (g->verbose)
    fprintf (stderr, "sending SIGTERM to process %d\n", g->pid);

  kill (g->pid, SIGTERM);

  return 0;
}

/* This function is called whenever qemu prints something on stdout.
 * Qemu's stdout is also connected to the guest's serial console, so
 * we see kernel messages here too.
 */
static void
stdout_event (void *data, int watch, int fd, int events)
{
  guestfs_h *g = (guestfs_h *) data;
  char buf[4096];
  int n;

#if 0
  if (g->verbose)
    fprintf (stderr,
	     "stdout_event: %p g->state = %d, fd = %d, events = 0x%x\n",
	     g, g->state, fd, events);
#endif

  if (g->fd[1] != fd) {
    error (g, "stdout_event: internal error: %d != %d", g->fd[1], fd);
    return;
  }

  n = read (fd, buf, sizeof buf);
  if (n == 0) {
    /* Hopefully this indicates the qemu child process has died. */
    if (g->verbose)
      fprintf (stderr, "stdout_event: %p: child process died\n", g);
    /*kill (g->pid, SIGTERM);*/
    waitpid (g->pid, NULL, 0);
    if (g->stdout_watch >= 0)
      main_loop.remove_handle (g, g->stdout_watch);
    if (g->sock_watch >= 0)
      main_loop.remove_handle (g, g->sock_watch);
    close (g->fd[0]);
    close (g->fd[1]);
    close (g->sock);
    g->fd[0] = -1;
    g->fd[1] = -1;
    g->sock = -1;
    g->pid = 0;
    g->start_t = 0;
    g->stdout_watch = -1;
    g->sock_watch = -1;
    g->state = CONFIG;
    if (g->subprocess_quit_cb)
      g->subprocess_quit_cb (g, g->subprocess_quit_cb_data);
    return;
  }

  if (n == -1) {
    if (errno != EAGAIN)
      perrorf (g, "read");
    return;
  }

  /* In verbose mode, copy all log messages to stderr. */
  if (g->verbose)
    write (2, buf, n);

  /* It's an actual log message, send it upwards if anyone is listening. */
  if (g->log_message_cb)
    g->log_message_cb (g, g->log_message_cb_data, buf, n);
}

/* The function is called whenever we can read something on the
 * guestfsd (daemon inside the guest) communication socket.
 */
static void
sock_read_event (void *data, int watch, int fd, int events)
{
  guestfs_h *g = (guestfs_h *) data;
  XDR xdr;
  unsigned len;
  int n;

  if (g->verbose)
    fprintf (stderr,
	     "sock_read_event: %p g->state = %d, fd = %d, events = 0x%x\n",
	     g, g->state, fd, events);

  if (g->sock != fd) {
    error (g, "sock_read_event: internal error: %d != %d", g->sock, fd);
    return;
  }

  if (g->msg_in_size <= g->msg_in_allocated) {
    g->msg_in_allocated += 4096;
    g->msg_in = safe_realloc (g, g->msg_in, g->msg_in_allocated);
  }
  n = read (g->sock, g->msg_in + g->msg_in_size,
	    g->msg_in_allocated - g->msg_in_size);
  if (n == 0)
    /* Disconnected?  Ignore it because stdout_watch will get called
     * and will do the cleanup.
     */
    return;

  if (n == -1) {
    if (errno != EAGAIN)
      perrorf (g, "read");
    return;
  }

  g->msg_in_size += n;

  /* Have we got enough of a message to be able to process it yet? */
  if (g->msg_in_size < 4) return;

  xdrmem_create (&xdr, g->msg_in, g->msg_in_size, XDR_DECODE);
  if (!xdr_uint32_t (&xdr, &len)) {
    error (g, "can't decode length word");
    goto cleanup;
  }

  /* Length is normally the length of the message, but when guestfsd
   * starts up it sends a "magic" value (longer than any possible
   * message).  Check for this.
   */
  if (len == 0xf5f55ff5) {
    if (g->state != LAUNCHING)
      error (g, "received magic signature from guestfsd, but in state %d",
	     g->state);
    else if (g->msg_in_size != 4)
      error (g, "received magic signature from guestfsd, but msg size is %d",
	     g->msg_in_size);
    else {
      g->state = READY;
      if (g->launch_done_cb_internal)
	g->launch_done_cb_internal (g, g->launch_done_cb_internal_data);
      if (g->launch_done_cb)
	g->launch_done_cb (g, g->launch_done_cb_data);
    }

    goto cleanup;
  }

  /* If this happens, it's pretty bad and we've probably lost synchronization.*/
  if (len > GUESTFS_MESSAGE_MAX) {
    error (g, "message length (%u) > maximum possible size (%d)",
	   len, GUESTFS_MESSAGE_MAX);
    goto cleanup;
  }

  if (g->msg_in_size-4 < len) return; /* Need more of this message. */

  /* This should not happen, and if it does it probably means we've
   * lost all hope of synchronization.
   */
  if (g->msg_in_size-4 > len) {
    error (g, "len = %d, but msg_in_size-4 = %d", len, g->msg_in_size-4);
    goto cleanup;
  }

  /* Got the full message, begin processing it. */
  if (g->verbose) {
    int i, j;

    for (i = 0; i < g->msg_in_size; i += 16) {
      printf ("%04x: ", i);
      for (j = i; j < MIN (i+16, g->msg_in_size); ++j)
	printf ("%02x ", (unsigned char) g->msg_in[j]);
      for (; j < i+16; ++j)
	printf ("   ");
      printf ("|");
      for (j = i; j < MIN (i+16, g->msg_in_size); ++j)
	if (isprint (g->msg_in[j]))
	  printf ("%c", g->msg_in[j]);
	else
	  printf (".");
      for (; j < i+16; ++j)
	printf (" ");
      printf ("|\n");
    }
  }

  /* Not in the expected state. */
  if (g->state != BUSY)
    error (g, "state %d != BUSY", g->state);

  /* Push the message up to the higher layer.  Note that unlike
   * launch_done_cb / launch_done_cb_internal, we only call at
   * most one of the callback functions here.
   */
  g->state = READY;
  if (g->reply_cb_internal)
    g->reply_cb_internal (g, g->reply_cb_internal_data, &xdr);
  else if (g->reply_cb)
    g->reply_cb (g, g->reply_cb, &xdr);

 cleanup:
  /* Free the message buffer if it's grown excessively large. */
  if (g->msg_in_allocated > 65536) {
    free (g->msg_in);
    g->msg_in = NULL;
    g->msg_in_size = g->msg_in_allocated = 0;
  } else
    g->msg_in_size = 0;

  xdr_destroy (&xdr);
}

/* The function is called whenever we can write something on the
 * guestfsd (daemon inside the guest) communication socket.
 */
static void
sock_write_event (void *data, int watch, int fd, int events)
{
  guestfs_h *g = (guestfs_h *) data;
  int n;

  if (g->verbose)
    fprintf (stderr,
	     "sock_write_event: %p g->state = %d, fd = %d, events = 0x%x\n",
	     g, g->state, fd, events);

  if (g->sock != fd) {
    error (g, "sock_write_event: internal error: %d != %d", g->sock, fd);
    return;
  }

  if (g->state != BUSY) {
    error (g, "sock_write_event: state %d != BUSY", g->state);
    return;
  }

  if (g->verbose)
    fprintf (stderr, "sock_write_event: writing %d bytes ...\n",
	     g->msg_out_size - g->msg_out_pos);

  n = write (g->sock, g->msg_out + g->msg_out_pos,
	     g->msg_out_size - g->msg_out_pos);
  if (n == -1) {
    if (errno != EAGAIN)
      perrorf (g, "write");
    return;
  }

  if (g->verbose)
    fprintf (stderr, "sock_write_event: wrote %d bytes\n", n);

  g->msg_out_pos += n;

  /* More to write? */
  if (g->msg_out_pos < g->msg_out_size)
    return;

  if (g->verbose)
    fprintf (stderr, "sock_write_event: done writing, switching back to reading events\n");

  free (g->msg_out);
  g->msg_out_pos = g->msg_out_size = 0;

  if (main_loop.remove_handle (g, g->sock_watch) == -1) {
    error (g, "remove_handle failed in sock_write_event");
    return;
  }
  g->sock_watch =
    main_loop.add_handle (g, g->sock,
			  GUESTFS_HANDLE_READABLE,
			  sock_read_event, g);
  if (g->sock_watch == -1) {
    error (g, "add_handle failed in sock_write_event");
    return;
  }
}

/* Dispatch a call to the remote daemon.  This function just queues
 * the call in msg_out, to be sent when we next enter the main loop.
 * Returns -1 for error, or the message serial number.
 */
static int
dispatch (guestfs_h *g, int proc_nr, xdrproc_t xdrp, char *args)
{
  char buffer[GUESTFS_MESSAGE_MAX];
  struct guestfs_message_header hdr;
  XDR xdr;
  unsigned len;
  int serial = g->msg_next_serial++;

  if (g->state != READY) {
    error (g, "dispatch: state %d != READY", g->state);
    return -1;
  }

  /* Serialize the header. */
  hdr.prog = GUESTFS_PROGRAM;
  hdr.vers = GUESTFS_PROTOCOL_VERSION;
  hdr.proc = proc_nr;
  hdr.direction = GUESTFS_DIRECTION_CALL;
  hdr.serial = serial;
  hdr.status = GUESTFS_STATUS_OK;

  xdrmem_create (&xdr, buffer, sizeof buffer, XDR_ENCODE);
  if (!xdr_guestfs_message_header (&xdr, &hdr)) {
    error (g, "xdr_guestfs_message_header failed");
    return -1;
  }

  /* Serialize the args.  If any, because some message types
   * have no parameters.
   */
  if (xdrp) {
    if (!(*xdrp) (&xdr, args)) {
      error (g, "dispatch failed to marshal args");
      return -1;
    }
  }

  len = xdr_getpos (&xdr);
  xdr_destroy (&xdr);

  /* Allocate the outgoing message buffer. */
  g->msg_out = safe_malloc (g, len + 4);

  g->msg_out_size = len + 4;
  g->msg_out_pos = 0;
  g->state = BUSY;

  xdrmem_create (&xdr, g->msg_out, 4, XDR_ENCODE);
  if (!xdr_uint32_t (&xdr, &len)) {
    error (g, "xdr_uint32_t failed in dispatch");
    goto cleanup1;
  }

  memcpy (g->msg_out + 4, buffer, len);

  /* Change the handle to sock_write_event. */
  if (main_loop.remove_handle (g, g->sock_watch) == -1) {
    error (g, "remove_handle failed in dispatch");
    goto cleanup1;
  }
  g->sock_watch =
    main_loop.add_handle (g, g->sock,
			  GUESTFS_HANDLE_WRITABLE,
			  sock_write_event, g);
  if (g->sock_watch == -1) {
    error (g, "add_handle failed in dispatch");
    goto cleanup1;
  }

  return serial;

 cleanup1:
  free (g->msg_out);
  g->msg_out = NULL;
  g->msg_out_size = 0;
  g->state = READY;
  return -1;
}

/* Check the return message from a call for validity. */
static int
check_reply_header (guestfs_h *g,
		    const struct guestfs_message_header *hdr,
		    int proc_nr, int serial)
{
  if (hdr->prog != GUESTFS_PROGRAM) {
    error (g, "wrong program (%d/%d)", hdr->prog, GUESTFS_PROGRAM);
    return -1;
  }
  if (hdr->vers != GUESTFS_PROTOCOL_VERSION) {
    error (g, "wrong protocol version (%d/%d)",
	   hdr->vers, GUESTFS_PROTOCOL_VERSION);
    return -1;
  }
  if (hdr->direction != GUESTFS_DIRECTION_REPLY) {
    error (g, "unexpected message direction (%d/%d)",
	   hdr->direction, GUESTFS_DIRECTION_REPLY);
    return -1;
  }
  if (hdr->proc != proc_nr) {
    error (g, "unexpected procedure number (%d/%d)", hdr->proc, proc_nr);
    return -1;
  }
  if (hdr->serial != serial) {
    error (g, "unexpected serial (%d/%d)", hdr->serial, serial);
    return -1;
  }

  return 0;
}

/* The high-level actions are autogenerated by generator.ml.  Include
 * them here.
 */
#include "guestfs-actions.c"

/* Structure-freeing functions.  These rely on the fact that the
 * structure format is identical to the XDR format.  See note in
 * generator.ml.
 */
void
guestfs_free_lvm_pv_list (struct guestfs_lvm_pv_list *x)
{
  xdr_free ((xdrproc_t) xdr_guestfs_lvm_int_pv_list, (char *) x);
  free (x);
}

void
guestfs_free_lvm_vg_list (struct guestfs_lvm_vg_list *x)
{
  xdr_free ((xdrproc_t) xdr_guestfs_lvm_int_vg_list, (char *) x);
  free (x);
}

void
guestfs_free_lvm_lv_list (struct guestfs_lvm_lv_list *x)
{
  xdr_free ((xdrproc_t) xdr_guestfs_lvm_int_lv_list, (char *) x);
  free (x);
}

/* This is the default main loop implementation, using select(2). */

struct handle_cb_data {
  guestfs_handle_event_cb cb;
  void *data;
};

static fd_set rset;
static fd_set wset;
static fd_set xset;
static int select_init_done = 0;
static int max_fd = -1;
static int nr_fds = 0;
static struct handle_cb_data *handle_cb_data = NULL;

static void
select_init (void)
{
  if (!select_init_done) {
    FD_ZERO (&rset);
    FD_ZERO (&wset);
    FD_ZERO (&xset);

    select_init_done = 1;
  }
}

static int
select_add_handle (guestfs_h *g, int fd, int events,
		   guestfs_handle_event_cb cb, void *data)
{
  select_init ();

  if (fd < 0 || fd >= FD_SETSIZE) {
    error (g, "fd %d is out of range", fd);
    return -1;
  }

  if ((events & ~(GUESTFS_HANDLE_READABLE |
		  GUESTFS_HANDLE_WRITABLE |
		  GUESTFS_HANDLE_HANGUP |
		  GUESTFS_HANDLE_ERROR)) != 0) {
    error (g, "set of events (0x%x) contains unknown events", events);
    return -1;
  }

  if (events == 0) {
    error (g, "set of events is empty");
    return -1;
  }

  if (FD_ISSET (fd, &rset) || FD_ISSET (fd, &wset) || FD_ISSET (fd, &xset)) {
    error (g, "fd %d is already registered", fd);
    return -1;
  }

  if (cb == NULL) {
    error (g, "callback is NULL");
    return -1;
  }

  if ((events & GUESTFS_HANDLE_READABLE))
    FD_SET (fd, &rset);
  if ((events & GUESTFS_HANDLE_WRITABLE))
    FD_SET (fd, &wset);
  if ((events & GUESTFS_HANDLE_HANGUP) || (events & GUESTFS_HANDLE_ERROR))
    FD_SET (fd, &xset);

  if (fd > max_fd) {
    max_fd = fd;
    handle_cb_data = safe_realloc (g, handle_cb_data,
				   sizeof (struct handle_cb_data) * (max_fd+1));
  }
  handle_cb_data[fd].cb = cb;
  handle_cb_data[fd].data = data;

  nr_fds++;

  /* Any integer >= 0 can be the handle, and this is as good as any ... */
  return fd;
}

static int
select_remove_handle (guestfs_h *g, int fd)
{
  select_init ();

  if (fd < 0 || fd >= FD_SETSIZE) {
    error (g, "fd %d is out of range", fd);
    return -1;
  }

  if (!FD_ISSET (fd, &rset) && !FD_ISSET (fd, &wset) && !FD_ISSET (fd, &xset)) {
    error (g, "fd %d was not registered", fd);
    return -1;
  }

  FD_CLR (fd, &rset);
  FD_CLR (fd, &wset);
  FD_CLR (fd, &xset);

  if (fd == max_fd) {
    max_fd--;
    handle_cb_data = safe_realloc (g, handle_cb_data,
				   sizeof (struct handle_cb_data) * (max_fd+1));
  }

  nr_fds--;

  return 0;
}

static int
select_add_timeout (guestfs_h *g, int interval,
		    guestfs_handle_timeout_cb cb, void *data)
{
  select_init ();

  abort ();			/* XXX not implemented yet */
}

static int
select_remove_timeout (guestfs_h *g, int timer)
{
  select_init ();

  abort ();			/* XXX not implemented yet */
}

/* Note that main loops can be nested. */
static int level = 0;

static void
select_main_loop_run (guestfs_h *g)
{
  int old_level, fd, r, events;
  fd_set rset2, wset2, xset2;

  select_init ();

  old_level = level++;
  while (level > old_level) {
    if (nr_fds == 0) {
      level = old_level;
      break;
    }

    rset2 = rset;
    wset2 = wset;
    xset2 = xset;
    r = select (max_fd+1, &rset2, &wset2, &xset2, NULL);
    if (r == -1) {
      perrorf (g, "select");
      level = old_level;
      break;
    }

    for (fd = 0; r > 0 && fd <= max_fd; ++fd) {
      events = 0;
      if (FD_ISSET (fd, &rset2))
	events |= GUESTFS_HANDLE_READABLE;
      if (FD_ISSET (fd, &wset2))
	events |= GUESTFS_HANDLE_WRITABLE;
      if (FD_ISSET (fd, &xset2))
	events |= GUESTFS_HANDLE_ERROR | GUESTFS_HANDLE_HANGUP;
      if (events) {
	r--;
	handle_cb_data[fd].cb (handle_cb_data[fd].data,
			       fd, fd, events);
      }
    }
  }
}

static void
select_main_loop_quit (guestfs_h *g)
{
  select_init ();

  if (level == 0) {
    error (g, "cannot quit, we are not in a main loop");
    return;
  }

  level--;
}
