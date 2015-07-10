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
#include "send.h"

int stream_send(const char *fn, int pipes[], int npipes)
{
  int fd = open(fn, O_RDONLY, 0);
  if(fd == -1) {
    fprintf(stderr, "Could not open file %s for reading: %s\n", fn,
            strerror(errno));
    exit(EXIT_FAILURE);
  }
    
  char *buffers[npipes];
  ssize_t left[npipes], size[npipes];

  int maxfd = 0;
  fd_set writefds;
  for(int i = 0 ; i < npipes ; i++) {
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
    for(int i = 0 ; i < npipes ; i++) {
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
    for(int i = 0 ; i < npipes ; i++) {
      if(FD_ISSET(pipes[i], &writefds)) {
        do {
          if(left[i] == 0) {
            left[i] = read(fd, buffers[i]+2*sizeof(ssize_t), BUFFERSIZE-2*sizeof(ssize_t));
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
      for(int i = 0 ; i < npipes ; i++) {
        if(left[i] > 0) {
          all_written = 0;
          break;
        }
      }
    }
  }

  /* flush any leftover caches and close pipes */
  for(int i = 0 ; i < npipes ; i++) {
    if(close(pipes[i]) == -1) {
      fprintf(stderr, "Could not close pipe fd: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    }
  }

  close(fd);

  return 0;
}
