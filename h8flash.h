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

#define DEFAULT_VID 0x045b
#define DEFAULT_PID 0x0025

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
#define VERBOSE_PRINT(...) do { if (verbose) printf(__VA_ARGS__); } while(0)

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
#define WRITE                0x50

enum mat_t {user, userboot};

struct devinfo_t {
	char code[4];
	char name[256];
};

struct devicelist_t {
	int numdevs;
	struct devinfo_t devs[0];
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
	struct multirate_t *muls[0];
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
	unsigned int start;
	unsigned int end;
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

enum port_type {serial, usb};

struct port_t {
	enum port_type type;
	char *dev;
	int (*connect_target)(char *port);
	int (*send_data)(const unsigned char *data, int len);
	int (*receive_byte)(unsigned char *data);
	int (*setbaud)(int bitrate);
	void (*close)(void);
};

struct port_t *open_serial(char *portname);
struct port_t *open_usb(unsigned short vid, unsigned short pid);
struct devicelist_t *get_devicelist(struct port_t *port);
struct clockmode_t *get_clockmode(struct port_t *port);
struct multilist_t *get_multirate(struct port_t *port);
struct freqlist_t *get_freqlist(struct port_t *port);
struct arealist_t *get_arealist(struct port_t *port, enum mat_t mat);
int get_writesize(struct port_t *port);
int select_device(struct port_t *port, const char *code);
int set_clockmode(struct port_t *port, int mode);
int write_rom(struct port_t *port, const unsigned char *romimage, 
	      struct writeinfo_t *writeinfo);
int setup_connection(struct port_t *port, int input_freq);
void dump_configs(struct port_t *p);

extern int verbose;
