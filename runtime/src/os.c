// Ovyth :: runtime/src/os.c
//
// Running a command and capturing what it produced.
//
// This is the capability an autonomous agent needs: the language could already
// read and write files and call an API, but it could not *do* anything to the
// machine. `os.run("ls -la")` now returns the output and the exit status, so an
// agent can act and observe.
//
// fork/exec rather than popen, for two reasons: popen gives one stream (stderr
// and stdout merged, or one lost) and cannot report the exit status. Both
// pipes are drained with poll() so neither can fill its buffer and deadlock the
// child while the other is being read.
//
// ov_os_exec() below is the second, lower-level path: no shell, an argv list, a
// deadline, and control over where the child's streams go. It uses posix_spawn
// rather than fork+exec because the child side of posix_spawn is libc's own
// pre-reduced path -- nothing of ours runs between the fork and the exec, which
// is what makes it safe in a process that owns a garbage collector and a heap,
// and it is also faster (vfork semantics: no page-table copy).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ovrt.h"
#include "ovrt_helpers.h"

/* Wait, without burning a core.
 *
 * Automation waits constantly: backoff between retries, polling for a file,
 * pacing API calls. Before this existed, waiting was a busy loop -- a script
 * that needed a 200ms pause held a core at 100% to get it.
 *
 * CLOCK_MONOTONIC, so the wait is immune to the wall clock being changed
 * underneath the script. EINTR resumes with the *remaining* time rather than
 restarting, so a signal does not silently extend the wait by the full
 * duration. */
void ov_os_sleep(OvValue ms) {
  double want = 0.0;
  if (ov_tagof(ms) == OV_INT) want = (double)ms.i;
  else if (ov_tagof(ms) == OV_FLOAT) want = ms.f;
  if (want <= 0.0) return;
  struct timespec req;
  req.tv_sec = (time_t)(want / 1000.0);
  req.tv_nsec = (long)((want - (double)req.tv_sec * 1000.0) * 1000000.0);
  if (req.tv_nsec < 0) req.tv_nsec = 0;
  if (req.tv_nsec > 999999999L) req.tv_nsec = 999999999L;
  while (clock_nanosleep(CLOCK_MONOTONIC, 0, &req, &req) == EINTR) {
    // interrupted: `req` now holds the remaining time; loop with it
  }
}

/* Read whatever is ready from `fd`, appending to a growing buffer.
 *
 * `*buf` and `*cap` carry the buffer across calls (one pipe is read in several
 * poll rounds), and `*len` is the byte count so far. Returns 0 on success.
 * Growth is geometric, so a long log costs log(n) reallocations rather than
 * one per read. */
static int drain_fd(int fd, char** buf, size_t* cap, size_t* len) {
  for (;;) {
    if (*len + 4096 + 1 > *cap) {
      size_t nc = *cap ? *cap * 2 : 4096;
      char* b = (char*)realloc(*buf, nc);
      if (!b) return -1;
      *buf = b;
      *cap = nc;
    }
    ssize_t got = read(fd, *buf + *len, 4096);
    if (got > 0) { *len += (size_t)got; continue; }
    if (got == 0) return 0;           // closed: this pipe is done
    if (errno == EINTR) continue;     // interrupted by a signal: retry
    return 0;                          // any other error: stop reading it
  }
}

/* Is standard output a terminal? A program that decorates only when a human is
 * watching needs to ask, and the language had no way to ask. */
int ov_os_tty(void) { return isatty(STDOUT_FILENO) != 0; }

// Run `cmd` through /bin/sh and report what happened.
//
// out receives {cmd, code, ok, stdout, stderr}: `code` is the child's exit
// status (127 when the shell could not be started), with stdout and stderr
// kept apart so a caller can tell a command's output from its complaints.
// Returns 0 when the command ran, 1 when it could not be started at all.
int ov_os_run(const char* cmd, int* threw, OvValue* out) {
  *threw = 0;
  if (!cmd || !cmd[0]) return ov_h_fail("os.run needs a command", 22);

  int outp[2], errp[2];
  if (pipe(outp) != 0) return ov_h_fail("cannot open a pipe", 18);
  if (pipe(errp) != 0) {
    close(outp[0]); close(outp[1]);
    return ov_h_fail("cannot open a pipe", 18);
  }

  // A child may exit before we finish writing, which would raise SIGPIPE and
  // kill the agent rather than the child.
  void (*old_pipe)(int) = signal(SIGPIPE, SIG_IGN);

  pid_t pid = fork();
  if (pid < 0) {
    close(outp[0]); close(outp[1]); close(errp[0]); close(errp[1]);
    signal(SIGPIPE, old_pipe);
    return ov_h_fail("cannot fork", 11);
  }
  if (pid == 0) {
    dup2(outp[1], STDOUT_FILENO);
    dup2(errp[1], STDERR_FILENO);
    close(outp[0]); close(outp[1]); close(errp[0]); close(errp[1]);
    execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
    _exit(127);  // execl only returns when it fails
  }

  close(outp[1]);
  close(errp[1]);

  // Both pipes at once: draining only stdout while the child fills stderr's
  // buffer would deadlock.
  struct pollfd fds[2];
  fds[0].fd = outp[0]; fds[0].events = POLLIN; fds[0].revents = 0;
  fds[1].fd = errp[0]; fds[1].events = POLLIN; fds[1].revents = 0;
  char* out_buf = NULL; size_t out_cap = 0, out_len = 0;
  char* err_buf = NULL; size_t err_cap = 0, err_len = 0;
  int live = 2;
  while (live > 0) {
    int r = poll(fds, 2, -1);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < 2; i++) {
      if (!fds[i].revents) continue;
      if (fds[i].revents & POLLIN) {
        char** buf = i == 0 ? &out_buf : &err_buf;
        size_t* cap = i == 0 ? &out_cap : &err_cap;
        size_t* len = i == 0 ? &out_len : &err_len;
        drain_fd(fds[i].fd, buf, cap, len);
      }
      if (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
        close(fds[i].fd);
        fds[i].fd = -1;
        live--;
      }
    }
  }
  close(outp[0]);
  close(errp[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  signal(SIGPIPE, old_pipe);

  OvStr* out_s = ov_str_new(out_buf ? out_buf : "", out_len);
  OvStr* err_s = ov_str_new(err_buf ? err_buf : "", err_len);
  free(out_buf);
  free(err_buf);

  OvMap* m = ov_map_new();
  ov_map_set(m, ov_str(ov_str_new("cmd", 3)), ov_str(ov_str_new(cmd, strlen(cmd))));
  ov_map_set(m, ov_str(ov_str_new("code", 4)), ov_int(code));
  ov_map_set(m, ov_str(ov_str_new("ok", 2)), ov_bool(code == 0));
  ov_map_set(m, ov_str(ov_str_new("stdout", 6)), ov_str(out_s));
  ov_map_set(m, ov_str(ov_str_new("stderr", 6)), ov_str(err_s));
  *out = ov_map(m);
  return 0;
}

/* -------------------------------------------------------------- ov_os_exec
 *
 * Spawn a program without a shell, and bound every way it can go wrong.
 *
 * os.run is safe and simple, but it is a string handed to /bin/sh: arguments
 * are re-parsed by another language, a hung child hangs the caller forever,
 * and the child inherits everything the parent has open. Automation needs the
 * opposite of all three, so this is the primitive underneath:
 *
 *   os.exec(["grep", needle, path],
 *           timeout=5000, cwd="/srv", env={"PATH": "/usr/bin"},
 *           stdin="", stdout="capture", max_output=1048576)
 *   -> {ok, status, code, pid, stdout, stderr, elapsed_ms}
 *
 * status is "ok" (exited 0), "failed" (non-zero exit), "signal" (died from a
 * signal), "timeout" (killed at the deadline) or "truncated" (output hit the
 * ceiling) -- the reason, so a caller branches on the cause instead of
 * parsing a message.
 *
 * How each hazard is designed out rather than patched up:
 *
 *   child-side safety   posix_spawn's child side is libc's own pre-reduced
 *                       path: nothing of ours runs between the fork and the
 *                       exec, so the GC's locks and the heap are never
 *                       observed mid-transition. It is also faster than
 *                       fork+exec (vfork semantics, no page-table copy).
 *   killing ourselves   POSIX_SPAWN_SETPGROUP puts the child in a group of
 *                       its own at spawn time, atomically -- no window in
 *                       which it is still in ours, so kill(-pgid) can only
 *                       reach the child's own descendants.
 *   busy-wait timeouts  the deadline is poll()'s own timeout and SIGCHLD
 *                       arrives through a signalfd: zero wakeups, zero CPU
 *                       while waiting.
 *   fd inheritance      every pipe is O_CLOEXEC and the child's descriptors
 *                       are replaced by file actions applied inside the
 *                       spawn itself.
 *   unbounded output    max_output (64MB default) stops growing the buffer
 *                       but keeps draining, so the child never blocks on a
 *                       full pipe, and reports "truncated" rather than
 *                       cutting silently.
 */
typedef struct {
  int fd;              /* pipe read end polled; -1 once closed */
  char* buf;
  size_t len, cap;
  size_t max;
  int truncated;
} CapBuf;

/* Drain what is ready. At the ceiling the data is still read and discarded:
 * stopping would block the child on a full pipe and turn a completed run
 * into a timeout. */
static void capbuf_drain(CapBuf* c) {
  char scratch[4096];
  for (;;) {
    if (c->len >= c->max) {
      ssize_t got = read(c->fd, scratch, sizeof scratch);
      if (got > 0) { c->truncated = 1; continue; }
      if (got == 0) return;
      if (errno == EINTR) continue;
      return;
    }
    if (c->len + 4096 + 1 > c->cap) {
      size_t nc = c->cap ? c->cap * 2 : 8192;
      if (nc > c->max) nc = c->max;
      if (nc < 4096) nc = 4096;
      char* b = (char*)realloc(c->buf, nc);
      if (!b) { c->truncated = 1; return; }  /* out of memory: stop growing */
      c->buf = b;
      c->cap = nc;
    }
    ssize_t got = read(c->fd, c->buf + c->len, c->cap - c->len);
    if (got > 0) {
      c->len += (size_t)got;
      if (c->len >= c->max) c->truncated = 1;
      continue;
    }
    if (got == 0) return;              /* writer closed: this pipe is done */
    if (errno == EINTR) continue;
    return;                            /* any other error: stop reading it */
  }
}

static void free_argv(char** a);

/* argv from an OvList of strings. Non-strings are rejected rather than
 * stringified: a nil in the middle of an argv is a bug in the caller, and
 * silently spawning "" is worse than failing. */
static char* dup_bytes(const char* b, size_t n) {
  char* p = (char*)malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, b, n);
  p[n] = '\0';
  return p;
}

static char** argv_from_value(OvValue v, const char** why) {
  *why = NULL;
  /* A list, or a string array: an all-string literal such as ["grep", x]
   * lowers to a specialized array on the native backend, so both shapes are
   * the same argv and both are accepted. */
  int tag = ov_tagof(v);
  if (tag == OV_STRA) {
    OvStringArray* a = v.stra;
    if (!a) { *why = "os.exec needs an argv list"; return NULL; }
    char** out = (char**)calloc((size_t)a->len + 1, sizeof(char*));
    if (!out) { *why = "os.exec could not allocate argv"; return NULL; }
    for (size_t i = 0; i < (size_t)a->len; i++) {
      OvStr* item = ov_stra_get(a, i);
      if (!item) {
        *why = "os.exec argv entries must all be strings";
        free_argv(out);
        return NULL;
      }
      out[i] = dup_bytes(item->bytes, item->len);
      if (!out[i]) {
        for (size_t j = 0; j < i; j++) free(out[j]);
        free(out);
        *why = "os.exec could not allocate argv";
        return NULL;
      }
    }
    return out;
  }
  if (tag != OV_LIST || !v.list) { *why = "os.exec needs an argv list"; return NULL; }
  size_t n = v.list->len;
  char** out = (char**)calloc(n + 1, sizeof(char*));
  if (!out) { *why = "os.exec could not allocate argv"; return NULL; }
  for (size_t i = 0; i < n; i++) {
    OvValue e = ov_list_get(v.list, i);
    if (ov_tagof(e) != OV_STRING) {
      for (size_t j = 0; j < i; j++) free(out[j]);
      free(out);
      *why = "os.exec argv entries must all be strings";
      return NULL;
    }
    out[i] = dup_bytes(e.str->bytes, e.str->len);
    if (!out[i]) {
      for (size_t j = 0; j < i; j++) free(out[j]);
      free(out);
      *why = "os.exec could not allocate argv";
      return NULL;
    }
  }
  return out;
}

/* environ unless the caller supplied a map. A child gets exactly the
 * variables it was given -- not the script's whole environment, which is how
 * credentials stop leaking into programs never meant to see them. NULL means
 * "inherit". */
static char** envp_from_value(OvValue m, int* ok) {
  *ok = 1;
  if (ov_tagof(m) != OV_MAP || !m.map) return NULL;
  /* The map's storage is private; pairs is the route through it. */
  OvList* pairs = ov_map_pairs(m.map);
  if (!pairs) { *ok = 0; return NULL; }
  size_t n = (size_t)pairs->len;
  char** out = (char**)calloc(n / 2 + 1, sizeof(char*));
  if (!out) { *ok = 0; return NULL; }
  size_t k = 0;
  for (size_t i = 0; i + 1 < n; i += 2) {
    OvValue key = ov_list_get(pairs, i);
    OvValue val = ov_list_get(pairs, i + 1);
    if (ov_tagof(key) != OV_STRING || ov_tagof(val) != OV_STRING) { *ok = 0; goto bad; }
    size_t len = key.str->len + 1 + val.str->len;
    out[k] = (char*)malloc(len + 1);
    if (!out[k]) { *ok = 0; goto bad; }
    memcpy(out[k], key.str->bytes, key.str->len);
    out[k][key.str->len] = '=';
    memcpy(out[k] + key.str->len + 1, val.str->bytes, val.str->len);
    out[k][len] = '\0';
    k++;
  }
  return out;
bad:
  for (size_t j = 0; j < k; j++) free(out[j]);
  free(out);
  return NULL;
}

static void free_argv(char** a) {
  if (!a) return;
  for (size_t i = 0; a[i]; i++) free(a[i]);
  free(a);
}

static void free_envp(char** a) {
  if (!a) return;
  for (size_t i = 0; a[i]; i++) free(a[i]);
  free(a);
}

static double number_of(OvValue v, double fallback) {
  if (ov_tagof(v) == OV_INT) return (double)v.i;
  if (ov_tagof(v) == OV_FLOAT) return v.f;
  return fallback;
}

/* stdout/stderr: 0 capture (default), 1 inherit, 2 the given fd. */
static int stream_mode(OvValue v, int* fd_out) {
  *fd_out = -1;
  if (ov_tagof(v) == OV_STRING) {
    if (v.str->len == 7 && !memcmp(v.str->bytes, "inherit", 7)) return 1;
    return 0;
  }
  if (ov_tagof(v) == OV_INT) { *fd_out = (int)v.i; return 2; }
  return 0;
}

static double elapsed_since(const struct timespec* t0) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)(now.tv_sec - t0->tv_sec) * 1000.0 +
         (double)(now.tv_nsec - t0->tv_nsec) / 1000000.0;
}

int ov_os_exec(OvValue argv, OvValue timeout, OvValue cwd, OvValue env,
               OvValue stdin_v, OvValue stdout_v, OvValue stderr_v,
               OvValue max_output, int* threw, OvValue* out) {
  *threw = 0;
  *out = ov_nil();
  const char* why = "os.exec could not start the program";
  char** args = NULL;
  char** envp = NULL;
  int outp[2] = {-1, -1}, errp[2] = {-1, -1}, inp[2] = {-1, -1};
  int sfd = -1;
  int out_mode = 0, out_fd = -1, err_mode = 0, err_fd = -1;
  /* Declared with the other state so the failure labels below never read an
   * indeterminate value: the empty set is the process's normal mask, which
   * makes "restore" a no-op when nothing was blocked yet. */
  sigset_t chld, old_mask;
  sigemptyset(&old_mask);
  sigemptyset(&chld);
  sigaddset(&chld, SIGCHLD);

  args = argv_from_value(argv, &why);
  if (!args) goto fail;
  if (!args[0]) { why = "os.exec needs a program to run"; goto fail; }

  int env_ok = 1;
  envp = envp_from_value(env, &env_ok);
  if (!env_ok) { why = "os.exec env entries must be strings"; goto fail; }

  double budget = number_of(timeout, 0.0);            /* 0 = no deadline */
  size_t max_bytes = (size_t)number_of(max_output, 64.0 * 1024 * 1024);
  if (max_bytes < 4096) max_bytes = 4096;
  out_mode = stream_mode(stdout_v, &out_fd);
  err_mode = stream_mode(stderr_v, &err_fd);

  /* stdin: "inherit" -> the caller's own, "" or "null" or anything omitted
   * -> /dev/null (the safe default: a program that reads stdin must not eat
   * the script's input), a non-empty string -> fed to the child, an int -> the
   * read end of a pipe the caller owns (how two children are wired without a
   * shell). */
  int stdin_mode = 0;
  if (ov_tagof(stdin_v) == OV_INT) {
    stdin_mode = 3;
  } else if (ov_tagof(stdin_v) == OV_STRING) {
    if (stdin_v.str->len == 7 && !memcmp(stdin_v.str->bytes, "inherit", 7))
      stdin_mode = 1;
    else if (stdin_v.str->len > 0)
      stdin_mode = 2;
  }

  if (out_mode == 0 && pipe2(outp, O_CLOEXEC) != 0) { why = "os.exec could not open a pipe"; goto fail; }
  if (err_mode == 0 && pipe2(errp, O_CLOEXEC) != 0) { why = "os.exec could not open a pipe"; goto fail; }
  if (stdin_mode == 2 && pipe2(inp, O_CLOEXEC) != 0) { why = "os.exec could not open a pipe"; goto fail; }

  /* SIGCHLD is blocked (never ignored: an ignored SIGCHLD is auto-reaped and
   * waitpid would block forever) and delivered through a signalfd, so the
   * wait has no race and no handler of ours runs inside it. The child gets
   * the original mask back through POSIX_SPAWN_SETSIGMASK below. */
  sigprocmask(SIG_BLOCK, &chld, &old_mask);
  sfd = signalfd(-1, &chld, SFD_CLOEXEC | SFD_NONBLOCK);
  if (sfd < 0) { why = "os.exec could not watch for the child"; goto fail; }

  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  if (posix_spawn_file_actions_init(&fa) != 0 || posix_spawnattr_init(&attr) != 0) {
    why = "os.exec could not prepare the spawn";
    goto fail;
  }
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
  posix_spawnattr_setpgroup(&attr, 0);           /* the child leads its own group */
  posix_spawnattr_setsigmask(&attr, &old_mask);  /* and does not inherit our block */
  if (ov_tagof(cwd) == OV_STRING && cwd.str->len > 0)
    posix_spawn_file_actions_addchdir_np(&fa, cwd.str->bytes);

  /* The child's descriptors, applied inside the spawn. */
  if (stdin_mode == 0)
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  else if (stdin_mode == 3) {
    /* The caller's own fd: the parent keeps it (it owns the pipe), so only the
     * child's descriptor 0 is replaced. */
    posix_spawn_file_actions_adddup2(&fa, (int)stdin_v.i, 0);
  }
  else if (stdin_mode == 2) {
    /* The child reads the pipe's read end; the parent writes the write end. */
    posix_spawn_file_actions_adddup2(&fa, inp[0], 0);
    posix_spawn_file_actions_addclose(&fa, inp[1]);
  }
  if (out_mode == 0) {
    posix_spawn_file_actions_adddup2(&fa, outp[1], 1);
    posix_spawn_file_actions_addclose(&fa, outp[0]);
  } else if (out_mode == 2) {
    posix_spawn_file_actions_adddup2(&fa, out_fd, 1);
  }
  if (err_mode == 0) {
    posix_spawn_file_actions_adddup2(&fa, errp[1], 2);
    posix_spawn_file_actions_addclose(&fa, errp[0]);
  } else if (err_mode == 2) {
    posix_spawn_file_actions_adddup2(&fa, err_fd, 2);
  }

  /* Nothing of ours runs on the child side of this call.
   *
   * posix_spawnp when the name carries no slash, so `os.exec(["grep", ...])`
   * finds grep on PATH the way every shell does; posix_spawn (an exact path)
   * when it does, so a relative or absolute path is never shadowed by
   * something else on PATH. */
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  pid_t pid = -1;
  int search_path = strchr(args[0], '/') == NULL;
  int spawn_rc = search_path
      ? posix_spawnp(&pid, args[0], &fa, &attr, args, envp ? envp : environ)
      : posix_spawn(&pid, args[0], &fa, &attr, args, envp ? envp : environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  if (spawn_rc != 0 || pid < 0) {
    why = strerror(spawn_rc ? spawn_rc : errno);
    if (!why || !*why) why = "os.exec could not start the program";
    goto fail;
  }

  /* Our copies of the write ends go away immediately, so a child that exits
   * closes the pipes and poll() sees EOF. */
  if (outp[1] >= 0) { close(outp[1]); outp[1] = -1; }
  if (errp[1] >= 0) { close(errp[1]); errp[1] = -1; }
  if (inp[0] >= 0) { close(inp[0]); inp[0] = -1; }

  CapBuf ob = {outp[0], NULL, 0, 0, max_bytes, 0};
  CapBuf eb = {errp[0], NULL, 0, 0, max_bytes, 0};
  const char* feed = NULL;
  size_t feed_len = 0, feed_off = 0;
  if (stdin_mode == 2) { feed = stdin_v.str->bytes; feed_len = stdin_v.str->len; }

  int timed_out = 0, reaped = 0, final_st = 0;
  for (;;) {
    struct pollfd fds[4];
    int nfds = 0, i_ob = -1, i_eb = -1, i_in = -1, i_sf = -1;
    if (ob.fd >= 0) { i_ob = nfds; fds[nfds].fd = ob.fd; fds[nfds].events = POLLIN; fds[nfds].revents = 0; nfds++; }
    if (eb.fd >= 0) { i_eb = nfds; fds[nfds].fd = eb.fd; fds[nfds].events = POLLIN; fds[nfds].revents = 0; nfds++; }
    if (inp[1] >= 0 && feed_off < feed_len) { i_in = nfds; fds[nfds].fd = inp[1]; fds[nfds].events = POLLOUT; fds[nfds].revents = 0; nfds++; }
    i_sf = nfds; fds[nfds].fd = sfd; fds[nfds].events = POLLIN; fds[nfds].revents = 0; nfds++;

    /* The remaining budget is the poll's own timeout: wake on data, on the
     * child, or at the deadline -- never in between. */
    int wait_ms = -1;
    if (budget > 0.0) {
      double remaining = budget - elapsed_since(&t0);
      if (remaining <= 0.0) { timed_out = 1; break; }
      wait_ms = (int)remaining + 1;
    }
    int r = poll(fds, (nfds_t)nfds, wait_ms);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) { timed_out = 1; break; }

    if (i_ob >= 0 && (fds[i_ob].revents & (POLLIN | POLLHUP | POLLERR))) capbuf_drain(&ob);
    if (i_eb >= 0 && (fds[i_eb].revents & (POLLIN | POLLHUP | POLLERR))) capbuf_drain(&eb);
    if (i_in >= 0 && (fds[i_in].revents & POLLOUT)) {
      ssize_t w = write(inp[1], feed + feed_off, feed_len - feed_off);
      if (w > 0) {
        feed_off += (size_t)w;
        if (feed_off >= feed_len) { close(inp[1]); inp[1] = -1; }
      } else if (w < 0 && errno != EINTR && errno != EAGAIN) {
        close(inp[1]);
        inp[1] = -1;   /* child went away: nothing more to feed */
      }
    }
    if (i_sf >= 0 && (fds[i_sf].revents & POLLIN)) {
      struct signalfd_siginfo si;
      while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) { /* drain */ }
      pid_t got = waitpid(pid, &final_st, WNOHANG);
      if (got == pid) { reaped = 1; break; }
    }
    if (ob.fd >= 0 && (fds[i_ob].revents & (POLLHUP | POLLERR)) && !(fds[i_ob].revents & POLLIN)) {
      close(ob.fd);
      ob.fd = -1;
    }
    if (eb.fd >= 0 && (fds[i_eb].revents & (POLLHUP | POLLERR)) && !(fds[i_eb].revents & POLLIN)) {
      close(eb.fd);
      eb.fd = -1;
    }
  }

  int killed = 0;
  if (timed_out) {
    /* The child leads its own group, so this reaches every descendant and
     * nothing else -- in particular, never the script itself. */
    kill(-pid, SIGKILL);
    killed = 1;
    while (waitpid(pid, &final_st, 0) < 0 && errno == EINTR) {}
  } else if (!reaped) {
    while (waitpid(pid, &final_st, 0) < 0 && errno == EINTR) {}
  }

  double elapsed = elapsed_since(&t0);

  /* Once reaped, take what is already buffered without waiting for EOF: a
   * grandchild that inherited the pipe would otherwise hold it open and a
   * "finished" run would hang. */
  if (ob.fd >= 0) {
    fcntl(ob.fd, F_SETFL, O_NONBLOCK);
    capbuf_drain(&ob);
    close(ob.fd);
    ob.fd = -1;
  }
  if (eb.fd >= 0) {
    fcntl(eb.fd, F_SETFL, O_NONBLOCK);
    capbuf_drain(&eb);
    close(eb.fd);
    eb.fd = -1;
  }
  if (inp[1] >= 0) { close(inp[1]); inp[1] = -1; }
  if (sfd >= 0) { close(sfd); sfd = -1; }
  sigprocmask(SIG_SETMASK, &old_mask, NULL);

  int code = WIFEXITED(final_st) ? WEXITSTATUS(final_st) : 128 + WTERMSIG(final_st);
  const char* status;
  if (killed) status = "timeout";
  else if (WIFSIGNALED(final_st)) status = "signal";
  else if (code != 0) status = "failed";
  else if (ob.truncated || eb.truncated) status = "truncated";
  else status = "ok";
  int ok_flag = !killed && WIFEXITED(final_st) && code == 0 &&
                !ob.truncated && !eb.truncated;

  ov_gc_begin_mutation();
  OvMap* m = ov_map_new();
  ov_map_set(m, ov_str(ov_str_new("ok", 2)), ov_bool(ok_flag));
  ov_map_set(m, ov_str(ov_str_new("status", 6)), ov_str(ov_str_new(status, strlen(status))));
  ov_map_set(m, ov_str(ov_str_new("code", 4)), ov_int(code));
  ov_map_set(m, ov_str(ov_str_new("pid", 3)), ov_int((int64_t)pid));
  ov_map_set(m, ov_str(ov_str_new("elapsed_ms", 10)), ov_float(elapsed));
  ov_map_set(m, ov_str(ov_str_new("stdout", 6)), ov_str(ov_str_new(ob.buf ? ob.buf : "", ob.len)));
  ov_map_set(m, ov_str(ov_str_new("stderr", 6)), ov_str(ov_str_new(eb.buf ? eb.buf : "", eb.len)));
  ov_gc_end_mutation();

  free(ob.buf);
  free(eb.buf);
  *out = ov_map(m);
  goto done;

fail:
  /* A failure is a failure: it is reported, never answered with a silent nil.
   * (This existed and was caught by the first differential test run.) */
  *threw = 1;
  if (outp[0] >= 0) close(outp[0]);
  if (outp[1] >= 0) close(outp[1]);
  if (errp[0] >= 0) close(errp[0]);
  if (errp[1] >= 0) close(errp[1]);
  if (inp[0] >= 0) close(inp[0]);
  if (inp[1] >= 0) close(inp[1]);
  if (sfd >= 0) close(sfd);
  sigprocmask(SIG_SETMASK, &old_mask, NULL);
done:
  free_envp(envp);
  free_argv(args);
  if (*threw) return ov_h_fail(why, strlen(why));
  return 0;
}

/* ------------------------------------------------------ fd-level I/O (W3/W4)
 *
 * A pipeline is two children and a pipe: the first writes, the second reads,
 * and the caller owns both ends. That needs four things the language did not
 * have: a way to make a pipe, to read any fd, to write any fd, and to close
 * one. The same four primitives are the file layer underneath (W4): an fd is
 * an fd, whether it came from a pipe, a socket or a file.
 *
 * Blocking, not poll-based, and deliberately so for this slice: W5 adds the
 * readiness layer, and a blocking read that reports EINTR correctly is the
 * honest primitive it will sit on.
 */
int ov_os_pipe(OvValue* out) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return ov_h_fail("os.pipe could not create a pipe", 31);
  ov_gc_begin_mutation();
  OvMap* m = ov_map_new();
  ov_map_set(m, ov_str(ov_str_new("read", 4)), ov_int(fds[0]));
  ov_map_set(m, ov_str(ov_str_new("write", 5)), ov_int(fds[1]));
  ov_gc_end_mutation();
  *out = ov_map(m);
  return 0;
}

/* Read up to `max` bytes from `fd`. Returns "" at end of file -- the same
 * convention file.read and net.read already use, so a caller tests EOF one
 * way across all three. */
int ov_os_read_fd(int fd, int64_t max, OvValue* out) {
  if (max < 0) max = 0;
  if (max > (64 * 1024 * 1024)) max = 64 * 1024 * 1024;
  char stackbuf[8192];
  char* buf = stackbuf;
  if ((size_t)max > sizeof stackbuf) {
    buf = (char*)malloc((size_t)max);
    if (!buf) return ov_h_fail("os.read could not allocate a buffer", 35);
  }
  ssize_t got = 0;
  for (;;) {
    got = read(fd, buf, (size_t)max);
    if (got >= 0) break;
    if (errno == EINTR) continue;   /* a signal must not end the read */
    break;
  }
  int rc = 0;
  if (got < 0) {
    rc = ov_h_fail_with(ov_str(ov_str_new(strerror(errno), strlen(strerror(errno)))));
  } else {
    *out = ov_str(ov_str_new(buf, (size_t)got));
  }
  if (buf != stackbuf) free(buf);
  return rc;
}

/* Write every byte, retrying short writes: a partial write to a pipe is
 * normal, and a truncated line to a downstream child is a silent corruption.
 * Returns the number of bytes written. */
int ov_os_write_fd(int fd, OvValue data, int64_t* wrote) {
  if (ov_tagof(data) != OV_STRING) return ov_h_fail("os.write needs a string", 26);
  /* A write to a pipe whose reader is gone raises SIGPIPE *before* it can
   * return EPIPE, so the default action would kill the process instead of
   * reporting the failure. Ignore it for the duration of the write and
   * restore it after: the process's own signal disposition is not ours to
   * change permanently. */
  void (*old)(int) = signal(SIGPIPE, SIG_IGN);
  const char* p = data.str->bytes;
  size_t left = data.str->len;
  *wrote = 0;
  while (left > 0) {
    ssize_t n = write(fd, p, left);
    if (n > 0) {
      p += n;
      left -= (size_t)n;
      *wrote += (int64_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    /* EPIPE: the reader went away. Report what made it, not "write failed". */
    signal(SIGPIPE, old);
    const char* why = (n < 0 && errno == EPIPE) ? "os.write: the reader closed the pipe"
                                                : "os.write failed";
    return ov_h_fail(why, strlen(why));
  }
  signal(SIGPIPE, old);
  return 0;
}

void ov_os_close(int fd) {
  if (fd >= 0) close(fd);
}

/* file.open(path, mode) -> an fd. One fd surface for files, pipes and
 * sockets, so streaming a 4GB log or tailing a growing one is the same code
 * as reading a pipe. */
int ov_file_open_fd(const char* path, const char* mode, int64_t* out_fd) {
  /* O_CLOEXEC: a child the script spawns must not inherit a file handle the
   * script opened. Passing one deliberately still works -- posix_spawn's
   * dup2 file action clears the flag on the child's descriptor. */
  int flags = O_RDONLY | O_CLOEXEC;
  if (!strcmp(mode, "w")) flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
  else if (!strcmp(mode, "a")) flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
  else if (!strcmp(mode, "rw")) flags = O_RDWR | O_CREAT | O_CLOEXEC;
  else if (!strcmp(mode, "r") || !mode[0]) flags = O_RDONLY | O_CLOEXEC;
  else return ov_h_fail("file.open mode must be r, w, a or rw", 36);
  int fd = open(path, flags, 0644);
  if (fd < 0) return ov_h_fail_with(ov_str(ov_str_new(strerror(errno), strlen(strerror(errno)))));
  *out_fd = fd;
  return 0;
}

/* file.seek(fd, offset, whence) -> the new position, or -1. whence is
 * "start" (default), "current" or "end". */
int64_t ov_file_seek_fd(int fd, int64_t off, int whence) {
  return (int64_t)lseek(fd, (off_t)off, whence);
}

/* os.stat(path) -> {size, mode, mtime, is_dir, is_file, is_link}. Reading a
 * file's metadata used to require opening it (and failed on anything that
 * could not be opened for reading); stat(2) answers without touching it. */
int ov_os_stat_path(const char* path, OvValue* out) {
  struct stat st;
  if (stat(path, &st) != 0) return ov_h_fail_with(ov_str(ov_str_new(strerror(errno), strlen(strerror(errno)))));
  ov_gc_begin_mutation();
  OvMap* m = ov_map_new();
  ov_map_set(m, ov_str(ov_str_new("size", 4)), ov_int((int64_t)st.st_size));
  ov_map_set(m, ov_str(ov_str_new("mode", 4)), ov_int((int64_t)st.st_mode));
  ov_map_set(m, ov_str(ov_str_new("mtime", 5)), ov_int((int64_t)st.st_mtime));
  ov_map_set(m, ov_str(ov_str_new("is_dir", 6)), ov_bool(S_ISDIR(st.st_mode)));
  ov_map_set(m, ov_str(ov_str_new("is_file", 7)), ov_bool(S_ISREG(st.st_mode)));
  ov_map_set(m, ov_str(ov_str_new("is_link", 7)), ov_bool(S_ISLNK(st.st_mode)));
  ov_gc_end_mutation();
  *out = ov_map(m);
  return 0;
}
