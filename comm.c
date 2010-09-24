/*
 *  Renesas CPU On-chip Flash memory writer
 *  target communication
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
#include "h8flash.h"

#define TRY1COUNT 60
#define BAUD_ADJUST_LEN 30

#define ACK                  0x06

#define QUERY_DEVICE         0x20
#define QUERY_DEVICE_RES     0x30
#define QUERY_CLOCKMODE      0x21
#define QUERY_CLOCKMODE_RES  0x31
#define QUERY_MULTIRATE      0x22
#define QUERY_MULTIRATE_RES  0x32
#define QUERY_FREQ           0x23
#define QUERY_FREQ_RES       0x33
#define QUERY_BOOT_AREA      0x24
#define QUERY_BOOT_AREA_RES  0x34
#define QUERY_USER_AREA      0x25
#define QUERY_USER_AREA_RES  0x35
#define QUERY_WRITESIZE      0x27
#define QUERY_WRITESIZE_RES  0x37

#define SELECT_DEVICE        0x10
#define SET_CLOCKMODE        0x11

#define SET_BITRATE          0x3f

#define WRITEMODE            0x40
#define WRITE_USERBOOT       0x42
#define WRITE_USER           0x43
#define BLANKCHECK_USERBOOT  0x4c
#define BLANKCHECK_USER      0x4d
#define WRITE128             0x50

/* NAK answer list */
const unsigned char naktable[] = {0x80, 0x90, 0x91, 0xbf, 0xc0, 0xc2, 0xc3, 0xc8,
				  0xcc, 0xcd, 0xd0, 0xd2, 0xd8};

/* send multibyte command */
static void send(int ser_fd, char *data, int len)
{
	unsigned char sum;
	write(ser_fd, data, len);
	for(sum = 0; len > 0; len--, data++)
		sum += *data;
	sum = 0x100 - sum;
	write(ser_fd, &sum, 1);
}

/* receive 1byte */
static int receive_byte(int ser_fd, char *data)
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

/* receive answer */
static int receive(int ser_fd, char *data)
{
	int len;
	unsigned char *rxptr;
	unsigned char sum;

	rxptr = data;
	if (receive_byte(ser_fd, rxptr) != 1)
		return -1;
	rxptr++;
	/* ACK */
	if (*data == ACK) {
		return 1;
	}
	/* NAK */
	if (memchr(naktable, *data, sizeof(naktable))) {
		if (receive_byte(ser_fd, rxptr) != 1)
			return -1;
		else
			return 2;
	}

	/* multibyte response */
	if (receive_byte(ser_fd, rxptr) != 1)
		return -1;
	rxptr++;
	len = *(data + 1) + 1;
	for(; len > 0; len--) {
		if (receive_byte(ser_fd, rxptr) != 1)
			return -1;
		rxptr++;
	}

	/* 0 byte body */
	if (*(data + 1) == 0)
		return 0;

	/* sum check */
	for (sum = 0, rxptr = data, len = *(data + 1) + 3; len > 0; len--, rxptr++)
		sum += *rxptr;
	if (sum != 0)
		return -1;
	return *(data + 1);
}

/* big endian to cpu endian convert 32bit */
static __inline__ int getlong(unsigned char *p)
{
	return (*p << 24) | (*(p+1) << 16) | (*(p+2) << 8) | *(p+3);
}
	
/* big endian to cpu endian convert 16bit */
static __inline__ short getword(unsigned char *p)
{
	return (*p << 8) | *(p+1);
}

/* cpu endian to big endian 32bit */
static __inline__ void setlong(unsigned char *buf, unsigned long val)
{
	*(buf + 0) = (val >> 24) & 0xff;
	*(buf + 1) = (val >> 16) & 0xff;
	*(buf + 2) = (val >>  8) & 0xff;
	*(buf + 3) = (val      ) & 0xff;
}

/* set host bitrate */
static int setbaud(int ser_fd, int bitrate)
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

/* host serial open */
int open_serial(const char *ser_port)
{
	int ser_fd;
	struct termios serattr;

	ser_fd = open(ser_port, O_RDWR);
	if (ser_fd == -1) {
		perror("PROGNAME: ");
		return -1;
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

	return ser_fd;
}

/* connection target CPU */
int connect_target(int ser_fd)
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
	if ((receive_byte(ser_fd, buf) == 1) && (buf[0] == 0xe6))
		return 1; /* ok */
	else
		return 0; /* ng */
}

/* get target device list */
struct devicelist_t *get_devicelist(int ser_fd)
{
	unsigned char rxbuf[255+3];
	unsigned char *devp;
	struct devicelist_t *devlist;
	int devno;

	rxbuf[0] = QUERY_DEVICE;
	write(ser_fd, rxbuf, 1);
	if (receive(ser_fd, rxbuf) == -1)
		return NULL;
	if (rxbuf[0] != QUERY_DEVICE_RES)
		return NULL;

	devno = rxbuf[2];
	devlist = (struct devicelist_t *)malloc(sizeof(struct devicelist_t) + 
                                                sizeof(struct devinfo_t) * devno);
	if (devlist == NULL)
		return NULL;

	devlist->numdevs = devno;
	devp = &rxbuf[3];
	for(devno = 0; devno < devlist->numdevs; devno++) {
		memcpy(devlist->devs[devno].code, devp + 1, 4);
		memcpy(devlist->devs[devno].name, devp + 5, *devp-4);
		devlist->devs[devno].name[*devp - 4] = 0;
		devp += 1 + 4 + *devp;
	}
	return devlist;
}

/* set target device ID */
int select_device(int ser_fd, const char *code)
{
	char buf[6] = {SELECT_DEVICE, 0x04, 0x00, 0x00, 0x00, 0x00};
	
	memcpy(&buf[2], code, 4);
	send(ser_fd, buf, sizeof(buf));
	if (receive(ser_fd, buf) != 1)
		return 0;
	else
		return 1;
}

/* get target clock mode */
struct clockmode_t *get_clockmode(int ser_fd)
{
	unsigned char rxbuf[255+3];
	unsigned char *clkmdp;
	struct clockmode_t *clocks;
	int numclock;

	rxbuf[0] = QUERY_CLOCKMODE;
	write(ser_fd, rxbuf, 1);
	if (receive(ser_fd, rxbuf) == -1)
		return NULL;
	if (rxbuf[0] != QUERY_CLOCKMODE_RES)
		return NULL;

	numclock = rxbuf[2];
	clocks = (struct clockmode_t *)malloc(sizeof(struct clockmode_t) + 
                                              sizeof(int) * numclock);
	if (clocks == NULL)
		return NULL;

	clocks->nummode = numclock;
	clkmdp = &rxbuf[3];
	for(numclock = 0; numclock < clocks->nummode; numclock++)
		clocks->mode[numclock] = *clkmdp++;
	return clocks;
}

/* set target clock mode */
int set_clockmode(int ser_fd, int mode)
{
	char buf[3] = {SET_CLOCKMODE, 0x01, 0x00};
	
	buf[2] =  mode;
	send(ser_fd, buf, sizeof(buf));
	if (receive(ser_fd, buf) != 1)
		return 0;
	else
		return 1;
}

/* get target multiplier/divider rate */
struct multilist_t *get_multirate(int ser_fd)
{
	unsigned char rxbuf[255+3];
	unsigned char *mulp;
	struct multilist_t *multilist;
	int nummulti;
	int numrate;
	int listsize;
	signed char rate;

	rxbuf[0] = QUERY_MULTIRATE;
	write(ser_fd, rxbuf, 1);
	if (receive(ser_fd, rxbuf) == -1)
		return NULL;
	if (rxbuf[0] != QUERY_MULTIRATE_RES)
		return NULL;

	/* calc multilist size */
	nummulti = rxbuf[2];
	listsize = sizeof(struct multilist_t);
	mulp = &rxbuf[3];
	for (; nummulti > 0; nummulti--) {
		listsize += sizeof(struct multirate_t) + sizeof(int) * (*mulp);
		mulp += *mulp + 1;
	}

	multilist = (struct multilist_t *)malloc(listsize);
	if (multilist == NULL)
		return NULL;

	/* setup multilist */
	multilist->nummulti = rxbuf[2];
	mulp = &rxbuf[3];
	for (nummulti = 0; nummulti < multilist->nummulti; nummulti++) {
		multilist->muls[nummulti].numrate = *mulp++;
		for (numrate = 0; numrate < multilist->muls[nummulti].numrate; numrate++) {
			rate = *mulp++;
			multilist->muls[nummulti].rate[numrate] = rate;
		}
	}			
	return multilist;
}

/* get target operation frequency list */
struct freqlist_t *get_freqlist(int ser_fd)
{
	unsigned char rxbuf[255+3];
	unsigned char *freqp;
	struct freqlist_t *freqlist;
	int numfreq;

	rxbuf[0] = QUERY_FREQ;
	write(ser_fd, rxbuf, 1);
	if (receive(ser_fd, rxbuf) == -1)
		return NULL;
	if (rxbuf[0] != QUERY_FREQ_RES)
		return NULL;

	numfreq = rxbuf[2];
	freqlist = (struct freqlist_t *)malloc(sizeof(struct freqlist_t) + 
                                               sizeof(struct freq_t) * numfreq);
	if (freqlist == NULL)
		return NULL;

	freqlist->numfreq = numfreq;
	freqp = &rxbuf[3];
	for(numfreq = 0; numfreq < freqlist->numfreq; numfreq++) {
		freqlist->freq[numfreq].min = getword(freqp);
		freqlist->freq[numfreq].max = getword(freqp+2);
		freqp += 4;
	}
	return freqlist;
}

/* get target rom mapping */
struct arealist_t *get_arealist(enum mat_t mat, int ser_fd)
{
	char ans;
	unsigned char rxbuf[255+3];
	unsigned char *areap;
	struct arealist_t *arealist;
	int numarea;

	switch(mat) {
	case user:
		rxbuf[0] = QUERY_USER_AREA;
		ans      = QUERY_USER_AREA_RES;
		break;
	case userboot:
		rxbuf[0] = QUERY_BOOT_AREA;
		ans      = QUERY_BOOT_AREA_RES;
		break;
	default:
		return NULL;
	}
	write(ser_fd, rxbuf, 1);
	if (receive(ser_fd, rxbuf) == -1)
		return NULL;
	if (rxbuf[0] != ans)
		return NULL;

	numarea = rxbuf[2];
	arealist = (struct arealist_t *)malloc(sizeof(struct arealist_t) + 
                                               sizeof(struct area_t) * numarea);
	if (arealist == NULL)
		return NULL;

	arealist->areas = numarea;
	areap = &rxbuf[3];
	for(numarea = 0; numarea < arealist->areas; numarea++) {
		arealist->area[numarea].start = getlong(areap);
		arealist->area[numarea].end   = getlong(areap+4);
		areap += 8;
	}
	return arealist;
}

/* get write page size */
int get_writesize(int ser_fd)
{
	unsigned char rxbuf[5];

	rxbuf[0] = QUERY_WRITESIZE;
	write(ser_fd, rxbuf, 1);
	if (receive(ser_fd, rxbuf) == -1)
		return -1;
	if (rxbuf[0] != QUERY_WRITESIZE_RES)
		return -1;

	if (rxbuf[1] != 2)
		return -1;
	return rxbuf[2] << 8 | rxbuf[3];
}

/* set target bitrate */
int set_bitrate(int ser_fd, int bitrate, int freq, int coremul, int peripheralmul)
{
	char buf[9] = {SET_BITRATE, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

	buf[2] = (bitrate >> 8) & 0xff;
	buf[3] = bitrate & 0xff;

	buf[4] = (freq >> 8) & 0xff;
	buf[5] = freq & 0xff;

	buf[6] = (peripheralmul == 0)?1:2;

	buf[7] = coremul;
	buf[8] = peripheralmul;

	send(ser_fd, buf, sizeof(buf));
	if (receive(ser_fd, buf) != 1)
		return 0;

	if (!setbaud(ser_fd, bitrate))
		return 0;

	usleep(10000);
	buf[0] = ACK;
	write(ser_fd, buf, 1);
	if (receive(ser_fd, buf) != 1)
		return 0;
	else
		return 1;
}

/* check blank page */
static int skipcheck(unsigned char *data)
{
	unsigned char r = 0xff;
	int c;
	for (c = 0; c < 128; c++)
		r &= *data++;
	return (r == 0xff);
}

/* write rom image */
int write_rom(int ser_fd, const unsigned char *romimage, struct writeinfo_t *writeinfo)
{
	unsigned char *buf = NULL;
	unsigned long romaddr;

	buf = (unsigned char *)malloc(5 + writeinfo->size);
	if (buf == NULL) {
		perror("");
		return 0;
	}

	puts("erase flash...");
	/* enter writemode */
	*(buf + 0) = WRITEMODE;
	write(ser_fd, buf, 1);
	if (receive(ser_fd, buf) != 1) {
		fputs(PROGNAME ": writemode start failed", stderr);
		goto error;
	}

	/* mat select */
	switch (writeinfo->mat) {
	case user:     
		*(buf + 0) = WRITE_USER;
		break;
	case userboot: 
		*(buf + 0) = WRITE_USERBOOT;
		break;
	}
	write(ser_fd, buf, 1);
	if (receive(ser_fd, buf) != 1) {
		fputs(PROGNAME ": writemode start failed", stderr);
		goto error;
	}

	/* writing loop */
	for (romaddr = writeinfo->area.start; 
	     romaddr < writeinfo->area.end; 
	     romaddr += writeinfo->size) {
		if (skipcheck((unsigned char *)(romimage + romaddr)))
			continue;
		/* set write data */
		*(buf + 0) = WRITE128;
		setlong(buf + 1, romaddr);
		if ((romaddr + writeinfo->size) < writeinfo->area.end) {
			memcpy(buf + 5, romimage + romaddr, writeinfo->size);
		} else {
			/* lastpage  < 128byte*/
			memcpy(buf + 5, romimage + romaddr, 
			       (writeinfo->area.end - romaddr));
			memset(buf + 5 + writeinfo->area.end - romaddr, 0xff, 
			       writeinfo->size - (writeinfo->area.end - romaddr));
		}
		/* write */
		send(ser_fd, buf, 5 + writeinfo->size);
		if (receive(ser_fd, buf) != 1) {
			fprintf(stderr, PROGNAME ": write data %08lx failed.", romaddr);
			goto error;
		}
		if (verbose)
			printf("write - %08lx\n",romaddr);
		else {
			printf("writing %ld/%ld byte\r", romaddr, writeinfo->area.end); 
			fflush(stdout);
		}
	}
	/* write finish */
	*(buf + 0) = WRITE128;
	memset(buf + 1, 0xff, 4);
	send(ser_fd, buf, 5);
	if (receive(ser_fd, buf) != 1) {
		fputs(PROGNAME ": writemode exit failed", stderr);
		goto error;
	}
	free(buf);
	if (!verbose)
		printf("writing %ld/%ld byte\n", writeinfo->area.end, writeinfo->area.end); 


	return 1;
 error:
	free(buf);
	return 0;
}
