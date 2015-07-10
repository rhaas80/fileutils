#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "streamcopy.h"
#include "socket.h"

void setup_sockets(int socks[], int nsocks, char *sockname)
{
  struct sockaddr_un server;
  int sd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(sd == -1) {
      fprintf(stderr, "socket: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
  }
  server.sun_family = AF_UNIX;
  strcpy(server.sun_path, sockname);
  unlink(server.sun_path);
  const int len = strlen(server.sun_path) + sizeof(server.sun_family);
  if(bind(sd, (struct sockaddr *)&server, len) == -1) {
    fprintf(stderr, "bind: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  if(listen(sd, nsocks) == -1) {
    fprintf(stderr, "listen: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  for(int i = 0 ; i < nsocks ; i++) {
    socks[i] = accept(sd, NULL, NULL);
    if(socks[i] == -1) {
      fprintf(stderr, "accept: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    }
  }

  if(close(sd) == -1) {
    fprintf(stderr, "close: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }
  unlink(server.sun_path);
}

int pipe_to_socket(char *sockname)
{
    int sd;
    char buf[BUFFERSIZE];
    struct sockaddr_un server;

    sd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(sd == -1) {
      fprintf(stderr, "socket: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    }

    server.sun_family = AF_UNIX;
    strcpy(server.sun_path, sockname);
    const int len = strlen(server.sun_path) + sizeof(server.sun_family);
    int connfail = 1;
    for(int i = 0 ; i < 10 && connfail ; i++) {
      connfail = connect(sd, (struct sockaddr *)&server, len);
      if(connfail)
        sleep(1);
    }
    if(connfail)
    {
      fprintf(stderr, "could not connect to socket %s: %s\n", sockname,
              strerror(errno));
      exit(EXIT_FAILURE);
    }

    while(1)
    {
      int size = recv(sd, buf, BUFSIZ, 0);
      if(size == -1) {
        fprintf(stderr, "error reading from socket: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      } 

      if(size == 0)
        break;

      int written = write(1, buf, size);
      if(written == -1) {
        fprintf(stderr, "error writing to stdout: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      } 
    }

    if(close(sd) == -1) {
      fprintf(stderr, "error closing socket: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    }

    return 0;
}

