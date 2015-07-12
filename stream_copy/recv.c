#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "streamcopy.h"
#include "recv.h"

int stream_recv(const char *fn) {
  int fd = open(fn, O_CREAT|O_RDWR, 0777);
  if(fd == -1) {
    fprintf(stderr, "Could not open %s for output: %s\n", fn, strerror(errno));
    exit(EXIT_FAILURE);
  }

  char *buffer = malloc(BUFFERSIZE); 
  if(buffer == NULL) {
    fprintf(stderr, "Could not allocate buffer space\n");
    exit(EXIT_FAILURE);
  }

  while(!feof(stdin)) {
    int head_read = fread(buffer, 1, 2*sizeof(ssize_t), stdin);
    if(head_read != BUFFERSIZE && ferror(stdin)) {
      fprintf(stderr, "Could not read from stdin: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    }
    if(head_read == 0)
     continue; /* eof */
    ssize_t off = ((ssize_t*)buffer)[0];
    ssize_t size = ((ssize_t*)buffer)[1];
    if(size > 0) {
      int data_read = fread(buffer+2*sizeof(ssize_t), 1, size, stdin);
      if(data_read != BUFFERSIZE && ferror(stdin)) {
        fprintf(stderr, "Could not read from stdin: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      }
      if(data_read == 0)
       continue; /* eof */

      if(lseek(fd, off, SEEK_SET) == -1) {
        fprintf(stderr, "Could not seek in %s: %s\n", fn, strerror(errno));
        exit(EXIT_FAILURE);
      }
      ssize_t written = write(fd, buffer+2*sizeof(ssize_t), size);
      if(written == -1) {
        fprintf(stderr, "Could not write to %s: %s\n", fn, strerror(errno));
        exit(EXIT_FAILURE);
      }
    } else {
      if(ftruncate(fd, off) == -1) {
        fprintf(stderr, "Could not set final file size of %s to %zd: %s\n",
                fn, off, strerror(errno));
        exit(EXIT_FAILURE);
      }
    }
  }

  if(close(fd)) {
    fprintf(stderr, "Could not write to %s: %s\n", fn, strerror(errno));
    exit(EXIT_FAILURE);
  }

  return 0;
}

void setup_recvs(const char *dst, const char *host, const char *sockname,
                 int nprocs)
{
  for(int i = 0 ; i < nprocs ; i++) {
    int pipefd[2];
    if(pipe(pipefd) == -1) {
      fprintf(stderr, "Could not create pipe: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    }

    pid_t pid = fork();
    if(pid == -1) {
      perror("fork failed");
      exit(EXIT_FAILURE);
    } else if (pid == 0) {
      /* (transfer) parent */
      continue;
    }
    /* transfer never gets past here */

    pid = fork();
    if(pid == -1) {
      perror("fork failed");
      exit(EXIT_FAILURE);
    } else if (pid == 0) {
      /* child */
      if(dup2(pipefd[1], 1) == -1) {
        fprintf(stderr, "Could not dup pipe fd: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      }
      if(close(pipefd[0]) == -1 || close(pipefd[1]) == -1) {
        fprintf(stderr, "Could not close pipe fd: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      }

      char *argv[] = {
        getenv("SHELL"), "-c", "${0} ${1+\"$@\"}",
        "ssh", "-o", "ControlPath=none", strdup(host), getcmd(), "-connect",
        strdup(sockname), NULL
      };
      execv(argv[0], argv);
      /* only get here if something went wrong */
      fprintf(stderr, "Could not execute %s: %s", argv[0], strerror(errno));
      exit(EXIT_FAILURE);
    } else {
      /* parent */
      if(dup2(pipefd[0], 0) == -1) {
        fprintf(stderr, "Could not dup pipe fd: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      }
      if(close(pipefd[0]) == -1 || close(pipefd[1]) == -1) {
        fprintf(stderr, "Could not close pipe fd: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      }
      stream_recv(dst);
      exit(0);
    }
  }
  
}
