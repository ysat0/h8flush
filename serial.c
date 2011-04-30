/*
 *  Renesas CPU On-chip Flash memory writer
 *  serial I/O
 *
 * Yoshinori Sato <ysato@users.sourceforge.jp>
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License version 2.1 (or later).
 */

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <fcntl.h>
#include <stdlib.h>
#include <libgen.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include "h8flash.h"

#define TRY1COUNT 60
#define BAUD_ADJUST_LEN 30

static int ser_fd;
static int lock_fd;
static char lockname[FILENAME_MAX];

/* send byte stream */
static int send_data(const unsigned char *buf, int len)
{
	return write(ser_fd, buf, len);
}

/* receive 1byte */
static int receive_byte(unsigned char *data)
{
	int r;
	struct timeval tv;
	fd_set fdset;

	*data = 0;
	tv.tv_sec = 60;
	tv.tv_usec = 0;
	FD_ZERO(&fdset);
	FD_SET(ser_fd, &fdset);
	r = select(ser_fd + 1, &fdset, NULL, NULL, &tv);
	if (r == -1)
		return -1;
	return read(ser_fd, data, 1);
}

/* set host bitrate */
static int setbaud(int bitrate)
{
	int b;
	struct termios serattr;

	b = 0;
	switch (bitrate) {
	case 96:   b = B9600;  break;
	case 192:  b = B19200; break;
	case 384:  b = B38400; break;
	case 576:  b = B57600; break;
	case 1152: b = B115200; break;
	}
	if (b == 0)
		return 0;

	tcgetattr(ser_fd, &serattr);
	cfsetospeed(&serattr, b);
	cfsetispeed(&serattr, b);
	tcsetattr(ser_fd, TCSANOW, &serattr);
	return 1;
}

/* connect to target CPU */
static int connect_target(void)
{
	int try1;
	int r;
	struct timeval tv;
	fd_set fdset;
	unsigned char buf[BAUD_ADJUST_LEN];

	/* wait connection establish  */
	for(try1 = 0; try1 < TRY1COUNT; try1++) {
		memset(buf, 0x00, BAUD_ADJUST_LEN);
		/* send dummy data */
		write(ser_fd, buf, BAUD_ADJUST_LEN);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		FD_ZERO(&fdset);
		FD_SET(ser_fd, &fdset);
		/* wait reply */
		r = select(ser_fd + 1, &fdset, NULL, NULL, &tv);
		if (r == -1)
			return 0;
		if ((r > 0) && (read(ser_fd, buf, 1) == 1) && buf[0] == 0)
			goto connect;
		if (try1 == 0) {
			printf("now connection"); 
			fflush(stdout);
		} else {
			putchar('.');
			fflush(stdout);
		}
	}
	putchar('\n');
	return 0;
 connect:
	/* connect done */
	buf[0] = 0x55;
	write(ser_fd, buf, 1);
	if ((receive_byte(buf) == 1) && (buf[0] == 0xe6))
		return 1; /* ok */
	else
		return 0; /* ng */
}

void port_close(void)
{
	close(ser_fd);
	close(lock_fd);
	unlink(lockname);
}

static struct port_t serial_port = {
	.type = serial,
	.send_data = send_data,
	.receive_byte = receive_byte,
	.connect_target = connect_target,
	.setbaud = setbaud,
	.close = port_close,
};

static int serial_lock(const char *lock)
{
	struct stat s;
	int fd;
	pid_t pid;
	char buf[128];

	if (stat(lock, &s) == 0) {
		fd = open(lock, O_RDWR);
		if (fd == -1)
			return -1;
		read(fd, buf, sizeof(buf));
		pid = atoi(buf);
		if (pid > 0) {
			if (kill(pid, 0) != -1 || errno != ESRCH)
				return -1;
			else {
				lseek(fd, 0, SEEK_SET);
				ftruncate(fd, 0);
			}
		}
	} else {
		fd = creat(lock, 0666);
		if (fd == -1)
			return -1;
	}
	pid = getpid();
	sprintf(buf,"%8d",pid);
	if (write(fd, buf, 8) != 8)
		return -1;
	return fd;
}

/* host serial open */
struct port_t *open_serial(char *ser_port)
{
	struct termios serattr;

	snprintf(lockname, sizeof(lockname), LOCKDIR "/LCK..%s", basename(ser_port));
	lock_fd = serial_lock(lockname);
	if (lock_fd == -1) {
		fputs(PROGNAME ": Serial port lock failed.\n",stderr);
		return NULL;
	}

	ser_fd = open(ser_port, O_RDWR);
	if (ser_fd == -1) {
		perror("PROGNAME: ");
		return NULL;
	}

	tcgetattr(ser_fd, &serattr);
	serattr.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON | IXOFF);
	serattr.c_oflag &= ~OPOST;
	serattr.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	serattr.c_cflag &= ~(CSIZE | PARENB);
	serattr.c_cflag |= CS8 | CLOCAL;
	serattr.c_cc[VMIN] = 1;
	serattr.c_cc[VTIME] = 0;
	cfsetospeed(&serattr, B9600);
	cfsetispeed(&serattr, B9600);
	tcsetattr(ser_fd, TCSANOW, &serattr);

	return &serial_port;
}

