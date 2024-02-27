.PHONY: nbdkit valgrind

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

valgrind:
	g++ -g -o0 -o valgrindtest valgrindtest.cpp
	valgrind  --leak-check=full --track-origins=yes --error-limit=no --show-leak-kinds=all --error-exitcode=999 --tool=memcheck ./valgrindtest 666

statictest:
	g++ -g -o0 -o staticwearleveltest staticwearleveltest.cpp
	./staticwearleveltest
