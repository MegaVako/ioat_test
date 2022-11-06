dmesg -C
dmesg -D
make -j8
dmesg -E
insmod ioat_test.ko $*
dmesg -D
make clean
dmesg -E
rmmod ioat_test 
dmesg
