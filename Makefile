KBUILD_EXTRA_SYMBOLS += /lib/modules/$(shell uname -r)/build/Module.symvers
export KBUILD_EXTRA_SYMBOLS
ccflags-y := -Wunused -O0 -mpreferred-stack-boundary=4 -mavx2 #-ftree-vectorize

obj-m += ioat_test.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
