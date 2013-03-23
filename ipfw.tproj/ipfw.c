/*
 * Copyright (c) 1996 Alex Nash, Paul Traina, Poul-Henning Kamp
 * Copyright (c) 1994 Ugen J.S.Antsilevich
 *
 * Idea and grammar partially left from:
 * Copyright (c) 1993 Daniel Boulet
 *
 * Redistribution and use in source forms, with and without modification,
 * are permitted provided that this entire comment appears intact.
 *
 * Redistribution in binary form may occur without any restrictions.
 * Obviously, it would be nice if you gave credit where credit is due
 * but requiring it would be too onerous.
 *
 * This software is provided ``AS IS'' without any warranties of any kind.
 *
 * NEW command line interface for IP firewall facility
 *
 * $Id: ipfw.c,v 1.4 2001/07/17 22:59:33 lindak Exp $
 *
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#include <net/if.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip_var.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip_fw.h>
#include <net/route.h> /* def. of struct route */
#include <sys/param.h>
#include <sys/mbuf.h>
#include <netinet/ip_dummynet.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

int 		lineno = -1;

int 		s;				/* main RAW socket 	   */
int 		do_resolv=0;			/* Would try to resolve all */
int		do_acct=0;			/* Show packet/byte count  */
int		do_time=0;			/* Show time stamps        */
int		do_quiet=0;			/* Be quiet in add and flush  */
int		do_force=0;			/* Don't ask for confirmation */
int		do_pipe=0;                      /* this cmd refers to a pipe */

struct icmpcode {
	int	code;
	char	*str;
};

static struct icmpcode icmpcodes[] = {
      { ICMP_UNREACH_NET,		"net" },
      { ICMP_UNREACH_HOST,		"host" },
      { ICMP_UNREACH_PROTOCOL,		"protocol" },
      { ICMP_UNREACH_PORT,		"port" },
      { ICMP_UNREACH_NEEDFRAG,		"needfrag" },
      { ICMP_UNREACH_SRCFAIL,		"srcfail" },
      { ICMP_UNREACH_NET_UNKNOWN,	"net-unknown" },
      { ICMP_UNREACH_HOST_UNKNOWN,	"host-unknown" },
      { ICMP_UNREACH_ISOLATED,		"isolated" },
      { ICMP_UNREACH_NET_PROHIB,	"net-prohib" },
      { ICMP_UNREACH_HOST_PROHIB,	"host-prohib" },
      { ICMP_UNREACH_TOSNET,		"tosnet" },
      { ICMP_UNREACH_TOSHOST,		"toshost" },
      { ICMP_UNREACH_FILTER_PROHIB,	"filter-prohib" },
      { ICMP_UNREACH_HOST_PRECEDENCE,	"host-precedence" },
      { ICMP_UNREACH_PRECEDENCE_CUTOFF,	"precedence-cutoff" },
      { 0, NULL }
};

static void show_usage(const char *fmt, ...);

static int
mask_bits(struct in_addr m_ad)
{
	int h_fnd=0,h_num=0,i;
	u_long mask;

	mask=ntohl(m_ad.s_addr);
	for (i=0;i<sizeof(u_long)*CHAR_BIT;i++) {
		if (mask & 1L) {
			h_fnd=1;
			h_num++;
		} else {
			if (h_fnd)
				return -1;
		}
		mask=mask>>1;
	}
	return h_num;
}                         

static void
print_port(prot, port, comma)
	u_char  prot;
	u_short port;
	const char *comma;
{
	struct servent *se;
	struct protoent *pe;
	const char *protocol;
	int printed = 0;

	if (do_resolv) {
		pe = getprotobynumber(prot);
		if (pe)
			protocol = pe->p_name;
		else
			protocol = NULL;

		se = getservbyport(htons(port), protocol);
		if (se) {
			printf("%s%s", comma, se->s_name);
			printed = 1;
		}
	} 
	if (!printed)
		printf("%s%d",comma,port);
}

static void
print_iface(char *key, union ip_fw_if *un, int byname)
{
	char ifnb[FW_IFNLEN+1];

	if (byname) {
		strncpy(ifnb, un->fu_via_if.name, FW_IFNLEN);
		ifnb[FW_IFNLEN]='\0';
		if (un->fu_via_if.unit == -1)
			printf(" %s %s*", key, ifnb);
		else 
			printf(" %s %s%d", key, ifnb, un->fu_via_if.unit);
	} else if (un->fu_via_ip.s_addr != 0) {
		printf(" %s %s", key, inet_ntoa(un->fu_via_ip));
	} else
		printf(" %s any", key);
}

static void
print_reject_code(int code)
{
	struct icmpcode *ic;

	for (ic = icmpcodes; ic->str; ic++)
		if (ic->code == code) {
			printf("%s", ic->str);
			return;
		}
	printf("%u", code);
}

static void
show_ipfw(struct ip_fw *chain, int pcwidth, int bcwidth)
{
	char *comma;
	u_long adrt;
	struct hostent *he;
	struct protoent *pe;
	int i, mb;
	int nsp = IP_FW_GETNSRCP(chain);
	int ndp = IP_FW_GETNDSTP(chain);

	if (do_resolv)
		setservent(1/*stayopen*/);

	printf("%05u ", chain->fw_number);

	if (do_acct) 
		printf("%*qu %*qu ",pcwidth,chain->fw_pcnt,bcwidth,chain->fw_bcnt);

	if (do_time)
	{
		if (chain->timestamp)
		{
			char timestr[30];

			strcpy(timestr, ctime((time_t *)&chain->timestamp));
			*strchr(timestr, '\n') = '\0';
			printf("%s ", timestr);
		}
		else
			printf("                         ");
	}

	switch (chain->fw_flg & IP_FW_F_COMMAND)
	{
		case IP_FW_F_ACCEPT:
			printf("allow");
			break;
		case IP_FW_F_DENY:
			printf("deny");
			break;
		case IP_FW_F_COUNT:
			printf("count");
			break;
		case IP_FW_F_DIVERT:
			printf("divert %u", chain->fw_divert_port);
			break;
		case IP_FW_F_TEE:
			printf("tee %u", chain->fw_divert_port);
			break;
		case IP_FW_F_SKIPTO:
			printf("skipto %u", chain->fw_skipto_rule);
			break;
                case IP_FW_F_PIPE:
                        printf("pipe %u", chain->fw_skipto_rule);
                        break ;
		case IP_FW_F_REJECT:
			if (chain->fw_reject_code == IP_FW_REJECT_RST)
				printf("reset");
			else {
				printf("unreach ");
				print_reject_code(chain->fw_reject_code);
			}
			break;
		case IP_FW_F_FWD:
			printf("fwd %s", inet_ntoa(chain->fw_fwd_ip.sin_addr));
			if(chain->fw_fwd_ip.sin_port)
				printf(",%d", chain->fw_fwd_ip.sin_port);
			break;
		default:
			errx(EX_OSERR, "impossible");
	}
   
	if (chain->fw_flg & IP_FW_F_PRN)
		printf(" log");

	pe = getprotobynumber(chain->fw_prot);
	if (pe)
		printf(" %s", pe->p_name);
	else
		printf(" %u", chain->fw_prot);

	printf(" from %s", chain->fw_flg & IP_FW_F_INVSRC ? "not " : "");

	adrt=ntohl(chain->fw_smsk.s_addr);
	if (adrt==ULONG_MAX && do_resolv) {
		adrt=(chain->fw_src.s_addr);
		he=gethostbyaddr((char *)&adrt,sizeof(u_long),AF_INET);
		if (he==NULL) {
			printf(inet_ntoa(chain->fw_src));
		} else
			printf("%s",he->h_name);
	} else {
		if (adrt!=ULONG_MAX) {
			mb=mask_bits(chain->fw_smsk);
			if (mb == 0) {
				printf("any");
			} else {
				if (mb > 0) {
					printf(inet_ntoa(chain->fw_src));
					printf("/%d",mb);
				} else {
					printf(inet_ntoa(chain->fw_src));
					printf(":");
					printf(inet_ntoa(chain->fw_smsk));
				}
			}
		} else
			printf(inet_ntoa(chain->fw_src));
	}

	if (chain->fw_prot == IPPROTO_TCP || chain->fw_prot == IPPROTO_UDP) {
		comma = " ";
		for (i = 0; i < nsp; i++) {
			print_port(chain->fw_prot, chain->fw_uar.fw_pts[i], comma);
			if (i==0 && (chain->fw_flg & IP_FW_F_SRNG))
				comma = "-";
			else
				comma = ",";
		}
	}

	printf(" to %s", chain->fw_flg & IP_FW_F_INVDST ? "not " : "");

	adrt=ntohl(chain->fw_dmsk.s_addr);
	if (adrt==ULONG_MAX && do_resolv) {
		adrt=(chain->fw_dst.s_addr);
		he=gethostbyaddr((char *)&adrt,sizeof(u_long),AF_INET);
		if (he==NULL) {
			printf(inet_ntoa(chain->fw_dst));
		} else
			printf("%s",he->h_name);
	} else {
		if (adrt!=ULONG_MAX) {
			mb=mask_bits(chain->fw_dmsk);
			if (mb == 0) {
				printf("any");
			} else {
				if (mb > 0) {
					printf(inet_ntoa(chain->fw_dst));
					printf("/%d",mb);
				} else {
					printf(inet_ntoa(chain->fw_dst));
					printf(":");
					printf(inet_ntoa(chain->fw_dmsk));
				}
			}
		} else
			printf(inet_ntoa(chain->fw_dst));
	}

	if (chain->fw_prot == IPPROTO_TCP || chain->fw_prot == IPPROTO_UDP) {
		comma = " ";
		for (i = 0; i < ndp; i++) {
			print_port(chain->fw_prot, chain->fw_uar.fw_pts[nsp+i], comma);
			if (i==0 && (chain->fw_flg & IP_FW_F_DRNG))
				comma = "-";
			else
				comma = ",";
		}
	}

	/* Direction */
	if ((chain->fw_flg & IP_FW_F_IN) && !(chain->fw_flg & IP_FW_F_OUT))
		printf(" in");
	if (!(chain->fw_flg & IP_FW_F_IN) && (chain->fw_flg & IP_FW_F_OUT))
		printf(" out");

	/* Handle hack for "via" backwards compatibility */
	if ((chain->fw_flg & IF_FW_F_VIAHACK) == IF_FW_F_VIAHACK) {
		print_iface("via",
		    &chain->fw_in_if, chain->fw_flg & IP_FW_F_IIFNAME);
	} else {
		/* Receive interface specified */
		if (chain->fw_flg & IP_FW_F_IIFACE)
			print_iface("recv", &chain->fw_in_if,
			    chain->fw_flg & IP_FW_F_IIFNAME);
		/* Transmit interface specified */
		if (chain->fw_flg & IP_FW_F_OIFACE)
			print_iface("xmit", &chain->fw_out_if,
			    chain->fw_flg & IP_FW_F_OIFNAME);
	}

	if (chain->fw_flg & IP_FW_F_FRAG)
		printf(" frag");

	if (chain->fw_ipopt || chain->fw_ipnopt) {
		int 	_opt_printed = 0;
#define PRINTOPT(x)	{if (_opt_printed) printf(",");\
			printf(x); _opt_printed = 1;}

		printf(" ipopt ");
		if (chain->fw_ipopt  & IP_FW_IPOPT_SSRR) PRINTOPT("ssrr");
		if (chain->fw_ipnopt & IP_FW_IPOPT_SSRR) PRINTOPT("!ssrr");
		if (chain->fw_ipopt  & IP_FW_IPOPT_LSRR) PRINTOPT("lsrr");
		if (chain->fw_ipnopt & IP_FW_IPOPT_LSRR) PRINTOPT("!lsrr");
		if (chain->fw_ipopt  & IP_FW_IPOPT_RR)   PRINTOPT("rr");
		if (chain->fw_ipnopt & IP_FW_IPOPT_RR)   PRINTOPT("!rr");
		if (chain->fw_ipopt  & IP_FW_IPOPT_TS)   PRINTOPT("ts");
		if (chain->fw_ipnopt & IP_FW_IPOPT_TS)   PRINTOPT("!ts");
	} 

	if (chain->fw_tcpf & IP_FW_TCPF_ESTAB) 
		printf(" established");
	else if (chain->fw_tcpf == IP_FW_TCPF_SYN &&
	    chain->fw_tcpnf == IP_FW_TCPF_ACK)
		printf(" setup");
	else if (chain->fw_tcpf || chain->fw_tcpnf) {
		int 	_flg_printed = 0;
#define PRINTFLG(x)	{if (_flg_printed) printf(",");\
			printf(x); _flg_printed = 1;}

		printf(" tcpflg ");
		if (chain->fw_tcpf  & IP_FW_TCPF_FIN)  PRINTFLG("fin");
		if (chain->fw_tcpnf & IP_FW_TCPF_FIN)  PRINTFLG("!fin");
		if (chain->fw_tcpf  & IP_FW_TCPF_SYN)  PRINTFLG("syn");
		if (chain->fw_tcpnf & IP_FW_TCPF_SYN)  PRINTFLG("!syn");
		if (chain->fw_tcpf  & IP_FW_TCPF_RST)  PRINTFLG("rst");
		if (chain->fw_tcpnf & IP_FW_TCPF_RST)  PRINTFLG("!rst");
		if (chain->fw_tcpf  & IP_FW_TCPF_PSH)  PRINTFLG("psh");
		if (chain->fw_tcpnf & IP_FW_TCPF_PSH)  PRINTFLG("!psh");
		if (chain->fw_tcpf  & IP_FW_TCPF_ACK)  PRINTFLG("ack");
		if (chain->fw_tcpnf & IP_FW_TCPF_ACK)  PRINTFLG("!ack");
		if (chain->fw_tcpf  & IP_FW_TCPF_URG)  PRINTFLG("urg");
		if (chain->fw_tcpnf & IP_FW_TCPF_URG)  PRINTFLG("!urg");
	} 
	if (chain->fw_flg & IP_FW_F_ICMPBIT) {
		int type_index;
		int first = 1;

		printf(" icmptype");

		for (type_index = 0; type_index < IP_FW_ICMPTYPES_DIM * sizeof(unsigned) * 8; ++type_index)
			if (chain->fw_uar.fw_icmptypes[type_index / (sizeof(unsigned) * 8)] & 
				(1U << (type_index % (sizeof(unsigned) * 8)))) {
				printf("%c%d", first == 1 ? ' ' : ',', type_index);
				first = 0;
			}
	}
	printf("\n");
	if (do_resolv)
		endservent();
}

static void
list(ac, av)
	int	ac;
	char 	**av;
{
	struct ip_fw *rules;
	struct dn_pipe *pipes;
	void *data = NULL;
	int pcwidth = 0;
	int bcwidth = 0;
	int n, num = 0;

	/* get rules or pipes from kernel, resizing array as necessary */
	{
		const int unit = do_pipe ? sizeof(*pipes) : sizeof(*rules);
		const int ocmd = do_pipe ? IP_DUMMYNET_GET : IP_FW_GET;
		int nalloc = 0;
		int nbytes;

		while (num >= nalloc) {
			nalloc = nalloc * 2 + 200;
			nbytes = nalloc * unit;
			if ((data = realloc(data, nbytes)) == NULL)
				err(EX_OSERR, "realloc");
			if (getsockopt(s, IPPROTO_IP, ocmd, data, &nbytes) < 0)
				err(EX_OSERR, "getsockopt(IP_%s_GET)",
				    do_pipe ? "DUMMYNET" : "FW");
			num = nbytes / unit;
		}
	}

	/* display requested pipes */
	if (do_pipe) {
	    u_long rulenum;

	    pipes = (struct dn_pipe *) data;
	    if (ac > 0)
		rulenum = strtoul(*av++, NULL, 10);
	    else
		rulenum = 0 ;
	    for (n = 0; n < num; n++) {
		struct dn_pipe *const p = &pipes[n];
		double b = p->bandwidth ;
		char buf[30] ;
		char qs[30] ;
		char plr[30] ;
		int l ;

		if (rulenum != 0 && rulenum != p->pipe_nr)
			continue;
		if (b == 0)
		    sprintf(buf, "unlimited");
		else if (b >= 1000000)
		    sprintf(buf, "%7.3f Mbit/s", b/1000000 );
		else if (b >= 1000)
		    sprintf(buf, "%7.3f Kbit/s", b/1000 );
		else
		    sprintf(buf, "%7.3f bit/s ", b );

		if ( (l = p->queue_size_bytes) != 0 ) {
		    if (l >= 8192)
			sprintf(qs,"%d KB", l / 1024);
		    else
			sprintf(qs,"%d B", l);
		} else
		    sprintf(qs,"%3d sl.", p->queue_size);
		if (p->plr)
		    sprintf(plr,"plr %f", 1.0*p->plr/(double)(0x7fffffff));
		else
		    plr[0]='\0';

		printf("%05d: %s %4d ms %s %s -- %d pkts (%d B) %d drops\n",
		    p->pipe_nr, buf, p->delay, qs, plr,
		    p->r_len, p->r_len_bytes, p->r_drops);
	    }
	    free(data);
	    return;
	}

	/* if showing stats, figure out column widths ahead of time */
	rules = (struct ip_fw *) data;
	if (do_acct) {
		for (n = 0; n < num; n++) {
			struct ip_fw *const r = &rules[n];
			char temp[32];
			int width;

			/* packet counter */
			width = sprintf(temp, "%qu", r->fw_pcnt);
			if (width > pcwidth)
				pcwidth = width;

			/* byte counter */
			width = sprintf(temp, "%qu", r->fw_bcnt);
			if (width > bcwidth)
				bcwidth = width;
		}
	}
	if (ac == 0) {
		/* display all rules */
		for (n = 0; n < num; n++) {
			struct ip_fw *const r = &rules[n];

			show_ipfw(r, pcwidth, bcwidth);
		}
	} else {
		/* display specific rules requested on command line */
		int exitval = EX_OK;

		while (ac--) {
			u_long rnum;
			char *endptr;
			int seen;

			/* convert command line rule # */
			rnum = strtoul(*av++, &endptr, 10);
			if (*endptr) {
				exitval = EX_USAGE;
				warnx("invalid rule number: %s", *(av - 1));
				continue;
			}
			for (seen = n = 0; n < num; n++) {
				struct ip_fw *const r = &rules[n];

				if (r->fw_number > rnum)
					break;
				if (r->fw_number == rnum) {
					show_ipfw(r, pcwidth, bcwidth);
					seen = 1;
				}
			}
			if (!seen) {
				/* give precedence to other error(s) */
				if (exitval == EX_OK)
					exitval = EX_UNAVAILABLE;
				warnx("rule %lu does not exist", rnum);
			}
		}
		if (exitval != EX_OK)
			exit(exitval);
	}
	free(data);
}

static void
show_usage(const char *fmt, ...)
{
	if (fmt) {
		char buf[100];
		va_list args;

		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		warnx("error: %s", buf);
	}
	fprintf(stderr, "usage: ipfw [options]\n"
"    flush\n"
"    add [number] rule\n"
"    delete number ...\n"
"    list [number ...]\n"
"    show [number ...]\n"
"    zero [number ...]\n"
"  rule:  action proto src dst extras...\n"
"    action:\n"
"      {allow|permit|accept|pass|deny|drop|reject|unreach code|\n"
"       reset|count|skipto num|divert port|tee port|fwd ip} [log]\n"
"    proto: {ip|tcp|udp|icmp|<number>}\n"
"    src: from [not] {any|ip[{/bits|:mask}]} [{port|port-port},[port],...]\n"
"    dst: to [not] {any|ip[{/bits|:mask}]} [{port|port-port},[port],...]\n"
"  extras:\n"
"    fragment     (may not be used with ports or tcpflags)\n"
"    in\n"
"    out\n"
"    {xmit|recv|via} {iface|ip|any}\n"
"    {established|setup}\n"
"    tcpflags [!]{syn|fin|rst|ack|psh|urg},...\n"
"    ipoptions [!]{ssrr|lsrr|rr|ts},...\n"
"    icmptypes {type[,type]}...\n");

	exit(EX_USAGE);
}

static int
lookup_host (host, ipaddr)
	char *host;
	struct in_addr *ipaddr;
{
	struct hostent *he = gethostbyname(host);

    if (!he) {
        if (inet_aton(host, ipaddr))
            return(0);
        else
            return(-1);
    }
	*ipaddr = *(struct in_addr *)he->h_addr_list[0];

	return(0);
}

static void
fill_ip(ipno, mask, acp, avp)
	struct in_addr *ipno, *mask;
	int *acp;
	char ***avp;
{
	int ac = *acp;
	char **av = *avp;
	char *p = 0, md = 0;

	if (ac && !strncmp(*av,"any",strlen(*av))) {
		ipno->s_addr = mask->s_addr = 0; av++; ac--;
	} else {
		p = strchr(*av, '/');
		if (!p) 
			p = strchr(*av, ':');
		if (p) {
			md = *p;
			*p++ = '\0'; 
		}

		if (lookup_host(*av, ipno) != 0)
			show_usage("hostname ``%s'' unknown", *av);
		switch (md) {
			case ':':
				if (!inet_aton(p,mask))
					show_usage("bad netmask ``%s''", p);
				break;
			case '/':
				if (atoi(p) == 0) {
					mask->s_addr = 0;
				} else if (atoi(p) > 32) {
					show_usage("bad width ``%s''", p);
				} else {
					mask->s_addr =
					    htonl(~0 << (32 - atoi(p)));
				}
				break;
			default:
				mask->s_addr = htonl(~0);
				break;
		}
		ipno->s_addr &= mask->s_addr;
		av++;
		ac--;
	}
	*acp = ac;
	*avp = av;
}

static void
fill_reject_code(u_short *codep, char *str)
{
	struct icmpcode *ic;
	u_long val;
	char *s;

	val = strtoul(str, &s, 0);
	if (s != str && *s == '\0' && val < 0x100) {
		*codep = val;
		return;
	}
	for (ic = icmpcodes; ic->str; ic++)
		if (!strcasecmp(str, ic->str)) {
			*codep = ic->code;
			return;
		}
	show_usage("unknown ICMP unreachable code ``%s''", str);
}

static void
add_port(cnt, ptr, off, port)
	u_short *cnt, *ptr, off, port;
{
	if (off + *cnt >= IP_FW_MAX_PORTS)
		errx(EX_USAGE, "too many ports (max is %d)", IP_FW_MAX_PORTS);
	ptr[off+*cnt] = port;
	(*cnt)++;
}

static int
lookup_port(const char *arg, int test, int nodash)
{
	int		val;
	char		*earg, buf[32];
	struct servent	*s;

	snprintf(buf, sizeof(buf), "%s", arg);
	buf[strcspn(arg, nodash ? "-," : ",")] = 0;
	val = (int) strtoul(buf, &earg, 0);
	if (!*buf || *earg) {
		setservent(1);
		if ((s = getservbyname(buf, NULL))) {
			val = htons(s->s_port);
		} else {
			if (!test) {
				errx(EX_DATAERR, "unknown port ``%s''", arg);
			}
			val = -1;
		}
	} else {
		if (val < 0 || val > 0xffff) {
			if (!test) {
				errx(EX_DATAERR, "port ``%s'' out of range", arg);
			}
			val = -1;
		}
	}
	return(val);
}

static int
fill_port(cnt, ptr, off, arg)
	u_short *cnt, *ptr, off;
	char *arg;
{
	char *s;
	int initial_range = 0;

	s = arg + strcspn(arg, "-,");	/* first port name can't have a dash */
	if (*s == '-') {
		*s++ = '\0';
		if (strchr(arg, ','))
			errx(EX_USAGE, "port range must be first in list");
		add_port(cnt, ptr, off, *arg ? lookup_port(arg, 0, 0) : 0x0000);
		arg = s;
		s = strchr(arg,',');
		if (s)
			*s++ = '\0';
		add_port(cnt, ptr, off, *arg ? lookup_port(arg, 0, 0) : 0xffff);
		arg = s;
		initial_range = 1;
	}
	while (arg != NULL) {
		s = strchr(arg,',');
		if (s)
			*s++ = '\0';
		add_port(cnt, ptr, off, lookup_port(arg, 0, 0));
		arg = s;
	}
	return initial_range;
}

static void
fill_tcpflag(set, reset, vp)
	u_char *set, *reset;
	char **vp;
{
	char *p = *vp,*q;
	u_char *d;

	while (p && *p) {
		struct tpcflags {
			char * name;
			u_char value;
		} flags[] = {
			{ "syn", IP_FW_TCPF_SYN },
			{ "fin", IP_FW_TCPF_FIN },
			{ "ack", IP_FW_TCPF_ACK },
			{ "psh", IP_FW_TCPF_PSH },
			{ "rst", IP_FW_TCPF_RST },
			{ "urg", IP_FW_TCPF_URG }
		};
		int i;

		if (*p == '!') {
			p++;
			d = reset;
		} else {
			d = set;
		}
		q = strchr(p, ',');
		if (q) 
			*q++ = '\0';
		for (i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i)
			if (!strncmp(p, flags[i].name, strlen(p))) {
				*d |= flags[i].value;
				break;
			}
		if (i == sizeof(flags) / sizeof(flags[0]))
			show_usage("invalid tcp flag ``%s''", p);
		p = q;
	}
}

static void
fill_ipopt(u_char *set, u_char *reset, char **vp)
{
	char *p = *vp,*q;
	u_char *d;

	while (p && *p) {
		if (*p == '!') {
			p++;
			d = reset;
		} else {
			d = set;
		}
		q = strchr(p, ',');
		if (q) 
			*q++ = '\0';
		if (!strncmp(p,"ssrr",strlen(p))) *d |= IP_FW_IPOPT_SSRR;
		if (!strncmp(p,"lsrr",strlen(p))) *d |= IP_FW_IPOPT_LSRR;
		if (!strncmp(p,"rr",strlen(p)))   *d |= IP_FW_IPOPT_RR;
		if (!strncmp(p,"ts",strlen(p)))   *d |= IP_FW_IPOPT_TS;
		p = q;
	}
}

static void
fill_icmptypes(types, vp, fw_flg)
	u_long *types;
	char **vp;
	u_int *fw_flg;
{
	char *c = *vp;

	while (*c)
	{
		unsigned long icmptype;

		if ( *c == ',' )
			++c;

		icmptype = strtoul(c, &c, 0);

		if ( *c != ',' && *c != '\0' )
			show_usage("invalid ICMP type");

		if (icmptype >= IP_FW_ICMPTYPES_DIM * sizeof(unsigned) * 8)
			show_usage("ICMP type out of range");

		types[icmptype / (sizeof(unsigned) * 8)] |= 
			1 << (icmptype % (sizeof(unsigned) * 8));
		*fw_flg |= IP_FW_F_ICMPBIT;
	}
}

static void
delete(ac,av)
	int ac;
	char **av;
{
	struct ip_fw rule;
	struct dn_pipe pipe;
	int i;
	int exitval = EX_OK;

	memset(&rule, 0, sizeof rule);
	memset(&pipe, 0, sizeof pipe);

	av++; ac--;

	/* Rule number */
	while (ac && isdigit(**av)) {
            if (do_pipe) {
                pipe.pipe_nr = atoi(*av); av++; ac--;
                i = setsockopt(s, IPPROTO_IP, IP_DUMMYNET_DEL,
                    &pipe, sizeof pipe);
                if (i) {
                    exitval = 1;
                    warn("rule %u: setsockopt(%s)", pipe.pipe_nr, "IP_DUMMYNET_DEL");
                }
            } else {
		rule.fw_number = atoi(*av); av++; ac--;
		i = setsockopt(s, IPPROTO_IP, IP_FW_DEL, &rule, sizeof rule);
		if (i) {
			exitval = EX_UNAVAILABLE;
			warn("rule %u: setsockopt(%s)", rule.fw_number, "IP_FW_DEL");
		}
	}
	}
	if (exitval != EX_OK)
		exit(exitval);
}

static void
verify_interface(union ip_fw_if *ifu)
{
	struct ifreq ifr;

	/*
	 *	If a unit was specified, check for that exact interface.
	 *	If a wildcard was specified, check for unit 0.
	 */
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s%d", 
			 ifu->fu_via_if.name,
			 ifu->fu_via_if.unit == -1 ? 0 : ifu->fu_via_if.unit);

	if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0)
		warnx("warning: interface ``%s'' does not exist", ifr.ifr_name);
}

static void
fill_iface(char *which, union ip_fw_if *ifu, int *byname, int ac, char *arg)
{
	if (!ac)
	    show_usage("missing argument for ``%s''", which);

	/* Parse the interface or address */
	if (!strcmp(arg, "any")) {
		ifu->fu_via_ip.s_addr = 0;
		*byname = 0;
	} else if (!isdigit(*arg)) {
		char *q;

		*byname = 1;
		strncpy(ifu->fu_via_if.name, arg, sizeof(ifu->fu_via_if.name));
		ifu->fu_via_if.name[sizeof(ifu->fu_via_if.name) - 1] = '\0';
		for (q = ifu->fu_via_if.name;
		    *q && !isdigit(*q) && *q != '*'; q++)
			continue;
		ifu->fu_via_if.unit = (*q == '*') ? -1 : atoi(q);
		*q = '\0';
		verify_interface(ifu);
	} else if (!inet_aton(arg, &ifu->fu_via_ip)) {
		show_usage("bad ip address ``%s''", arg);
	} else
		*byname = 0;
}

static void
config_pipe(int ac, char **av)
{
       struct dn_pipe pipe;
        int i ;
        char *end ;
 
        memset(&pipe, 0, sizeof pipe);
 
        av++; ac--;
        /* Pipe number */
        if (ac && isdigit(**av)) {
                pipe.pipe_nr = atoi(*av); av++; ac--;
        }
        while (ac > 1) {
            if (!strncmp(*av,"bw",strlen(*av)) ||
                ! strncmp(*av,"bandwidth",strlen(*av))) {
                pipe.bandwidth = strtoul(av[1], &end, 0);
                if (*end == 'K')
                        end++, pipe.bandwidth *= 1000 ;
                else if (*end == 'M')
                        end++, pipe.bandwidth *= 1000000 ;
                if (*end == 'B')
                        pipe.bandwidth *= 8 ;
                av+=2; ac-=2;
            } else if (!strncmp(*av,"delay",strlen(*av)) ) {
                pipe.delay = strtoul(av[1], NULL, 0);
                av+=2; ac-=2;
            } else if (!strncmp(*av,"plr",strlen(*av)) ) {
                
                double d = strtod(av[1], NULL);
                pipe.plr = (int)(d*0x7fffffff) ;
                av+=2; ac-=2;
            } else if (!strncmp(*av,"queue",strlen(*av)) ) {
                end = NULL ;
                pipe.queue_size = strtoul(av[1], &end, 0);
                if (*end == 'K') {
                    pipe.queue_size_bytes = pipe.queue_size*1024 ;
                    pipe.queue_size = 0 ;
                } else if (*end == 'B') {
                    pipe.queue_size_bytes = pipe.queue_size ;
                    pipe.queue_size = 0 ;
                }
                av+=2; ac-=2;
            } else
                show_usage("unrecognised option ``%s''", *av);
        }
        if (pipe.pipe_nr == 0 )
            show_usage("pipe_nr %d be > 0", pipe.pipe_nr);
        if (pipe.queue_size > 100 )
            show_usage("queue size %d must be 2 <= x <= 100", pipe.queue_size);
        if (pipe.delay > 10000 )
            show_usage("delay %d must be < 10000", pipe.delay);
#if 0
        printf("configuring pipe %d bw %d delay %d size %d\n",
                pipe.pipe_nr, pipe.bandwidth, pipe.delay, pipe.queue_size);
#endif
        i = setsockopt(s,IPPROTO_IP, IP_DUMMYNET_CONFIGURE, &pipe,sizeof pipe);
        if (i)
                err(1, "setsockopt(%s)", "IP_DUMMYNET_CONFIGURE");
                
}

static void
add(ac,av)
	int ac;
	char **av;
{
	struct ip_fw rule;
	int i;
	u_char proto;
	struct protoent *pe;
	int saw_xmrc = 0, saw_via = 0;
	
	memset(&rule, 0, sizeof rule);

	av++; ac--;

	/* Rule number */
	if (ac && isdigit(**av)) {
		rule.fw_number = atoi(*av); av++; ac--;
	}

	/* Action */
	if (ac == 0)
		show_usage("missing action");
	if (!strncmp(*av,"accept",strlen(*av))
		    || !strncmp(*av,"pass",strlen(*av))
		    || !strncmp(*av,"allow",strlen(*av))
		    || !strncmp(*av,"permit",strlen(*av))) {
		rule.fw_flg |= IP_FW_F_ACCEPT; av++; ac--;
	} else if (!strncmp(*av,"count",strlen(*av))) {
		rule.fw_flg |= IP_FW_F_COUNT; av++; ac--;
        } else if (!strncmp(*av,"pipe",strlen(*av))) {
                rule.fw_flg |= IP_FW_F_PIPE; av++; ac--;
                if (!ac)
                        show_usage("missing pipe number");
                rule.fw_divert_port = strtoul(*av, NULL, 0); av++; ac--;
	} else if (!strncmp(*av,"divert",strlen(*av))) {
		rule.fw_flg |= IP_FW_F_DIVERT; av++; ac--;
		if (!ac)
			show_usage("missing %s port", "divert");
		rule.fw_divert_port = strtoul(*av, NULL, 0); av++; ac--;
		if (rule.fw_divert_port == 0) {
			struct servent *s;
			setservent(1);
			s = getservbyname(av[-1], "divert");
			if (s != NULL)
				rule.fw_divert_port = ntohs(s->s_port);
			else
				show_usage("illegal %s port", "divert");
		}
	} else if (!strncmp(*av,"tee",strlen(*av))) {
		rule.fw_flg |= IP_FW_F_TEE; av++; ac--;
		if (!ac)
			show_usage("missing %s port", "tee divert");
		rule.fw_divert_port = strtoul(*av, NULL, 0); av++; ac--;
		if (rule.fw_divert_port == 0) {
			struct servent *s;
			setservent(1);
			s = getservbyname(av[-1], "divert");
			if (s != NULL)
				rule.fw_divert_port = ntohs(s->s_port);
			else
				show_usage("illegal %s port", "tee divert");
		}
#ifndef IPFW_TEE_IS_FINALLY_IMPLEMENTED
		err(EX_USAGE, "the ``tee'' action is not implemented");
#endif
	} else if (!strncmp(*av,"fwd",strlen(*av)) ||
		   !strncmp(*av,"forward",strlen(*av))) {
		struct in_addr dummyip;
		char *pp;
		rule.fw_flg |= IP_FW_F_FWD; av++; ac--;
		if (!ac)
			show_usage("missing forwarding IP address");
		rule.fw_fwd_ip.sin_len = sizeof(struct sockaddr_in);
		rule.fw_fwd_ip.sin_family = AF_INET;
		rule.fw_fwd_ip.sin_port = 0;
		pp = strchr(*av, ':');
		if(pp == NULL)
			pp = strchr(*av, ',');
		if(pp != NULL)
		{
			*(pp++) = '\0';
			rule.fw_fwd_ip.sin_port = lookup_port(pp, 1, 1);
			if(rule.fw_fwd_ip.sin_port == (unsigned int)-1)
				show_usage("illegal forwarding port");
		}
		fill_ip(&(rule.fw_fwd_ip.sin_addr), &dummyip, &ac, &av);
		if (rule.fw_fwd_ip.sin_addr.s_addr == 0)
			show_usage("illegal forwarding IP address");

	} else if (!strncmp(*av,"skipto",strlen(*av))) {
		rule.fw_flg |= IP_FW_F_SKIPTO; av++; ac--;
		if (!ac)
			show_usage("missing skipto rule number");
		rule.fw_skipto_rule = strtoul(*av, NULL, 0); av++; ac--;
	} else if ((!strncmp(*av,"deny",strlen(*av))
		    || !strncmp(*av,"drop",strlen(*av)))) {
		rule.fw_flg |= IP_FW_F_DENY; av++; ac--;
	} else if (!strncmp(*av,"reject",strlen(*av))) {
		rule.fw_flg |= IP_FW_F_REJECT; av++; ac--;
		rule.fw_reject_code = ICMP_UNREACH_HOST;
	} else if (!strncmp(*av,"reset",strlen(*av))) {
		rule.fw_flg |= IP_FW_F_REJECT; av++; ac--;
		rule.fw_reject_code = IP_FW_REJECT_RST;	/* check TCP later */
	} else if (!strncmp(*av,"unreach",strlen(*av))) {
		rule.fw_flg |= IP_FW_F_REJECT; av++; ac--;
		fill_reject_code(&rule.fw_reject_code, *av); av++; ac--;
	} else {
		show_usage("invalid action ``%s''", *av);
	}

	/* [log] */
	if (ac && !strncmp(*av,"log",strlen(*av))) {
		rule.fw_flg |= IP_FW_F_PRN; av++; ac--;
	}

	/* protocol */
	if (ac == 0)
		show_usage("missing protocol");
	if ((proto = atoi(*av)) > 0) {
		rule.fw_prot = proto; av++; ac--;
	} else if (!strncmp(*av,"all",strlen(*av))) {
		rule.fw_prot = IPPROTO_IP; av++; ac--;
	} else if ((pe = getprotobyname(*av)) != NULL) {
		rule.fw_prot = pe->p_proto; av++; ac--;
	} else {
		show_usage("invalid protocol ``%s''", *av);
	}

	if (rule.fw_prot != IPPROTO_TCP
	    && (rule.fw_flg & IP_FW_F_COMMAND) == IP_FW_F_REJECT
	    && rule.fw_reject_code == IP_FW_REJECT_RST)
		show_usage("``reset'' is only valid for tcp packets");

	/* from */
	if (ac && !strncmp(*av,"from",strlen(*av))) { av++; ac--; }
	else
		show_usage("missing ``from''");

	if (ac && !strncmp(*av,"not",strlen(*av))) {
		rule.fw_flg |= IP_FW_F_INVSRC;
		av++; ac--;
	}
	if (!ac)
		show_usage("missing arguments");

	fill_ip(&rule.fw_src, &rule.fw_smsk, &ac, &av);

	if (ac && (isdigit(**av) || lookup_port(*av, 1, 1) >= 0)) {
		u_short nports = 0;

		if (fill_port(&nports, rule.fw_uar.fw_pts, 0, *av))
			rule.fw_flg |= IP_FW_F_SRNG;
		IP_FW_SETNSRCP(&rule, nports);
		av++; ac--;
	}

	/* to */
	if (ac && !strncmp(*av,"to",strlen(*av))) { av++; ac--; }
	else
		show_usage("missing ``to''");

	if (ac && !strncmp(*av,"not",strlen(*av))) {
		rule.fw_flg |= IP_FW_F_INVDST;
		av++; ac--;
	}
	if (!ac)
		show_usage("missing arguments");

	fill_ip(&rule.fw_dst, &rule.fw_dmsk, &ac, &av);

	if (ac && (isdigit(**av) || lookup_port(*av, 1, 1) >= 0)) {
		u_short	nports = 0;

		if (fill_port(&nports,
		    rule.fw_uar.fw_pts, IP_FW_GETNSRCP(&rule), *av))
			rule.fw_flg |= IP_FW_F_DRNG;
		IP_FW_SETNDSTP(&rule, nports);
		av++; ac--;
	}

	if ((rule.fw_prot != IPPROTO_TCP) && (rule.fw_prot != IPPROTO_UDP)
	    && (IP_FW_GETNSRCP(&rule) || IP_FW_GETNDSTP(&rule))) {
		show_usage("only TCP and UDP protocols are valid"
		    " with port specifications");
	}

	while (ac) {
		if (!strncmp(*av,"in",strlen(*av))) { 
			rule.fw_flg |= IP_FW_F_IN;
			av++; ac--; continue;
		}
		if (!strncmp(*av,"out",strlen(*av))) { 
			rule.fw_flg |= IP_FW_F_OUT;
			av++; ac--; continue;
		}
		if (ac && !strncmp(*av,"xmit",strlen(*av))) {
			union ip_fw_if ifu;
			int byname;

			if (saw_via) {
badviacombo:
				show_usage("``via'' is incompatible"
				    " with ``xmit'' and ``recv''");
			}
			saw_xmrc = 1;
			av++; ac--; 
			fill_iface("xmit", &ifu, &byname, ac, *av);
			rule.fw_out_if = ifu;
			rule.fw_flg |= IP_FW_F_OIFACE;
			if (byname)
				rule.fw_flg |= IP_FW_F_OIFNAME;
			av++; ac--; continue;
		}
		if (ac && !strncmp(*av,"recv",strlen(*av))) {
			union ip_fw_if ifu;
			int byname;

			if (saw_via)
				goto badviacombo;
			saw_xmrc = 1;
			av++; ac--; 
			fill_iface("recv", &ifu, &byname, ac, *av);
			rule.fw_in_if = ifu;
			rule.fw_flg |= IP_FW_F_IIFACE;
			if (byname)
				rule.fw_flg |= IP_FW_F_IIFNAME;
			av++; ac--; continue;
		}
		if (ac && !strncmp(*av,"via",strlen(*av))) {
			union ip_fw_if ifu;
			int byname = 0;

			if (saw_xmrc)
				goto badviacombo;
			saw_via = 1;
			av++; ac--; 
			fill_iface("via", &ifu, &byname, ac, *av);
			rule.fw_out_if = rule.fw_in_if = ifu;
			if (byname)
				rule.fw_flg |=
				    (IP_FW_F_IIFNAME | IP_FW_F_OIFNAME);
			av++; ac--; continue;
		}
		if (!strncmp(*av,"fragment",strlen(*av))) {
			rule.fw_flg |= IP_FW_F_FRAG;
			av++; ac--; continue;
		}
		if (!strncmp(*av,"ipoptions",strlen(*av))) { 
			av++; ac--; 
			if (!ac)
				show_usage("missing argument"
				    " for ``ipoptions''");
			fill_ipopt(&rule.fw_ipopt, &rule.fw_ipnopt, av);
			av++; ac--; continue;
		}
		if (rule.fw_prot == IPPROTO_TCP) {
			if (!strncmp(*av,"established",strlen(*av))) { 
				rule.fw_tcpf  |= IP_FW_TCPF_ESTAB;
				av++; ac--; continue;
			}
			if (!strncmp(*av,"setup",strlen(*av))) { 
				rule.fw_tcpf  |= IP_FW_TCPF_SYN;
				rule.fw_tcpnf  |= IP_FW_TCPF_ACK;
				av++; ac--; continue;
			}
			if (!strncmp(*av,"tcpflags",strlen(*av))) { 
				av++; ac--; 
				if (!ac)
					show_usage("missing argument"
					    " for ``tcpflags''");
				fill_tcpflag(&rule.fw_tcpf, &rule.fw_tcpnf, av);
				av++; ac--; continue;
			}
		}
		if (rule.fw_prot == IPPROTO_ICMP) {
			if (!strncmp(*av,"icmptypes",strlen(*av))) {
				av++; ac--;
				if (!ac)
					show_usage("missing argument"
					    " for ``icmptypes''");
				fill_icmptypes(rule.fw_uar.fw_icmptypes,
				    av, &rule.fw_flg);
				av++; ac--; continue;
			}
		}
		show_usage("unknown argument ``%s''", *av);
	}

	/* No direction specified -> do both directions */
	if (!(rule.fw_flg & (IP_FW_F_OUT|IP_FW_F_IN)))
		rule.fw_flg |= (IP_FW_F_OUT|IP_FW_F_IN);

	/* Sanity check interface check, but handle "via" case separately */
	if (saw_via) {
		if (rule.fw_flg & IP_FW_F_IN)
			rule.fw_flg |= IP_FW_F_IIFACE;
		if (rule.fw_flg & IP_FW_F_OUT)
			rule.fw_flg |= IP_FW_F_OIFACE;
	} else if ((rule.fw_flg & IP_FW_F_OIFACE) && (rule.fw_flg & IP_FW_F_IN))
		show_usage("can't check xmit interface of incoming packets");

	/* frag may not be used in conjunction with ports or TCP flags */
	if (rule.fw_flg & IP_FW_F_FRAG) {
		if (rule.fw_tcpf || rule.fw_tcpnf)
			show_usage("can't mix 'frag' and tcpflags");

		if (rule.fw_nports)
			show_usage("can't mix 'frag' and port specifications");
	}

	if (!do_quiet)
		show_ipfw(&rule, 10, 10);
	i = setsockopt(s, IPPROTO_IP, IP_FW_ADD, &rule, sizeof rule);
	if (i)
		err(EX_UNAVAILABLE, "setsockopt(%s)", "IP_FW_ADD");
}

static void
zero (ac, av)
	int ac;
	char **av;
{
	av++; ac--;

	if (!ac) {
		/* clear all entries */
		if (setsockopt(s,IPPROTO_IP,IP_FW_ZERO,NULL,0)<0)
			err(EX_UNAVAILABLE, "setsockopt(%s)", "IP_FW_ZERO");
		if (!do_quiet)
			printf("Accounting cleared.\n");
	} else {
		struct ip_fw rule;
		int failed = EX_OK;

		memset(&rule, 0, sizeof rule);
		while (ac) {
			/* Rule number */
			if (isdigit(**av)) {
				rule.fw_number = atoi(*av); av++; ac--;
				if (setsockopt(s, IPPROTO_IP,
				    IP_FW_ZERO, &rule, sizeof rule)) {
					warn("rule %u: setsockopt(%s)", rule.fw_number,
						 "IP_FW_ZERO");
					failed = EX_UNAVAILABLE;
				}
				else if (!do_quiet)
					printf("Entry %d cleared\n",
					    rule.fw_number);
			} else
				show_usage("invalid rule number ``%s''", *av);
		}
		if (failed != EX_OK)
			exit(failed);
	}
}

static int
ipfw_main(ac,av)
	int 	ac;
	char 	**av;
{

	int 		ch;
	extern int 	optreset; /* XXX should be declared in <unistd.h> */

	if ( ac == 1 ) {
		show_usage(NULL);
	}

	/* Set the force flag for non-interactive processes */
	do_force = !isatty(STDIN_FILENO);

	optind = optreset = 1;
	while ((ch = getopt(ac, av, "afqtN")) != -1)
	switch(ch) {
		case 'a':
			do_acct=1;
			break;
		case 'f':
			do_force=1;
			break;
		case 'q':
			do_quiet=1;
			break;
		case 't':
			do_time=1;
			break;
		case 'N':
	 		do_resolv=1;
			break;
		default:
			show_usage(NULL);
	}

	ac -= optind;
	if (*(av+=optind)==NULL) {
		 show_usage("Bad arguments");
	}

        if (!strncmp(*av, "pipe", strlen(*av))) {
                do_pipe = 1 ;
                ac-- ;
                av++ ;
        }
	if (!ac) {
		show_usage("pipe requires arguments");
	}
        /* allow argument swapping */
        if (ac > 1 && *av[0]>='0' && *av[0]<='9') {
                char *p = av[0] ;
                av[0] = av[1] ;
                av[1] = p ;
        }
	if (!strncmp(*av, "add", strlen(*av))) {
		add(ac,av);
        } else if (do_pipe && !strncmp(*av, "config", strlen(*av))) {
                config_pipe(ac,av);
	} else if (!strncmp(*av, "delete", strlen(*av))) {
		delete(ac,av);
	} else if (!strncmp(*av, "flush", strlen(*av))) {
		int do_flush = 0;

		if ( do_force || do_quiet )
			do_flush = 1;
		else {
			int c;

			/* Ask the user */
			printf("Are you sure? [yn] ");
			do {
				fflush(stdout);
				c = toupper(getc(stdin));
				while (c != '\n' && getc(stdin) != '\n')
					if (feof(stdin))
						return (0);
			} while (c != 'Y' && c != 'N');
			printf("\n");
			if (c == 'Y') 
				do_flush = 1;
		}
		if ( do_flush ) {
			if (setsockopt(s,IPPROTO_IP,IP_FW_FLUSH,NULL,0) < 0)
				err(EX_UNAVAILABLE, "setsockopt(%s)", "IP_FW_FLUSH");
			if (!do_quiet)
				printf("Flushed all rules.\n");
		}
	} else if (!strncmp(*av, "zero", strlen(*av))) {
		zero(ac,av);
	} else if (!strncmp(*av, "print", strlen(*av))) {
		list(--ac,++av);
	} else if (!strncmp(*av, "list", strlen(*av))) {
		list(--ac,++av);
	} else if (!strncmp(*av, "show", strlen(*av))) {
		do_acct++;
		list(--ac,++av);
	} else {
		show_usage("Bad arguments");
	}
	return 0;
}

int 
main(ac, av)
	int	ac;
	char	**av;
{
#define MAX_ARGS	32
#define WHITESP		" \t\f\v\n\r"
	char	buf[BUFSIZ];
	char	*a, *p, *args[MAX_ARGS], *cmd = NULL;
	char	linename[10];
	int 	i, c, qflag, pflag, status;
	FILE	*f = NULL;
	pid_t	preproc = 0;

	s = socket( AF_INET, SOCK_RAW, IPPROTO_RAW );
	if ( s < 0 )
		err(EX_UNAVAILABLE, "socket");

	setbuf(stdout,0);

	if (ac > 1 && access(av[ac - 1], R_OK) == 0) {
		qflag = pflag = i = 0;
		lineno = 0;

		while ((c = getopt(ac, av, "D:U:p:q")) != -1)
			switch(c) {
			case 'D':
				if (!pflag)
					errx(EX_USAGE, "-D requires -p");
				if (i > MAX_ARGS - 2)
					errx(EX_USAGE,
					     "too many -D or -U options");
				args[i++] = "-D";
				args[i++] = optarg;
				break;

			case 'U':
				if (!pflag)
					errx(EX_USAGE, "-U requires -p");
				if (i > MAX_ARGS - 2)
					errx(EX_USAGE,
					     "too many -D or -U options");
				args[i++] = "-U";
				args[i++] = optarg;
				break;

			case 'p':
				pflag = 1;
				cmd = optarg;
				args[0] = cmd;
				i = 1;
				break;

			case 'q':
				qflag = 1;
				break;

			default:
				show_usage(NULL);
			}

		av += optind;
		ac -= optind;
		if (ac != 1)
			show_usage("extraneous filename arguments");

		if ((f = fopen(av[0], "r")) == NULL)
			err(EX_UNAVAILABLE, "fopen: %s", av[0]);

		if (pflag) {
			/* pipe through preprocessor (cpp or m4) */
			int pipedes[2];

			args[i] = 0;

			if (pipe(pipedes) == -1)
				err(EX_OSERR, "cannot create pipe");

			switch((preproc = fork())) {
			case -1:
				err(EX_OSERR, "cannot fork");

			case 0:
				/* child */
				if (dup2(fileno(f), 0) == -1 ||
				    dup2(pipedes[1], 1) == -1)
					err(EX_OSERR, "dup2()");
				fclose(f);
				close(pipedes[1]);
				close(pipedes[0]);
				execvp(cmd, args);
				err(EX_OSERR, "execvp(%s) failed", cmd);

			default:
				/* parent */
				fclose(f);
				close(pipedes[1]);
				if ((f = fdopen(pipedes[0], "r")) == NULL) {
					int savederrno = errno;

					(void)kill(preproc, SIGTERM);
					errno = savederrno;
					err(EX_OSERR, "fdopen()");
				}
			}
		}

		while (fgets(buf, BUFSIZ, f)) {
			lineno++;
			sprintf(linename, "Line %d", lineno);
			args[0] = linename;

			if (*buf == '#')
				continue;
			if ((p = strchr(buf, '#')) != NULL)
				*p = '\0';
			i=1;
			if (qflag) args[i++]="-q";
			for (a = strtok(buf, WHITESP);
			    a && i < MAX_ARGS; a = strtok(NULL, WHITESP), i++)
				args[i] = a;
			if (i == (qflag? 2: 1))
				continue;
			if (i == MAX_ARGS)
				errx(EX_USAGE, "%s: too many arguments", linename);
			args[i] = NULL;

			ipfw_main(i, args); 
		}
		fclose(f);
		if (pflag) {
			if (waitpid(preproc, &status, 0) != -1) {
				if (WIFEXITED(status)) {
					if (WEXITSTATUS(status) != EX_OK)
						errx(EX_UNAVAILABLE,
						     "preprocessor exited with status %d",
						     WEXITSTATUS(status));
				} else if (WIFSIGNALED(status)) {
					errx(EX_UNAVAILABLE,
					     "preprocessor exited with signal %d",
					     WTERMSIG(status));
				}
			}
		}

	} else
		ipfw_main(ac,av);
	return EX_OK;
}
