CFLAGS = -g -std=gnu99 -Wformat -Wall -Wswitch-default -Wswitch-enum -Wextra -Wshadow -Wwrite-strings -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wmissing-field-initializers -Wno-unused-parameter
CXXFLAGS = -g -std=gnu++98 -Wformat -Wall -Wswitch-default -Wswitch-enum -Wextra -Wshadow -Wwrite-strings -Wmissing-field-initializers -Wno-unused-parameter

# set these to suit your system
HDF5_DIR=$(HOME)

# only needed if you use the parallel HDF5 version (not likely)
MPIOPTS=-L/usr/lib/openmpi/lib -I/usr/lib/openmpi/include -lmpi

all: hdf5_merge hdf5_split_iterations hdf5_extract
	echo "All done"

%: %.c
	gcc -O3 $(CFLAGS) $(MPIOPTS) -L$(HDF5_DIR)/lib -I$(HDF5_DIR)/include -o $@ $< -lm -lz -lhdf5
