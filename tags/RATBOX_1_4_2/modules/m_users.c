/*
 *  ircd-ratbox: an advanced Internet Relay Chat Daemon(ircd).
 *  m_users.c: Gives some basic user statistics.
 *
 *  Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center
 *  Copyright (C) 1996-2002 Hybrid Development Team
 *  Copyright (C) 2002-2004 ircd-ratbox development team
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
#include "handlers.h"
#include "client.h"
#include "ircd.h"
#include "numeric.h"
#include "s_serv.h"
#include "s_conf.h"
#include "send.h"
#include "msg.h"
#include "parse.h"
#include "modules.h"

static void m_users(struct Client *, struct Client *, int, char **);
static void mo_users(struct Client *, struct Client *, int, char **);

struct Message users_msgtab = {
	"USERS", 0, 0, 0, 0, MFLG_SLOW, 0,
	{m_unregistered, m_users, mo_users, mo_users}
};

#ifndef STATIC_MODULES
void
_modinit(void)
{
	mod_add_cmd(&users_msgtab);
}

void
_moddeinit(void)
{
	mod_del_cmd(&users_msgtab);
}

const char *_version = "$Revision$";
#endif

/*
 * m_users
 *      parv[0] = sender prefix
 *      parv[1] = servername
 */
static void
m_users(struct Client *client_p, struct Client *source_p, int parc, char *parv[])
{
	if(!ConfigServerHide.disable_remote)
	{
		if(hunt_server(client_p, source_p, ":%s USERS :%s", 1, parc, parv) != HUNTED_ISME)
			return;
	}

	sendto_one(source_p, form_str(RPL_LOCALUSERS), me.name, parv[0],
		   ConfigServerHide.hide_servers ? Count.total : Count.local,
		   ConfigServerHide.hide_servers ? Count.max_tot : Count.max_loc);

	sendto_one(source_p, form_str(RPL_GLOBALUSERS), me.name, parv[0],
		   Count.total, Count.max_tot);
}

/*
 * mo_users
 *      parv[0] = sender prefix
 *      parv[1] = servername
 */
static void
mo_users(struct Client *client_p, struct Client *source_p, int parc, char *parv[])
{
	if(hunt_server(client_p, source_p, ":%s USERS :%s", 1, parc, parv) == HUNTED_ISME)
	{
		if(!IsOper(source_p) && ConfigServerHide.hide_servers)
			sendto_one(source_p, form_str(RPL_LOCALUSERS), me.name, parv[0],
				   Count.total, Count.max_tot);
		else
			sendto_one(source_p, form_str(RPL_LOCALUSERS), me.name, parv[0],
				   Count.local, Count.max_loc);

		sendto_one(source_p, form_str(RPL_GLOBALUSERS), me.name, parv[0],
			   Count.total, Count.max_tot);
	}
}
