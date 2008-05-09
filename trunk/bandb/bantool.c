/*
 *  ircd-ratbox: A slightly useful ircd.
 *  bantool.c: The ircd-ratbox database managment tool.
 *
 *  Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center
 *  Copyright (C) 1996-2002 Hybrid Development Team
 *  Copyright (C) 2002-2008 ircd-ratbox development team
 *  Copyright (C) 2008 Daniel J Reidy <dubkat@gmail.com>
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
 *
 *
 * The following server admins have either contributed various configs to test against,
 * or helped with debugging and feature requests. Many thanks to them.
 * stevoo / efnet.port80.se
 * AndroSyn / irc2.choopa.net, irc.igs.ca
 * Salvation / irc.blessed.net
 * JamesOff / efnet.demon.co.uk
 *
 * Thanks to AndroSyn for challenging me to learn C on the fly :)
 * BUGS Direct Question, Bug Reports, and Feature Requests to #ratbox on EFnet.
 * BUGS Complaints >/dev/null
 *
 */

#define _XOPEN_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "stdinc.h"
#include "rsdb.h"

#define EmptyString(x) ((x == NULL) || (*(x) == '\0'))
#define CheckEmpty(x) EmptyString(x) ? "" : x

#define BT_VERSION "0.3.3"

typedef enum
{
	BANDB_KLINE,
	BANDB_KLINE_PERM,
	BANDB_DLINE,
	BANDB_DLINE_PERM,
	BANDB_XLINE,
	BANDB_XLINE_PERM,
	BANDB_RESV,
	BANDB_RESV_PERM,
	LAST_BANDB_TYPE
} bandb_type;


static char bandb_letter[LAST_BANDB_TYPE] = {
	'K', 'K', 'D', 'D', 'X', 'X', 'R', 'R'
};

static const char *bandb_table[LAST_BANDB_TYPE] = {
	"kline", "kline", "dline", "dline", "xline", "xline", "resv", "resv"
};

static const char *bandb_suffix[LAST_BANDB_TYPE] = {
	"", ".perm",
	"", ".perm",
	"", ".perm",
	"", ".perm"
};

static char me[PATH_MAX];

/* counters */
static int klines = 0;
static int dlines = 0;
static int xlines = 0;
static int resvs = 0;
static int err = 0;

/* task flags */
static int i_didnt_use_a_flag = YES;
static int export = NO;
static int import = NO;
static int pretend = NO;
static int verbose = NO;
static int wipe = NO;
static int dupes_ok = NO;	/* by default we do not allow duplicate bans to be entered. */

static int parse_k_file(FILE * file);
static int parse_x_file(FILE * file);
static int parse_d_file(FILE * file);
static int parse_r_file(FILE * file);
static int table_has_rows(const char *table);
static int table_exists(const char *table);

static inline char *smalldate(const char *string);
static const char *clean_gecos_field(const char *gecos);
static char *getfield(char *newline);
static char *strip_quotes(const char *string);

static void export_config(const char *conf, int id);
static void import_config(const char *conf, int id);
static void check_schema(void);
static void print_help(int i_exit);
static void wipe_schema(void);
static void drop_dupes(const char *user, const char *host, const char *t);

/*
 * TODO xlines need to have \" escaped.
 * this is a temporary place holder.
 */
#define string_escape(x) x

int
main(int argc, char *argv[])
{
	char etc[PATH_MAX];
	char conf[PATH_MAX];
	int opt;
	int i;
	
	rb_strlcpy(me, argv[0], sizeof(me));

	while ((opt = getopt(argc, argv, "hiepvwd")) != -1)
	{
		switch (opt)
		{
		case 'h':
			print_help(EXIT_SUCCESS);
			break;
		case 'i':
			i_didnt_use_a_flag = NO;
			import = YES;
			break;
		case 'e':
			i_didnt_use_a_flag = NO;
			export = YES;
			break;
		case 'p':
			pretend = YES;
			break;
		case 'v':
			verbose = YES;
			break;
		case 'w':
			wipe = YES;
			break;
		case 'd':
			dupes_ok = YES;
			break;
		default:	/* '?' */
			print_help(EXIT_FAILURE);
		}
	}
	/* they should really read the help. */
	if(i_didnt_use_a_flag)
		print_help(EXIT_FAILURE);

	if((import && export) || (export && wipe))
	{
		fprintf(stderr,
			"* Error: Conflicting flags. You may only do import [-i] or export [-e].\n");
		fprintf(stderr, "* For an explination of commands, run: %s -h\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	if(argv[optind] != NULL)
		rb_strlcpy(etc, argv[optind], sizeof(etc));
	else
		rb_strlcpy(etc, ETCPATH, sizeof(ETCPATH));

	if(pretend == NO)
	{
		rsdb_init(NULL);
		check_schema();
		if(import && wipe)
		{
			dupes_ok = YES;	/* dont check for dupes if we are wiping the db clean */
			for (i = 0; i < 3; i++)
				fprintf(stdout,
					"* WARNING: YOU ARE ABOUT TO WIPE YOUR DATABASE!\n");

			fprintf(stdout, "* Press ^C to abort! ");
			fflush(stdout);
			rb_sleep(10, 0);
			fprintf(stdout, "Carrying on...\n");
			wipe_schema();
		}
	}

	/* checking for our files to import or export */
	for (i = 0; i < LAST_BANDB_TYPE; i++)
	{
		rb_snprintf(conf, sizeof(conf), "%s/%s.conf%s",
			    etc, bandb_table[i], bandb_suffix[i]);

		if(import && pretend == NO)
			rsdb_transaction(RSDB_TRANS_START);

		if(import)
			import_config(conf, i);

		if(export)
			export_config(conf, i);

		if(import && pretend == NO)
			rsdb_transaction(RSDB_TRANS_END);

		/* ratbox doesn't currently support permanent klines */
		if(export)
			i++;

	}

	if(import)
	{
		if(err)
			fprintf(stderr, "* I was unable to locate %i config files to import.\n",
				err);

		fprintf(stdout, "* Import Stats: Klines: %i Dlines: %i Xlines: %i Jupes: %i \n",
			klines, dlines, xlines, resvs);

		fprintf(stdout,
			"\n* If your IRC server is currently running, newly imported bans \n* will not take effect until you issue the command: /quote rehash bans\n");

		if(pretend)
			fprintf(stdout,
				"* Pretend mode engaged. Nothing was actually entered into the database.\n");
	}
/*	
	if(export)
	{
		if(err)
			fprintf(stderr, "* I had %d unknown errors.\n", err);
		
		fprintf(stdout, "* Export Stats: Klines: %d Dlines: %d Xlines: %d Jupes: %d \n",
				klines, dlines, xlines, resvs);
		
		if(pretend)
			fprintf(stdout,
					"* Pretend mode engaged. Nothing was actually exported.\n");
	}
*/
	return 0;
}

void
import_config(const char *conf, int id)
{
	FILE *fd;
	fprintf(stdout, "* checking for %s: ", conf);	/* debug  */

	/* open config for reading, or skip to the next */
	if(!(fd = fopen(conf, "r")))
	{
		fprintf(stdout, "\tmissing.\n");
		err++;
		return;
	}
	fprintf(stdout, "\tok.\n");

	switch (bandb_letter[id])
	{
	case 'K':
		klines += parse_k_file(fd);
		break;
	case 'D':
		dlines += parse_d_file(fd);
		break;
	case 'X':
		xlines += parse_x_file(fd);
		break;
	case 'R':
		resvs += parse_r_file(fd);
		break;
	default:
		exit(EXIT_FAILURE);
	}
}

void
export_config(const char *conf, int id)
{
	struct rsdb_table table;
	static char buf[512];
	FILE *fd = NULL;
	int j;

	if(!table_has_rows(bandb_table[id]))
	{
		return;
	}
	fprintf(stdout, "* exporting to %s: ", conf);	/* debug  */
	if(!(fd = fopen(conf, "w")))
	{
		fprintf(stdout, "\terror.\n");
		return;		/* no write permission? */
	}
	fprintf(stdout, "\topened.\n");

	rsdb_exec_fetch(&table,
			"SELECT DISTINCT mask1,mask2,reason,oper,time FROM %s WHERE 1 ORDER BY time",
			bandb_table[id]);
	/** 
	 * TODO DELETE THIS COMMENT LATER ONCE VERIFIED
	 * ban.conf formats.
	 * DLINE: ok.
	 * "194.158.192.0/19","laptop scammers","","2005/3/17 05.33","stevoo!stevoo@efnet.port80.se{stevoo}",1111033988
	 * KLINE: ok.
	 * "james","you.are.a.hacker.wa-nn-a.be","byebye","","2006/1/10 01.45","stevoo!stevoo@stevoo.bgpmonkey.net{stevoo}",1136853940
	 * XLINE: ok.
	 * "wSTEAM","0","Drones (JamesOff 20060221)","JamesOff!james@oper.efnet.demon.co.uk{efnet.demon.co.uk}",1140526148
	 * RESV: ok.
	 * "darkacid","banned nickname","stevoo!stevoo@efnet.port80.se{stevoo}",1111034233
	 * "#dethnet","drones are not welcome on this server","HardyP80!hardy@efnet.port80.se{hardy}",1115814351
	 */
	for (j = 0; j < table.row_count; j++)
	{
		switch (id)
		{
		case BANDB_DLINE:
			rb_snprintf(buf, sizeof(buf),
				    "\"%s\",\"%s\",\"\",\"%s\",\"%s\",%s\n",
				    table.row[j][0], table.row[j][2], smalldate(table.row[j][4]),
				    table.row[j][3], table.row[j][4]);
			break;

		case BANDB_XLINE:
			rb_snprintf(buf, sizeof(buf),
				    "\"%s\",\"0\",\"%s\",\"%s\",%s\n",
				    string_escape(table.row[j][0]), table.row[j][2],
				    table.row[j][3], table.row[j][4]);
			break;

		case BANDB_RESV:
			rb_snprintf(buf, sizeof(buf),
				    "\"%s\",\"%s\",\"%s\",%s\n",
				    table.row[j][0], table.row[j][2],
				    table.row[j][3], table.row[j][4]);
			break;


		default:	/* Klines */
			rb_snprintf(buf, sizeof(buf),
				    "\"%s\",\"%s\",\"%s\",\"\",\"%s\",\"%s\",%s\n",
				    table.row[j][0], table.row[j][1], table.row[j][2],
				    smalldate(table.row[j][4]), table.row[j][3], table.row[j][4]);
			break;
		}

		fprintf(fd, "%s", buf);
	}

	rsdb_exec_fetch_end(&table);
	fclose(fd);
}

int
parse_k_file(FILE * file)
{
	char *user_field = NULL;
	char *reason_field = NULL;
	char *operreason_field = NULL;
	char newreason[REASONLEN];
	char *host_field = NULL;
	char *oper_field = NULL;
	char *timestamp = NULL;
	char line[BUFSIZE];
	int i = 0;
	char *p;

	while (fgets(line, sizeof(line), file))
	{
		if((p = strpbrk(line, "\r\n")) != NULL)
			*p = '\0';

		if((*line == '\0') || (*line == '#'))
			continue;

		user_field = getfield(line);
		if(EmptyString(user_field))
			continue;

		host_field = getfield(NULL);
		if(EmptyString(host_field))
			continue;

		reason_field = getfield(NULL);
		if(EmptyString(reason_field))
			continue;

		operreason_field = getfield(NULL);
		getfield(NULL);
		oper_field = getfield(NULL);
		timestamp = strip_quotes(oper_field + strlen(oper_field) + 2);

		/* append operreason_field to reason_field */
		if(!EmptyString(operreason_field))
			rb_snprintf(newreason, sizeof(newreason), "%s | %s", reason_field,
				    operreason_field);
		else
			rb_snprintf(newreason, sizeof(newreason), "%s", reason_field);

		if(pretend == NO)
		{
			if(dupes_ok == NO)
				drop_dupes(user_field, host_field, "kline");

			rsdb_exec(NULL,
				  "INSERT INTO kline (mask1, mask2, oper, time, reason) VALUES('%Q','%Q','%Q','%Q','%Q')",
				  user_field, host_field, oper_field, timestamp, newreason);
		}

		if(pretend && verbose)
			fprintf(stdout, "KLINE: user(%s@%s) oper(%s) reason(%s) time(%s)\n",
				user_field, host_field, oper_field, newreason, timestamp);

		i++;
	}
	return i;
}

int
parse_x_file(FILE * file)
{
	const char *gecos_field = NULL;
	char *reason_field = NULL;
	char newreason[REASONLEN];
	char *oper_field = NULL;
	char *operreason_field = NULL;
	char *timestamp = NULL;
	char line[BUFSIZE];
	char *p;
	int i = 0;

	while (fgets(line, sizeof(line), file))
	{
		if((p = strpbrk(line, "\r\n")))
			*p = '\0';

		if((*line == '\0') || (line[0] == '#'))
			continue;

		gecos_field = clean_gecos_field(getfield(line));
		if(EmptyString(gecos_field))
			continue;


		/* field for xline types, which no longer exist */
		getfield(NULL);

		reason_field = getfield(NULL);
		if(EmptyString(reason_field))
			continue;

		oper_field = getfield(NULL);
		timestamp = oper_field + strlen(oper_field) + 2;

		/* append operreason_field to reason_field */
		if(!EmptyString(operreason_field))
			rb_snprintf(newreason, sizeof(newreason), "%s | %s", reason_field,
				    operreason_field);
		else
			rb_snprintf(newreason, sizeof(newreason), "%s", reason_field);

		if(pretend == NO)
		{
			if(dupes_ok == NO)
				drop_dupes(gecos_field, NULL, "xline");

			rsdb_exec(NULL,
				  "INSERT INTO xline (mask1, mask2, oper, time, reason) VALUES('%Q','%Q','%Q','%Q','%Q')",
				  gecos_field, NULL, oper_field, timestamp, newreason);
		}

		if(pretend && verbose)
			fprintf(stdout, "XLINE: gecos(%s) oper(%s) reason(%s) timestamp(%s)\n",
				gecos_field, oper_field, newreason, timestamp);

		i++;
	}
	return i;
}

int
parse_d_file(FILE * file)
{
	char *reason_field = NULL;
	char newreason[REASONLEN];
	char *host_field = NULL;
	char *oper_field = NULL;
	char *operreason_field = NULL;
	char *timestamp = NULL;
	char line[BUFSIZE];
	char *p;
	int i = 0;

	while (fgets(line, sizeof(line), file))
	{
		if((p = strpbrk(line, "\r\n")))
			*p = '\0';

		if((*line == '\0') || (line[0] == '#'))
			continue;

		host_field = getfield(line);
		if(EmptyString(host_field))
			continue;

		reason_field = getfield(NULL);
		if(EmptyString(reason_field))
			continue;

		operreason_field = getfield(NULL);
		getfield(NULL);
		oper_field = getfield(NULL);
		timestamp = strip_quotes(oper_field + strlen(oper_field) + 2);

		/* append operreason_field to reason_field */
		if(!EmptyString(operreason_field))
			rb_snprintf(newreason, sizeof(newreason), "%s | %s", reason_field,
				    operreason_field);
		else
			rb_snprintf(newreason, sizeof(newreason), "%s", reason_field);

		if(pretend == NO)
		{
			if(dupes_ok == NO)
				drop_dupes(host_field, NULL, "dline");

			rsdb_exec(NULL,
				  "INSERT INTO dline (mask1, mask2, oper, time, reason) VALUES('%Q','%Q','%Q','%Q','%Q')",
				  host_field, NULL, oper_field, timestamp, newreason);
		}

		if(pretend && verbose)
			fprintf(stdout, "DLINE: ip(%s) oper(%s) reason(%s) timestamp(%s)\n",
				host_field, oper_field, newreason, timestamp);

		i++;
	}
	return i;
}


int
parse_r_file(FILE * file)
{
	char *reason_field;
	char *host_field;
	char line[BUFSIZE];
	char *oper_field = NULL;
	char *timestamp = NULL;
	int i = 0;
	char *p;

	while (fgets(line, sizeof(line), file))
	{
		if((p = strpbrk(line, "\r\n")))
			*p = '\0';

		if((*line == '\0') || (line[0] == '#'))
			continue;

		host_field = getfield(line);
		if(EmptyString(host_field))
			continue;

		reason_field = getfield(NULL);
		if(EmptyString(reason_field))
			continue;

		oper_field = getfield(NULL);
		timestamp = strip_quotes(oper_field + strlen(oper_field) + 2);

		if(pretend == NO)
		{
			if(dupes_ok == NO)
				drop_dupes(host_field, NULL, "resv");

			rsdb_exec(NULL,
				  "INSERT INTO resv (mask1, mask2, oper, time, reason) VALUES('%Q','%Q','%Q','%Q','%Q')",
				  host_field, NULL, oper_field, timestamp, reason_field);
		}

		if(pretend && verbose)
			fprintf(stdout, "JUPE: mask(%s) reason(%s) oper(%s) timestamp(%s)\n",
				host_field, reason_field, oper_field, timestamp);

		i++;
	}
	return i;
}

/**
 * getfield
 *
 * inputs	- input buffer
 * output	- next field
 * side effects	- field breakup for ircd.conf file.
 */
char *
getfield(char *newline)
{
	static char *line = NULL;
	char *end, *field;

	if(newline != NULL)
		line = newline;

	if(line == NULL)
		return (NULL);

	field = line;

	/* XXX make this skip to first " if present */
	if(*field == '"')
		field++;
	else
		return (NULL);	/* mal-formed field */

	end = strchr(line, ',');

	while (1)
	{
		/* no trailing , - last field */
		if(end == NULL)
		{
			end = line + strlen(line);
			line = NULL;

			if(*end == '"')
			{
				*end = '\0';
				return field;
			}
			else
				return NULL;
		}
		else
		{
			/* look for a ", to mark the end of a field.. */
			if(*(end - 1) == '"')
			{
				line = end + 1;
				end--;
				*end = '\0';
				return field;
			}

			/* search for the next ',' */
			end++;
			end = strchr(end, ',');
		}
	}

	return NULL;
}

/**
 * strip away "quotes" from around strings
 */
static char *
strip_quotes(const char *string)
{
	static char buff[14];	/* int(11) + 2 + \0 */
	char *str = buff;
	if(string == NULL)
		return NULL;

	while (*string)
	{
		if(*string != '"')
		{
			*str++ = *string;
		}
		string++;
	}
	*str = '\0';
	return buff;
}

/**
 * change spaces to \s in gecos field
 */
static const char *
clean_gecos_field(const char *gecos)
{
	static char buf[BUFSIZE * 2];
	char *str = buf;

	if(gecos == NULL)
		return NULL;
	while (*gecos)
	{
		if(*gecos == ' ')
		{
			*str++ = '\\';
			*str++ = 's';
		}
		else
			*str++ = *gecos;
		gecos++;
	}
	*str = '\0';
	return buf;
}

/**
 * verify the database integrity, and if necessary create apropriate tables
 */
static void
check_schema(void)
{
	int i;
	for (i = 0; i < LAST_BANDB_TYPE; i++)
	{
		if(!table_exists(bandb_table[i]))
			rsdb_exec(NULL,
				  "CREATE TABLE %s (mask1 TEXT, mask2 TEXT, oper TEXT, time INTEGER, reason TEXT)",
				  bandb_table[i]);

		i++;		/* skip over .perm */
	}
}

/**
 * check that appropriate tables exist.
 */
static int
table_exists(const char *dbtab)
{
	struct rsdb_table table;
	rsdb_exec_fetch(&table, "SELECT name FROM sqlite_master WHERE type='table' AND name='%s'",
			dbtab);
	rsdb_exec_fetch_end(&table);
	return table.row_count;
}

/**
 * check that there are actual entries in a table
 */
static int
table_has_rows(const char *dbtab)
{
	struct rsdb_table table;
	rsdb_exec_fetch(&table, "SELECT * FROM %s", dbtab);
	rsdb_exec_fetch_end(&table);
	return table.row_count;
}

/**
 * completly wipes out an existing ban.db of all entries.
 */
static void
wipe_schema(void)
{
	int i;
	rsdb_transaction(RSDB_TRANS_START);
	for (i = 0; i < LAST_BANDB_TYPE; i++)
	{
		rsdb_exec(NULL, "DROP TABLE %s", bandb_table[i]);
		i++;		/* double increment to skip over .perm */
	}
	rsdb_transaction(RSDB_TRANS_END);

	check_schema();
}

/**
 * remove pre-existing duplicate bans from the database.
 * we favor the new, imported ban over the one in the database
 */
void
drop_dupes(const char *user, const char *host, const char *t)
{
	rsdb_exec(NULL, "DELETE FROM %s WHERE mask1='%Q' AND mask2='%Q'", t, user, host);
}

/**
 * convert unix timestamp to human readable (small) date
 */
static inline char *
smalldate(const char *string)
{
	static char buf[MAX_DATE_STRING];
	struct tm time;
	strptime(string, "%s", &time);	/* convert string digits into a time */
	strftime(buf, sizeof(buf), "%Y/%m/%d %H.%M", &time);	/* convert from timestamp to smalldate */
	return buf;
}

/**
 * you are here ->.
 */
void
print_help(int i_exit)
{
	char version[10];
	rb_strlcpy(version, BT_VERSION, sizeof(version));

	fprintf(stderr, "bantool v.%s - the ircd-ratbox database tool.\n", version);
	fprintf(stderr, "Copyright (C) 2008 Daniel J Reidy <dubkat@gmail.com>\n");
	fprintf(stderr, "$Id$\n\n");
	fprintf(stderr, "This program is distributed in the hope that it will be useful,\n"
		"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		"GNU General Public License for more details.\n\n");

	fprintf(stderr, "Usage: %s <-i|-p> [-v] [-h] [-d] [-w] [path]\n", me);
	fprintf(stderr, "       -h : Display some slightly useful help.\n");
	fprintf(stderr, "       -i : Actually import configs into your database.\n");
	fprintf(stderr, "       -e : Export your database to old-style flat files.\n");
	fprintf(stderr,
		"            This is suitable for redistrubuting your banlists, or creating backups.\n");
	fprintf(stderr,
		"       -p : pretend, checks for the configs, and parses them, then tells you some data...\n");
	fprintf(stderr, "            but does not touch your database.\n");
	fprintf(stderr,
		"       -v : Be verbose... and it *is* very verbose! (intended for debugging)\n");
	fprintf(stderr, "       -d : Enable checking for redunant entries.\n");
	fprintf(stderr, "       -w : Completly wipe your database clean. May be used with -i \n");
	fprintf(stderr, "     path : An optional directory containing old ratbox configs.\n");
	fprintf(stderr, "            If not specified, it looks in PREFIX/etc.\n");
	exit(i_exit);
}
