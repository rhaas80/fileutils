#include <map>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>

#include <hdf5.h>

using namespace std;

/*****************************************************************************/
/*                           macro definitions                               */
/*****************************************************************************/
/* macro to do an HDF5 call, check its return code, and print a warning
   in case of an error */
#define CHECK_ERROR(hdf5_call)                                                \
          do                                                                  \
          {                                                                   \
            int _error_code = hdf5_call;                                      \
                                                                              \
                                                                              \
            if (_error_code < 0)                                              \
            {                                                                 \
              fprintf (stderr, "WARNING: line %d: HDF5 call '%s' returned "   \
                               "error code %d\n",                             \
                                __LINE__, #hdf5_call, _error_code);           \
              nerrors++;                                                      \
            }                                                                 \
          } while (0)

#define GLOBAL_PARAMETERS_AND_ATTRIBUTES_GROUP "Parameters and Global Attributes"
#define CCTK_INT int

/*****************************************************************************/
/*                           global variables                                */
/*****************************************************************************/
struct calldata_t
{
  const char *basename;
  const char *infile;
};
typedef map<CCTK_INT, hid_t> outfiles_t;

static outfiles_t outfiles;
static int nerrors;
static int verbose;

/*****************************************************************************/
/*                           local function prototypes                       */
/*****************************************************************************/
static herr_t LinkObject (hid_t copy_from, const char *objectname, void *arg);
static void usage(char *argv[]);

 /*@@
   @routine    main
   @date       Sat 27 Mar 2010
   @author     Roland Haas
   @desc
               Main routine of the HDF5 file splitter
   @enddesc

   @calls      LinkObject

   @var        argc
   @vdesc      number of command line arguments
   @vtype      int
   @vio        in
   @endvar
   @var        argv
   @vdesc      command line arguments
   @vtype      char *[]
   @vio        in
   @endvar

   @returntype int
   @returndesc
               0 for success, negative return values indicate an error
   @endreturndesc
@@*/
int main(int argc, char **argv)
{
  const char *fn;
  const char *basename;
  hid_t infile;
  calldata_t calldata;

  /* parse options */
  for (int i = 1 ; i < argc ; i++)
  {
    int opt;

    /* check if this option is valid */
    if(strlen(argv[i]) != 2 || *argv[i] != '-') /* not an option after all */
      continue;

    /* remove option from list of arguments */
    opt = argv[i][1];
    for(int j = i+1 ; j < argc ; j++)
      argv[j-1] = argv[j];
    argc -= 1;
    i -= 1; /* re-parse the current option */

    /* parse option */
    if(opt == 'v') {
      verbose += 1;
    } else if(opt == 'h') {
      usage(argv);
      return (0);
      break;
    } else if(opt == '-') {
      break;
    } else {
      fprintf(stderr, "unknown option '-%c'.", opt);
      usage(argv);
      return (1);
    }
  }

  /* give some help if called with incorrect number of parameters */
  if (argc < 3)
  {
    usage(argv);
    return (1);
  }

  // loop over input files found
  basename = argv[argc-1];
  for(int i = 1 ; i < argc-1 && nerrors == 0 ; i++)
  {
    fn = argv[i];
    CHECK_ERROR (infile = H5Fopen (fn, H5F_ACC_RDONLY, H5P_DEFAULT));
    if (infile >= 0)
    {
      if(verbose >= 1)
        fprintf (stdout, "processing file '%s'\n", fn);
      calldata.basename = basename;
      calldata.infile = fn;
      CHECK_ERROR (H5Giterate (infile, "/", NULL, LinkObject, &calldata));
    }
    else
    {
      fprintf (stderr, "ERROR: Cannot open HDF5 input file '%s' !\n\n",
               fn);
      exit (1);
    }
    CHECK_ERROR (H5Fclose (infile));
  }

  return nerrors != 0;
}

/*****************************************************************************/
/*                           local routines                                  */
/*****************************************************************************/

 /*@@
   @routine    usage
   @date       Wed Oct 28 12:03:18 EDT 2009
   @author     Roland Haas
   @desc
               Print usage information of the HDF5 file merger
   @enddesc

   @calls      fprintf

   @var        argv
   @vdesc      command line arguments
   @vtype      char *[]
   @vio        in
   @endvar

   @returntype void
@@*/
void usage(char *argv[])
{
    fprintf (stderr, "Usage: %s [-v] [-h] <infile1> [<infile2> ...] <basename>\n",argv[0]);
    fprintf (stderr, "       -h : this message\n");
    fprintf (stderr, "       -v : output each file name as it is processed,\n"
                     "            twice outputs datasets as well\n");
}

 /*@@
   @routine    LinkObject
   @date       Sat 27 Mar 2010
   @author     Roland Haas
   @desc
               Iterator recursively called by H5Giterate() for every object
               in the input file
               It links the current object to the correct output file based 
               on its iteration.
   @enddesc

   @var        from
   @vdesc      identifier for the group the current object belongs to
   @vtype      hid_t
   @vio        in
   @endvar
   @var        objectname
   @vdesc      name of the current object
   @vtype      const char *
   @vio        in
   @endvar
   @var        calldata
   @vdesc      user-supplied argument, printf base name of create files
   @vio        in
   @endvar

   @returntype int
   @returndesc
               0 - continue the iteration for following group objects
               1 - short-curcuit, no further iteration of this group
   @endreturndesc
@@*/
static herr_t LinkObject (hid_t group,
                          const char *objectname,
                          void * calldata_arg)
{
  hid_t dataset, attr;
  CCTK_INT iteration;
  H5G_stat_t object_info;
  hid_t outfile;
  calldata_t *calldata = (calldata_t *)calldata_arg;
  char buf[1024];
  size_t len_written;
  const char *basename = calldata->basename;
  const char *infile = calldata->infile;

  // we are interested only in datasets
  CHECK_ERROR (H5Gget_objinfo (group, objectname, 0, &object_info));
  if (object_info.type == H5G_DATASET) // skip parameters group
  {
    CHECK_ERROR (dataset = H5Dopen (group, objectname, H5P_DEFAULT));
  
    CHECK_ERROR (attr = H5Aopen_name (dataset, "timestep"));
    CHECK_ERROR (H5Aread (attr, H5T_NATIVE_INT, &iteration));
    CHECK_ERROR (H5Aclose (attr));

    CHECK_ERROR (H5Dclose (dataset));

    outfiles_t::const_iterator outfile_it = outfiles.lower_bound(iteration);
    if(outfile_it != outfiles.end() && outfile_it->first == iteration)
    {
      outfile = outfile_it->second;
    }
    else
    {
      len_written = snprintf(buf, sizeof(buf), basename, iteration);
      assert(len_written < sizeof(buf));
      
      CHECK_ERROR (outfile = H5Fcreate (buf, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));

      // link in parameters group
      //CHECK_ERROR (H5Lcreate_external(infile, "/" GLOBAL_PARAMETERS_AND_ATTRIBUTES_GROUP, 
      //                                outfile, GLOBAL_PARAMETERS_AND_ATTRIBUTES_GROUP, 
      //                                H5P_DEFAULT, H5P_DEFAULT));
      CHECK_ERROR (H5Ocopy(group, GLOBAL_PARAMETERS_AND_ATTRIBUTES_GROUP, 
                           outfile, GLOBAL_PARAMETERS_AND_ATTRIBUTES_GROUP, 
                           H5P_DEFAULT, H5P_DEFAULT));

      // store for later use
      outfiles[iteration] = outfile;
    }

    if(verbose >= 2)
      fprintf(stdout, "copying dataset '%s'\n", objectname);

    // link object into proper file
    len_written = snprintf(buf, sizeof(buf), "/%s", objectname);
    assert(len_written < sizeof(buf));
    //CHECK_ERROR (H5Lcreate_external(infile, buf, outfile, objectname,
    //                                H5P_DEFAULT, H5P_DEFAULT));
    CHECK_ERROR (H5Ocopy(group, objectname, outfile, objectname,
                         H5P_DEFAULT, H5P_DEFAULT));
  } // is dataset

  return -nerrors; // negative values signal errors to the iterator
}

