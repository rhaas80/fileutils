#include <sys/types.h>
#include <sys/stat.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

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
                                /* 500 */
};

#define TMAGIC   "ustar"        /* ustar and a null */
#define TMAGLEN  6
#define TVERSION "00"           /* 00 and no null */
#define TVERSLEN 2

/* Values used in typeflag field.  */
#define REGTYPE  '0'            /* regular file */
#define SYMTYPE  '2'            /* reserved */

#define BLOCKSIZE 512
#define BUFFERSIZE (BLOCKSIZE*100)

#define MAX_FILE_SIZE ((8L<<(3*(sizeof(((struct posix_header*)0)->size)-1)))-1)

union hdr_union
{
  struct posix_header hdr;
  char block[BLOCKSIZE];
};

void write_tarfile(int nfiles, char **filenames)
{
  char buffer[BUFFERSIZE];

  for(int i = 0 ; i < nfiles ; i++)
  {
    const char *filename = filenames[i];
    struct stat statbuf;
    union hdr_union hdr;
    struct group *grp;
    struct passwd *pwd;
    unsigned long int checksum;

    assert(sizeof(hdr) == BLOCKSIZE);

    lstat(filename, &statbuf);
    grp = getgrgid(statbuf.st_gid);
    pwd = getpwuid(statbuf.st_uid);
    if(statbuf.st_size > MAX_FILE_SIZE)
    {
      fprintf(stderr, "size %zd of file %s too big. Maximum size is %lu\n",
              statbuf.st_size, filename, MAX_FILE_SIZE);
      exit(1);
    }

    assert(S_ISLNK(statbuf.st_mode) || S_ISREG(statbuf.st_mode));

    memset(hdr.block, 0, BLOCKSIZE);
    if(S_ISLNK(statbuf.st_mode))
    {
      if((size_t)statbuf.st_size >= sizeof(hdr.hdr.linkname))
      {
        fprintf(stderr, "linked filename %s too long. Last part must be shorter than %zu characters\n",
                hdr.hdr.linkname, sizeof(hdr.hdr.linkname)-1);
        exit(1);
      }
      size_t sz_read =
        readlink(filename, hdr.hdr.linkname, sizeof(hdr.hdr.linkname));
      if(sz_read == (size_t)-1)
      {
        fprintf(stderr, "Could not read link %s: %s\n", filename,
                strerror(errno));
        exit(1);
      }
      hdr.hdr.linkname[statbuf.st_size] = '\0';
      statbuf.st_size = 0; // tar requires zero size for links
    }
    else
    {
      strcpy(hdr.hdr.linkname, "");
    }

    // name is set at the end due to funny handling of long file names
    snprintf(hdr.hdr.mode, sizeof(hdr.hdr.mode), "%0*o",
             (int)sizeof(hdr.hdr.mode)-1, statbuf.st_mode);
    snprintf(hdr.hdr.uid, sizeof(hdr.hdr.uid), "%0*o",
             (int)sizeof(hdr.hdr.uid)-1, statbuf.st_uid);
    snprintf(hdr.hdr.gid, sizeof(hdr.hdr.gid), "%0*o",
             (int)sizeof(hdr.hdr.gid)-1, statbuf.st_gid);
    snprintf(hdr.hdr.size, sizeof(hdr.hdr.size), "%0*lo",
             (int)sizeof(hdr.hdr.size)-1, statbuf.st_size);
    snprintf(hdr.hdr.mtime, sizeof(hdr.hdr.mtime), "%0*lo",
             (int)sizeof(hdr.hdr.mtime)-1, statbuf.st_mtime);
    memset(hdr.hdr.chksum, ' ', sizeof(hdr.hdr.chksum));
    hdr.hdr.typeflag = S_ISLNK(statbuf.st_mode) ? SYMTYPE : REGTYPE;
    // link name already set
    strncpy(hdr.hdr.magic, TMAGIC, sizeof(hdr.hdr.magic));
    strncpy(hdr.hdr.version, TVERSION, sizeof(hdr.hdr.version));
    snprintf(hdr.hdr.uname, sizeof(hdr.hdr.uname), "%s", pwd->pw_name);
    snprintf(hdr.hdr.gname, sizeof(hdr.hdr.gname), "%s", grp->gr_name);
    snprintf(hdr.hdr.devmajor, sizeof(hdr.hdr.devmajor), "%0*o",
             (int)sizeof(hdr.hdr.devmajor)-1, 0);
    snprintf(hdr.hdr.devminor, sizeof(hdr.hdr.devminor), "%0*o",
             (int)sizeof(hdr.hdr.devminor)-1, 0);
    if(strlen(filename) < sizeof(hdr.hdr.name))
    {
      strcpy(hdr.hdr.prefix, "");
      strncpy(hdr.hdr.name, filename, sizeof(hdr.hdr.name));
    }
    else
    {
      const char *p = strchr(filename+strlen(filename)-sizeof(hdr.hdr.name)-1, '/');
      if(p == NULL)
      {
        fprintf(stderr, "filename %s too long. Last part must be shorter than %zu characters\n",
                filename, sizeof(hdr.hdr.name)-1);
        exit(1);
      }
      if((size_t)(p-filename) > sizeof(hdr.hdr.prefix))
      {
        fprintf(stderr, "filename %s too long. First part must be shorter than %zu characters\n",
                filename, sizeof(hdr.hdr.prefix)-1);
        exit(1);
      }
      snprintf(hdr.hdr.prefix, sizeof(hdr.hdr.prefix), "%.*s", (int)(p-filename),
               filename);
      snprintf(hdr.hdr.name, sizeof(hdr.hdr.name), "%s", p+1);
    }

    checksum = 0;
    for(size_t j = 0 ; j < sizeof(hdr.block) ; j++)
      checksum += hdr.block[j];
    snprintf(hdr.hdr.chksum, sizeof(hdr.hdr.chksum), "%0*lo",
             (int)sizeof(hdr.hdr.chksum)-1, checksum);

    fwrite(&hdr.block, BLOCKSIZE, 1, stdout);

    if(S_ISREG(statbuf.st_mode))
    {
      FILE * fh = fopen(filename, "rb");
      if(fh == NULL)
      {
        fprintf(stderr, "Could not open %s for reading: %s\n", filename,
                strerror(errno));
        exit(1);
      }
      while(!feof(fh))
      {
        size_t sz_read = fread(&buffer, 1, sizeof(buffer), fh);
        if(ferror(fh))
        {
          fprintf(stderr, "Error reading from %s: %s\n", filename,
                  strerror(errno));
          exit(1);
        }
        size_t to_write = (sz_read + BLOCKSIZE-1) & ~(BLOCKSIZE-1);
        memset(buffer+sz_read, 0, to_write-sz_read);
        size_t sz_written = fwrite(buffer, 1, to_write, stdout);
        if(sz_written != to_write)
        {
          fprintf(stderr, "Error writing: %s\n", strerror(errno));
          exit(1);
        }
      }
      fclose(fh);
    }
  }

  memset(buffer, 0, 2*BLOCKSIZE);
  fwrite(buffer, BLOCKSIZE, 2, stdout);
}

int main(int argc, char **argv)
{
  write_tarfile(argc-1, argv+1);

  return 0;
}
