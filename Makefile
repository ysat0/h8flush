CC = gcc
CFLAGS = -Wall -O2 -g
OBJS = main.o comm.o
TARGET= h8flash

all: h8flash

$(TARGET): main.o comm.o
	$(CC) -o $(TARGET) $(OBJS)

main.o: main.c h8flash.h

comm.o: comm.c h8flash.h

.phony: clean

clean:
	rm -f *.o *~ $(TARGET)
