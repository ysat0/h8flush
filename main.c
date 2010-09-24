/*
 *  Renesas CPU On-chip Flash memory writer
 *  main
 *
 * Yoshinori Sato <ysato@users.sourceforge.jp>
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License version 2.1 (or later).
 */

#include <stdio.h>
#include <getopt.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <signal.h>
#include <libgen.h>
#include "h8flash.h"

#define SREC_MAXLEN (256*2 + 4 + 1)

#define VERBOSE_PRINT(...) do { if (verbose) printf(__VA_ARGS__); } while(0)

int verbose = 0;

const static struct option long_options[] = {
	{"userboot", no_argument, NULL, 'u'},
	{"port", required_argument, NULL, 'p'},
	{"freq", required_argument, NULL, 'f'},
	{"binary", no_argument, NULL, 'b'},
	{"verbose", no_argument, NULL, 'V'},
	{0, 0, 0, 0}
};

static void usage(void)
{
	puts(PROGNAME " [-p serial port][-f input clock frequency][-b][--userboot] filename");
}

/* read raw binary */
static int write_binary(FILE *fp, struct writeinfo_t *writeinfo, int ser_fd)
{
	int fno;
	struct stat bin_st;
	unsigned char *bin_buf;
	unsigned long bin_len;

	fno = fileno(fp);

	fstat(fno, &bin_st);
	bin_len = bin_st.st_size;

	if ((bin_buf = (char *)mmap(NULL, bin_len, PROT_READ, MAP_SHARED, fno, 0)) == MAP_FAILED)
		goto error_perror;
	writeinfo->area.end = bin_len - 1;
	if (!write_rom(ser_fd, bin_buf, writeinfo))
		goto error;
	munmap(bin_buf, bin_len);
	fclose(fp);
	return bin_len;
 error_perror:
	perror(PROGNAME);
 error:	
	if (bin_buf)
		munmap(bin_buf, bin_len);
	fclose(fp);
	return 0;
}

/* read srec binary */
static int write_srec(FILE *fp, struct writeinfo_t *writeinfo, int ser_fd)
{
	unsigned char *romimage = NULL;
	unsigned char *bufp;
	unsigned long last_addr = 0;
	int buff_size;
	static char linebuf[SREC_MAXLEN + 1];
	char *lp;
	char hexbuf[9];
	int sum;
	int len;
	unsigned int addr;
	const static int address_len[]={0,4,6,8,0,0,0,8,6,4};
	int r = 0;
	int l;

	romimage = (char *)malloc(writeinfo->area.end - writeinfo->area.start + 1);
	if (!romimage) {
		perror(PROGNAME);
		goto error;
	}
	memset(romimage, 0xff, writeinfo->area.end - writeinfo->area.start + 1);

	while (fgets(linebuf, sizeof(linebuf), fp)) {
		/* check valid Srecord */
		if (linebuf[0] != 'S' ||
		    isdigit(linebuf[1]) == 0)
			continue;
		if (!(l = address_len[linebuf[1] & 0x0f]))
			continue;

		/* get length */
		memcpy(hexbuf, &linebuf[2], 2);
		hexbuf[2] = '\0';
		sum = len = strtoul(hexbuf, NULL, 16);

		/* get address */
		memcpy(hexbuf, &linebuf[4], l);
		hexbuf[l] = '\0';
		addr = strtoul(hexbuf, NULL, 16);
		len -= l / 2;

		/* address part checksum */
		lp = &linebuf[4];
		for (; l > 0; l -= 2, lp += 2) {
			memcpy(hexbuf, lp, 2);
			hexbuf[2] = '\0';
			sum += strtoul(hexbuf, NULL, 16);
		}

		/* area check */
		if (addr < writeinfo->area.start || 
		    (addr + len - 1) > writeinfo->area.end) {
			fprintf(stderr, "srec address %08x is out of romarea\n", addr);
			goto error;
		}
		bufp = romimage + addr;
		if (last_addr < (addr + len - 1))
			last_addr = (addr + len - 1);

		/* parse body */
		for (; len > 1; --len, lp += 2, buff_size++) {
			unsigned char d;
			memcpy(hexbuf, lp, 2);
			hexbuf[2] = '\0';
			d = strtoul(hexbuf, NULL, 16);
			*bufp++ = d;
			sum    += d;
		}

		/* checksum */
		memcpy(hexbuf, lp, 2);
		hexbuf[2] = '\0';
		sum += strtoul(hexbuf, NULL, 16);
		if ((sum & 0xff) != 0xff) {
			fputs("\n" PROGNAME ": Checksum unmatch\n",stderr);
			r = 0;
			goto error;
		}
	}
	writeinfo->area.end = last_addr;
	r = write_rom(ser_fd, romimage, writeinfo);
 error:
	free(romimage);
	fclose(fp);
	return r;
}

/* read rom writing data */
static int writefile_to_rom(char *fn, int force_binary, struct writeinfo_t *writeinfo, int ser_fd)
{
	FILE *fp = NULL;
	static char linebuf[SREC_MAXLEN + 1];
	char hexbuf[3];
	int sum;
	int len;
	char *p;

	hexbuf[2] = '\0';

	/* open download data file */
	fp = fopen(fn, "r");
	if (fp == NULL) {
		perror(PROGNAME);
		return -1;
	}
	/* get head */
	if (fgets(linebuf, sizeof(linebuf), fp) == NULL && ferror(fp)) {
		fclose(fp);
		return -1;
	}
	fseek(fp,0,SEEK_SET);

	/* check 'S??' */
	if (force_binary ||
	    linebuf[0] != 'S' ||
	    isdigit(linebuf[1]) == 0)
		return write_binary(fp, writeinfo, ser_fd);

	/* check body (calcurate checksum) */
	memcpy(hexbuf, &linebuf[2], 2);
	sum = len = strtoul(hexbuf, NULL, 16);
	for (p = &linebuf[4]; len > 0; --len, p += 2) {
		memcpy(hexbuf, p, 2);
		sum += strtoul(hexbuf, NULL, 16);
	}
	if ((sum & 0xff) == 0xff)
		/* checksum ok is Srecord format */
		return write_srec(fp, writeinfo, ser_fd);
	else
		return write_binary(fp, writeinfo, ser_fd);
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

#define C_MULNO 0
#define P_MULNO 1
#define C_FREQNO 0
#define P_FREQNO 1

/* change communicate bitrate */
static int change_bitrate(int ser_fd, int in_freq,
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
	for (rateno = 0, core_freq = -1; rateno < multi->muls[C_MULNO].numrate; rateno++) {
		if (multi->muls[C_MULNO].rate[rateno] > 0)
			clock = in_freq * multi->muls[C_MULNO].rate[rateno];
		else
			clock = in_freq / -multi->muls[C_MULNO].rate[rateno];
		if (!(clock >= freq->freq[C_FREQNO].min && clock <= freq->freq[C_FREQNO].max))
			continue;
		if (core_freq < clock) {
			core_mul  = multi->muls[C_MULNO].rate[rateno];
			core_freq = clock;
		}
	}

	/* select peripheral clock freqency */
	if (multi->nummulti > P_MULNO) {
		for (rateno = 0, peripheral_freq = -1; 
		     rateno < multi->muls[P_MULNO].numrate; rateno++) {
			if (multi->muls[P_MULNO].rate[rateno] > 0)
				clock = in_freq * multi->muls[P_MULNO].rate[rateno];
			else
				clock = in_freq / -multi->muls[P_MULNO].rate[rateno];
			if (clock < freq->freq[P_FREQNO].min || 
			    clock > freq->freq[P_FREQNO].max)
				continue;
			if (peripheral_freq < clock) {
				peripheral_mul  = multi->muls[P_MULNO].rate[rateno];
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
	return set_bitrate(ser_fd, rate, in_freq, core_mul, peripheral_mul);
}

/* connect to target chip */
static int setup_connection(const char *ser_port, int input_freq)
{
	int ser_fd;
	int c;
	int r = -1;
	struct devicelist_t *devicelist = NULL;
	struct clockmode_t  *clockmode  = NULL;
	struct multilist_t  *multilist  = NULL;
	struct freqlist_t   *freqlist   = NULL;

	/* serial port open */
	ser_fd = open_serial(ser_port);
	if (ser_fd == -1)
		return -1;

	/* connect target */
	if (!connect_target(ser_fd)) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("target no response\n", stderr);
		goto error;
	}

	/* query target infomation */
	devicelist = get_devicelist(ser_fd);
	if (devicelist == NULL) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("devicelist error\n", stderr);
		goto error;
	}
	if (verbose) {
		char codes[5];
		printf("supports devices: %d\n", devicelist->numdevs);
		for (c = 0; c < devicelist->numdevs; c++) {
			memcpy(codes, devicelist->devs[c].code, 4);
			codes[4] = '\0';
			printf("%d: %s - %s\n", c+1, codes, devicelist->devs[c].name);
		}
	}

	/* query target clockmode */
	clockmode = get_clockmode(ser_fd);
	if (clockmode == NULL) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("clockmode error\n",stderr);
		goto error;
	}
	if (verbose) {
		if (clockmode->nummode > 0) {
			printf("supports clockmode %d:", clockmode->nummode);
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
	if (!select_device(ser_fd, devicelist->devs[SELDEV].code)) {
		fputs("device select error", stderr);
		goto error;
	}

	/* SELCLK clockmode select */
	if (clockmode->nummode > 0) {
		if (clockmode->nummode < SELCLK) {
			fprintf(stderr, "Select clock (%d) not supported.\n", SELCLK);
			goto error;
		}
		if (!set_clockmode(ser_fd, clockmode->mode[SELCLK])) {
			fputs("clock select error", stderr);
			goto error;
		}
	} else {
		if (!set_clockmode(ser_fd, 0)) {
			fputs("clock select error", stderr);
			goto error;
		}
	}

	/* query multiplier/devider rate */
	multilist = get_multirate(ser_fd);
	if (multilist == NULL) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("multilist error\n",stderr);
		goto error;
	}
	if (verbose) {
		int c1,c2;
		printf("supports multirate: %d\n", multilist->nummulti);
		for (c1 = 0; c1 < multilist->nummulti; c1++) {
			printf("%d:", c1 + 1);
			for (c2 = 0; c2 < multilist->muls[c1].numrate; c2++)
				printf(" %d", multilist->muls[c1].rate[c2]);
		}
		printf("\n");
	}

	/* query operation frequency range */
	freqlist = get_freqlist(ser_fd);
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
	if (!change_bitrate(ser_fd, input_freq, multilist, freqlist)) {
		fputs("set bitrate failed\n",stderr);
		goto error;
	}

	r =ser_fd;
 error:
	free(devicelist);
	free(clockmode);
	free(multilist);
	free(freqlist);
	if (r == -1 && ser_fd >= 0)
		close(ser_fd);
	return r;
}

/* get target rommap */
static int get_rominfo(int ser_fd, struct writeinfo_t *writeinfo)
{
	struct arealist_t *arealist = NULL;
	int c;
	
	/* get target rommap list */
	arealist = get_arealist(writeinfo->mat, ser_fd);
	if (arealist == NULL) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("area list error\n",stderr);
		return 0;
	}
	if (verbose) {
		printf("area map\n");
		for (c = 0; c < arealist->areas; c++)
			printf("%08lx - %08lx\n", arealist->area[c].start, 
                                                  arealist->area[c].end); 
	}

	/* check write area info */
	if (arealist->areas < SELAREA) {
		fputs("illigal areamap\n", stderr);
		free(arealist);
		return 0;
	}
	writeinfo->area = arealist->area[SELAREA];
	free(arealist);

	/* get writeing size */
	writeinfo->size = get_writesize(ser_fd);
	if (writeinfo->size < 0) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("writesize error\n",stderr);
		return 0;
	}
	VERBOSE_PRINT("writesize %d byte\n", writeinfo->size);
	return 1;
}

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

static int get_freq_num(const char *arg)
{
	int scale = 100;
	int period = 0;
	int val = 0;

	while (*arg && period < 3) {
		if (period > 0) {
			scale /= 10;
			period++;
		}
		if (*arg == '.') {
			period = 1;
			arg++;
			continue;
		}
		if (!isdigit(*arg))
			break ;
		val *= 10;
		val += *arg & 0x0f;
		arg++;
	}
	return val * scale;
}

int main(int argc, char *argv[])
{
	char ser_port[FILENAME_MAX] = DEFAULT_SERIAL;
	char lockname[FILENAME_MAX];
	int c;
	int long_index;
	int input_freq;
	int force_binary;
	int ser_fd;
	int lock_fd;
	int r;
	struct writeinfo_t writeinfo;

	writeinfo.mat = user;
	force_binary = 0;
	input_freq = 0;
	/* parse argment */
	while ((c = getopt_long(argc, argv, "p:f:bV", 
				long_options, &long_index)) >= 0) {
		switch (c) {
		case 'u':
			writeinfo.mat = userboot;
			break ;
		case 'p':
			strncpy(ser_port, optarg, sizeof(ser_port));
			ser_port[sizeof(ser_port) - 1] = '\0';
			break ;
		case 'f':
			input_freq = get_freq_num(optarg);
			break ;
		case 'b':
			force_binary = 1;
			break ;
		case 'V':
			verbose = 1;
			break ;
		case '?':
			usage();
			return 1;
		}
	}

	if (optind >= argc) {
		usage();
		return 1;
	}

	snprintf(lockname, sizeof(lockname), LOCKDIR "/LCK..%s", basename(ser_port));
	lock_fd = serial_lock(lockname);
	if (lock_fd == -1) {
		fputs(PROGNAME ": Serial port lock failed.\n",stderr);
		return 1;
	}

	r = 1;
	ser_fd = setup_connection(ser_port, input_freq);
	if(ser_fd < 0)
		goto error;
	puts("connect target");

	if(!get_rominfo(ser_fd, &writeinfo))
		goto error;

	if(writefile_to_rom(argv[optind], force_binary, &writeinfo, ser_fd)) {
		VERBOSE_PRINT("write %08lx - %08lx ", writeinfo.area.start, writeinfo.area.end);
		r = 0;
	}
 error:
	puts((r==0)?"done":"write failed");
	if (ser_fd >= 0)
		close(ser_fd);

	close(lock_fd);
	unlink(lockname);
	
	return r;
}
