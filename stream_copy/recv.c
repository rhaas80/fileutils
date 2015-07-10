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

#define BUFFERSIZE (BUFSIZ + 2*sizeof(ssize_t))

int main(int argc, char *argv[]) {
  int fd = open(argv[1], O_CREAT|O_RDWR, 0777);
  if(fd == -1) {
    fprintf(stderr, "Could not open %s for output: %s\n", argv[1], strerror(errno));
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
        fprintf(stderr, "Could not seek in %s: %s\n", argv[1], strerror(errno));
        exit(EXIT_FAILURE);
      }
      ssize_t written = write(fd, buffer+2*sizeof(ssize_t), size);
      if(written == -1) {
        fprintf(stderr, "Could not write to %s: %s\n", argv[1], strerror(errno));
        exit(EXIT_FAILURE);
      }
    } else {
      if(ftruncate(fd, off) == -1) {
        fprintf(stderr, "Could not set final file size of %s to %zd: %s\n",
                argv[1], off, strerror(errno));
        exit(EXIT_FAILURE);
      }
    }
  }

  if(close(fd)) {
    fprintf(stderr, "Could not write to %s: %s\n", argv[1], strerror(errno));
    exit(EXIT_FAILURE);
  }

  return 0;
}
