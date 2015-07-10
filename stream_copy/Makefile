all: send recv

send: send.o
recv: recv.o

%.o: %.c Makefile
	gcc -std=gnu99 -g -O3 -c $< -o $@

%: %.o Makefile
	gcc -g -o $@ $<

.PONY: clean all

clean:
	rm -rf send send.o recv recv.o
