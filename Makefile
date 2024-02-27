.PHONY: nbdkit

all: nbd

nbdkit:
	(cd nbdkit; autoreconf -i; ./configure; make -j)

nbd:
	g++ -fPIC -shared -I nbdkit/include -DFTL_DEBUG=1 -g -o0 -o nbdftl.so nbdftl.cpp 

nbdserver:
	nbdkit/nbdkit -fv ./nbdftl.so

nbdtest:
	sudo nbd-client localhost /dev/nbd9
	sudo fio nbd.fio
	sudo nbd-client -d /dev/nbd9

clean:
	rm -f nbdftl.so lba.bin flash.bin
