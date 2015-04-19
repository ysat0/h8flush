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

enum mat_t {user, userboot};

struct area_t {
	unsigned int start;
	unsigned int end;
	int size;
	char *image;
};

struct arealist_t {
	int areas;
	struct area_t area[0];
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

struct comm_t {
	struct arealist_t *(*get_arealist)(struct port_t *port, enum mat_t mat);
	int (*write_rom)(struct port_t *port, struct arealist_t *arealist,
			 enum mat_t mat);
	int (*setup_connection)(struct port_t *port, int input_freq, char endian);
	void (*dump_configs)(struct port_t *p);
};

struct port_t *open_serial(char *portname);
struct port_t *open_usb(unsigned short vid, unsigned short pid);
struct comm_t *comm_v1();
struct comm_t *comm_v2();

extern int verbose;
