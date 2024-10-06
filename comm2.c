/*
 *  Renesas CPU On-chip Flash memory writer
 *  target communication (New)
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
#include <stdint.h>
#include <ctype.h>
#include "h8flash.h"

#define TRY1COUNT 60
#define BAUD_ADJUST_LEN 30

#define SOH 0x01
#define ETX 0x03
#define ETB 0x17
#define SOD 0x81

/* big endian to cpu endian convert 32bit */
static __inline__ int getlong(uint32_t *p)
{
	uint8_t *b = (uint8_t *)p;
	return (*b << 24) | (*(b+1) << 16) | (*(b+2) << 8) | *(b+3);
}
	
/* big endian to cpu endian convert 16bit */
static __inline__ short getword(uint16_t *p)
{
	uint8_t *b = (uint8_t *)p;
	return (*b << 8) | *(b+1);
}

/* cpu endian to big endian 32bit */
static __inline__ void setlong(unsigned char *buf, unsigned long val)
{
	*(buf + 0) = (val >> 24) & 0xff;
	*(buf + 1) = (val >> 16) & 0xff;
	*(buf + 2) = (val >>  8) & 0xff;
	*(buf + 3) = (val      ) & 0xff;
}

/* cpu endian to big endian 16bit */
static __inline__ void setword(unsigned char *buf, unsigned short val)
{
	*(buf + 0) = (val >>  8) & 0xff;
	*(buf + 1) = (val      ) & 0xff;
}

/* send multibyte command */
static void send(struct port_t *p, unsigned char *data, int len,
		 unsigned char head, unsigned char tail)
{
	unsigned char buf[2];
	unsigned char sum;
	
	p->send_data(&head, 1);
	setword(buf, len);
	p->send_data(buf, 2);
	p->send_data(data, len);
	if (len > 0) {
		for(sum = 0; len > 0; len--, data++)
			sum += *data;
	}
	sum += buf[0];
	sum += buf[1];
	sum = 0x100 - sum;
	p->send_data(&sum, 1);
	p->send_data(&tail, 1);
}

/* receive answer */
static unsigned int receive(struct port_t *p, unsigned char *data)
{
	int len;
	unsigned char *rxptr;
	unsigned char sum = 0;

	rxptr = data;
	/* Header */
	for (len = 0; len < 3; len++) {
		if (p->receive_byte(rxptr) != 1)
			return -1;
		rxptr++;
	}

	/* Res + Data */
	len = getword((uint16_t *)(data + 1));
	for(; len > 0; len--) {
		if (p->receive_byte(rxptr) != 1)
			return -1;
		rxptr++;
	}

	/* SUM + ETX/ETB */
	for (len = 0; len < 2; len++) {
		if (p->receive_byte(rxptr) != 1)
			return -1;
		rxptr++;
	}

	/* sum check */
	for (sum = 0, rxptr = data + 1, len = getword((uint16_t *)(data + 1)) + 3;
	     len > 0; len--, rxptr++)
		sum += *rxptr;
	if (sum != 0)
		return -1;
	return *(data + 3);
}

struct raw_devtype_t {
	uint8_t  sod;
	uint16_t len;
	uint8_t  res;
	uint64_t typ;
	uint32_t osa;
	uint32_t osi;
	uint32_t cpa;
	uint32_t cpi;
	uint8_t  sum;
	uint8_t  etx;
} __attribute__((packed,aligned(1)));

struct devtype_t {
	uint64_t typ;
	uint32_t osa;
	uint32_t osi;
	uint32_t cpa;
	uint32_t cpi;
};

/* get target device type */
static  int get_devtype(struct port_t *port, struct devtype_t *type)
{
	struct raw_devtype_t raw_type;
	unsigned char cmd[] = {0x38};

	send(port, cmd, 1, SOH, ETX);
	if (receive(port, (unsigned char *)&raw_type) == -1)
		return -1;
	send(port, cmd, 1, SOD, ETX);
       	if (receive(port, (unsigned char *)&raw_type) == -1)
		return -1;
	if (raw_type.res != 0x38)
		return -1;
	type->typ = raw_type.typ;
	type->osa = getlong(&raw_type.osa);
	type->osi = getlong(&raw_type.osi);
	type->cpa = getlong(&raw_type.cpa);
	type->cpi = getlong(&raw_type.cpi);
	return 0;
}

/* set endian */
static int set_endian(struct port_t *port, unsigned int endian)
{
	unsigned char cmd[2] = {0x36, 0x00};
	unsigned char rcv[8];

	cmd[1] = endian;
	send(port, cmd, 2, SOH, ETX);
	return receive(port, rcv);
}

struct raw_freq_t {
	uint8_t  sod;
	uint16_t len;
	uint8_t  res;
	uint32_t fq;
	uint32_t pf;
	uint8_t  sum;
	uint8_t  etx;
} __attribute__((packed,aligned(1)));

/* set frequency */
static int set_frequency(struct port_t *port, unsigned int input, unsigned int system)
{
	unsigned char cmd[] = {0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	struct raw_freq_t freq;
	unsigned int core, peripheral;
	setlong(cmd + 1, input);
	setlong(cmd + 5, system);
	send(port, cmd, sizeof(cmd), SOH, ETX);
	if (receive(port, (unsigned char *)&freq) == -1)
		return -1;
	send(port, cmd, 1, SOD, ETX);
       	if (receive(port, (unsigned char *)&freq) == -1)
		return -1;
	core = getlong(&freq.fq);
	peripheral = getlong(&freq.pf);
	VERBOSE_PRINT("Core %dHz / Pereipheral %dHz\n", core, peripheral);
	return peripheral;
}

/* bitrate candidate list */
static const int rate_list[]={115200,57600,38400,19200,9600};

/* bitrate error margine (%) */
#define ERR_MARGIN 4

/* select communication bitrate */
static int adjust_bitrate(int p_freq)
{
 	int brr;
	int errorrate;
	int rate_no;

	for (rate_no = 0; rate_no < sizeof(rate_list) / sizeof(int); rate_no++) {
		brr = p_freq / (32 * rate_list[rate_no]) - 2;
		errorrate = abs(p_freq / ((brr + 1) * rate_list[rate_no] * 32));
		if (errorrate <= ERR_MARGIN)
			return rate_list[rate_no];
	}
	return 0;
}

/* set target bitrate */
static int set_bitrate(struct port_t *p, int bitrate)
{
	unsigned char cmd[] = {0x34, 0x00, 0x00, 0x00, 0x00};
	unsigned char rcv[6];
	setlong(cmd + 1, bitrate);
	send(p, cmd, sizeof(cmd), SOH, ETX);
	if (receive(p, rcv) != 0x34)
		return 0;

	if (p->setbaud) {
		if (!p->setbaud(bitrate / 100))
			return 0;

	}
	usleep(10000);
	return 1;
}

#define C_MULNO 0
#define P_MULNO 1
#define C_FREQNO 0
#define P_FREQNO 1

/* change communicate bitrate */
static int change_bitrate(struct port_t *p, int peripheral_freq)
{
	int rate;


	/* select bitrate from peripheral cock*/
	rate = adjust_bitrate(peripheral_freq);
	if (rate == 0)
		return 0;

	VERBOSE_PRINT("bitrate %d bps\n",rate);

	/* setup host/target bitrate */
	return set_bitrate(p, rate);
}

static int syncro(struct port_t *p)
{
	unsigned char cmd[] = {0x00};
	unsigned char rcv[6];
	send(p, cmd, sizeof(cmd), SOH, ETX);
	return receive(p, rcv) == 0x00;
}

struct raw_signature_t {
	uint8_t  sod;
	uint16_t len;
	uint8_t  res;
	uint8_t  dev[16];
	struct {
		uint8_t  type;
		uint32_t size;
		uint16_t num;
	} __attribute__((packed,aligned(1))) bank[6] ;
        uint8_t  sum;
	uint8_t  etx;
} __attribute__((packed,aligned(1)));

/* get target rom mapping */
static struct arealist_t *get_arealist(struct port_t *p, enum mat_t mat)
{
	unsigned char cmd[] = {0x3a};
	struct raw_signature_t raw_sig;
	unsigned int id[] = {0x00, 0x02};
	int numarea;
	int i;
	unsigned int addr = 0;
	struct arealist_t *arealist;

	send(p, cmd, 1, SOH, ETX);
	if (receive(p, (unsigned char *)&raw_sig) < 0)
		return NULL;
	send(p, cmd, 1, SOD, ETX);
	if (receive(p, (unsigned char *)&raw_sig) < 0)
		return NULL;
	
	/* lookup area */
	for(numarea = 0, i = 0; i < 6; i++) {
		if (raw_sig.bank[i].type == id[mat])
			numarea += getword(&raw_sig.bank[i].num);
	}
	arealist = (struct arealist_t *)malloc(sizeof(struct arealist_t) + 
                                               sizeof(struct area_t) * numarea);
	if (arealist == NULL)
		return NULL;

	arealist->areas = numarea;
	/* setup area list*/
	for(numarea = 0, i = 0; i < 6; i++) {
		if (raw_sig.bank[i].type == id[mat]) {
			unsigned int sz;
			int j;
			sz = getlong(&raw_sig.bank[i].size);
			for(j = 0; j < getword(&raw_sig.bank[i].num); j++) {
				arealist->area[numarea].start = addr - sz;
				arealist->area[numarea].end = addr - 1;
				arealist->area[numarea].size = sz;
				if (!(arealist->area[numarea].image = malloc(sz)))
					return NULL;
				memset(arealist->area[numarea].image, 0xff, arealist->area[numarea].size);
				addr -= sz;
				numarea++;
			}
		}
	}
	return arealist;
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
static int write_rom(struct port_t *port, struct arealist_t *arealist, enum mat_t mat)
{
	uint8_t erase[] = {0x12, 0x00, 0x00, 0x00, 0x00};
	uint8_t write[] = {0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	uint8_t data[257];
	unsigned char rcv[8];
	unsigned int wsize, total;
	int i, j, r;
	struct area_t *area;
	for (total = 0, i = 0; i < arealist->areas; i++) {
		total += arealist->area[i].size;
	}
	/* writing loop */
	for (wsize = 0, i = 0; i < arealist->areas; i++) {
		area = &arealist->area[i];
		if (skipcheck(area->image, area->size)) {
			wsize += area->size;
			if (verbose)
				printf("skip - %08x\n",area->start);
			else {
				printf("writing %d/%d byte\r",
				       wsize, 
				       total); 
				fflush(stdout);
			}
			continue;
		}
		setlong(erase + 1, area->start);
		send(port, erase, sizeof(erase), SOH, ETX);
		if ((r=receive(port, rcv)) > 0x80) {
			return -1;
		}
		setlong(write + 1, area->start);
		setlong(write + 5, area->end);
		send(port, write, sizeof(write), SOH, ETX);
		if (receive(port, rcv) > 0x80)
			return -1;
		for(j = 0; j < area->size / 256; j++) {
			data[0] = 0x13;
			memcpy(&data[1],
			       area->image + j * (sizeof(data) - 1),
			       sizeof(data) -1);
			if (j < (area->size / 256 -1))
				send(port, data, sizeof(data), SOD, ETB);
			else
				send(port, data, sizeof(data), SOD, ETX);
			if ((r = receive(port, rcv)) > 0x80) {
				return -1;
			}
		}
		wsize += area->size;
		if (verbose)
			printf("write - %08x\n", area->start);
		else {
			printf("writing %d/%d byte\r",
			       wsize, 
			       total); 
			fflush(stdout);
		}
	}
	if (!verbose)
		putc('\n', stdout);

	return 0;
}

/* connect to target chip */
static int setup_connection(struct port_t *p, int input_freq, char endian)
{
	struct devtype_t dt;
	int e = -1;
	int pf;

	input_freq *= 10000;
	if(get_devtype(p, &dt) < 0) {
		fputs("device type failed", stderr);
		return -1;
	}
	switch(toupper(endian)) {
	case 'L':
		e = 1;
		break;
	case 'B':
		e = 0;
		break;
	}
	if (e == -1 || set_endian(p, e) < 0) {
		fputs("endian setup failed", stderr);
		return -1;
	}

	pf = set_frequency(p, input_freq, dt.cpa);
	if (pf < 0) {
		fputs("frequency setup failed", stderr);
		return -1;
	}
		
	/* set writeing bitrate */
	if (!change_bitrate(p, pf)) {
		fputs("set bitrate failed\n",stderr);
		return -1;
	}

	if (!syncro(p)) {
		fputs("sync failed\n",stderr);
		return -1;
	}

	return 0;
}

static void dump_configs(struct port_t *p)
{
	struct devtype_t dt;

	if(get_devtype(p, &dt) < 0) {
		fputs("device type failed", stderr);
		return;
	}
	printf("type code: %016llx\n", dt.typ);
	printf("input max: %dHz\n", dt.osa);
	printf("input min: %dHz\n", dt.osi);
	printf("sys max: %dHz\n", dt.cpa);
	printf("sys min: %dHz\n", dt.cpi);
}	

static struct comm_t v2 = {
	.get_arealist = get_arealist,
	.write_rom = write_rom,
	.setup_connection = setup_connection,
	.dump_configs = dump_configs,
};

struct comm_t *comm_v2(void)
{
	return &v2;
}
