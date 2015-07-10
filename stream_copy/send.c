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

void setup_transfers(int pipes[], int npipes, char *argv[])
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
        fprintf(stderr, "Could not close read end of pipe fd: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      }
      if(fcntl(pipefd[1], F_SETFL, O_NONBLOCK) == -1) {
        fprintf(stderr, "Could not make write end of pipe nonblocking: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      }
      pipes[i] = pipefd[1];
    }
  }
}

#define BUFFERSIZE (BUFSIZ + 2*sizeof(ssize_t))
int main(int argc, char *argv[])
{
  assert(argc >= 3 && "Usage: send NPROCS [tunnel] [opts] recv");
  const int nprocs = atoi(argv[1]);

  int pipes[nprocs];
  char *buffers[nprocs];
  ssize_t left[nprocs], size[nprocs];

  setup_transfers(pipes, nprocs, &argv[2]);

  int maxfd = 0;
  fd_set writefds;
  for(int i = 0 ; i < nprocs ; i++) {
    if(pipes[i] > maxfd)
      maxfd = pipes[i];

    left[i] = size[i] = 0;
    if((buffers[i] = malloc(BUFFERSIZE)) == NULL) {
      fprintf(stderr, "Could not allocate buffer space\n");
      exit(EXIT_FAILURE);
    }
  }

  ssize_t offset = 0;
  for(int all_written = 0, all_read = 0 ; !all_read || !all_written ; ) {
    FD_ZERO(&writefds);
    for(int i = 0 ; i < nprocs ; i++) {
      /* at least one has left[i] != 0 due to the check at the end of the
       * foo_loop */
      if(!(all_read && left[i] == 0)) {
        FD_SET(pipes[i], &writefds);
      }
    }
    int retval = select(maxfd+1, NULL, &writefds, NULL, NULL);
    if(retval == -1) {
      fprintf(stderr, "Could not wait for writing end of pipe: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    }
    for(int i = 0 ; i < nprocs ; i++) {
      if(FD_ISSET(pipes[i], &writefds)) {
        do {
          if(left[i] == 0) {
            left[i] = read(0, buffers[i]+2*sizeof(ssize_t), BUFFERSIZE-2*sizeof(ssize_t));
            if(left[i] == -1) {
              fprintf(stderr, "Could not read from stdin: %s\n", strerror(errno));
              exit(EXIT_FAILURE);
            } else if(left[i] == 0) {
              /* write a 0 byte termination packet to tell the writer about
               * EOF, but only once */
              if(all_read)
                break;
              all_read = 1;
            }
            ((ssize_t*)buffers[i])[0] = offset;
            ((ssize_t*)buffers[i])[1] = left[i];
            offset += left[i];
            left[i] += 2*sizeof(ssize_t);
            size[i] = left[i];
          }
          if(left[i] > 0) {
              ssize_t written = write(pipes[i], &buffers[i][size[i]-left[i]], left[i]);
              if(written == -1) {
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                  written = 0;
                  break;
                } else {
                  fprintf(stderr, "Could not write to pipe: %s\n", strerror(errno));
                  exit(EXIT_FAILURE);
                }
              }
              left[i] -= written;
          }
        } while(left[i] == 0); /* write until we'd block */
      }
    }
    /* when all is read, wait until all is written before ending */
    if(all_read) {
      all_written = 1;
      for(int i = 0 ; i < nprocs ; i++) {
        if(left[i] > 0) {
          all_written = 0;
          break;
        }
      }
    }
  }

  /* flush any leftover caches and close pipes */
  for(int i = 0 ; i < nprocs ; i++) {
    if(close(pipes[i]) == -1) {
      fprintf(stderr, "Could not close pipe fd: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    }
  }

  /* wait for all children */
  while(wait(NULL) > 0) {};

  return 0;
}
