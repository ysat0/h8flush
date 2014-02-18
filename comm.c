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
#include <errno.h>
#include "h8flash.h"

#define TRY1COUNT 60
#define BAUD_ADJUST_LEN 30

/* NAK answer list */
const unsigned char naktable[] = {0x80, 0x90, 0x91, 0xbf, 0xc0, 0xc2, 0xc3, 0xc8,
				  0xcc, 0xcd, 0xd0, 0xd2, 0xd8};

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

/* send multibyte command */
static void send(struct port_t *p, unsigned char *data, int len)
{
	unsigned char sum;
	p->send_data(data, len);
	if (len > 1) {
		for(sum = 0; len > 0; len--, data++)
			sum += *data;
		sum = 0x100 - sum;
		p->send_data(&sum, 1);
	}
}

/* receive answer */
static int receive(struct port_t *p, unsigned char *data)
{
	int len;
	unsigned char *rxptr;
	unsigned char sum;

	rxptr = data;
	if (p->receive_byte(rxptr) != 1)
		return -1;
	rxptr++;
	/* ACK */
	if (*data == ACK) {
		return 1;
	}
	/* NAK */
	if (memchr(naktable, *data, sizeof(naktable))) {
		if (p->receive_byte(rxptr) != 1)
			return -1;
		else
			return 2;
	}

	/* multibyte response */
	if (p->receive_byte(rxptr) != 1)
		return -1;
	rxptr++;
	len = *(data + 1) + 1;
	for(; len > 0; len--) {
		if (p->receive_byte(rxptr) != 1)
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

/* get target device list */
struct devicelist_t *get_devicelist(struct port_t *port)
{
	unsigned char rxbuf[255+3];
	unsigned char *devp;
	struct devicelist_t *devlist;
	int devno;

	rxbuf[0] = QUERY_DEVICE;
	send(port, rxbuf, 1);
	if (receive(port, rxbuf) == -1)
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
		devp += *devp;
	}
	return devlist;
}

/* set target device ID */
int select_device(struct port_t *port, const char *code)
{
	unsigned char buf[6] = {SELECT_DEVICE, 0x04, 0x00, 0x00, 0x00, 0x00};
	
	memcpy(&buf[2], code, 4);
	send(port, buf, sizeof(buf));
	if (receive(port, buf) != 1)
		return 0;
	return 1;
}

/* get target clock mode */
struct clockmode_t *get_clockmode(struct port_t *port)
{
	unsigned char rxbuf[255+3];
	unsigned char *clkmdp;
	struct clockmode_t *clocks;
	int numclock;

	rxbuf[0] = QUERY_CLOCKMODE;
	send(port, rxbuf, 1);
	if (receive(port, rxbuf) == -1)
		return NULL;
	if (rxbuf[0] != QUERY_CLOCKMODE_RES)
		return NULL;

	numclock = rxbuf[2];
	if (numclock == 0) {
		numclock = 1;
		rxbuf[3] = 0;
	}
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
int set_clockmode(struct port_t *port, int mode)
{
	unsigned char buf[3] = {SET_CLOCKMODE, 0x01, 0x00};
	
	buf[2] =  mode;
	send(port, buf, sizeof(buf));
	if (receive(port, buf) != 1)
		return 0;
	else
		return 1;
}

/* get target multiplier/divider rate */
struct multilist_t *get_multirate(struct port_t *port)
{
	unsigned char rxbuf[255+3];
	unsigned char *mulp;
	struct multilist_t *multilist;
	struct multirate_t *rate;
	int nummulti;
	int numrate;
	int listsize;

	rxbuf[0] = QUERY_MULTIRATE;
	send(port, rxbuf, 1);
	if (receive(port, rxbuf) == -1)
		return NULL;
	if (rxbuf[0] != QUERY_MULTIRATE_RES)
		return NULL;

	/* calc multilist size */
	nummulti = rxbuf[2];
	listsize = sizeof(struct multilist_t) + sizeof(struct multirate_t *) * nummulti;
	mulp = &rxbuf[3];
	for(; nummulti > 0; nummulti--) {
		listsize += sizeof(struct multirate_t) + sizeof(int) * *mulp;
		mulp += *mulp;
	}
	multilist = (struct multilist_t *)malloc(listsize);
	if (multilist == NULL)
		return NULL;

	/* setup list */
	multilist->nummulti = rxbuf[2];
	rate = (struct multirate_t *)&multilist->muls[multilist->nummulti];
	mulp = &rxbuf[3];
	for (nummulti = 0; nummulti < multilist->nummulti; nummulti++) {
		multilist->muls[nummulti] = rate;
		rate->numrate = *mulp++;
		for (numrate = 0; numrate < rate->numrate; numrate++) {
			rate->rate[numrate] = *mulp++;
		}
		rate = (struct multirate_t *)&rate->rate[numrate];
	}

	return multilist;
}

/* get target operation frequency list */
struct freqlist_t *get_freqlist(struct port_t *port)
{
	unsigned char rxbuf[255+3];
	unsigned char *freqp;
	struct freqlist_t *freqlist;
	int numfreq;

	rxbuf[0] = QUERY_FREQ;
	send(port, rxbuf, 1);
	if (receive(port, rxbuf) == -1)
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
struct arealist_t *get_arealist(struct port_t *port, enum mat_t mat)
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
	send(port, rxbuf, 1);
	if (receive(port, rxbuf) == -1)
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
int get_writesize(struct port_t *port)
{
	unsigned char rxbuf[5];
	unsigned short size;

	rxbuf[0] = QUERY_WRITESIZE;
	send(port, rxbuf, 1);
	if (receive(port, rxbuf) == -1)
		return -1;
	if (rxbuf[0] != QUERY_WRITESIZE_RES)
		return -1;

	if (rxbuf[1] != 2)
		return -1;
	size = rxbuf[2] << 8 | rxbuf[3];
	return size;
}

/* bitrate candidate list */
static const int rate_list[]={1152,576,384,192,96};

/* bitrate error margine (%) */
#define ERR_MARGIN 4

/* select communication bitrate */
static int adjust_bitrate(int p_freq)
{
 	int brr;
	int errorrate;
	int rate_no;

	for (rate_no = 0; rate_no < sizeof(rate_list) / sizeof(int); rate_no++) {
		brr = (p_freq * 100) / (32 * rate_list[rate_no]);
		errorrate = abs((p_freq * 10000) / ((brr + 1) * rate_list[rate_no] * 32) - 100);
		if (errorrate <= ERR_MARGIN)
			return rate_list[rate_no];
	}
	return 0;
}

/* set target bitrate */
static int set_bitrate(struct port_t *p, int bitrate, int freq, int coremul, int peripheralmul)
{
	unsigned char buf[9] = {SET_BITRATE, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

	buf[2] = (bitrate >> 8) & 0xff;
	buf[3] = bitrate & 0xff;

	buf[4] = (freq >> 8) & 0xff;
	buf[5] = freq & 0xff;

	buf[6] = (peripheralmul == 0)?1:2;

	buf[7] = coremul;
	buf[8] = peripheralmul;

	send(p, buf, sizeof(buf));
	if (receive(p, buf) != 1)
		return 0;

	if (p->setbaud) {
		if (!p->setbaud(bitrate))
			return 0;

	}
	usleep(10000);
	buf[0] = ACK;
	send(p, buf, 1);
	if (receive(p, buf) != 1)
		return 0;
	else
		return 1;
}

#define C_MULNO 0
#define P_MULNO 1
#define C_FREQNO 0
#define P_FREQNO 1

/* change communicate bitrate */
static int change_bitrate(struct port_t *p, int in_freq,
			  struct multilist_t *multi, struct freqlist_t *freq)
{
	int rateno;
	int core_mul, peripheral_mul;
	int core_freq, peripheral_freq;
	int clock;
	int rate;

	core_mul       = 0;
	peripheral_mul = 0;
	/* select cpu core clock frequency */
	for (rateno = 0, core_freq = -1; rateno < multi->muls[C_MULNO]->numrate; rateno++) {
		if (multi->muls[C_MULNO]->rate[rateno] > 0)
			clock = in_freq * multi->muls[C_MULNO]->rate[rateno];
		else
			clock = in_freq / -multi->muls[C_MULNO]->rate[rateno];
		if (!(clock >= freq->freq[C_FREQNO].min && clock <= freq->freq[C_FREQNO].max))
			continue;
		if (core_freq < clock) {
			core_mul  = multi->muls[C_MULNO]->rate[rateno];
			core_freq = clock;
		}
	}

	/* select peripheral clock freqency */
	if (multi->nummulti > P_MULNO) {
		for (rateno = 0, peripheral_freq = -1; 
		     rateno < multi->muls[P_MULNO]->numrate; rateno++) {
			if (multi->muls[P_MULNO]->rate[rateno] > 0)
				clock = in_freq * multi->muls[P_MULNO]->rate[rateno];
			else
				clock = in_freq / -multi->muls[P_MULNO]->rate[rateno];
			if (clock < freq->freq[P_FREQNO].min || 
			    clock > freq->freq[P_FREQNO].max)
				continue;
			if (peripheral_freq < clock) {
				peripheral_mul  = multi->muls[P_MULNO]->rate[rateno];
				peripheral_freq = clock;
			}
		}
	} else {
		peripheral_mul  = 0;
		peripheral_freq = core_freq;
	}

	/* select clock check */
	if (core_freq == -1 || peripheral_freq == -1) {
		fprintf(stderr,"input frequency (%d.%d MHz) is out of range\n", 
			in_freq / 100, in_freq % 100);
		return 0;
	}

	VERBOSE_PRINT("core multiple rate=%d, freq=%d.%d MHz\n", 
		      core_mul, core_freq / 100, core_freq % 100);
	VERBOSE_PRINT("peripheral multiple rate=%d, freq=%d.%d MHz\n", 
		      peripheral_mul, peripheral_freq / 100, peripheral_freq % 100);

	/* select bitrate from peripheral cock*/
	rate = adjust_bitrate(peripheral_freq);
	if (rate == 0)
		return 0;

	VERBOSE_PRINT("bitrate %d bps\n",rate * 100);

	/* setup host/target bitrate */
	return set_bitrate(p, rate, in_freq, core_mul, peripheral_mul);
}

/* check blank page */
static int skipcheck(unsigned char *data, unsigned short size)
{
	unsigned char r = 0xff;
	int c;
	for (c = 0; c < size; c++)
		r &= *data++;
	return (r == 0xff);
}

/* write rom image */
int write_rom(struct port_t *port, const unsigned char *romimage, struct writeinfo_t *writeinfo)
{
	unsigned char *buf = NULL;
	unsigned int romaddr;

	buf = (unsigned char *)malloc(5 + writeinfo->size);
	if (buf == NULL) {
		perror("");
		return 0;
	}

	puts("Erase flash...");
	/* enter writemode */
	buf[0] = WRITEMODE;
	send(port, buf, 1);
	if (receive(port, buf) != 1) {
		printf("%02x ", buf[0]);
		fputs(PROGNAME ": writemode start failed\n", stderr);
		goto error;
	}

	/* mat select */
	switch (writeinfo->mat) {
	case user:     
		buf[0] = WRITE_USER;
		break;
	case userboot: 
		buf[0] = WRITE_USERBOOT;
		break;
	}
	send(port, buf, 1);
	if (receive(port, buf) != 1) {
		printf("%02x ", buf[0]);
		fputs(PROGNAME ": writemode start failed\n", stderr);
		goto error;
	}

	/* writing loop */
	for (romaddr = writeinfo->area.start; 
	     romaddr >= writeinfo->area.start && romaddr < writeinfo->area.end; 
	     romaddr += writeinfo->size) {
		if (skipcheck((unsigned char *)(romimage + romaddr - writeinfo->area.start), writeinfo->size))
			continue;
		/* set write data */
		*(buf + 0) = WRITE;
		setlong(buf + 1, romaddr);
		if ((romaddr + writeinfo->size) < writeinfo->area.end) {
			memcpy(buf + 5, romimage + romaddr - writeinfo->area.start, writeinfo->size);
		} else {
			/* lastpage  < writesize */
			memcpy(buf + 5, romimage + romaddr - writeinfo->area.start, 
			       (writeinfo->area.end - romaddr));
			memset(buf + 5 + writeinfo->area.end - romaddr, 0xff, 
			       writeinfo->size - (writeinfo->area.end - romaddr));
		}
		/* write */
		send(port, buf, 5 + writeinfo->size);
		if (receive(port, buf) != 1) {
			fprintf(stderr, PROGNAME ": write data %08x failed.", romaddr);
			goto error;
		}
		if (verbose)
			printf("write - %08x\n",romaddr);
		else {
			printf("writing %d/%d byte\r", romaddr - writeinfo->area.start, 
			       writeinfo->area.end  - writeinfo->area.start); 
			fflush(stdout);
		}
	}
	/* write finish */
	*(buf + 0) = WRITE;
	memset(buf + 1, 0xff, 4);
	send(port, buf, 5);
	if (receive(port, buf) != 1) {
		fputs(PROGNAME ": writemode exit failed", stderr);
		goto error;
	}
	free(buf);
	if (!verbose)
		printf("writing %d/%d byte\n", writeinfo->area.end  - writeinfo->area.start, 
		       writeinfo->area.end - writeinfo->area.start); 


	return 1;
 error:
	free(buf);
	return 0;
}

/* connect to target chip */
int setup_connection(struct port_t *p, int input_freq)
{
	int c;
	int r = -1;
	struct devicelist_t *devicelist = NULL;
	struct clockmode_t  *clockmode  = NULL;
	struct multilist_t  *multilist  = NULL;
	struct freqlist_t   *freqlist   = NULL;

	/* connect target */
	if (!p->connect_target(p->dev)) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("target no response\n", stderr);
		goto error;
	}

	/* query target infomation */
	devicelist = get_devicelist(p);
	if (devicelist == NULL) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("devicelist error\n", stderr);
		goto error;
	}
	if (verbose) {
		char codes[5];
		printf("Support devices: %d\n", devicelist->numdevs);
		for (c = 0; c < devicelist->numdevs; c++) {
			memcpy(codes, devicelist->devs[c].code, 4);
			codes[4] = '\0';
			printf("%d: %s - %s\n", c+1, codes, devicelist->devs[c].name);
		}
	}

	/* query target clockmode */
	clockmode = get_clockmode(p);
	if (clockmode == NULL) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("clockmode error\n",stderr);
		goto error;
	}
	if (verbose) {
		if (clockmode->nummode > 0) {
			printf("Support clock modes %d:", clockmode->nummode);
			for (c = 0; c < clockmode->nummode; c++) {
				printf(" %02x", clockmode->mode[c]);
			}
			printf("\n");
		} else
			printf("no clockmode support\n");
	}
	
	/* SELDEV devicetype select */
	if (devicelist->numdevs < SELDEV) {
		fprintf(stderr, "Select Device (%d) not supported.\n", SELDEV);
		goto error;
	}
	if (!select_device(p, devicelist->devs[SELDEV].code)) {
		fputs("device select error", stderr);
		goto error;
	}

	/* SELCLK clockmode select */
	if (clockmode->nummode > 0) {
		if (clockmode->nummode < SELCLK) {
			fprintf(stderr, "Select clock (%d) not supported.\n", SELCLK);
			goto error;
		}
		if (!set_clockmode(p, clockmode->mode[SELCLK])) {
			fputs("clock select error", stderr);
			goto error;
		}
	} else {
		if (!set_clockmode(p, 0)) {
			fputs("clock select error", stderr);
			goto error;
		}
	}

	/* query multiplier/devider rate */
	multilist = get_multirate(p);
	if (multilist == NULL) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("multilist error\n",stderr);
		goto error;
	}
	if (verbose) {
		int c1,c2;
		printf("Support multiple rate: %d\n", multilist->nummulti);
		for (c1 = 0; c1 < multilist->nummulti; c1++) {
			printf("%d:", c1 + 1);
			for (c2 = 0; c2 < multilist->muls[c1]->numrate; c2++)
				printf(" %d", multilist->muls[c1]->rate[c2]);
			printf("\n");
		}
	}

	/* query operation frequency range */
	freqlist = get_freqlist(p);
	if (freqlist == NULL) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("freqlist error\n",stderr);
		goto error;
	}
	if (verbose) {
		printf("operation frequencies: %d\n", freqlist->numfreq);
		for (c = 0; c < freqlist->numfreq; c++) {
			printf("%d: %d.%d - %d.%d\n", c + 1,
			       freqlist->freq[c].min / 100,freqlist->freq[c].min % 100, 
			       freqlist->freq[c].max / 100,freqlist->freq[c].max % 100);
		}
	}

	/* set writeing bitrate */
	if (!change_bitrate(p, input_freq, multilist, freqlist)) {
		fputs("set bitrate failed\n",stderr);
		goto error;
	}

	r = 0;
 error:
	free(devicelist);
	free(clockmode);
	free(multilist);
	free(freqlist);
	return r;
}

/* connect to target chip */
void dump_configs(struct port_t *p)
{
	struct devicelist_t *devicelist = NULL;
	struct clockmode_t  *clockmode  = NULL;
	struct multilist_t  *multilist  = NULL;
	struct freqlist_t   *freqlist   = NULL;
	int dev;
	int clk;
	int c1,c2;

	/* connect target */
	if (!p->connect_target(p->dev)) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("target no response\n", stderr);
		goto error;
	}

	/* query target infomation */
	devicelist = get_devicelist(p);
	if (devicelist == NULL) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("devicelist error\n", stderr);
		goto error;
	}
	/* query target clockmode */
	clockmode = get_clockmode(p);
	if (clockmode == NULL) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("clockmode error\n",stderr);
		goto error;
	}
	for(dev = 0; dev < devicelist->numdevs; dev++) {
		if (!select_device(p, devicelist->devs[dev].code)) {
			fputs("device select error", stderr);
			goto error;
		}
		for (clk = 0; clk < clockmode->nummode; clk++) {
			if (!set_clockmode(p, clockmode->mode[clk])) {
				fputs("clock select error", stderr);
				goto error;
			}
			
			printf("dev: %s - clock: %d\n", devicelist->devs[dev].name, clk);
			multilist = get_multirate(p);
			if (multilist == NULL) {
				if (errno != 0)
					perror(PROGNAME);
				else
					fputs("multilist error\n",stderr);
				goto error;
			}
			printf("multiple / divide rate\n");
			for (c1 = 0; c1 < multilist->nummulti; c1++) {
				for (c2 = 0; c2 < multilist->muls[c1]->numrate; c2++)
					printf(" %d", (int)multilist->muls[c1]->rate[c2]);
				printf("\n");
			}

			/* query operation frequency range */
			freqlist = get_freqlist(p);
			if (freqlist == NULL) {
				if (errno != 0)
					perror(PROGNAME);
				else
					fputs("freqlist error\n",stderr);
				goto error;
			}
			printf("operation frequency (MHz)\n");
			for (c1 = 0; c1 < freqlist->numfreq; c1++) {
				printf("%d.%d - %d.%d\n",
				       freqlist->freq[c1].min / 100,freqlist->freq[c1].min % 100, 
				       freqlist->freq[c1].max / 100,freqlist->freq[c1].max % 100);
			}
			free(multilist);
			free(freqlist);
			multilist = NULL;
			freqlist = NULL;
		}
	}
error:
	free(multilist);
	free(freqlist);
	free(devicelist);
	free(clockmode);
}	
			
