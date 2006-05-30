/*
 *  ircd-ratbox: A slightly useful ircd
 *  helper.c: Starts and deals with ircd helpers
 *  
 *  Copyright (C) 2006 Aaron Sethman <androsyn@ratbox.org>
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

/* setup all the stuff a new child needs */
ircd_helper *
ircd_helper_child(ircd_helper_cb *read_cb, ircd_helper_cb *error_cb, log_cb *ilog, restart_cb *irestart, die_cb *idie, 
		  int maxcon, size_t lb_heap_size, size_t dh_size)
{
	ircd_helper *helper;
	int maxfd, x = 0;
	char *tifd, *tofd, *tmaxfd;
	
	tifd = getenv("IFD");
	tofd = getenv("OFD");
	tmaxfd = getenv("MAXFD");
	
	if(tifd == NULL || tofd == NULL || tmaxfd == NULL)
		return NULL;

	helper = ircd_malloc(sizeof(ircd_helper));
	helper->ifd = (int)strtol(tifd, NULL, 10);     
	helper->ofd = (int)strtol(tofd, NULL, 10);
	maxfd = (int)strtol(tmaxfd, NULL, 10);

#ifndef _WIN32
        for(x = 0; x < maxfd; x++)
        {
        	if(x != helper->ifd && x != helper->ofd)
        		close(x);
	}
#endif

	ircd_lib(ilog, irestart, idie, 0, maxfd, lb_heap_size, dh_size);
	ircd_linebuf_newbuf(&helper->sendq);
	ircd_linebuf_newbuf(&helper->recvq);

	ircd_open(helper->ifd, FD_PIPE, "incoming connection");
	ircd_open(helper->ofd, FD_PIPE, "outgoing connection");
	ircd_set_nb(helper->ifd);
	ircd_set_nb(helper->ofd);
	
	helper->read_cb = read_cb;
	helper->error_cb = error_cb;
	return helper;
}

/*
 * start_fork_helper
 * starts a new ircd helper
 * note that this function doesn't start doing reading..thats the job of the caller
 */

ircd_helper *
ircd_helper_start(const char *name, const char *fullpath, ircd_helper_cb *read_cb, ircd_helper_cb *error_cb)
{
	ircd_helper *helper;
	const char *parv[2];
	char buf[128];
	char fx[16], fy[16];
	int ifd[2];
	int ofd[2];
	pid_t pid;
			
	if(access(fullpath, X_OK) == -1)
		return NULL;
	
	helper = ircd_malloc(sizeof(ircd_helper));

	ircd_snprintf(buf, sizeof(buf), "%s helper - read", name);
	if(ircd_pipe(ifd, buf) < 0) 
	{
		ircd_free(helper);
		return NULL;
	}
	ircd_snprintf(buf, sizeof(buf), "%s helper - write", name);
	if(ircd_pipe(ofd, buf) < 0)
	{
		ircd_free(helper);
		return NULL;
	}
	
	ircd_snprintf(fx, sizeof(fx), "%d", ifd[1]);
	ircd_snprintf(fy, sizeof(fy), "%d", ofd[0]);
	
	ircd_set_nb(ifd[0]);
	ircd_set_nb(ifd[1]);
	ircd_set_nb(ofd[0]);
	ircd_set_nb(ofd[1]);
	
	setenv("IFD", fy, 1);
	setenv("OFD", fx, 1);
	setenv("MAXFD", "256", 1);
	
	ircd_snprintf(buf, sizeof(buf), "-ircd %s daemon", name);
	parv[0] = buf;
	parv[1] = NULL;

#ifdef _WIN32      
	SetHandleInformation((HANDLE)ifd[1], HANDLE_FLAG_INHERIT, 1);
	SetHandleInformation((HANDLE)ofd[0], HANDLE_FLAG_INHERIT, 1);
#endif
                
	pid = ircd_spawn_process(fullpath, (const char **)parv);
                        
	if(pid == -1)
	{
		ircd_close(ifd[0]);
		ircd_close(ifd[1]);
		ircd_close(ofd[0]);
		ircd_close(ofd[1]);
		ircd_free(helper);
		return NULL;
	}

	ircd_close(ifd[1]);
	ircd_close(ofd[0]);
	
	ircd_linebuf_newbuf(&helper->sendq);
	ircd_linebuf_newbuf(&helper->recvq);
		
	helper->ifd = ifd[0];
	helper->ofd = ofd[1];
	helper->read_cb = read_cb;
	helper->error_cb = error_cb;	
	helper->fork_count = 0;
	helper->pid = pid;

	return helper;
}


void
ircd_helper_restart(ircd_helper *helper)
{
	helper->error_cb(helper);
}


static void
ircd_helper_write_sendq(int fd, void *helper_ptr)
{
	ircd_helper *helper = helper_ptr;
	int retlen;
	
	if(ircd_linebuf_len(&helper->sendq) > 0)
	{
		while((retlen = ircd_linebuf_flush(fd, &helper->sendq)) > 0)
			;;
		if(retlen == 0 || (retlen < 0 && !ignoreErrno(errno)))
			ircd_helper_restart(helper);
		
	}
	if(helper->ofd < 0)
		return;

	if(ircd_linebuf_len(&helper->sendq) > 0)
		ircd_setselect(helper->ofd, IRCD_SELECT_WRITE, ircd_helper_write_sendq, &helper->sendq);
}

void
ircd_helper_write(ircd_helper *helper, const char *format, ...)
{
	va_list ap;
	if(helper->ifd < 0 || helper->ofd < 0) 
		return; /* XXX error */
	
	va_start(ap, format);
	ircd_linebuf_putmsg(&helper->sendq, format, &ap, NULL);
	ircd_helper_write_sendq(helper->ofd, helper);	
	va_end(ap);
}

void
ircd_helper_read(int fd, void *helper_ptr)
{
	ircd_helper *helper = helper_ptr;
	char buf[READBUF_SIZE];
	int length;
	
	while((length = ircd_read(helper->ifd, buf, sizeof(buf))) > 0)
	{
		ircd_linebuf_parse(&helper->recvq, buf, length, 0);
		helper->read_cb(helper);
	}

	if(length == 0 || (length < 0 && !ignoreErrno(errno)))
		ircd_helper_restart(helper);
	
	if(helper->ifd < 0)
		return;
	
	ircd_setselect(helper->ifd, IRCD_SELECT_READ, ircd_helper_read, helper);
}

void
ircd_helper_close(ircd_helper *helper)
{
	if(helper == NULL)
		return;
		
	ircd_close(helper->ifd);
	ircd_close(helper->ofd);
	ircd_free(helper);	
}

int
ircd_helper_readline(ircd_helper *helper, void *buf, size_t bufsize)
{
	return ircd_linebuf_get(&helper->recvq, buf, bufsize, LINEBUF_COMPLETE, LINEBUF_PARSED);
}
