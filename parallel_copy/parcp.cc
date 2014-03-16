#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <queue>
#include <string>
#include <map>
#include <cstring>
#include <cerrno>
#include <cassert>
#include <cstdlib>
#include <vector>
#include <iostream>

#define NUM_THREADS 4
#define CHUNK_SIZE 80000

struct serialized_packet_t
{
  char type[4];  // fourcc of packet
  char fid[8];   // the unique ID for the file during the run
  char size[16]; // space padded ASCII size in bytes excluding header
                 // excl NUL byte
  // payload
};

#define TYPE_DATA "DATA"
#define TYPE_FILE "FILE"
#define TYPE_WORK "WORK"

class packet_t;

// a message port that accepts packets
// pushing a packet enqueues it to the port's list of avaialable packets
// pulling removes the oldest from the list, it blocks if no package is
// available
class port_t
{
  public:
    port_t();
    ~port_t();

    void push_packet(packet_t* packet);
    packet_t* pull_packet();
  private:
    port_t(const port_t&);
    port_t& operator=(const port_t&);

    pthread_mutex_t lock;
    pthread_cond_t wait;

    std::queue<packet_t*> packets;
};

port_t::port_t()
{
  pthread_mutex_init(&lock, NULL);
  pthread_cond_init(&wait, NULL);
}

port_t::~port_t()
{
  pthread_cond_destroy(&wait);
  pthread_mutex_destroy(&lock);
}

// enqueue a new packet, signalling possible waiting threads that there is a
// package
void port_t::push_packet(packet_t* packet)
{
  pthread_mutex_lock(&lock);
  packets.push(packet);
  pthread_cond_signal(&wait);
  pthread_mutex_unlock(&lock);
}

// obtain a packet from the port, block if none is available
packet_t* port_t::pull_packet()
{
  packet_t* retval;

  pthread_mutex_lock(&lock);
  while(packets.empty())
    pthread_cond_wait(&wait, &lock);
  retval = packets.front();
  packets.pop();
  pthread_mutex_unlock(&lock);

  return retval;
}

// TODO: turn into class with serialize member and reply member
struct packet_t
{
  port_t* reply_port;
  char type[4];
  int size;
  char fid[8];
  std::vector<char> data;
};

// format a number to string of a given length, not NUL at the end
static void fmtnum(char *dst, int len, size_t number)
{
  char buffer[32];
  assert(len < int(sizeof(buffer)));
  snprintf(buffer, sizeof(buffer), "%*zu", (int)len, number);
  memcpy(dst, buffer, size_t(len));
}

// this is port of the controlling thread. It accepts work requests by the
// workers as well as data pushes by the workers.
port_t master_port;

// each worker reads files as instructed by the controlling thread. It pushes
// the data to the controller as a sequence of DATA packets. The last packet
// has zero size and indicates EOF. A worker requests new work by sending a
// WORK packet to the controller.
void* worker(void *callarg)
{
  port_t* myport = static_cast<port_t*>(callarg);

  while(true) {
    packet_t* packet = myport->pull_packet();
    //std::cerr << "Got packet: "
    //          << std::string(packet->type, sizeof(packet->type))
    //          << " size: " << packet->size
    //          << " payload: " << std::string(&packet->data[0], packet->data.size())
    //          << std::endl;

    if(strncmp(packet->type, TYPE_FILE, sizeof(packet->type)) != 0) {
      std::cerr << "Unexpected type "
                << std::string(packet->type, sizeof(packet->type)) << std::endl;
      master_port.push_packet(packet);
      exit(1);
    }

    std::string fn(&packet->data[0], packet->data.size());
    FILE* fh = fopen(fn.c_str(), "rb");
    if(fh == NULL) {
      std::cerr << "Could not open file " << fn << ": " << strerror(errno)
                << std::endl;
      exit(1);
    }

    packet->data.resize(CHUNK_SIZE);
    memcpy(packet->type, TYPE_DATA, sizeof(packet->type));

    while(!feof(fh)) {
      size_t sz_read = fread(&packet->data[0], 1, packet->data.size(), fh);
      if(ferror(fh)) {
        std::cerr << "Could not read from file " << fn << ": "
                  << strerror(errno) << std::endl;
        exit(1);
      }
      packet->size = sz_read;
      master_port.push_packet(packet);
      packet = myport->pull_packet();
      if(strncmp(packet->type, TYPE_DATA, sizeof(packet->type)) != 0) {
        std::cerr << "Unexpected type "
                  << std::string(packet->type, sizeof(packet->type))
                  << std::endl;
        exit(1);
      }
    }

    fclose(fh);

    // terminate file
    packet->size = 0;
    master_port.push_packet(packet);
    packet = myport->pull_packet();

    // request more work
    memcpy(packet->type, TYPE_WORK, sizeof(packet->type));
    master_port.push_packet(packet);
  }

  return NULL;
}

// the controlling thread for archive creation. It creates the worker threads
// and instructs them to read files. It accepts data packets from the workers
// and writes them as a stream to stdout.
void sender()
{
  std::vector<pthread_t> threads(NUM_THREADS);
  for(size_t i = 0 ; i < threads.size() ; ++i) {
    port_t* port = new port_t();
    packet_t* packet = new packet_t();
    if(port == NULL) {
      std::cerr << "Could not allocate memory for port and packet for thread "
                << i << std::endl;
      exit(1);
    }
    const int ierr =
      pthread_create(&threads[i], NULL, worker, static_cast<void*>(port));
    if(ierr) {
      std::cerr << "Could not create thread " << i << ": " << strerror(errno)
                << std::endl;
      exit(1);
    }
    packet->reply_port = port;
    memcpy(packet->type, TYPE_WORK, sizeof(packet->type));
    packet->size = 0;
    master_port.push_packet(packet);
  }

  int fid = 0;  // unique ID for each file
  int active_threads = threads.size(); // number of threads that are processing
                                       // a file
  // loop as long as we either have files to process or not all workers are
  // done
  while(!std::cin.eof() || active_threads > 0) {
    packet_t* packet = master_port.pull_packet();
    if(strncmp(packet->type, TYPE_WORK, sizeof(packet->type)) == 0) {
      active_threads -= 1;

      std::string fn;
      if(std::getline(std::cin, fn).good()) {
        fid += 1;

        // tell worker to start reading file
        memcpy(packet->type, TYPE_FILE, sizeof(packet->type));
        packet->size = fn.size();
        fmtnum(packet->fid, int(sizeof(packet->fid)), fid);
        packet->data.resize(packet->size);
        memcpy(&packet->data[0], fn.c_str(), packet->size);

        // send FILE packet to stream
        serialized_packet_t ser_packet;
        memcpy(ser_packet.type, packet->type, sizeof(packet->type));
        fmtnum(ser_packet.size, int(sizeof(ser_packet.size)), packet->size);
        memcpy(ser_packet.fid, packet->fid, sizeof(packet->fid));
        fwrite(&ser_packet, sizeof(ser_packet), 1, stdout);
        fwrite(&packet->data[0], 1, packet->size, stdout);

        packet->reply_port->push_packet(packet);
        active_threads += 1;
      }
    } else if(strncmp(packet->type, TYPE_DATA, sizeof(packet->type)) == 0) {
      // accept data from worker and write to stream
      serialized_packet_t ser_packet;
      memcpy(ser_packet.type, packet->type, sizeof(packet->type));
      fmtnum(ser_packet.size, int(sizeof(ser_packet.size)), packet->size);
      memcpy(ser_packet.fid, packet->fid, sizeof(packet->fid));
      fwrite(&ser_packet, sizeof(ser_packet), 1, stdout);
      fwrite(&packet->data[0], 1, packet->size, stdout);
      packet->reply_port->push_packet(packet);
    } else {
      std::cerr << "Unexpected type "
                << std::string(packet->type, sizeof(packet->type))
                << std::endl;
      exit(1);
    }
  }
}

// extracts the data packets from a stream from stdin and re-creates the files
// in the stream
void receiver()
{
  serialized_packet_t ser_packet;
  std::vector<char> buf;
  std::map<std::string, FILE*> filehandles;
  std::map<std::string, std::string> filenames;

  while(fread(&ser_packet, sizeof(ser_packet), 1, stdin))
  {
    // parse packet header into variables
    std::string fid(ser_packet.fid, sizeof(ser_packet.fid));
    char sizebuf[sizeof(ser_packet.size)+1];
    memcpy(sizebuf, ser_packet.size, sizeof(ser_packet.size));
    sizebuf[sizeof(ser_packet.size)] = '\0';

    buf.resize(atoi(sizebuf));
    const size_t sz = fread(&buf[0], 1, buf.size(), stdin);
    if(sz == 0 && ferror(stdin)) {
      std::cerr << "failed to read from stdin: " << strerror(errno)
                << std::endl;
      exit(1);
    }

    if(strncmp(ser_packet.type, TYPE_FILE, sizeof(ser_packet.type)) == 0) {
      // new file, create it and record its file-id
      std::string fn(&buf[0], buf.size());

      // behave like tar, forbid absolute paths
      // TODO: move into writer
      if(fn[0] == '/') {
        static bool warned = false;
        if(!warned) {
          std::cerr << "stripping absolute path from filename " << fn
                    << std::endl;
          warned = true;
        }
        fn.erase(0,1);
      }

      // create directory path to file if needed
      // TODO: handle ".." etc properly
      for(size_t start = 0, slash = fn.find('/', start) ;
          slash != std::string::npos ;
          start = slash+1, slash = fn.find('/', start)) {
        const int ierr = mkdir(fn.substr(0, slash).c_str(), 0777);
        if(ierr && errno != EEXIST) {
          std::cerr << "failed to create directory '" << fn.substr(0, slash) << "': "
                    << strerror(errno) << std::endl;
          exit(1);
        }
      }

      FILE* fh = fopen(fn.c_str(), "wb");
      if(fh == NULL) {
        std::cerr << "failed to open '" << fn << "' for writing: "
                  << strerror(errno) << std::endl;
        exit(1);
      }
      if(filehandles[fid]) {
        std::cerr << "corrupt input, id " << fid << " for file " << fn
                  << " not unique" << std::endl;
        exit(1);
      }
      std::clog << "creating file " << fn << std::endl;
      filehandles[fid] = fh;
      filenames[fid] = fn;
    } else if(strncmp(ser_packet.type, TYPE_DATA,
                      sizeof(ser_packet.type)) == 0) {
      // a data packet, write to the correct file and close the file once the
      // zero size packet arrives
      FILE* fh = filehandles[fid];
      if(fh == NULL) {
        std::cerr << "corrupt input, unknown id " << fid << std::endl;
        exit(1);
      }
      const size_t written = fwrite(&buf[0], 1, buf.size(), fh);
      if(written != buf.size()) {
        std::cerr << "failed to write to " << filenames[fid]
                  << strerror(errno) << std::endl;
        exit(1);
      }
      if(written == 0) { // EOF marker
        const std::string fn = filenames[fid];
        if(fclose(fh)) {
          std::cerr << "failed to write to " << fn
                    << strerror(errno) << std::endl;
          exit(1);
        }
        std::clog << "finished file " << fn << std::endl;
        filehandles.erase(fid);
        filenames.erase(fid);
      }
    } else {
      std::cerr << "Unexpected type "
                << std::string(ser_packet.type, sizeof(ser_packet.type))
                << std::endl;
      exit(1);
    }
  }
  if(ferror(stdin)) {
    std::cerr << "failed to read from stdin: " << strerror(errno) << std::endl;
    exit(1);
  }
}

int main(int argc, char **argv)
{
  // TODO: do proper cmdline parsing using getopt or commandline.c
  if(argc != 2 || (strcmp(argv[1], "-create") && strcmp(argv[1], "-extract"))) {
    std::cerr << "usage: " << argv[0] << " [-create|-extract]" << std::endl;
    exit(1);
  }

  if(strcmp(argv[1], "-create") == 0)
    sender();
  else
    receiver();

  return 0;
}
