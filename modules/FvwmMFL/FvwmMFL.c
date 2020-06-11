/* FvwmMFL -- Fvwm3 Module Front Loader
 *
 * This program accepts listeners over a UDS for the purposes of receiving
 * information from FVWM3.
 *
 * Released under the same license as FVWM3 itself.
 */

#include "config.h"

#include "fvwm/fvwm.h"

#include "libs/Module.h"
#include "libs/safemalloc.h"
#include "libs/queue.h"
#include "libs/fvwmsignal.h"
#include "libs/vpacket.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <err.h>
#include <errno.h>
#include <stdbool.h>

#include <bson/bson.h>

#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_compat.h>
#include <event2/listener.h>
#include <event2/util.h>

/* XXX - should also be configurable via getenv() */
#define SOCK "/tmp/fvwm_mfl.sock"

const char *myname = "FvwmMFL";
static int debug;
struct fvwm_msg;

struct client {
	struct bufferevent	*comms;
	int			 flags;
	struct fvwm_msg		*fm;

	TAILQ_ENTRY(client)	 entry;
};

TAILQ_HEAD(clients, client);
struct clients  clientq;

struct fvwm_comms {
	int		 fd[2];
	struct event	*read_ev;
	ModuleArgs	*m;
};
struct fvwm_comms	 fc;

struct fvwm_msg {
	bson_t	*msg;
	int	 fw;
};

struct event_flag {
	const char	*event;
	int		 flag;
} etf[] = {
	{"new_window", M_ADD_WINDOW},
	{"configure_window", M_CONFIGURE_WINDOW},
	{"new_page", M_NEW_PAGE},
	{"new_desk", M_NEW_DESK},
	{"raise_window", M_RAISE_WINDOW},
	{"lower_window", M_LOWER_WINDOW},
	{"focus_change", M_FOCUS_CHANGE},
	{"destroy_window", M_DESTROY_WINDOW},
	{"iconify", M_ICONIFY},
	{"deiconify", M_DEICONIFY},
	{"window_name", M_WINDOW_NAME},
	{"icon_name", M_ICON_NAME},
	{"res_class", M_RES_CLASS},
	{"res_name", M_RES_NAME},
	{"icon_location", M_ICON_LOCATION},
	{"map", M_MAP},
	{"icon_file", M_ICON_FILE},
	{"window_shade", M_WINDOWSHADE},
	{"dewindow_shade", M_DEWINDOWSHADE},
	{"restack", M_RESTACK},
};


static void fvwm_read(int, short, void *);
static void broadcast_to_client(FvwmPacket *);
static void setup_signal_handlers(void);
static inline const char *flag_to_event(int);
static RETSIGTYPE HandleTerminate(int sig);
static int client_set_interest(struct client *, const char *);
static struct fvwm_msg *handle_packet(int, unsigned long *, unsigned long);
static struct fvwm_msg *fvwm_msg_new(void);
static void fvwm_msg_free(struct fvwm_msg *);

static struct fvwm_msg *
fvwm_msg_new(void)
{
	struct fvwm_msg		*fm;

	fm = fxcalloc(1, sizeof *fm);

	return (fm);
}

static void
fvwm_msg_free(struct fvwm_msg *fm)
{
	bson_destroy(fm->msg);
	free(fm);
}

static RETSIGTYPE
HandleTerminate(int sig)
{
	unlink(SOCK);
	fvwmSetTerminate(sig);
	SIGNAL_RETURN;
}

static void
setup_signal_handlers(void)
{
    struct sigaction  sigact;

    memset(&sigact, 0, sizeof sigact);

    sigemptyset(&sigact.sa_mask);
    sigaddset(&sigact.sa_mask, SIGTERM);
    sigaddset(&sigact.sa_mask, SIGINT);
    sigaddset(&sigact.sa_mask, SIGQUIT);
    sigaddset(&sigact.sa_mask, SIGPIPE);
    sigaddset(&sigact.sa_mask, SIGCHLD);
    sigact.sa_flags = SA_INTERRUPT; /* to interrupt ReadFvwmPacket() */
    sigact.sa_handler = HandleTerminate;

    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGPIPE, &sigact, NULL);
    sigaction(SIGCHLD, &sigact, NULL);
    sigaction(SIGQUIT, &sigact, NULL);
}

static struct fvwm_msg *
handle_packet(int type, unsigned long *body, unsigned long len)
{
	struct fvwm_msg		*fm = NULL;
	const char		*type_name = flag_to_event(type);
	char			 xwid[20];

	if (type_name == NULL)
		goto out;

	fm = fvwm_msg_new();

	sprintf(xwid, "0x%lx", body[0]);

	switch(type) {
	case M_ADD_WINDOW:
	case M_CONFIGURE_WINDOW: {
		struct ConfigWinPacket *cwp = (void *)body;
		fm->msg = BCON_NEW(type_name,
		  "{",
		    "window", BCON_UTF8(xwid),
		    "title_height", BCON_INT64(cwp->title_height),
		    "border_width", BCON_INT64(cwp->border_width),
		    "frame", "{",
		      "window", BCON_INT64(cwp->frame),
		      "x", BCON_INT64(cwp->frame_x),
		      "y", BCON_INT64(cwp->frame_y),
		      "width", BCON_INT64(cwp->frame_width),
		      "height", BCON_INT64(cwp->frame_height),
		    "}",
		    "hints",
		    "{",
			"base_width", BCON_INT64(cwp->hints_base_width),
			"base_height", BCON_INT64(cwp->hints_base_height),
			"inc_width", BCON_INT64(cwp->hints_width_inc),
			"inc_height", BCON_INT64(cwp->hints_height_inc),
			"orig_inc_width", BCON_INT64(cwp->orig_hints_width_inc),
			"orig_inc_height", BCON_INT64(cwp->orig_hints_height_inc),
			"min_width", BCON_INT64(cwp->hints_min_width),
			"min_height", BCON_INT64(cwp->hints_min_height),
			"max_width", BCON_INT64(cwp->hints_max_width),
			"max_height", BCON_INT64(cwp->hints_max_height),
		     "}",
		     "ewmh",
		      "{",
		        "layer", BCON_INT64(cwp->ewmh_hint_layer),
			"desktop", BCON_INT64(cwp->ewmh_hint_desktop),
			"window_type", BCON_INT64(cwp->ewmh_window_type),
		      "}",
		  "}"
		);
		return (fm);
	}
	case M_MAP:
	case M_LOWER_WINDOW:
	case M_RAISE_WINDOW:
	case M_DESTROY_WINDOW: {
		fm->msg = BCON_NEW(type_name,
		  "{",
		      "window", BCON_UTF8(xwid),
		  "}"
		);

		return (fm);
	}
	case M_FOCUS_CHANGE: {
		fm->msg = BCON_NEW(type_name,
		  "{",
		      "window", BCON_UTF8(xwid),
		      "type", BCON_INT64(body[2]),
		      "hilight",
		      "{",
		          "text_colour", BCON_INT64(body[3]),
			  "bg_colour", BCON_INT64(body[4]),
		      "}",
		  "}"
		);

		return (fm);
	}
	case M_WINDOW_NAME:
	case M_ICON_NAME:
	case M_RES_CLASS:
	case M_RES_NAME: {
		fm->msg = BCON_NEW(type_name,
		  "{",
		      "window", BCON_UTF8(xwid),
		      "name", BCON_UTF8((char *)&body[3]),
		  "}"
		);

		return (fm);
	}
	case M_NEW_DESK: {
		fm->msg = BCON_NEW(type_name,
		  "{",
		      "desk", BCON_INT64(body[0]),
		      "monitor_id", BCON_INT32(body[1]),
		  "}"
		);

		return (fm);
	}
	case M_NEW_PAGE: {
		fm->msg = BCON_NEW(type_name,
		  "{",
		      "virtual_scr",
		      "{",
		          "vx", BCON_INT64(body[0]),
			  "vy", BCON_INT64(body[1]),
			  "vx_pages", BCON_INT64(body[5]),
			  "vy_pages", BCON_INT64(body[6]),
			  "current_desk", BCON_INT64(body[2]),
		       "}",
		       "display",
		       "{",
		           "width", BCON_INT64(body[3]),
			   "height", BCON_INT64(body[4]),
		        "}",
			"monitor_id", BCON_INT32(body[7]),
		  "}"
		);

		return (fm);
	}
	default:
		break;
	}
out:
	fvwm_msg_free(fm);
	return (NULL);
}

static inline const char *
flag_to_event(int flag)
{
	size_t	 i;

	for (i = 0; i < (sizeof(etf) / sizeof(etf[0])); i++) {
		if (etf[i].flag & flag)
			return (etf[i].event);
	}

	return (NULL);
}

static inline bool
strsw(const char *pre, const char *str)
{
	return (strncmp(pre, str, strlen(pre)) == 0);
}

#define EFLAGSET	0x1
#define EFLAGUNSET	0x2

static int
client_set_interest(struct client *c, const char *event)
{
	size_t		i;
	int		flag_type = 0;
	bool		changed = false;
#define PRESET "set"
#define PREUNSET "unset"

	if (strsw(PRESET, event)) {
		event += strlen(PRESET) + 1;
		flag_type = EFLAGSET;
	} else if (strsw(PREUNSET, event)) {
		event += strlen(PREUNSET) + 1;
		flag_type = EFLAGUNSET;
	}

	if (strcmp(event, "debug") == 0) {
		debug = (flag_type == EFLAGSET) ? 1 : 0;

		/* Never send to FVWM3. */
		return (true);
	}

	for (i = 0; i < (sizeof(etf) / sizeof(etf[0])); i++) {
		if (strcmp(etf[i].event, event) == 0) {
			changed = true;
			if (flag_type == EFLAGSET)
				c->flags |= etf[i].flag;
			else
				c->flags &= ~etf[i].flag;
		}
	}

	SetSyncMask(fc.fd, c->flags);

	return (changed);
}

static void
broadcast_to_client(FvwmPacket *packet)
{
	struct client		*c;
	struct fvwm_msg		*fm;
	char			*as_json;
	size_t			 json_len;
	unsigned long		*body = packet->body;
	unsigned long		 type =	packet->type;
	unsigned long		 length = packet->size;

	TAILQ_FOREACH(c, &clientq, entry) {
		if (!(c->flags & type))
			continue;

		if ((fm = handle_packet(type, body, length)) == NULL)
			continue;

		as_json = bson_as_relaxed_extended_json(fm->msg, &json_len);
		if (as_json == NULL) {
			free(fm);
			continue;
		}
		c->fm = fm;

		bufferevent_write(c->comms, as_json, strlen(as_json));
		free(fm);
	}
}

static void
client_read_cb(struct bufferevent *bev, void *ctx)
{
	struct evbuffer	*input = bufferevent_get_input(bev);
	struct client	*c = (struct client *)ctx;
	size_t		 len = evbuffer_get_length(input);
	char		 *data = fxmalloc(len);

	evbuffer_copyout(input, data, len);

	/* Remove the newline if there is one. */
	if (data[strlen(data) - 1] == '\n')
		data[strlen(data) - 1] = '\0';

	if (*data == '\0')
		goto out;

	if (client_set_interest(c, data))
		goto out;

	SendText(fc.fd, data, c->fm->fw);

out:
	free(data);
	evbuffer_drain(input, len);
}

static void
client_write_cb(struct bufferevent *bev, void *ctx)
{
	struct client	*c = ctx;

	if (debug)
		fprintf(stderr, "Writing... (client %p)...\n", c);
}

static void client_err_cb(struct bufferevent *bev, short events, void *ctx)
{
	struct client	*c = (struct client *)ctx, *clook;

	if (events & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
		TAILQ_FOREACH(clook, &clientq, entry) {
			if (c == clook) {
				TAILQ_REMOVE(&clientq, c, entry);
				bufferevent_free(bev);
			}
		}
	}
}


static void
accept_conn_cb(struct evconnlistener *l, evutil_socket_t fd,
		struct sockaddr *add, int socklen, void *ctx)
{
	/* We got a new connection! Set up a bufferevent for it. */
	struct client		*c;
	struct event_base	*base = evconnlistener_get_base(l);

	c = fxmalloc(sizeof *c);

	c->comms = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);

	bufferevent_setcb(c->comms, client_read_cb, client_write_cb,
	    client_err_cb, c);
	bufferevent_enable(c->comms, EV_READ|EV_WRITE|EV_PERSIST);

	TAILQ_INSERT_TAIL(&clientq, c, entry);
}

static void accept_error_cb(struct evconnlistener *listener, void *ctx)
{
	struct event_base *base = evconnlistener_get_base(listener);
	int err = EVUTIL_SOCKET_ERROR();
	fprintf(stderr, "Got an error %d (%s) on the listener. "
		"Shutting down.\n", err, evutil_socket_error_to_string(err));

	event_base_loopexit(base, NULL);
}

static void
fvwm_read(int efd, short ev, void *data)
{
	FvwmPacket	*packet;

	SendUnlockNotification(fc.fd);

	if ((packet = ReadFvwmPacket(efd)) == NULL) {
		if (debug)
			fprintf(stderr, "Couldn't read from FVWM - exiting.\n");
		exit(0);
	}

	broadcast_to_client(packet);
}

int main(int argc, char **argv)
{
	struct event_base     *base;
	struct evconnlistener *fmd_cfd;
	struct sockaddr_un    sin;

	TAILQ_INIT(&clientq);

	setup_signal_handlers();

	if ((fc.m = ParseModuleArgs(argc, argv, 1)) == NULL) {
		fprintf(stderr, "%s must be started by FVWM3\n", myname);
		return (1);
	}

	/* Create new event base */
	if ((base = event_base_new()) == NULL) {
		fprintf(stderr, "Couldn't start libevent\n");
		return (1);
	}

	memset(&sin, 0, sizeof(sin));
	sin.sun_family = AF_LOCAL;
	strcpy(sin.sun_path, SOCK);

	/* Create a new listener */
	fmd_cfd = evconnlistener_new_bind(base, accept_conn_cb, NULL,
		  LEV_OPT_CLOSE_ON_FREE, -1,
		  (struct sockaddr *)&sin, sizeof(sin));
	if (fmd_cfd == NULL) {
		perror("Couldn't create listener");
		return 1;
	}
	evconnlistener_set_error_cb(fmd_cfd, accept_error_cb);

	/* Setup comms to fvwm3. */
	fc.fd[0] = fc.m->to_fvwm;
	fc.fd[1] = fc.m->from_fvwm;

	if (evutil_make_socket_nonblocking(fc.fd[0]) < 0)
		fprintf(stderr, "fvwm to_fvwm socket non-blocking failed");
	if (evutil_make_socket_nonblocking(fc.fd[1]) < 0)
		fprintf(stderr, "fvwm to_fvwm socket non-blocking failed");

	fc.read_ev = event_new(base, fc.fd[1], EV_READ|EV_PERSIST, fvwm_read, NULL);
	event_add(fc.read_ev, NULL);

	SendFinishedStartupNotification(fc.fd);

	event_base_dispatch(base);

	return (0);
}
