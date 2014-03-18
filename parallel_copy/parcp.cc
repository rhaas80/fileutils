#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>

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
#define TYPE_STAT "STAT"

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

// tar file format
/* from gnu tar docs. Likely makes this file GPL */
/* http://www.gnu.org/software/tar/manual/html_node/Standard.html */

/* tar Header Block, from POSIX 1003.1-1990.  */

/* POSIX header.  */

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
};

#define TMAGIC   "ustar"        /* ustar and a null */
#define TMAGLEN  6
#define TVERSION "00"           /* 00 and no null */
#define TVERSLEN 2

/* Values used in typeflag field.  */
#define REGTYPE  '0'            /* regular file */
#define SYMTYPE  '2'            /* reserved */

#define BLOCKSIZE 512

#define MAX_FILE_SIZE ((8L<<(3*(sizeof(((struct posix_header*)0)->size)-1)))-1)

// format a number to string of a given length, not NUL at the end
static void fmtnum(char *dst, int len, size_t number)
{
  char buffer[32];
  assert(len < int(sizeof(buffer)));
  snprintf(buffer, sizeof(buffer), "%*zu", (int)len, number);
  memcpy(dst, buffer, size_t(len));
}

static size_t round_to_block(size_t sz)
{
  return (sz + BLOCKSIZE-1) & ~(BLOCKSIZE-1);
}

// create a tar header for a file in the provided buffer hdr which must be at
// least BLOCKSIZE bytes large
static char make_tarheader(const std::string fn, posix_header* hdr)
{
  const char* filename = fn.c_str();
  struct stat statbuf;
  struct group *grp, grp_buf;
  struct passwd *pwd, pwd_buf;
  std::vector<char> grpstrings(100), pwdstrings(100);
  unsigned long int checksum;

  const int lstat_ierr = lstat(filename, &statbuf);
  if(lstat_ierr) {
    std::cerr << "failed to stat file '" << fn << "':"
              << strerror(errno) << std::endl;
    exit(1);
  }

  int grp_ierr;
  while((grp_ierr = getgrgid_r(statbuf.st_gid, &grp_buf, &grpstrings[0],
                               grpstrings.size(), &grp)) == ERANGE) {
    grpstrings.resize(2*grpstrings.size());
  }
  if(grp_ierr) {
    std::cerr << "failed to get group name for gid " << statbuf.st_gid
              << ":" << strerror(errno) << std::endl;
    exit(1);
  }

  int pwd_ierr;
  while((pwd_ierr = getpwuid_r(statbuf.st_uid, &pwd_buf, &pwdstrings[0],
                    pwdstrings.size(), &pwd)) == ERANGE) {
    pwdstrings.resize(2*pwdstrings.size());
  }
  if(pwd_ierr) {
    std::cerr << "failed to get user name for uid " << statbuf.st_uid
              << ":" << strerror(errno) << std::endl;
    exit(1);
  }

  if(statbuf.st_size > MAX_FILE_SIZE)
  {
    fprintf(stderr, "size %zd of file %s too big. Maximum size is %lu\n",
            statbuf.st_size, filename, MAX_FILE_SIZE);
    exit(1);
  }

  assert(S_ISLNK(statbuf.st_mode) || S_ISREG(statbuf.st_mode));

  memset(hdr, 0, BLOCKSIZE);
  if(S_ISLNK(statbuf.st_mode))
  {
    if((size_t)statbuf.st_size >= sizeof(hdr->linkname))
    {
      fprintf(stderr, "linked filename %s too long. Last part must be shorter than %zu characters\n",
              hdr->linkname, sizeof(hdr->linkname)-1);
      exit(1);
    }
    size_t sz_read =
      readlink(filename, hdr->linkname, sizeof(hdr->linkname));
    if(sz_read == (size_t)-1)
    {
      fprintf(stderr, "Could not read link %s: %s\n", filename,
              strerror(errno));
      exit(1);
    }
    hdr->linkname[statbuf.st_size] = '\0';
    statbuf.st_size = 0; // tar requires zero size for links
  }
  else
  {
    strcpy(hdr->linkname, "");
  }

  // name is set at the end due to funny handling of long file names
  snprintf(hdr->mode, sizeof(hdr->mode), "%0*o",
           (int)sizeof(hdr->mode)-1, statbuf.st_mode);
  snprintf(hdr->uid, sizeof(hdr->uid), "%0*o",
           (int)sizeof(hdr->uid)-1, statbuf.st_uid);
  snprintf(hdr->gid, sizeof(hdr->gid), "%0*o",
           (int)sizeof(hdr->gid)-1, statbuf.st_gid);
  snprintf(hdr->size, sizeof(hdr->size), "%0*lo",
           (int)sizeof(hdr->size)-1, statbuf.st_size);
  snprintf(hdr->mtime, sizeof(hdr->mtime), "%0*lo",
           (int)sizeof(hdr->mtime)-1, statbuf.st_mtime);
  memset(hdr->chksum, ' ', sizeof(hdr->chksum));
  hdr->typeflag = S_ISLNK(statbuf.st_mode) ? SYMTYPE : REGTYPE;
  // link name already set
  strncpy(hdr->magic, TMAGIC, sizeof(hdr->magic));
  strncpy(hdr->version, TVERSION, sizeof(hdr->version));
  snprintf(hdr->uname, sizeof(hdr->uname), "%s", pwd->pw_name);
  snprintf(hdr->gname, sizeof(hdr->gname), "%s", grp->gr_name);
  snprintf(hdr->devmajor, sizeof(hdr->devmajor), "%0*o",
           (int)sizeof(hdr->devmajor)-1, 0);
  snprintf(hdr->devminor, sizeof(hdr->devminor), "%0*o",
           (int)sizeof(hdr->devminor)-1, 0);
  if(strlen(filename) < sizeof(hdr->name))
  {
    strcpy(hdr->prefix, "");
    strncpy(hdr->name, filename, sizeof(hdr->name));
  }
  else
  {
    const char *p = strchr(filename+strlen(filename)-sizeof(hdr->name)+1, '/');
    if(p == NULL)
    {
      fprintf(stderr, "filename %s too long. Last part must be shorter than %zu characters\n",
              filename, sizeof(hdr->name)-1);
      exit(1);
    }
    if((size_t)(p-filename) > sizeof(hdr->prefix))
    {
      fprintf(stderr, "filename %s too long. First part must be shorter than %zu characters\n",
              filename, sizeof(hdr->prefix)-1);
      exit(1);
    }
    snprintf(hdr->prefix, sizeof(hdr->prefix), "%.*s", (int)(p-filename),
             filename);
    snprintf(hdr->name, sizeof(hdr->name), "%s", p+1);
  }

  checksum = 0;
  for(size_t j = 0 ; j < BLOCKSIZE ; j++)
    checksum += reinterpret_cast<char*>(hdr)[j];
  snprintf(hdr->chksum, sizeof(hdr->chksum), "%0*lo",
           (int)sizeof(hdr->chksum)-1, checksum);

  return hdr->typeflag;
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

    packet->data.resize(BLOCKSIZE);
    const char typeflag = make_tarheader(fn, reinterpret_cast<posix_header*>(&packet->data[0]));
    memcpy(packet->type, TYPE_STAT, sizeof(packet->type));
    packet->size = BLOCKSIZE;
    master_port.push_packet(packet);
    packet = myport->pull_packet();

    if(typeflag == REGTYPE) {
      FILE* fh = fopen(fn.c_str(), "rb");
      if(fh == NULL) {
        std::cerr << "Could not open file " << fn << ": " << strerror(errno)
                  << std::endl;
        exit(1);
      }

      packet->data.resize(CHUNK_SIZE);
      memcpy(packet->type, TYPE_DATA, sizeof(packet->type));

      do {
        const size_t sz_read =
          fread(&packet->data[0], 1, packet->data.size(), fh);
        if(sz_read > 0) { // this avoid writing two termination packets
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
      } while(!feof(fh) && !ferror(fh));
      if(ferror(fh)) {
        std::cerr << "Could not read from file " << fn << ": "
                  << strerror(errno) << std::endl;
        exit(1);
      }

      fclose(fh);

      // terminate file
      packet->size = 0;
      master_port.push_packet(packet);
      packet = myport->pull_packet();
    }

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
    } else if(strncmp(packet->type, TYPE_DATA, sizeof(packet->type)) == 0 ||
              strncmp(packet->type, TYPE_STAT, sizeof(packet->type)) == 0) {
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
    if(sz != buf.size() || ferror(stdin)) {
      std::cerr << "failed to read " << buf.size()
                << " bytes from stdin (only " << sz << " read): "
                << strerror(errno) << std::endl;
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

      // we never erase the filename to detect corrupt files
      if(filenames.count(fid)) {
        std::cerr << "corrupt input, id " << fid << " for file " << fn
                  << " not unique" << std::endl;
        exit(1);
      }
      filenames[fid] = fn;
    } else if(strncmp(ser_packet.type, TYPE_STAT,
                      sizeof(ser_packet.type)) == 0) {
      // a tar header packet, act on type
      const posix_header* hdr = reinterpret_cast<posix_header*>(&buf[0]);
      if(hdr->typeflag == SYMTYPE) {
        const std::string fn = filenames[fid];
        std::clog << "creating file " << fn << std::endl;
        const int ierr = symlink(hdr->linkname, fn.c_str());
        if(ierr) {
          std::cerr << "failed to create symbolic link '" << fn << "' to target '"
                    << hdr->linkname << ": " << strerror(errno) << std::endl;
          exit(1);
        }
        std::clog << "finished file " << fn << std::endl;
      } else if(hdr->typeflag == REGTYPE) {
        const std::string fn = filenames[fid];
        FILE* fh = fopen(fn.c_str(), "wb");
        if(fh == NULL) {
          std::cerr << "failed to open '" << fn << "' for writing: "
                    << strerror(errno) << std::endl;
          exit(1);
        }
        if(filehandles.count(fid)) {
          std::cerr << "corrupt input, id " << fid << " for file " << fn
                    << " not unique" << std::endl;
          exit(1);
        }
        std::clog << "creating file " << fn << std::endl;
        filehandles[fid] = fh;
      } else {
        std::cerr << "unknown type flag '" << hdr->typeflag << "'"
                  << std::endl;
        exit(1);
      }
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

// extracts the data packets from a stream from stdin and creates a tar file
void maketar()
{
  serialized_packet_t ser_packet;
  std::vector<char> buf;
  std::map<std::string, size_t> fileoffsets;
  std::map<std::string, std::string> filenames;
  size_t sz_tarfile = 0;

  // TODO: fix this
  FILE* fh = fopen("test.tar", "wb");

  while(fread(&ser_packet, sizeof(ser_packet), 1, stdin)) {
    // parse packet header into variables
    std::string fid(ser_packet.fid, sizeof(ser_packet.fid));
    char sizebuf[sizeof(ser_packet.size)+1];
    memcpy(sizebuf, ser_packet.size, sizeof(ser_packet.size));
    sizebuf[sizeof(ser_packet.size)] = '\0';

    buf.resize(atoi(sizebuf));
    const size_t sz = fread(&buf[0], 1, buf.size(), stdin);
    if(sz != buf.size() || ferror(stdin)) {
      const std::string fn = filenames[fid];
      std::cerr << "failed to read " << buf.size()
                << " bytes from stdin (only " << sz << " read): "
                << strerror(errno) << std::endl;
      exit(1);
    }

    // TODO: Remove FILE packet from streams since the STAT packet can be used
    // as well
    if(strncmp(ser_packet.type, TYPE_FILE, sizeof(ser_packet.type)) == 0) {
      // new file record its file-id
      std::string fn(&buf[0], buf.size());

      // we never erase the filename to detect corrupt files
      if(filenames.count(fid)) {
        std::cerr << "corrupt input, id " << fid << " for file " << fn
                  << " not unique" << std::endl;
        exit(1);
      }
      filenames[fid] = fn;
    } else if(strncmp(ser_packet.type, TYPE_STAT,
                      sizeof(ser_packet.type)) == 0) {
      // a tar header packet, act on type
      const posix_header* hdr = reinterpret_cast<posix_header*>(&buf[0]);
      assert(buf.size() == BLOCKSIZE);
      if(hdr->typeflag == SYMTYPE) {
        const int ierr_fseek = fseek(fh, sz_tarfile, SEEK_SET);
        if(ierr_fseek) {
          std::cerr << "failed to seek to position " << sz_tarfile << ": "
                    << strerror(errno) << std::endl;
          exit(1);
        }
        const size_t written = fwrite(&buf[0], 1, buf.size(), fh);
        if(written != buf.size()) {
          std::cerr << "failed to write to " << filenames[fid]
                    << strerror(errno) << std::endl;
          exit(1);
        }
        sz_tarfile += buf.size();
      } else if(hdr->typeflag == REGTYPE) {
        const int ierr_fseek = fseek(fh, sz_tarfile, SEEK_SET);
        if(ierr_fseek) {
          std::cerr << "failed to seek to position " << sz_tarfile << ": "
                    << strerror(errno) << std::endl;
          exit(1);
        }
        const size_t written = fwrite(&buf[0], 1, buf.size(), fh);
        if(written != buf.size()) {
          std::cerr << "failed to write to " << filenames[fid] << ": "
                    << strerror(errno) << std::endl;
          exit(1);
        }
        sz_tarfile += buf.size();

        fileoffsets[fid] = sz_tarfile;
        sz_tarfile += round_to_block(strtol(hdr->size, NULL, 8));
      } else {
        std::cerr << "unknown type flag '" << hdr->typeflag << "'"
                  << std::endl;
        exit(1);
      }
    } else if(strncmp(ser_packet.type, TYPE_DATA,
                      sizeof(ser_packet.type)) == 0) {
      // a data packet, write to the correct location in tar file
      if(fileoffsets.count(fid) == 0) {
        std::cerr << "corrupt input, unknown id " << fid << std::endl;
        exit(1);
      }
      size_t offset = fileoffsets[fid];
      const int ierr_fseek = fseek(fh, offset, SEEK_SET);
      if(ierr_fseek) {
        std::cerr << "failed to seek to position " << sz_tarfile << ": "
                  << strerror(errno) << std::endl;
        exit(1);
      }
      const size_t written = fwrite(&buf[0], 1, buf.size(), fh);
      if(written != buf.size()) {
        std::cerr << "failed to write to " << filenames[fid]
                  << strerror(errno) << std::endl;
        exit(1);
      }
      fileoffsets[fid] += written;
      if(written == 0) { // EOF marker
        // we may write less than a multiple of BLOCKSIZE bytes and rely on the
        // OS to present the holes in the output file as null bytes
        fileoffsets.erase(fid);
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

  // write tar termination blocks
  buf.resize(2*BLOCKSIZE);
  memset(&buf[0], 0, buf.size());
  // TODO: add error checks
  const int ierr_fseek = fseek(fh, sz_tarfile, SEEK_SET);
  if(ierr_fseek) {
    std::cerr << "failed to seek to position " << sz_tarfile
              << strerror(errno) << std::endl;
    exit(1);
  }
  fwrite(&buf[0], 1, buf.size(), fh);
  fclose(fh);
}

int main(int argc, char **argv)
{
  // TODO: do proper cmdline parsing using getopt or commandline.c
  if(argc != 2 || (strcmp(argv[1], "-create") &&
                   strcmp(argv[1], "-extract") &&
                   strcmp(argv[1], "-tar"))) {
    std::cerr << "usage: " << argv[0] << " [-create|-extract|-tar]" << std::endl;
    exit(1);
  }

  if(strcmp(argv[1], "-create") == 0)
    sender();
  else if(strcmp(argv[1], "-extract") == 0)
    receiver();
  else if(strcmp(argv[1], "-tar") == 0)
    maketar();
  else
    exit(1);

  return 0;
}
