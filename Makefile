CC = gcc
CFLAGS = -Wall -O2 -g
OBJS = main.o comm.o serial.o usb.o
TARGET= h8flash

all: h8flash

$(TARGET): $(OBJS)
	$(CC) -o $(TARGET) $(OBJS) -lusb

main.o: main.c h8flash.h

comm.o: comm.c h8flash.h

serial.o: serial.c h8flash.h

usb.o: usb.c

.phony: clean

clean:
	rm -f *.o *~ $(TARGET)
