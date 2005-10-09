/* modules/m_xline.c
 * 
 *  Copyright (C) 2002-2003 Lee Hardy <lee@leeh.co.uk>
 *  Copyright (C) 2002-2005 ircd-ratbox development team
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1.Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * 2.Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * 3.The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * $Id$
 */

#include "stdinc.h"
#include "struct.h"
#include "send.h"
#include "client.h"
#include "class.h"
#include "ircd.h"
#include "numeric.h"
#include "s_log.h"
#include "s_serv.h"
#include "match.h"
#include "ircd_lib.h"
#include "parse.h"
#include "modules.h"
#include "s_conf.h"
#include "s_newconf.h"
#include "translog.h"

static int mo_xline(struct Client *client_p, struct Client *source_p, int parc, const char *parv[]);
static int me_xline(struct Client *client_p, struct Client *source_p, int parc, const char *parv[]);
static int mo_unxline(struct Client *client_p, struct Client *source_p, int parc, const char *parv[]);
static int me_unxline(struct Client *client_p, struct Client *source_p, int parc, const char *parv[]);

struct Message xline_msgtab = {
	"XLINE", 0, 0, 0, MFLG_SLOW,
	{mg_unreg, mg_not_oper, mg_ignore, mg_ignore, {me_xline, 5}, {mo_xline, 3}}
};
struct Message unxline_msgtab = {
	"UNXLINE", 0, 0, 0, MFLG_SLOW,
	{mg_unreg, mg_not_oper, mg_ignore, mg_ignore, {me_unxline, 2}, {mo_unxline, 2}}
};

mapi_clist_av1 xline_clist[] =  { &xline_msgtab, &unxline_msgtab, NULL };
DECLARE_MODULE_AV1(xline, NULL, NULL, xline_clist, NULL, NULL, "$Revision: 19295 $");

static int valid_xline(struct Client *, const char *, const char *);
static void apply_xline(struct Client *client_p, const char *name, 
			const char *reason, int temp_time);

static void remove_xline(struct Client *source_p, const char *gecos);


/* m_xline()
 *
 * parv[1] - thing to xline
 * parv[2] - optional type/reason
 * parv[3] - reason
 */
static int
mo_xline(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	struct ConfItem *aconf;
	const char *name;
	const char *reason;
	const char *target_server = NULL;
	int temp_time;
	int loc = 1;

	if(!IsOperXline(source_p))
	{
		sendto_one(source_p, POP_QUEUE, form_str(ERR_NOPRIVS),
			   me.name, source_p->name, "xline");
		return 0;
	}

	if((temp_time = valid_temp_time(parv[loc])) >= 0)
		loc++;
	/* we just set temp_time to -1! */
	else
		temp_time = 0;

	name = parv[loc];
	loc++;

	/* XLINE <gecos> ON <server> :<reason> */
	if(parc >= loc+2 && !irccmp(parv[loc], "ON"))
	{
		if(!IsOperRemoteBan(source_p))
		{
			sendto_one(source_p, POP_QUEUE, form_str(ERR_NOPRIVS),
				me.name, source_p->name, "remoteban");
			return 0;
		}

		target_server = parv[loc+1];
		loc += 2;
	}

	if(parc <= loc || EmptyString(parv[loc]))
	{
		sendto_one(source_p, POP_QUEUE, form_str(ERR_NEEDMOREPARAMS),
				me.name, source_p->name, "XLINE");
		return 0;
	}

	reason = parv[loc];

	if(target_server != NULL)
	{
		sendto_match_servs(source_p, target_server, CAP_ENCAP, NOCAPS,
				"ENCAP %s XLINE %d %s 2 :%s",
				target_server, temp_time, name, reason);

		if(!match(target_server, me.name))
			return 0;
	}
	else if(dlink_list_length(&cluster_conf_list) > 0)
		cluster_generic(source_p, "XLINE",
				(temp_time > 0) ? SHARED_TXLINE : SHARED_PXLINE,
				"%d %s 2 :%s",
				temp_time, name, reason);

	if((aconf = find_xline(name, 0)) != NULL)
	{
		sendto_one(source_p, POP_QUEUE, ":%s NOTICE %s :[%s] already X-Lined by [%s] - %s",
			   me.name, source_p->name, parv[1], aconf->host, aconf->passwd);
		return 0;
	}

	if(!valid_xline(source_p, name, reason))
		return 0;

	apply_xline(source_p, name, reason, temp_time);

	return 0;
}

static int
me_xline(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	struct ConfItem *aconf;
	const char *name, *reason;
	int temp_time;

	/* time name type :reason */
	if(!IsPerson(source_p))
		return 0;

	temp_time = atoi(parv[1]);
	name = parv[2];
	reason = parv[4];

	if(!find_shared_conf(source_p->username, source_p->host,
				source_p->user->server,
				(temp_time > 0) ? SHARED_TXLINE : SHARED_PXLINE))
		return 0;

	if(!valid_xline(source_p, name, reason))
		return 0;

	/* already xlined */
	if((aconf = find_xline(name, 0)) != NULL)
	{
		sendto_one(source_p, POP_QUEUE, ":%s NOTICE %s :[%s] already X-Lined by [%s] - %s",
				me.name, source_p->name, name, 
				aconf->host, aconf->passwd);
		return 0;
	}

	apply_xline(source_p, name, reason, temp_time);
	return 0;
}

/* valid_xline()
 *
 * inputs	- client xlining, gecos, reason and whether to warn
 * outputs	-
 * side effects - checks the xline for validity, erroring if needed
 */
static int
valid_xline(struct Client *source_p, const char *gecos,
	    const char *reason)
{
	if(EmptyString(reason))
	{
		sendto_one(source_p, POP_QUEUE, form_str(ERR_NEEDMOREPARAMS),
			   get_id(&me, source_p), 
			   get_id(source_p, source_p), "XLINE");
		return 0;
	}

	if(strchr(reason, ':') != NULL)
	{
		sendto_one_notice(source_p, POP_QUEUE,
				  ":Invalid character ':' in comment");
		return 0;
	}

	if(strchr(reason, '"'))
	{
		sendto_one_notice(source_p, POP_QUEUE,
				":Invalid character '\"' in comment");
		return 0;
	}

	if(!valid_wild_card_simple(gecos))
	{
		sendto_one_notice(source_p, POP_QUEUE,
				  ":Please include at least %d non-wildcard "
				  "characters with the xline",
				  ConfigFileEntry.min_nonwildcard_simple);
		return 0;
	}

	return 1;
}

/* check_xlines
 *
 * inputs       -
 * outputs      -
 * side effects - all clients will be checked for xlines
 */
static void
check_xlines(void)
{
	struct Client *client_p;
	struct ConfItem *aconf;
	dlink_node *ptr;
	dlink_node *next_ptr;

	DLINK_FOREACH_SAFE(ptr, next_ptr, lclient_list.head)
	{
		client_p = ptr->data;

		if(IsMe(client_p) || !IsPerson(client_p))
			continue;

		if((aconf = find_xline(client_p->info, 1)) != NULL)
		{
			if(IsExemptKline(client_p))
			{
				sendto_realops_flags(UMODE_ALL, L_ALL,
						     "XLINE over-ruled for %s, client is kline_exempt",
						     get_client_name(client_p, HIDE_IP));
				continue;
			}

			sendto_realops_flags(UMODE_ALL, L_ALL, "XLINE active for %s",
					     get_client_name(client_p, HIDE_IP));

			(void) exit_client(client_p, client_p, &me, "Bad user info");
			continue;
		}
	}
}

void
apply_xline(struct Client *source_p, const char *name, const char *reason,
		int temp_time)
{
	struct ConfItem *aconf;

	aconf = make_conf();
	aconf->status = CONF_XLINE;

	if(strstr(name, "\\s"))
	{
		char *tmp = LOCAL_COPY(name);
		char *orig = tmp;
		char *new = tmp;

		while(*orig)
		{
			if(*orig == '\\')
			{
				if(*(orig + 1) == 's')
				{
					*new++ = ' ';
					orig += 2;
				}
				/* otherwise skip that and the escaped
				 * character after it, so we dont mistake
				 * \\s as \s --fl
				 */
				else
				{
					*new++ = *orig++;
					*new++ = *orig++;
				}
			}
			else
				*new++ = *orig++;
		}

		*new = '\0';
		DupString(aconf->host, tmp);
	}
	else
		DupString(aconf->host, name);

	DupString(aconf->passwd, reason);
	collapse(aconf->host);

	if(temp_time > 0)
	{
		aconf->flags |= CONF_FLAGS_TEMPORARY;
		aconf->hold = ircd_currenttime + temp_time;

		sendto_realops_flags(UMODE_ALL, L_ALL,
			     "%s added temporary %d min. X-Line for [%s] [%s]",
			     get_oper_name(source_p), temp_time / 60,
			     aconf->host, reason);
		ilog(L_KLINE, "X %s %d %s %s",
			get_oper_name(source_p), temp_time / 60,
			name, reason);
		sendto_one_notice(source_p, POP_QUEUE, ":Added temporary %d min. X-Line [%s]",
				temp_time / 60, aconf->host);
	}
	else
	{
		translog_add_ban(TRANS_XLINE, source_p, aconf->host, "0", reason, NULL);

		sendto_realops_flags(UMODE_ALL, L_ALL, "%s added X-Line for [%s] [%s]",
				get_oper_name(source_p), 
				aconf->host, aconf->passwd);
		sendto_one_notice(source_p, POP_QUEUE, ":Added X-Line for [%s] [%s]",
					aconf->host, aconf->passwd);
		ilog(L_KLINE, "X %s 0 %s %s",
			get_oper_name(source_p), name, reason);
	}

	ircd_dlinkAddAlloc(aconf, &xline_conf_list);
	check_xlines();
}

/* mo_unxline()
 *
 * parv[1] - thing to unxline
 */
static int
mo_unxline(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	if(!IsOperXline(source_p))
	{
		sendto_one(source_p, POP_QUEUE, form_str(ERR_NOPRIVS),
			   me.name, source_p->name, "xline");
		return 0;
	}

	if(parc == 4 && !(irccmp(parv[2], "ON")))
	{
		if(!IsOperRemoteBan(source_p))
		{
			sendto_one(source_p, POP_QUEUE, form_str(ERR_NOPRIVS),
				me.name, source_p->name, "remoteban");
			return 0;
		}

		sendto_match_servs(source_p, parv[3], CAP_ENCAP, NOCAPS,
				"ENCAP %s UNXLINE %s",
				parv[3], parv[1]);

		if(match(parv[3], me.name) == 0)
			return 0;
	}
	else if(dlink_list_length(&cluster_conf_list))
		cluster_generic(source_p, "UNXLINE", SHARED_UNXLINE, 
				"%s", parv[1]);

	remove_xline(source_p, parv[1]);
	return 0;
}

static int
me_unxline(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	const char *name;

	/* name */
	if(!IsPerson(source_p))
		return 0;

	name = parv[1];

	if(!find_shared_conf(source_p->username, source_p->host,
				source_p->user->server, SHARED_UNXLINE))
		return 0;

	remove_xline(source_p, name);
	return 0;
}

static void
remove_xline(struct Client *source_p, const char *name)
{
	struct ConfItem *aconf;
	dlink_node *ptr;

	DLINK_FOREACH(ptr, xline_conf_list.head)
	{
		aconf = ptr->data;

		if(IsConfPermanent(aconf))
			continue;

		if(irccmp(aconf->host, name))
			continue;

		sendto_one_notice(source_p, POP_QUEUE, 
				":X-Line for [%s] is removed", name);
		sendto_realops_flags(UMODE_ALL, L_ALL,
				     "%s has removed the X-Line for: [%s]",
				     get_oper_name(source_p), name);
		ilog(L_KLINE, "UX %s %s", 
			get_oper_name(source_p), name);


		if((aconf->flags & CONF_FLAGS_TEMPORARY) == 0)
			translog_del_ban(TRANS_XLINE, name, NULL);

		free_conf(aconf);
		ircd_dlinkDestroy(ptr, &xline_conf_list);
		return;
	}
}

