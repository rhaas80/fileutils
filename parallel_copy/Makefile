CXXFLAGS = -g -std=gnu++98 -Wformat -Wall -Wswitch-default -Wswitch-enum -Wextra -Wshadow -Wwrite-strings -Wmissing-field-initializers -Wno-unused-parameter

all: parcp
	echo "All done"

%: %.cc
	g++ -O3 $(CXXFLAGS) -lpthread -o $@ $<
