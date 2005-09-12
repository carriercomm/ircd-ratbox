/*
 *  ircd-ratbox: A slightly useful ircd.
 *  m_whois.c: Shows who a user was.
 *
 *  Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center
 *  Copyright (C) 1996-2002 Hybrid Development Team
 *  Copyright (C) 2002-2005 ircd-ratbox development team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *  USA
 *
 *  $Id$
 */

#include "stdinc.h"
#include "struct.h"
#include "whowas.h"
#include "client.h"
#include "irc_string.h"
#include "ircd.h"
#include "numeric.h"
#include "send.h"
#include "s_conf.h"
#include "parse.h"
#include "modules.h"

static int m_whowas(struct Client *, struct Client *, int, const char **);

struct Message whowas_msgtab = {
	"WHOWAS", 0, 0, 0, MFLG_SLOW,
	{mg_unreg, {m_whowas, 2}, mg_ignore, mg_ignore, mg_ignore, {m_whowas, 2}}
};

mapi_clist_av1 whowas_clist[] = { &whowas_msgtab, NULL };
DECLARE_MODULE_AV1(whowas, NULL, NULL, whowas_clist, NULL, NULL, "$Revision: 19256 $");

/*
** m_whowas
**      parv[0] = sender prefix
**      parv[1] = nickname queried
*/
static int
m_whowas(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{	
	struct Whowas *temp;
	int cur = 0;
	int max = -1, found = 0;
	char *p;
	const char *nick;

	static time_t last_used = 0L;

	if(!IsOper(source_p))
	{
		if((last_used + ConfigFileEntry.pace_wait_simple) > ircd_currenttime)
		{
			sendto_one(source_p, HOLD_QUEUE, form_str(RPL_LOAD2HI),
				   me.name, source_p->name, "WHOWAS");
			sendto_one(source_p, POP_QUEUE, form_str(RPL_ENDOFWHOWAS),
				   me.name, source_p->name, parv[1]);
			return 0;
		}
		else
			last_used = ircd_currenttime;
	}


	if(parc > 2)
		max = atoi(parv[2]);

#if 0
	if(parc > 3)
		if(hunt_server(client_p, source_p, ":%s WHOWAS %s %s :%s", 3, parc, parv))
			return 0;
#endif

	if((p = strchr(parv[1], ',')))
		*p = '\0';

	nick = parv[1];

	temp = WHOWASHASH[hash_whowas_name(nick)];
	found = 0;
	for (; temp; temp = temp->next)
	{
		if(!irccmp(nick, temp->name))
		{
			sendto_one(source_p, HOLD_QUEUE, form_str(RPL_WHOWASUSER),
				   me.name, source_p->name, temp->name,
				   temp->username, temp->hostname, temp->realname);

			sendto_one_numeric(source_p, HOLD_QUEUE, RPL_WHOISSERVER,
					   form_str(RPL_WHOISSERVER),
					   temp->name, temp->servername,
					   myctime(temp->logoff));
			cur++;
			found++;
		}
		if(max > 0 && cur >= max)
			break;
	}

	if(!found)
		sendto_one(source_p, HOLD_QUEUE, form_str(ERR_WASNOSUCHNICK), 
			   me.name, source_p->name, nick);

	sendto_one(source_p, POP_QUEUE, form_str(RPL_ENDOFWHOWAS), 
		   me.name, source_p->name, parv[1]);
	return 0;
}
