all: transfer

OBJS:=transfer.o send.o recv.o socket.o pipe.o

%.o: %.c Makefile
	gcc -std=gnu99 -g -O3 -c $< -o $@

transfer: Makefile $(OBJS)
	gcc -g -o $@ $(OBJS)

.PONY: clean all

clean:
	rm -rf transfer $(OBJS)
