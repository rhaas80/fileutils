#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "streamcopy.h"
#include "pipe.h"

void setup_pipes(int pipes[], int npipes, char *argv[])
{
  for(int i = 0 ; i < npipes ; i++) {
    int pipefd[2];
    if(pipe(pipefd) == -1) {
      fprintf(stderr, "Could not create pipe: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    }

    pid_t pid = fork();
    if(pid == -1) {
      perror("fork failed");
      exit(EXIT_FAILURE);
    } else if(pid == 0) {
      /* child */
      if(dup2(pipefd[0], 0) == -1) {
        fprintf(stderr, "Could not dup pipe fd: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      }
      if(close(pipefd[0]) == -1 || close(pipefd[1]) == -1) {
        fprintf(stderr, "Could not close pipe fd: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      }
      execv(argv[0], argv);
      /* only get here if something went wrong */
      fprintf(stderr, "Could not execute %s: %s", argv[0], strerror(errno));
      exit(EXIT_FAILURE);
    } else {
      /* parent */
      if(close(pipefd[0]) == -1) {
        fprintf(stderr, "Could not close unused end of pipe fd: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      }
      if(fcntl(pipefd[1], F_SETFL, O_NONBLOCK) == -1) {
        fprintf(stderr, "Could not make pipe nonblocking: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      }
      pipes[i] = pipefd[1];
    }
  }
}
