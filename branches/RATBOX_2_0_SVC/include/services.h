/*
 *  OpenServices 1.0
 *  Base Structure and parsing tools.
 *
 *  Copyright (C) 2005 Alan "alz" Milford
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
 
 
#define SVC_PRIVMSG     0x01
#define SVC_NOTICE      0x02

/* service structure */
struct Service
{
    dlink_node node;            
    dlink_list commands; 
    MessageHandler unknown;
    struct Client *client_p;
};

/* Message table structure */
struct SVCMessage
{
    const char *cmd;
    int min_para;   /* minimum number of params */
    const char *help;

    MessageHandler handler;
};

//service message function pointer
typedef int (*sfn) (void *data);

struct Service *create_service(const char *nick, const char *username, const char *host, const char *gecos, int opered);

void destroy_service(struct Service *service_p);

int handle_services_message(struct Client *source_p, struct Client *target_p, const char *text);
void parse_services_message(struct Client *client_p, struct Client *target_p, const char *text, int length);
void process_unknown_command(struct Client *client_p, struct Client *target_p);
int is_svc_command(struct Service *service_p, struct SVCMessage *msg);
void svc_add_cmd(struct Service *service_p, struct SVCMessage *msg);
void svc_del_cmd(struct Service *service_p, struct SVCMessage *msg);
void svc_message(struct Service *service, struct Client *target_p, int type, const char *pattern, ...);
struct Service *find_service(struct Client *client_p);
void try_command(struct Client *source_p, struct Service *service_p, const char *cmd);
void svc_set_unknown(struct Service *service_p, MessageHandler unknown);
struct SVCMessage *svc_get_cmd(struct Service *service_p, char *cmd);


