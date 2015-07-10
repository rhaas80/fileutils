#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char *argv[])
{
    int sd;
    char buf[BUFSIZ];
    struct sockaddr_un server;

    assert(argc == 2 && "Usage: connect SOCKET");
    
    sd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(sd == -1) {
      fprintf(stderr, "socket: %s", strerror(errno));
      exit(EXIT_FAILURE);
    }

    server.sun_family = AF_UNIX;
    strcpy(server.sun_path, argv[1]);
    const int len = strlen(server.sun_path) + sizeof(server.sun_family);
    if(connect(sd, (struct sockaddr *)&server, len) == -1) {
      fprintf(stderr, "connect: %s", strerror(errno));
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
