#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <queue>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <vector>

#define NUM_PACKETS 10
#define NUM_THREADS 4
#define TARGET_NUM_FILES 100
#define TARGET_NUM_BYTES (10*1024*1024)

#define ENVCOMMAND "/usr/bin/env"
#define TAROPTS {(char*)ENVCOMMAND, (char*)"tar", (char*)"x", NULL}
#define BUFFER_SIZE 1000000
#define ROUNDUP(x) ((x + 511) & ~511)

#define DEBUG

template <class packet_t>
class port_t
{
  public:
    port_t();
    ~port_t();

    void push_packet(const packet_t &packet);
    packet_t pull_packet();
  private:
    port_t(const port_t&);
    port_t& operator=(const port_t&);

    pthread_mutex_t lock;
    pthread_cond_t wait;

    std::queue<packet_t> packets;
};

template<class packet_t>
port_t<packet_t>::port_t() : packets()
{
  pthread_mutex_init(&lock, NULL);
  pthread_cond_init(&wait, NULL);
}

template<class packet_t>
port_t<packet_t>::~port_t()
{
  pthread_cond_destroy(&wait);
  pthread_mutex_destroy(&lock);
}

// enqueue a new packet, signalling possible waiting threads that there is a
// package
template <class packet_t>
void port_t<packet_t>::push_packet(const packet_t& packet)
{
  pthread_mutex_lock(&lock);
  packets.push(packet);
  pthread_cond_signal(&wait);
  pthread_mutex_unlock(&lock);
}

// obtain a packet from the port, block if none is available
template <class packet_t>
packet_t port_t<packet_t>::pull_packet()
{
  pthread_mutex_lock(&lock);
  while(packets.empty())
    pthread_cond_wait(&wait, &lock);
  packet_t retval = packets.front();
  packets.pop();
  pthread_mutex_unlock(&lock);

  return retval;
}

struct tarentry_t {
  off_t offset;
  size_t length;
};

struct workrequest_t {
  port_t<tarentry_t> *requestor;
};

struct worker_t {
  pthread_t worker_thread;
  port_t<workrequest_t>* master_port;
  port_t<tarentry_t> worker_port;
  int pipefd;
  pid_t tarpid;
};

void *worker(void *callarg)
{
  worker_t *mydata = static_cast<worker_t*>(callarg);
  port_t<workrequest_t>& master_port = *(mydata->master_port);
  port_t<tarentry_t>& myport = mydata->worker_port;
  int pipefd = mydata->pipefd;
  pid_t pid = mydata->tarpid;
  
  // fire up communication with my creator
  for(int i = 0 ; i < NUM_PACKETS ; ++i) {
    workrequest_t request = {&myport};
    master_port.push_packet(request);
  }

  // main loop processing work requests
  char *buf = new char[BUFFER_SIZE];
  while(true) {
    tarentry_t tarentry = myport.pull_packet();
#   ifdef DEBUG
    fprintf(stderr, "Received request at %zd length %zu for pid %d\n",
            tarentry.offset, tarentry.length, pid);
#   endif
    if(tarentry.length == 0) // magic size to quit
      break;

    for(off_t cur = tarentry.offset, end = cur+tarentry.length ; cur < end ; ) {
      size_t toread = cur + BUFFER_SIZE > end ? end-cur : BUFFER_SIZE;
      ssize_t haveread = pread(0, buf, toread, cur);
      if(haveread == -1) {
        fprintf(stderr, "Could not read %zu bytes: %s\n", toread,
                strerror(errno));
        exit(EXIT_FAILURE);
      } else if(haveread == 0) {
        fprintf(stderr, "Unexpected end of file\n");
        exit(EXIT_FAILURE);
      }
      ssize_t havewritten = write(pipefd, buf, (size_t)haveread);
      if(havewritten == -1) {
        fprintf(stderr, "Could not write %zu bytes: %s\n", haveread,
                strerror(errno));
        exit(EXIT_FAILURE);
      } else if (havewritten != haveread) {
        // TOOD: this can happen on signals it seems, likely need to loop
        fprintf(stderr, "Could not write %zu bytes\n", haveread);
        exit(EXIT_FAILURE);
      }
      cur += haveread;
    }

    // get more work
    workrequest_t request = {&myport};
    master_port.push_packet(request);
  }

  // close down tar file for tar
  static char zeros[2*512];
  ssize_t havewritten = write(pipefd, zeros, sizeof(zeros));
  if(havewritten == -1) {
    fprintf(stderr, "Could not write %zu bytes: %s\n", sizeof(zeros),
            strerror(errno));
    exit(EXIT_FAILURE);
  } else if(havewritten != sizeof(zeros)) {
    // TOOD: this can happen on signals it seems, likely need to loop
    fprintf(stderr, "Could not write %zu bytes\n", sizeof(zeros));
    exit(EXIT_FAILURE);
  }
# ifdef DEBUG
  fprintf(stderr, "wrote %zu bytes of zeros\n", havewritten);
# endif

# ifdef DEBUG
  fprintf(stderr, "Worker waiting for tar pid %d to finish\n", pid);
# endif
  // tear down pipes and tar process
  int ierr = close(pipefd);
  if(ierr) {
    perror("Could not close pipe");
    exit(EXIT_FAILURE);
  }
  fprintf(stderr, "Worker doning\n");
  waitpid(pid, NULL, 0);

# ifdef DEBUG
  fprintf(stderr, "Worker done\n");
# endif
  return NULL;
}

// I need to first fork, then spawn of threads so that pipes are not
// accidentally inherited by forked off processes and this needs to be a serial
// operation
void start_workers(port_t<workrequest_t>* master_port,
                   std::vector<worker_t>& workers)
{
  for(size_t i = 0 ; i < workers.size() ; ++i) {
    // spawn a worker tar to do the actual work
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
      char *argv[] = TAROPTS;
      execv(ENVCOMMAND, argv);
      /* only get here if something went wrong */
      fprintf(stderr, "Could not execute %s: %s\n", argv[0], strerror(errno));
      exit(EXIT_FAILURE);
    } else {
      /* parent */
      if(close(pipefd[0]) == -1) {
        fprintf(stderr, "Could not close unused end of pipe fd: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      }
    }

    // parent only
    // fire off IO thread for this tar
    workers[i].master_port = master_port;
    // worker_port is already set
    workers[i].pipefd = pipefd[1];
    workers[i].tarpid = pid;
    const int ierr =
      pthread_create(&workers[i].worker_thread, NULL, worker,
                     static_cast<void*>(&workers[i]));
    if(ierr) {
      fprintf(stderr, "Could not create thread %d: %s\n", i, strerror(errno));
      exit(EXIT_FAILURE);
    }
  }
}

int main(int argc, char **argv)
{
  port_t<workrequest_t> master_port;
  std::vector<worker_t> workers(NUM_THREADS);
  start_workers(&master_port, workers);

  struct posix_header
  {                              /* byte offset */
    char name[100];               /*   0 */
    char mode[8];                 /* 100 */
    char uid[8];                  /* 108 */
    char gid[8];                  /* 116 */
    char size[12];                /* 124 */
    char mtime[12];               /* 136 */
    char chksum[8];               /* 148 */
    char typeflag;                /* 156 */
    char linkname[100];           /* 157 */
    char magic[6];                /* 257 */
    char version[2];              /* 263 */
    char uname[32];               /* 265 */
    char gname[32];               /* 297 */
    char devmajor[8];             /* 329 */
    char devminor[8];             /* 337 */
    char prefix[155];             /* 345 */
    char pad[12];                 /* 500 */
  } hdr;
  off_t cur = 0, entrystart = 0;
  size_t num_entries = 0;
  while(true) {
    ssize_t haveread = pread(0, (void*)&hdr, sizeof(hdr), cur);
    if(haveread == -1) {
      fprintf(stderr, "Could not read %zu bytes: %s\n", sizeof(hdr),
              strerror(errno));
      exit(EXIT_FAILURE);
    } else if(haveread == 0) {
      break;
    } else if(hdr.name[0] == 0) {
      // really this should check for 1024 bytes of zeros
      break;
    }

    // length of tar entry
    char szbuf[13];
    snprintf(szbuf, sizeof(szbuf), "%.12s", hdr.size);
    long int size = ROUNDUP(strtol(szbuf, NULL, 8));

    if((hdr.typeflag == '0' || hdr.typeflag == 0) &&
      // wait until we have collected enough data to make this worthwhile
      (num_entries >= TARGET_NUM_FILES ||
       cur + sizeof(hdr) + size - entrystart >= TARGET_NUM_BYTES)) {
      struct tarentry_t tarentry = {
        entrystart, (size_t)((cur - entrystart) + sizeof(hdr) + size)
      };
#     ifdef DEBUG
      fprintf(stderr, "Master waiting for work request\n");
#         endif
      struct workrequest_t workrequest = master_port.pull_packet();
#     ifdef DEBUG
      fprintf(stderr, "Pushing request for '%s' at %zd length %ld\n",
              hdr.name, entrystart, size);
#     endif
      workrequest.requestor->push_packet(tarentry);
      entrystart = cur = cur + sizeof(hdr) + size;
      num_entries = 0;
    } else {
      cur = cur + sizeof(hdr) + size;
      num_entries += 1;
    }
  }
  // take care of dangling entries at end of file
  if(entrystart != cur) {
    struct tarentry_t tarentry = {
      entrystart, (size_t)(cur - entrystart)
    };
    struct workrequest_t workrequest = master_port.pull_packet();
    workrequest.requestor->push_packet(tarentry);
  }

# ifdef DEBUG
  fprintf(stderr, "Master asking workers to finish up\n");
# endif
  // ask all threads to finish up
  for(size_t i = 0 ; i < workers.size() ; ++i) {
    struct tarentry_t tarentry = {
      0, 0 // magic packet to signal end of file
    };
    workers[i].worker_port.push_packet(tarentry);
  }
  
# ifdef DEBUG
  fprintf(stderr, "Master waiting for workers to finish up\n");
# endif
  // wait for threads to finish their work and exit
  for(size_t i = 0 ; i < workers.size() ; ++i) {
    const int ierr =
      pthread_join(workers[i].worker_thread, NULL);
    if(ierr) {
      fprintf(stderr, "Could not join threads: %s\n", strerror(ierr));
      exit(EXIT_FAILURE);
    }
  }

  return EXIT_SUCCESS;
}
