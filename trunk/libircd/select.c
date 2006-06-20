/*
 *  ircd-ratbox: A slightly useful ircd.
 *  select.c: select() compatible network routines.
 *
 *  Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center
 *  Copyright (C) 1996-2002 Hybrid Development Team
 *  Copyright (C) 2001 Adrian Chadd <adrian@creative.net.au>
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
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 *  USA
 *
 *  $Id$
 */

#include "ircd_lib.h"

/*
 * Note that this is only a single list - multiple lists is kinda pointless
 * under select because the list size is a function of the highest FD :-)
 *   -- adrian
 */

static fd_set select_readfds;
static fd_set select_writefds;

/*
 * You know, I'd rather have these local to ircd_select but for some
 * reason my gcc decides that I can't modify them at all..
 *   -- adrian
 */
static fd_set tmpreadfds;
static fd_set tmpwritefds;

static int maxfd;
static void select_update_selectfds(fde_t *F, short event, PF * handler);

/* XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX */
/* Private functions */

/*
 * set and clear entries in the select array ..
 */
static void
select_update_selectfds(fde_t *F, short event, PF * handler)
{
	/* Update the read / write set */
	if(event & IRCD_SELECT_READ)
	{
		if(handler) 
		{
			FD_SET(F->fd, &select_readfds);
			F->pflags |= IRCD_SELECT_READ;		
		} 
		else 
		{
			FD_CLR(F->fd, &select_readfds);
			F->pflags &= ~IRCD_SELECT_READ;
		}
	}

	if(event & IRCD_SELECT_WRITE)
	{
		if(handler) 
		{
			FD_SET(F->fd, &select_writefds);
			F->pflags |= IRCD_SELECT_WRITE;
		}
		else 
		{
			FD_CLR(F->fd, &select_writefds);
			F->pflags &= ~IRCD_SELECT_WRITE;
		}
	}

	if(F->pflags & (IRCD_SELECT_READ|IRCD_SELECT_WRITE))
	{
		if(F->fd > maxfd)
		{
			maxfd = F->fd;		
		}
	} 
	else if(F->fd <= maxfd)
	{
		while(maxfd >= 0 && !FD_ISSET(F->fd, &select_readfds) && !FD_ISSET(F->fd, &select_writefds))
			maxfd--;		
	}
}


/* XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX */
/* Public functions */

int
ircd_setup_fd(int fd)
{
	return 0;	
}


/*
 * init_netio
 *
 * This is a needed exported function which will be called to initialise
 * the network loop code.
 */
void
init_netio(void)
{
	FD_ZERO(&select_readfds);
	FD_ZERO(&select_writefds);
}

/*
 * ircd_setselect
 *
 * This is a needed exported function which will be called to register
 * and deregister interest in a pending IO state for a given FD.
 */
void
ircd_setselect(int fd, unsigned int type, PF * handler,
	       void *client_data)
{
	fde_t *F = find_fd(fd);
	lircd_assert(fd >= 0);
	lircd_assert(F->flags.open);

	if(type & IRCD_SELECT_READ)
	{
		F->read_handler = handler;
		F->read_data = client_data;
		select_update_selectfds(F, IRCD_SELECT_READ, handler);
	}
	if(type & IRCD_SELECT_WRITE)
	{
		F->write_handler = handler;
		F->write_data = client_data;
		select_update_selectfds(F, IRCD_SELECT_WRITE, handler);
	}
}

/*
 * Check all connections for new connections and input data that is to be
 * processed. Also check for connections with data queued and whether we can
 * write it out.
 */

/*
 * ircd_select
 *
 * Do IO events
 */

int
ircd_select(unsigned long delay)
{
	int num;
	int fd;
	PF *hdl;
	fde_t *F;
	struct timeval to;

	/* Copy over the read/write sets so we don't have to rebuild em */
	memcpy(&tmpreadfds, &select_readfds, sizeof(fd_set));
	memcpy(&tmpwritefds, &select_writefds, sizeof(fd_set));

	for (;;)
	{
		to.tv_sec = 0;
		to.tv_usec = delay * 1000;
		num = select(maxfd + 1, &tmpreadfds, &tmpwritefds, NULL, &to);
		if(num >= 0)
			break;
		if(ignoreErrno(errno))
			continue;
		ircd_set_time();
		/* error! */
		return -1;
		/* NOTREACHED */
	}
	ircd_set_time();

	if(num == 0)
		return 0;

	/* XXX we *could* optimise by falling out after doing num fds ... */
	for (fd = 0; fd < maxfd + 1; fd++)
	{
		F = find_fd(fd);
		if(F == NULL)
			continue;
		if(FD_ISSET(fd, &tmpreadfds))
		{
			hdl = F->read_handler;
			F->read_handler = NULL;
			if(hdl)
				hdl(fd, F->read_data);
		}

		if(F->flags.open == 0)
			continue;	/* Read handler closed us..go on */

		if(FD_ISSET(fd, &tmpwritefds))
		{
			hdl = F->write_handler;
			F->write_handler = NULL;
			if(hdl)
				hdl(fd, F->write_data);
		}

		if(F->read_handler == NULL)
			select_update_selectfds(F, IRCD_SELECT_READ, NULL);
		if(F->write_handler == NULL)
			select_update_selectfds(F, IRCD_SELECT_WRITE, NULL);
	}
	return 0;
}

