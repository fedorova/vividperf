#PERF_INCLUDE=/usr/src/linux-lts-raring-3.8.0/tools
#LINUX_INCLUDE=/usr/src/linux-headers-3.8.0-38/include
#ARCH_INCLUDE=/usr/src/linux-lts-raring-3.8.0/arch/x86/include

#CPPFLAGS = 	-I ${LINUX_INCLUDE} 


all:
	cc -o perf-manicured -g perf-manicured.c list.c
	cc -o test -g test.c -lrt

