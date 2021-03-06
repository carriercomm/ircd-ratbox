/*
 * resolver.c: dns resolving daemon for ircd-ratbox
 * Based on many things ripped from ratbox-services
 * and ircd-ratbox itself and who knows what else
 *
 * Copyright (C) 2003-2005 Lee Hardy <leeh@leeh.co.uk>
 * Copyright (C) 2003-2005 ircd-ratbox development team
 * Copyright (C) 2005 Aaron Sethman <androsyn@ratbox.org>
 *
 * $Id$
 */

#define READBUF_SIZE    16384

#include "ratbox_lib.h"
#include "internal.h"


#define MAXPARA 10
#define REQIDLEN 10

#define REQREV 0
#define REQFWD 1

#define REVIPV4 0
#define REVIPV6 1
#define REVIPV6INT 2
#define REVIPV6FALLBACK 3
#define FWDHOST 4

#define EmptyString(x) (!(x) || (*(x) == '\0'))

static rb_helper *res_helper;
static int do_rehash;
static void dns_readable(rb_fde_t * F, void *ptr);
static void dns_writeable(rb_fde_t * F, void *ptr);
static void process_adns_incoming(void);

static char readBuf[READBUF_SIZE];
static void resolve_ip(char **parv);
static void resolve_host(char **parv);


struct dns_request
{
	char reqid[REQIDLEN];
	int reqtype;
	int revfwd;
	adns_query query;
	union
	{
#ifdef RB_IPV6
		struct sockaddr_in6 in6;
#endif
		struct sockaddr_in in;
	} sins;
#ifdef RB_IPV6
	int fallback;
#endif
};

static adns_state dns_state;

/* void dns_select(void)
 * Input: None.
 * Output: None
 * Side effects: Re-register ADNS fds with the fd system. Also calls the
 *               callbacks into core ircd.
 */
static void
dns_select(void)
{
	struct adns_pollfd pollfds[MAXFD_POLL];
	int npollfds, i, fd;
	npollfds = adns__pollfds(dns_state, pollfds);
	for (i = 0; i < npollfds; i++)
	{
		fd = pollfds[i].fd;
		if(pollfds[i].events & ADNS_POLLIN)
			rb_setselect(rb_get_fde(fd), RB_SELECT_READ, dns_readable, NULL);
		if(pollfds[i].events & ADNS_POLLOUT)
			rb_setselect(rb_get_fde(fd), RB_SELECT_WRITE, dns_writeable, NULL);
	}
}

/* void dns_readable(int fd, void *ptr)
 * Input: An fd which has become readable, ptr not used.
 * Output: None.
 * Side effects: Read DNS responses from DNS servers.
 * Note: Called by the fd system.
 */
static void
dns_readable(rb_fde_t * F, void *ptr)
{
	adns_processreadable(dns_state, rb_get_fd(F), rb_current_time_tv());
	process_adns_incoming();
	dns_select();
}

/* void dns_writeable(int fd, void *ptr)
 * Input: An fd which has become writeable, ptr not used.
 * Output: None.
 * Side effects: Write any queued buffers out.
 * Note: Called by the fd system.
 */
static void
dns_writeable(rb_fde_t * F, void *ptr)
{
	adns_processwriteable(dns_state, rb_get_fd(F), rb_current_time_tv());
	process_adns_incoming();
	dns_select();
}

static void
rehash(int sig)
{
	do_rehash = 1;
}

static void
restart_resolver(void)
{
	/* Rehash dns configuration */
	adns__rereadconfig(dns_state);
}


static void
dummy_handler(int sig)
{
	return;
}

static void
setup_signals()
{
	struct sigaction act;

	act.sa_flags = 0;
	act.sa_handler = SIG_IGN;
	sigemptyset(&act.sa_mask);
	sigaddset(&act.sa_mask, SIGPIPE);
	sigaddset(&act.sa_mask, SIGALRM);
#ifdef SIGTRAP
	sigaddset(&act.sa_mask, SIGTRAP);
#endif

#ifdef SIGWINCH
	sigaddset(&act.sa_mask, SIGWINCH);
	sigaction(SIGWINCH, &act, 0);
#endif
	sigaction(SIGPIPE, &act, 0);
#ifdef SIGTRAP
	sigaction(SIGTRAP, &act, 0);
#endif

	act.sa_handler = dummy_handler;
	sigaction(SIGALRM, &act, 0);

	act.sa_handler = rehash;
	sigaddset(&act.sa_mask, SIGHUP);
	sigaction(SIGHUP, &act, 0);

}

static void
error_cb(rb_helper * helper)
{
	exit(1);
}

/*
request protocol:

INPUTS:

IPTYPE:    4, 5,  6, ipv4, ipv6.int/arpa, ipv6 respectively
requestid: identifier of the request
 

RESIP  requestid IPTYPE IP 
RESHST requestid IPTYPE hostname

OUTPUTS:
ERR error string = daemon failed and is going to shutdown
otherwise

FWD requestid PASS/FAIL hostname or reason for failure
REV requestid PASS/FAIL IP or reason

*/


static void
parse_request(rb_helper * helper)
{
	int len;
	static char *parv[MAXPARA + 1];
	int parc;
	while ((len = rb_helper_read(helper, readBuf, sizeof(readBuf))) > 0)
	{
		parc = rb_string_to_array(readBuf, parv, MAXPARA);
		if(parc != 4)
			exit(1);
		switch (*parv[0])
		{
		case 'I':
			resolve_ip(parv);
			break;
		case 'H':
			resolve_host(parv);
			break;
		default:
			break;
		}
	}


}


static void
send_answer(struct dns_request *req, adns_answer * reply)
{
	char response[64];
	int result = 0;
	int aftype = 0;
	if(reply && reply->status == adns_s_ok)
	{
		switch (req->revfwd)
		{
		case REQREV:
			{
				if(strlen(*reply->rrs.str) < 63)
				{
					strcpy(response, *reply->rrs.str);
					result = 1;
				}
				else
				{
					strcpy(response, "HOSTTOOLONG");
					result = 0;
				}
				break;
			}

		case REQFWD:
			{
				switch (reply->type)
				{
#ifdef RB_IPV6
				case adns_r_addr6:
					{
						char tmpres[65];
						rb_inet_ntop(AF_INET6,
							     &reply->rrs.addr->addr.inet6.sin6_addr,
							     tmpres, sizeof(tmpres) - 1);
						aftype = 6;
						if(*tmpres == ':')
						{
							strcpy(response, "0");
							strcat(response, tmpres);
						}
						else
							strcpy(response, tmpres);
						result = 1;
						break;
					}
#endif
				case adns_r_addr:
					{
						result = 1;
						aftype = 4;
						rb_inet_ntop(AF_INET,
							     &reply->rrs.addr->addr.inet.sin_addr,
							     response, sizeof(response));
						break;
					}
				default:
					{
						strcpy(response, "FAILED");
						result = 0;
						aftype = 0;
						break;
					}
				}
				break;
			}
		default:
			{
				exit(1);
			}
		}

	}
	else
	{
#ifdef RB_IPV6
		if(req->revfwd == REQREV && req->reqtype == REVIPV6FALLBACK && req->fallback == 0)
		{
			req->fallback = 1;
			result = adns_submit_reverse(dns_state,
						     (struct sockaddr *) &req->sins.in6,
						     adns_r_ptr_ip6_old,
						     adns_qf_owner | adns_qf_cname_loose |
						     adns_qf_quoteok_anshost, req, &req->query);
			rb_free(reply);
			if(result != 0)
			{
				rb_helper_write(res_helper, "%s 0 FAILED", req->reqid);
				rb_free(reply);
				rb_free(req);

			}
			return;
		}
#endif
		strcpy(response, "FAILED");
		result = 0;
	}
	rb_helper_write(res_helper, "%s %d %d %s\n", req->reqid, result, aftype, response);
	rb_free(reply);
	rb_free(req);
}


static void
process_adns_incoming(void)
{
	adns_query q, r;
	adns_answer *answer;
	struct dns_request *req;

	adns_forallqueries_begin(dns_state);
	while ((q = adns_forallqueries_next(dns_state, (void *) &r)) != NULL)
	{
		switch (adns_check(dns_state, &q, &answer, (void *) &req))
		{
		case EAGAIN:
			continue;
		case 0:
			send_answer(req, answer);
			continue;
		default:
			if(answer != NULL && answer->status == adns_s_systemfail)
				exit(2);
			send_answer(req, NULL);
			break;
		}

	}
}


static void
resolve_host(char **parv)
{
	struct dns_request *req;
	char *requestid = parv[1];
	char *iptype = parv[2];
	char *rec = parv[3];
	int result;
	int flags;
	req = rb_malloc(sizeof(struct dns_request));
	strcpy(req->reqid, requestid);

	req->revfwd = REQFWD;
	req->reqtype = FWDHOST;
	switch (*iptype)
	{
#ifdef RB_IPV6
	case '5':		/* I'm not sure why somebody would pass a 5 here, but okay */
	case '6':
		flags = adns_r_addr6;
		break;
#endif
	default:
		flags = adns_r_addr;
		break;
	}
	result = adns_submit(dns_state, rec, flags, adns_qf_owner, req, &req->query);
	if(result != 0)
	{
		/* Failed to even submit */
		send_answer(req, NULL);
	}

}


static void
resolve_ip(char **parv)
{
	char *requestid = parv[1];
	char *iptype = parv[2];
	char *rec = parv[3];
	struct dns_request *req;

	int result;
	int flags = adns_r_ptr;


	if(strlen(requestid) >= REQIDLEN)
	{
		exit(3);
	}
	req = rb_malloc(sizeof(struct dns_request));
	req->revfwd = REQREV;
	strcpy(req->reqid, requestid);
	switch (*iptype)
	{
	case '4':
		flags = adns_r_ptr;
		req->reqtype = REVIPV4;
		if(!rb_inet_pton(AF_INET, rec, &req->sins.in.sin_addr))
			exit(6);
		req->sins.in.sin_family = AF_INET;

		break;
#ifdef RB_IPV6
	case '5':		/* This is the case of having to fall back to "ip6.int" */
		req->reqtype = REVIPV6FALLBACK;
		flags = adns_r_ptr_ip6;
		if(!rb_inet_pton(AF_INET6, rec, &req->sins.in6.sin6_addr))
			exit(6);
		req->sins.in6.sin6_family = AF_INET6;
		req->fallback = 0;
		break;
	case '6':
		req->reqtype = REVIPV6;
		flags = adns_r_ptr_ip6;
		if(!rb_inet_pton(AF_INET6, rec, &req->sins.in6.sin6_addr))
			exit(6);
		req->sins.in6.sin6_family = AF_INET6;
		break;
#endif
	default:
		exit(7);
	}

	result = adns_submit_reverse(dns_state,
				     (struct sockaddr *) &req->sins,
				     flags,
				     adns_qf_owner | adns_qf_cname_loose |
				     adns_qf_quoteok_anshost, req, &req->query);

	if(result != 0)
	{
		send_answer(req, NULL);
	}

}

static void
timeout_adns(void *ptr)
{
	adns_processtimeouts(dns_state, rb_current_time_tv());
	process_adns_incoming();
}

static void
check_rehash(void *unused)
{
	if(do_rehash)
	{
		restart_resolver();
		do_rehash = 0;
	}
}

int
main(int argc, char **argv)
{
	res_helper = rb_helper_child(parse_request, error_cb, NULL, NULL, NULL, 256, 1024, 256, 256);	/* XXX fix me */

	if(res_helper == NULL)
	{
		fprintf(stderr,
			"This is ircd-ratbox resolver.  You know you aren't supposed to run me directly?\n");
		fprintf(stderr,
			"You get an Id tag for this: $Id$\n");
		fprintf(stderr, "Have a nice life\n");
		exit(1);
	}

	adns_init(&dns_state, adns_if_noautosys, 0);
	rb_set_time();
	setup_signals();
	rb_event_add("timeout_adns", timeout_adns, NULL, 5);
	rb_event_add("check_rehash", check_rehash, NULL, 5);
	dns_select();
	rb_helper_loop(res_helper, 0);
	return 1;
}
