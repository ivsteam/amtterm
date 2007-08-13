#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>

#include "tcp.h"
#include "redir.h"

#define APPNAME "amtterm"
#define BUFSIZE 512

/* ------------------------------------------------------------------ */

static int recv_tty(void *cb_data, unsigned char *buf, int len)
{
//    struct redir *r = cb_data;

    return write(0, buf, len);
}

static void state_tty(void *cb_data, enum redir_state old, enum redir_state new)
{
    struct redir *r = cb_data;

    if (!r->verbose)
	return;

    fprintf(stderr, APPNAME " state: %s -> %s\n",
	    redir_strstate(old), redir_strstate(new));
    switch (new) {
    case REDIR_CONN_SOL:
	fprintf(stderr, "serial-over-lan redirection ok\n");
	fprintf(stderr, "connected now, use ^] to escape\n");
	break;
    default:
	break;
    }
}

static int redir_loop(struct redir *r)
{
    unsigned char buf[BUFSIZE+1];
    struct timeval tv;
    int rc;
    fd_set set;

    for(;;) {
	if (r->state == REDIR_CLOSED ||
	    r->state == REDIR_ERROR)
	    break;

	FD_ZERO(&set);
	if (r->state == REDIR_CONN_SOL)
	    FD_SET(0,&set);
	FD_SET(r->sock,&set);
	tv.tv_sec  = HEARTBEAT_INTERVAL * 4 / 1000;
	tv.tv_usec = 0;
	switch (select(r->sock+1,&set,NULL,NULL,&tv)) {
	case -1:
	    perror("select");
	    return -1;
	case 0:
	    fprintf(stderr,"select: timeout\n");
	    return -1;
	}
	
	if (FD_ISSET(0,&set)) {
	    /* stdin has data */
	    rc = read(0,buf,BUFSIZE);
	    switch (rc) {
	    case -1:
		perror("read(stdin)");
		return -1;
	    case 0:
		fprintf(stderr,"EOF from stdin\n");
		return -1;
	    default:
		if (buf[0] == 0x1d) {
		    if (r->verbose)
			fprintf(stderr, "\n" APPNAME ": saw ^], exiting\n");
		    redir_sol_stop(r);
		}
		if (-1 == redir_sol_send(r, buf, rc))
		    return -1;
		break;
	    }
	}

	if (FD_ISSET(r->sock,&set)) {
	    if (-1 == redir_data(r))
		return -1;
	}
    }
    return 0;
}

/* ------------------------------------------------------------------ */

struct termios  saved_attributes;
int             saved_fl;

static void tty_save(void)
{
    fcntl(0,F_GETFL,&saved_fl);
    tcgetattr (0, &saved_attributes);
}

static void tty_noecho(void)
{
    struct termios tattr;
    
    memcpy(&tattr,&saved_attributes,sizeof(struct termios));
    tattr.c_lflag &= ~(ECHO);
    tcsetattr (0, TCSAFLUSH, &tattr);
}

static void tty_raw(void)
{
    struct termios tattr;
    
    fcntl(0,F_SETFL,O_NONBLOCK);
    memcpy(&tattr,&saved_attributes,sizeof(struct termios));
    tattr.c_lflag &= ~(ISIG|ICANON|ECHO);
    tattr.c_cc[VMIN] = 1;
    tattr.c_cc[VTIME] = 0;
    tcsetattr (0, TCSAFLUSH, &tattr);
}

static void tty_restore(void)
{
    fcntl(0,F_SETFL,saved_fl);
    tcsetattr (0, TCSANOW, &saved_attributes);
}

/* ------------------------------------------------------------------ */

static void usage(FILE *fp)
{
    fprintf(fp,
            "\n"
	    "This is " APPNAME ", release " VERSION ", I'll establish\n"
	    "serial-over-lan (sol) connections to your Intel AMT boxes.\n"
            "\n"
            "usage: " APPNAME " [options] host [port]\n"
            "options:\n"
            "   -h            print this text\n"
            "   -v            verbose (default)\n"
            "   -q            quiet\n"
            "   -u user       username (default: admin)\n"
            "   -p pass       password (default: $AMT_PASSWORD)\n"
            "\n"
            "By default port 16994 is used.\n"
	    "If no password is given " APPNAME " will ask for one.\n"
            "\n"
            "-- \n"
            "(c) 2007 Gerd Hoffmann <kraxel@redhat.com>\n"
	    "\n");
}

int main(int argc, char *argv[])
{
    struct addrinfo ai;
    struct redir r;
    char *port = "16994";
    char *host = NULL;
    char *h;
    int c;

    memset(&r, 0, sizeof(r));
    r.verbose = 1;
    memcpy(r.type, "SOL ", 4);
    strcpy(r.user, "admin");

    r.cb_data  = &r;
    r.cb_recv  = recv_tty;
    r.cb_state = state_tty;

    if (NULL != (h = getenv("AMT_PASSWORD")))
	snprintf(r.pass, sizeof(r.pass), "%s", h);

    for (;;) {
        if (-1 == (c = getopt(argc, argv, "hvqu:p:")))
            break;
        switch (c) {
	case 'v':
	    r.verbose = 1;
	    break;
	case 'q':
	    r.verbose = 0;
	    break;
	case 'u':
	    snprintf(r.user, sizeof(r.user), "%s", optarg);
	    break;
	case 'p':
	    snprintf(r.pass, sizeof(r.pass), "%s", optarg);
	    memset(optarg,'*',strlen(optarg)); /* rm passwd from ps list */
	    break;

        case 'h':
            usage(stdout);
            exit(0);
        default:
            usage(stderr);
            exit(1);
        }
    }

    if (optind < argc)
	host = argv[optind];
    if (optind+1 < argc)
	port = argv[optind+1];
    if (NULL == host) {
	usage(stderr);
	exit(1);
    }

    tty_save();
    if (0 == strlen(r.pass)) {
	tty_noecho();
	fprintf(stderr, "AMT password for host %s: ", host);
	fgets(r.pass, sizeof(r.pass), stdin);
	fprintf(stderr, "\n");
	if (NULL != (h = strchr(r.pass, '\r')))
	    *h = 0;
	if (NULL != (h = strchr(r.pass, '\n')))
	    *h = 0;
    }

    memset(&ai, 0, sizeof(ai));
    ai.ai_socktype = SOCK_STREAM;
    ai.ai_family = PF_UNSPEC;
    tcp_verbose = r.verbose;
    r.sock = tcp_connect(&ai, NULL, NULL, host, port);
    if (-1 == r.sock) {
	tty_restore();
	exit(1);
    }

    tty_raw();
    redir_start(&r);
    redir_loop(&r);
    tty_restore();
    
    exit(0);
}
