
all: nbd

nbd:
	g++ -fPIC -shared -I nbdkit/include -DFTL_DEBUG=1 -g -o0 -o nbdftl.so nbdftl.cpp 

nbdserver:
	nbdkit/nbdkit -fv ./nbdftl.so

clean:
	rm -f nbdftl.so lba.bin flash.bin
