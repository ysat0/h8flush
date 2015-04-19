/*
 *  Renesas CPU On-chip Flash memory writer
 *  main
 *
 * Yoshinori Sato <ysato@users.sourceforge.jp>
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License version 2.1 (or later).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <getopt.h>
#include <stddef.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#ifdef HAVE_GELF_H
#include <gelf.h>
#endif

#include "h8flash.h"

#define SREC_MAXLEN (256*2 + 4 + 1)

int verbose = 0;

const static struct option long_options[] = {
	{"userboot", no_argument, NULL, 'u'},
	{"port", required_argument, NULL, 'p'},
	{"freq", required_argument, NULL, 'f'},
	{"binary", no_argument, NULL, 'b'},
	{"verbose", no_argument, NULL, 'V'},
	{"list", no_argument, NULL, 'l'},
	{"endian", required_argument, NULL, 'e'},
	{0, 0, 0, 0}
};

static void usage(void)
{
	puts(PROGNAME "-f input clock frequency [-p port]]"
	     "[-b][--userboot][-l][-V] filename");
}

static struct area_t *lookup_area(struct arealist_t *arealist,
				  unsigned int addr)
{
	int i;
	for (i = 0; i < arealist->areas; i++) {
		if (arealist->area[i].start <= addr &&
		    arealist->area[i].end >= addr)
			return &arealist->area[i];
	}
	return NULL;
}

/* read raw binary */
static int write_binary(FILE *fp, struct comm_t *com,
			struct port_t *p, struct arealist_t *arealist,
			enum mat_t mat)
{
	int fno;
	struct stat bin_st;
	size_t bin_len, len;
	unsigned int addr;
	struct area_t *area;

	fno = fileno(fp);

	fstat(fno, &bin_st);
	bin_len = bin_st.st_size;
	addr = arealist->area[0].start;

	while(bin_len > 0) {
		area = lookup_area(arealist, addr);
		len = bin_len < (area->end - area->start)?
			bin_len:(area->end - area->start);
		if (len > read(area->image, len, fno))
			goto error_perror;
		bin_len -= len;
		addr += len;
	}
	if (!com->write_rom(p, arealist, mat))
		goto error;
	fclose(fp);
	return bin_len;
 error_perror:
	perror(PROGNAME);
 error:	
	fclose(fp);
	return 0;
}

/* read srec binary */
static int write_srec(FILE *fp, struct comm_t *com,
		      struct port_t *p, struct arealist_t *arealist,
		      enum mat_t mat)
{
	unsigned char *romimage = NULL;
	unsigned char *bufp;
	unsigned int last_addr = 0;
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
	struct area_t *area;

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

		area = lookup_area(arealist, addr);
		if (area == NULL) {
			fprintf(stderr, "%08x is out of ROM.", addr);
			goto error;
		}
		bufp = area->image + addr - area->start;

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
	r = com->write_rom(p, arealist, mat);
 error:
	fclose(fp);
	return r;
}

#ifdef HAVE_GELF_H
static int write_elf(FILE *fp, struct comm_t *com,
		     struct port_t *p, struct arealist_t *arealist,
		     enum mat_t mat)
{
	unsigned char *romimage = NULL;
	unsigned int romsize;
	int fd;
	size_t n;
	int i,j;
	Elf *elf = NULL;
	GElf_Phdr phdr;
	unsigned long top, last_addr = 0;
	int ret = -1;
	size_t sz, remain;
	struct area_t *area;
	
	elf_version(EV_CURRENT);
	fd = fileno(fp);
	elf = elf_begin(fd, ELF_C_READ, NULL);
	if (elf == NULL) {
		fputs(elf_errmsg(-1), stderr);
		goto error;
	}
	if(elf_kind(elf) != ELF_K_ELF) {
		fputs("Not ELF executable", stderr);
		goto error;
	}
	elf_getphdrnum(elf, &n);
	for (i = 0; i < n; i++) {
		if (gelf_getphdr(elf, i, &phdr) == NULL) {
			fputs(elf_errmsg(-1), stderr);
			goto error;
		}
		if (phdr.p_type != PT_LOAD)
			continue ;
		if (verbose) {
			printf("   offset   paddr    size\n");
			printf("%d: %08x %08x %08x\n",
			       i, phdr.p_offset, phdr.p_paddr, phdr.p_filesz);
		}
		if (phdr.p_filesz == 0)
			continue ;
		lseek(fd, phdr.p_offset, SEEK_SET);
		remain = phdr.p_filesz;
		top = phdr.p_paddr;
		while(remain > 0) {
			area = lookup_area(arealist, top);
			if (area == NULL) {
				fprintf(stderr, "%08x - %08x is out of ROM",
					top, top + remain);
				goto error;
			}
			j = remain < (area->end - area->start)?
				remain:(area->size);
			sz = read(fd, area->image + top - area->start, j);
			if (sz != j) {
				perror(PROGNAME);
				goto error;
			}
			remain -= j;
			top += j;
		}
	}
	ret = com->write_rom(p, arealist, mat);
error:
	if (elf)
		elf_end(elf);
	fclose(fp);
	return ret;
}		
#endif

/* read rom writing data */
static int writefile_to_rom(char *fn, int force_binary,
			    struct comm_t *com,
			    struct port_t *port,
			    struct arealist_t *arealist,
			    enum mat_t mat)
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
	if (fread(linebuf, sizeof(linebuf), 1, fp) < 0 && ferror(fp)) {
		fclose(fp);
		return -1;
	}
	fseek(fp,0,SEEK_SET);

#ifdef HAVE_GELF_H
	/* check ELF */
	if (!force_binary && memcmp(linebuf, ELFMAG, SELFMAG) == 0)
		return write_elf(fp, com, port, arealist, mat);
#endif
	/* check 'S??' */
	if (force_binary ||
	    linebuf[0] != 'S' ||
	    isdigit(linebuf[1]) == 0)
		return write_binary(fp, com, port, arealist, mat);

	/* check body (calcurate checksum) */
	memcpy(hexbuf, &linebuf[2], 2);
	sum = len = strtoul(hexbuf, NULL, 16);
	for (p = &linebuf[4]; len > 0; --len, p += 2) {
		memcpy(hexbuf, p, 2);
		sum += strtoul(hexbuf, NULL, 16);
	}
	if ((sum & 0xff) == 0xff)
		/* checksum ok is Srecord format */
		return write_srec(fp, com, port, arealist, mat);
	else
		return write_binary(fp, com, port, arealist, mat);
}

/* get target rommap */
static struct arealist_t *get_rominfo(struct comm_t *com, struct port_t *port,
				    enum mat_t mat)
{
	struct arealist_t *arealist = NULL;
	int c;
	
	/* get target rommap list */
	arealist = com->get_arealist(port, mat);
	if (arealist == NULL) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("area list error\n", stderr);
		return NULL;
	}
	if (verbose) {
		printf("area map\n");
		for (c = 0; c < arealist->areas; c++)
			printf("%08x - %08x %08xbyte\n",
			       arealist->area[c].start, 
			       arealist->area[c].end,
			       arealist->area[c].size); 
	}

	/* check write area info */
	if (arealist->areas < 0) {
		fputs("illigal areamap\n", stderr);
		free(arealist);
		return NULL;
	}
	return arealist;
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
	char port[FILENAME_MAX] = DEFAULT_SERIAL;
	int c;
	int long_index;
	int input_freq = 0;
	int force_binary = 0;
	int config_list = 0;
	int r;
	struct port_t *p = NULL;
	struct comm_t *com = NULL;
	struct arealist_t *arealist;
	enum mat_t mat = user;
	char endian='l';

	/* parse argment */
	while ((c = getopt_long(argc, argv, "p:f:bVle:", 
				long_options, &long_index)) >= 0) {
		switch (c) {
		case 'u':
			mat = userboot;
			break ;
		case 'p':
			strncpy(port, optarg, sizeof(port));
			port[sizeof(port) - 1] = '\0';
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
		case 'l':
			config_list = 1;
			break;
		case 'e':
			endian = optarg[0];
			if (endian != 'l' && endian !='b') {
				usage();
				return 1;
			}
			break;
		case '?':
			usage();
			return 1;
		}
	}

	if (optind >= argc && input_freq == 0 && !config_list) {
		usage();
		return 1;
	}

	r = 1;
#ifdef HAVE_USB_H
	if (strncasecmp(port, "usb", 3) == 0) {
		unsigned short vid = DEFAULT_VID;
		unsigned short pid = DEFAULT_PID;
		if (strlen(port) > 3) {
			if (sscanf("%04x:%04x", port + 3, &vid, &pid) != 2) {
				fputs("Unkonwn USB device id", stderr);
				goto error;
			}
		}
		p = open_usb(vid, pid);
	} else
		p = open_serial(port);
#else
	p = open_serial(port);
#endif
	if (p == NULL)
		goto error;

	switch (p->connect_target(port)) {
	case 0xff:
		r = -1;
		goto error;
	case 0xe6:
		VERBOSE_PRINT("Detect old protocol\n");
		com = comm_v1();
		break;
	case 0xc1:
		VERBOSE_PRINT("Detect new protocol\n");
		com = comm_v2();
		break;
	default:
		fputs("unknown_target", stderr);
		goto error;
	}

	if (config_list) {
		com->dump_configs(p);
		p->close();
		return 0;
	}

	if (com->setup_connection(p, input_freq, endian) < 0)
		goto error;
	puts("Connect target");

	if (!(arealist = get_rominfo(com, p, mat)))
		goto error;
		
	r = writefile_to_rom(argv[optind], force_binary, 
			     com, p, arealist, mat);
 error:
	puts((r==0)?"done":"write failed");
	if (p)
		p->close();
	return r;
}
