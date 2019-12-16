#Makefile                          

#CROSS_COMPILER=mips-linux-uclibc-gnu-
CC = $(CROSS_COMPILER)gcc
CPP = $(CROSS_COMPILER)g++

all: threadMain 

INCLUDES = -I.
LDFLAGS = -lpthread
CFLAGS = $(INCLUDES) -g -Wall -DHIGH_BIT_DEPTH=0 -DX265_DEPTH=8 -DX265_NS=x265

OBJS = threadmain.o \
	   threadpool.o \
	   threading.o \
	   constants.o \
	   common.o

$(OBJS):%.o:%.cpp
	$(CPP) -c $(CFLAGS) -o $@ $<

threadMain: $(OBJS)
	$(CPP) -o $@ $(OBJS) $(LDFLAGS)

.PHONY: clean distclean

clean:
	-rm -f $(OBJS)

distclean: clean
	-rm -f threadMain
