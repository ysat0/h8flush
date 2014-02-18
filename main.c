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
	{0, 0, 0, 0}
};

static void usage(void)
{
	puts(PROGNAME "-f input clock frequency [-p port]]"
	     "[-b][--userboot][-l][-V] filename");
}

/* read raw binary */
static int write_binary(FILE *fp, struct writeinfo_t *writeinfo,
			struct port_t *p)
{
	int fno;
	struct stat bin_st;
	unsigned char *bin_buf;
	unsigned long bin_len;

	fno = fileno(fp);

	fstat(fno, &bin_st);
	bin_len = bin_st.st_size;

	if ((bin_buf = (unsigned char *)mmap(NULL, bin_len, PROT_READ,
					     MAP_SHARED, fno, 0)) == MAP_FAILED)
		goto error_perror;
	writeinfo->area.end = bin_len - 1;
	if (!write_rom(p, bin_buf, writeinfo))
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
static int write_srec(FILE *fp, struct writeinfo_t *writeinfo, struct port_t *p)
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
	unsigned int romsize;

	romsize = writeinfo->area.end - writeinfo->area.start + 1;
	romimage = (unsigned char *)malloc(romsize);
	if (!romimage) {
		perror(PROGNAME);
		goto error;
	}
	memset(romimage, 0xff, romsize);

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
			fprintf(stderr,
				"srec address %08x is out of rom\n", addr);
			goto error;
		}
		bufp = romimage + addr - writeinfo->area.start;
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
	r = write_rom(p, romimage, writeinfo);
 error:
	free(romimage);
	fclose(fp);
	return r;
}

#ifdef HAVE_ELF_H
static int writefile_elf(FILE *fp, struct writeinfo_t *writeinfo,
			 struct port_t *p)
{
	unsigned char *romimage = NULL;
	unsigned int romsize;
	int fd;
	int n;
	int i;
	Elf *elf = NULL;
	GElf_Phdr phdr;
	unsigned long last_addr = 0;
	int ret = -1;

	romsize = writeinfo->area.end - writeinfo->area.start + 1;
	romimage = (unsigned char *)malloc(romsize);
	if (!romimage) {
		perror(PROGNAME);
		goto error;
	}
	memset(romimage, 0xff, romsize);
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
		if (gelf_getphdr(elf, &phdr) == NULL) {
			fputs(elf_errmsg(-1), stderr);
			goto error;
		}
		if (verbose) {
			printf("   offset   paddr    size\n");
			printf("%d: %08x %08x %08x\n",
			       n, phdr.p_offset, phdr.p_pddr, phdr,p_filesz);
		}
		if (phdr.p_paddr < writeinfo->area.start ||
		    (phdr.p_paddr + phdr.p_filesz) > writeinfo->area.end) {
			fprintf("%08x - %08x is out of rom",
				phdr.p_pddr, phdr.p_pddr + phdr,p_filesz);
			goto error;
		}
		lseek(fd, phdr.p_offset, SEEK_SET);
		sz = read(fd, romimage + (phdr.p_paddr - writeinfo->area.start),
			  phdr.p_filesz);
		if (sz != phdr.p_filesz) {
			fputs("File read error", stderr);
			goto error;
		}
		if (last_addr < (phdr.p_paddr + pdhr.filesz))
			last_addr = phdr.p_paddr + pdhr.filesz;
	}
	writeinfo->area.end = last_addr;
	ret = write_rom(p, romimage, writeinfo);
error:
	if (elf)
		elf_end(elf);
	fclose(fp);
	free(romimage);
	return ret;
}		
#endif

/* read rom writing data */
static int writefile_to_rom(char *fn, int force_binary, struct writeinfo_t *writeinfo, 
			    struct port_t *port)
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

#ifdef HAVE_ELF_H
	/* check ELF */
	if (!force_binary && memcmp(linebuf, "\x7fELF", 4) == 0)
		return write_elf(fp, writeinfo, port)
#endif
	/* check 'S??' */
	if (force_binary ||
	    linebuf[0] != 'S' ||
	    isdigit(linebuf[1]) == 0)
		return write_binary(fp, writeinfo, port);

	/* check body (calcurate checksum) */
	memcpy(hexbuf, &linebuf[2], 2);
	sum = len = strtoul(hexbuf, NULL, 16);
	for (p = &linebuf[4]; len > 0; --len, p += 2) {
		memcpy(hexbuf, p, 2);
		sum += strtoul(hexbuf, NULL, 16);
	}
	if ((sum & 0xff) == 0xff)
		/* checksum ok is Srecord format */
		return write_srec(fp, writeinfo, port);
	else
		return write_binary(fp, writeinfo, port);
}

/* get target rommap */
static int get_rominfo(struct port_t *port, struct writeinfo_t *writeinfo)
{
	struct arealist_t *arealist = NULL;
	int c;
	
	/* get target rommap list */
	arealist = get_arealist(port, writeinfo->mat);
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
			printf("%08x - %08x\n", arealist->area[c].start, 
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
	writeinfo->size = get_writesize(port);
	if (writeinfo->size < 0) {
		if (errno != 0)
			perror(PROGNAME);
		else
			fputs("writesize error\n", stderr);
		return 0;
	}
	VERBOSE_PRINT("writesize %d byte\n", writeinfo->size);
	return 1;
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
	int input_freq;
	int force_binary;
	int config_list;
	int r;
	struct writeinfo_t writeinfo;
	struct port_t *p = NULL;

	writeinfo.mat = user;
	force_binary = 0;
	input_freq = 0;
	config_list = 0;

	/* parse argment */
	while ((c = getopt_long(argc, argv, "p:f:bVl", 
				long_options, &long_index)) >= 0) {
		switch (c) {
		case 'u':
			writeinfo.mat = userboot;
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
		case '?':
			usage();
			return 1;
		}
	}

	if (optind >= argc || !config_list || input_freq == 0) {
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

	if (config_list) {
		dump_configs(p);
		p->close();
		return 0;
	}

	if (setup_connection(p, input_freq) < 0)
		goto error;
	puts("Connect target");

	if (!get_rominfo(p, &writeinfo))
		goto error;
		
	if (writefile_to_rom(argv[optind], force_binary, 
						&writeinfo, p)) {
		VERBOSE_PRINT("write %08x - %08x ", writeinfo.area.start, 
			      writeinfo.area.end);
		r = 0;
	}
 error:
	puts((r==0)?"done":"write failed");
	if (p)
		p->close();
	return r;
}
