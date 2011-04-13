CFLAGS = -g -std=gnu99 -Wformat -Wall -Wswitch-default -Wswitch-enum -Wextra -Wshadow -Wwrite-strings -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wmissing-field-initializers -Wno-unused-parameter
CXXFLAGS = -g -std=gnu++98 -Wformat -Wall -Wswitch-default -Wswitch-enum -Wextra -Wshadow -Wwrite-strings -Wmissing-field-initializers -Wno-unused-parameter
HDF5_DIR=/nethome/numrel
#HDF5_DIR=/usr/local/packages/hdf5-1.8.4-patch1 # Cygnus

all: hdf5_merge hdf5_split_iterations
	echo "All done"

hdf5_merge: hdf5_merge.c
	gcc -O3 $(CFLAGS) -L${HDF5_DIR}/lib -I${HDF5_DIR}/include -lhdf5 -o hdf5_merge hdf5_merge.c

hdf5_split_iterations: hdf5_split_iterations.cc
	g++ -O3 $(CXXFLAGS) -L${HDF5_DIR}/lib -I${HDF5_DIR}/include -lhdf5 -o hdf5_split_iterations hdf5_split_iterations.cc
