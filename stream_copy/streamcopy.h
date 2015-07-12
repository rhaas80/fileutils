#include <stdio.h>
#include <sys/types.h>

#define BUFFERSIZE (BUFSIZ + 2*sizeof(ssize_t))
#define getcmd() (getenv("TRANSFER_COMMAND") ? getenv("TRANSFER_COMMAND") : "transfer")
