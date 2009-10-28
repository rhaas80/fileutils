all: hdf5_merge
	echo "All done"

hdf5_merge: hdf5_merge.c
	gcc -O3 -L/nethome/numrel/lib -lhdf5 -o hdf5_merge hdf5_merge.c
