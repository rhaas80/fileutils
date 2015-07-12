#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <sys/wait.h>
#include <unistd.h>

#include "streamcopy.h"
#include "send.h"
#include "socket.h"
#include "recv.h"
#include "socket.h"
#include "pipe.h"

int main(int argc, char *argv[])
{
  /* to argument parsing */
  assert(argc >= 2);

  if(argv[1][0] == '-') {
    /* server calls up */
    if(strcmp(argv[1], "-send") == 0) {
      assert(argc == 5);
      int nprocs = atoi(argv[2]);
      char *src = argv[3];
      char *sockname = argv[4];
      int tunnels[nprocs];

      setup_sockets(tunnels, nprocs, sockname);

      stream_send(src, tunnels, nprocs);
    } else if(strcmp(argv[1], "-recv") == 0) {
      assert(argc == 3);
      char *dst = argv[2];

      stream_recv(dst);
    } else if(strcmp(argv[1], "-connect") == 0) {
      assert(argc == 3);
      char *sockname = argv[2];

      pipe_to_socket(sockname);
    } else {
      assert(0 && "Unknown service");
    }
  } else {
    assert(argc == 6);
    char *nprocs_s = argv[2];
    int nprocs = atoi(nprocs_s);
    int tunnels[nprocs];

    if(strcmp(argv[1], "push") == 0) {
      char *src = argv[3];
      char *host = argv[4];
      char *dst = argv[5];

      char *args[] = {
        getenv("SHELL"), "-c", "${0} ${1+\"$@\"}",
        "ssh", "-o", "ControlPath=none", host, getcmd(), "-recv", dst,
        NULL
      };
      setup_pipes(tunnels, nprocs, args);

      stream_send(src, tunnels, nprocs);
    } else if(strcmp(argv[1], "pull") == 0) {
      char *host = argv[3];
      char *src = argv[4];
      char *dst = argv[5];

      char *sockname;
      int len = asprintf(&sockname, ".streamcopy_%04x", (int)getpid());

      char *s_args[] = {
        getenv("SHELL"), "-c", "${0} ${1+\"$@\"}",
        "ssh", host, getcmd(), "-send", nprocs_s, src, sockname,
        NULL
      };
      int server;
      setup_pipes(&server, 1, s_args);

      setup_recvs(dst, host, sockname, nprocs);
    } else {
      assert(0 && "Unknwon command");
    }

    /* wait for all children */
    while(wait(NULL) > 0) {};
  }

  return 0;
}
