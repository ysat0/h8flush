/*
 *  Renesas CPU On-chip Flash memory writer
 *  common header
 *
 * Yoshinori Sato <ysato@users.sourceforge.jp>
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License version 2.1 (or later).
 */

/* -- configuration parametor ----------------- */

/* default using serialport */
#define DEFAULT_SERIAL "/dev/ttyS0"

/* use devicetype */
#define SELDEV 0
/* use clockmode */
#define SELCLK 0
/* write rom area */
#define SELAREA 0
/* serial lockfile directory */
#define LOCKDIR "/var/lock"

/* -------------------------------------------- */


#define PROGNAME "h8flash"

enum mat_t {user, userboot};

struct devinfo_t {
	char code[4];
	char name[256];
};

struct devicelist_t {
	int numdevs;
	struct devinfo_t devs[1];
};

struct clockmode_t {
	int nummode;
	int mode[0];
};

struct multirate_t {
	int numrate;
	int rate[0];
};

struct multilist_t {
	int nummulti;
	struct multirate_t muls[0];
};

struct freq_t {
	int min;
	int max;
};

struct freqlist_t {
	int numfreq;
	struct freq_t freq[0];
};

struct area_t {
	unsigned long start;
	unsigned long end;
};

struct arealist_t {
	int areas;
	struct area_t area[0];
};

struct writeinfo_t {
	enum mat_t mat;
	struct area_t area;
	int size;
};

int open_serial(const char *port);
int connect_target(int ser_fd);
struct devicelist_t *get_devicelist(int ser_fd);
struct clockmode_t *get_clockmode(int ser_fd);
struct multilist_t *get_multirate(int ser_fd);
struct freqlist_t *get_freqlist(int ser_fd);
struct arealist_t *get_arealist(enum mat_t mat, int ser_fd);
int get_writesize(int ser_fd);
int select_device(int ser_fd, const char *code);
int set_clockmode(int ser_fd, int mode);
int set_bitrate(int ser_fd, int bitrate, int freq, int coremul, int peripheralmul);
int write_rom(int ser_fd, const unsigned char *romimage, struct writeinfo_t *writeinfo);

extern int verbose;
