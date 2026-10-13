// Ovyth :: runtime/src/net.c
//
// A TCP server, because the agent needs to be reachable from somewhere other
// than the terminal it was started in.
//
// `os.run()` let the language act on its machine; `net.listen()` lets the
// machine be acted on from another device. Together they are the agent's
// remote control: a phone connects, and each line it sends goes through the
// exact same `os.run()` path a local turn would — the tool dispatch, the
// history, the guard, the sweep for a missing binary's spellings. Nothing
// about the agent changes; only where its input comes from and its output
// goes.
//
// Deliberately raw TCP rather than HTTP or WebSocket. A line protocol needs
// no framing, no handshake, and the binary is one file that depends on
// nothing; the client is `nc` on a phone. HTTP can be built on top of these
// primitives without replacing them, so this is the layer that does not need
// to change.
//
// Blocking by design: accept() blocks until a phone arrives, read() blocks
// until a line does. A server that must do something else meanwhile is a
// thread, and a thread in a runtime this small is a much bigger decision than
// this feature warrants.
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ovrt.h"
#include "ovrt_helpers.h"

/* Open a listening socket on `host`:`port` and return its fd, or -1.
 *
 * host may be "0.0.0.0" to reach the machine from the network (what a phone
 * needs) or "127.0.0.1" to stay reachable only from the machine itself.
 * SO_REUSEADDR is set so a restart after a crashed run does not spend a
 * minute in TIME_WAIT, which is the difference between "restart the agent"
 * and "reboot".
 *
 * On failure *err receives a short static description, so the caller can name
 * the reason instead of leaving the user to work out that the port is taken. */
int ov_net_listen(const char* host, int port, int backlog, char** err) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (err) *err = "cannot create a socket";
    return -1;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (!host || !*host || strcmp(host, "*") == 0)
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  else if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    if (err) *err = "not an IPv4 address; use 0.0.0.0 or 127.0.0.1";
    close(fd);
    return -1;
  }

  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    if (err) *err = strerror(errno);
    close(fd);
    return -1;
  }
  if (listen(fd, backlog < 1 ? 8 : backlog) != 0) {
    if (err) *err = strerror(errno);
    close(fd);
    return -1;
  }
  return fd;
}

/* Accept one connection, blocking until it arrives. Returns the client fd, or
 * -1. TCP_NODELAY is set on it: a phone on a cellular link should not wait
 * for Nagle to bundle a reply with whatever the agent says next. */
int ov_net_accept(int listen_fd) {
  struct sockaddr_in peer;
  socklen_t len = sizeof(peer);
  int fd = accept(listen_fd, (struct sockaddr*)&peer, &len);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return fd;
}

/* Blocking read of up to `len` bytes. Returns the count, 0 on disconnect, -1
 * on error. EINTR is retried: a signal arriving mid-read (a terminal resize,
 * a SIGCHLD from a tool the agent ran) must not end the session. */
int ov_net_read(int fd, char* buf, int len) {
  for (;;) {
    ssize_t n = recv(fd, buf, (size_t)len, 0);
    if (n >= 0) return (int)n;
    if (errno == EINTR) continue;
    return -1;
  }
}

/* Write every byte of `data`, retrying short sends. A partial send is a real
 * possibility on a congested link, and a truncated line would show up as the
 * agent forgetting half a sentence. Returns 0 on success. */
int ov_net_write(int fd, const char* data, int len) {
  int off = 0;
  while (off < len) {
    ssize_t n = send(fd, data + off, (size_t)(len - off), MSG_NOSIGNAL);
    if (n > 0) { off += (int)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    return -1;
  }
  return 0;
}

void ov_net_close(int fd) {
  if (fd >= 0) close(fd);
}

/* The port a listening socket actually bound to.
 *
 * Binding port 0 asks the kernel for a free one, and this is how the caller
 * finds out which it picked -- so a test or a multi-agent program can start
 * several listeners without racing over a hard-coded port and colliding. */
int ov_net_port(int fd) {
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);
  if (getsockname(fd, (struct sockaddr*)&addr, &len) != 0) return -1;
  return (int)ntohs(addr.sin_port);
}
