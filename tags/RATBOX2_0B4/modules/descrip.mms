# MMS/MMK Makefile for OpenVMS
# Copyright (c) 2001 Edward Brocklesby
# $Id$

CC=	CC
CFLAGS=	/INCLUDE_DIRECTORY=([-.INCLUDE],[-.ADNS])/STANDARD=ISOC94
LDFLAGS=

OBJECTS=	M_ACCEPT, M_ADMIN, M_AWAY, M_CAPAB, M_CHALLENGE, M_CLOSE, -
		M_CONNECT, M_CRYPTLINK, M_DLINE, M_ENCAP, M_EOB, M_ETRACE, -
		M_GLINE, M_HELP, M_HTM, M_INFO, M_INVITE, M_ISON, M_JOIN, -
		M_KLINE, M_KNOCK, M_LINKS, M_LIST, M_LOCOPS, M_LUSERS, M_MAP, -
		M_MOTD, M_NAMES, M_OPER, M_OPERWALL, M_PASS, M_PING, M_PONG, -
		M_POST, M_REHASH, M_RESTART, M_RESV, M_SET, M_STATS, M_SVINFO, -
		M_TESTLINE, M_TIME, M_TOPIC, M_TRACE, M_UNDLINE, M_UNGLINE, -
		M_USER, M_USERHOST, M_USERS, M_VERSION, M_WALLOPS, M_WHO, -
		M_WHOIS, M_WHOWAS, M_XLINE, STATIC_MODULES, M_UNKLINE

ALL : MODULES.OLB($(OBJECTS)) CORE.OLB

STATIC_MODULES.OBJ : STATIC_MODULES.C

STATIC_MODULES.C : STATIC_MODULES_C.COM
	@STATIC_MODULES_C "load_static_modules"

CORE.OLB :
	SET DEF [.CORE]
	MMK
	SET DEF [-]

CLEAN : 
	DELETE *.OLB;*
	DELETE *.OBJ;*
