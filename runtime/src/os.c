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
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ovrt.h"
#include "ovrt_helpers.h"

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
