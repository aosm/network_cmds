/*	$KAME: cfparse.y,v 1.114 2003/02/03 08:27:50 itojun Exp $	*/

%{
#include <sys/types.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet6/ipsec.h>
#include <netkey/key_var.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#if !defined(HAVE_GETADDRINFO) || !defined(HAVE_GETNAMEINFO)
#include "addrinfo.h"
#endif

#include "var.h"
#include "misc.h"
#include "vmbuf.h"
#include "plog.h"
#include "sockmisc.h"
#include "str2val.h"
#include "debug.h"

/*#include "cfparse.h"*/
#include "cftoken.h"
#include "algorithm.h"
#include "localconf.h"
#include "policy.h"
#include "sainfo.h"
#include "oakley.h"
#include "pfkey.h"
#include "remoteconf.h"
#include "grabmyaddr.h"
#include "isakmp_var.h"
#include "handler.h"
#include "isakmp.h"
#include "ipsec_doi.h"
#include "strnames.h"
#include "gcmalloc.h"
#ifdef HAVE_GSSAPI
#include "gssapi.h"
#endif
#include "vendorid.h"
#include <CoreFoundation/CFData.h>

struct proposalspec {
	time_t lifetime;		/* for isakmp/ipsec */
	int lifebyte;			/* for isakmp/ipsec */
	struct secprotospec *spspec;	/* the head is always current spec. */
	struct proposalspec *next;	/* the tail is the most prefered. */
	struct proposalspec *prev;
};

struct secprotospec {
	int prop_no;
	int trns_no;
	int strength;		/* for isakmp/ipsec */
	int encklen;		/* for isakmp/ipsec */
	time_t lifetime;	/* for isakmp */
	int lifebyte;		/* for isakmp */
	int proto_id;		/* for ipsec (isakmp?) */
	int ipsec_level;	/* for ipsec */
	int encmode;		/* for ipsec */
	int vendorid;		/* for isakmp */
	char *gssid;
	struct sockaddr *remote;
	int algclass[MAXALGCLASS];

	struct secprotospec *next;	/* the tail is the most prefiered. */
	struct secprotospec *prev;
	struct proposalspec *back;
};

static int num2dhgroup[] = {
	0,
	OAKLEY_ATTR_GRP_DESC_MODP768,
	OAKLEY_ATTR_GRP_DESC_MODP1024,
	OAKLEY_ATTR_GRP_DESC_EC2N155,
	OAKLEY_ATTR_GRP_DESC_EC2N185,
	OAKLEY_ATTR_GRP_DESC_MODP1536,
};

static struct remoteconf *cur_rmconf;
static int tmpalgtype[MAXALGCLASS];
static struct sainfo *cur_sainfo;
static int cur_algclass;

static struct proposalspec *prhead;	/* the head is always current. */

static struct proposalspec *newprspec __P((void));
static void cleanprhead __P((void));
static void insprspec __P((struct proposalspec *, struct proposalspec **));
static struct secprotospec *newspspec __P((void));
static void insspspec __P((struct secprotospec *, struct proposalspec **));

static int set_isakmp_proposal
	__P((struct remoteconf *, struct proposalspec *));
static void clean_tmpalgtype __P((void));
static int expand_isakmpspec __P((int, int, int *,
	int, int, time_t, int, int, int, char *, struct remoteconf *));
	
static int base64toCFData(vchar_t *, CFDataRef*);

#if 0
static int fix_lifebyte __P((u_long));
#endif
%}

%union {
	unsigned long num;
	vchar_t *val;
	struct remoteconf *rmconf;
	struct sockaddr *saddr;
	struct sainfoalg *alg;
}

	/* path */
%token PATH PATHTYPE
	/* include */
%token INCLUDE
	/* self information */
%token IDENTIFIER VENDORID
	/* logging */
%token LOGGING LOGLEV
	/* padding */
%token PADDING PAD_RANDOMIZE PAD_RANDOMIZELEN PAD_MAXLEN PAD_STRICT PAD_EXCLTAIL
	/* listen */
%token LISTEN X_ISAKMP X_ADMIN STRICT_ADDRESS
	/* timer */
%token RETRY RETRY_COUNTER RETRY_INTERVAL RETRY_PERSEND
%token RETRY_PHASE1 RETRY_PHASE2
	/* algorithm */
%token ALGORITHM_CLASS ALGORITHMTYPE STRENGTHTYPE
	/* sainfo */
%token SAINFO
	/* remote */
%token REMOTE ANONYMOUS
%token EXCHANGE_MODE EXCHANGETYPE DOI DOITYPE SITUATION SITUATIONTYPE
%token CERTIFICATE_TYPE CERTTYPE PEERS_CERTFILE VERIFY_CERT SEND_CERT SEND_CR
%token IDENTIFIERTYPE MY_IDENTIFIER PEERS_IDENTIFIER VERIFY_IDENTIFIER
%token SHARED_SECRET SECRETTYPE
%token OPEN_DIR_AUTH_GROUP IN_KEYCHAIN
%token CERTIFICATE_VERIFICATION VERIFICATION_MODULE VERIFICATION_OPTION
%token DNSSEC CERT_X509
%token NONCE_SIZE DH_GROUP KEEPALIVE PASSIVE INITIAL_CONTACT
%token PROPOSAL_CHECK PROPOSAL_CHECK_LEVEL
%token GENERATE_POLICY SUPPORT_MIP6
%token PROPOSAL
%token EXEC_PATH EXEC_COMMAND EXEC_SUCCESS EXEC_FAILURE
%token GSSAPI_ID
%token COMPLEX_BUNDLE

%token PREFIX PORT PORTANY UL_PROTO ANY
%token PFS_GROUP LIFETIME LIFETYPE_TIME LIFETYPE_BYTE STRENGTH

%token NUMBER SWITCH BOOLEAN
%token HEXSTRING QUOTEDSTRING ADDRSTRING
%token UNITTYPE_BYTE UNITTYPE_KBYTES UNITTYPE_MBYTES UNITTYPE_TBYTES
%token UNITTYPE_SEC UNITTYPE_MIN UNITTYPE_HOUR
%token EOS BOC EOC COMMA

%type <num> NUMBER BOOLEAN SWITCH keylength
%type <num> PATHTYPE IDENTIFIERTYPE LOGLEV 
%type <num> SECRETTYPE
%type <num> ALGORITHM_CLASS dh_group_num
%type <num> ALGORITHMTYPE STRENGTHTYPE
%type <num> PREFIX prefix PORT port ike_port
%type <num> ul_proto UL_PROTO
%type <num> EXCHANGETYPE DOITYPE SITUATIONTYPE
%type <num> CERTTYPE CERT_X509 PROPOSAL_CHECK_LEVEL
%type <num> VERIFICATION_MODULE VERIFICATION_OPTION
%type <num> unittype_time unittype_byte
%type <val> QUOTEDSTRING HEXSTRING ADDRSTRING sainfo_id
%type <val> identifierstring
%type <saddr> remote_index ike_addrinfo_port
%type <alg> algorithm

%%

statements
	:	/* nothing */
	|	statements statement
	;
statement
	:	path_statement
	|	include_statement
	|	identifier_statement
	|	logging_statement
	|	padding_statement
	|	listen_statement
	|	timer_statement
	|	sainfo_statement
	|	remote_statement
	|	special_statement
	;

	/* path */
path_statement
	:	PATH PATHTYPE QUOTEDSTRING
		{
			if ($2 > LC_PATHTYPE_MAX) {
				yyerror("invalid path type %d", $2);
				return -1;
			}

			/* free old pathinfo */
			if (lcconf->pathinfo[$2])
				racoon_free(lcconf->pathinfo[$2]);

			/* set new pathinfo */
			lcconf->pathinfo[$2] = strdup($3->v);
			vfree($3);
		}
		EOS
	;

	/* special */
special_statement
	:	COMPLEX_BUNDLE SWITCH { lcconf->complex_bundle = $2; } EOS
	;

	/* include */
include_statement
	:	INCLUDE QUOTEDSTRING EOS
		{
			char path[MAXPATHLEN];

			getpathname(path, sizeof(path),
				LC_PATHTYPE_INCLUDE, $2->v);
			vfree($2);
			if (yycf_switch_buffer(path) != 0)
				return -1;
		}
	;

	/* self infomation */
identifier_statement
	:	IDENTIFIER identifier_stmt
	;
identifier_stmt
	:	VENDORID
		{
			/*XXX to be deleted */
		}
		QUOTEDSTRING EOS
	|	IDENTIFIERTYPE QUOTEDSTRING
		{
			/*XXX to be deleted */
			$2->l--;	/* nuke '\0' */
			lcconf->ident[$1] = $2;
			if (lcconf->ident[$1] == NULL) {
				yyerror("failed to set my ident: %s",
					strerror(errno));
				return -1;
			}
		}
		EOS
	;

	/* logging */
logging_statement
	:	LOGGING log_level EOS
	;
log_level
	:	HEXSTRING
		{
			/*
			 * XXX ignore it because this specification
			 * will be obsoleted.
			 */
			yywarn("see racoon.conf(5), such a log specification will be obsoleted.");
			vfree($1);
		}
	|	LOGLEV
		{
			/*
			 * set the loglevel by configuration file only when
			 * the command line did not specify any loglevel.
			 */
			if (loglevel <= LLV_BASE)
				loglevel += $1;
		}
	;

	/* padding */
padding_statement
	:	PADDING BOC padding_stmts EOC
	;
padding_stmts
	:	/* nothing */
	|	padding_stmts padding_stmt
	;
padding_stmt
	:	PAD_RANDOMIZE SWITCH { lcconf->pad_random = $2; } EOS
	|	PAD_RANDOMIZELEN SWITCH { lcconf->pad_randomlen = $2; } EOS
	|	PAD_MAXLEN NUMBER { lcconf->pad_maxsize = $2; } EOS
	|	PAD_STRICT SWITCH { lcconf->pad_strict = $2; } EOS
	|	PAD_EXCLTAIL SWITCH { lcconf->pad_excltail = $2; } EOS
	;

	/* listen */
listen_statement
	:	LISTEN BOC listen_stmts EOC
	;
listen_stmts
	:	/* nothing */
	|	listen_stmts listen_stmt
	;
listen_stmt
	:	X_ISAKMP ike_addrinfo_port
		{
			struct myaddrs *p;

			p = newmyaddr();
			if (p == NULL) {
				yyerror("failed to allocate myaddrs");
				return -1;
			}
			p->addr = $2;
			if (p->addr == NULL) {
				yyerror("failed to copy sockaddr ");
				delmyaddr(p);
				return -1;
			}
			p->sock = -1;
			p->nattsock = -1;
			p->addrcount = 1;
			insmyaddr(p, &lcconf->myaddrs);

			lcconf->autograbaddr = 0;
		}
		EOS
	|	X_ADMIN
		{
			yyerror("admin directive is obsoleted.");
		}
		PORT EOS
	|	STRICT_ADDRESS { lcconf->strict_address = TRUE; } EOS
	;
ike_addrinfo_port
	:	ADDRSTRING ike_port
		{
			char portbuf[10];

			snprintf(portbuf, sizeof(portbuf), "%ld", $2);
			$$ = str2saddr($1->v, portbuf);
			vfree($1);
			if (!$$)
				return -1;
		}
	;
ike_port
	:	/* nothing */	{ $$ = PORT_ISAKMP; }
	|	PORT		{ $$ = $1; }
	;

	/* timer */
timer_statement
	:	RETRY BOC timer_stmts EOC
	;
timer_stmts
	:	/* nothing */
	|	timer_stmts timer_stmt
	;
timer_stmt
	:	RETRY_COUNTER NUMBER
		{
			lcconf->retry_counter = $2;
		}
		EOS
	|	RETRY_INTERVAL NUMBER unittype_time
		{
			lcconf->retry_interval = $2 * $3;
		}
		EOS
	|	RETRY_PERSEND NUMBER
		{
			lcconf->count_persend = $2;
		}
		EOS
	|	RETRY_PHASE1 NUMBER unittype_time
		{
			lcconf->retry_checkph1 = $2 * $3;
		}
		EOS
	|	RETRY_PHASE2 NUMBER unittype_time
		{
			lcconf->wait_ph2complete = $2 * $3;
		}
		EOS
	;

	/* sainfo */
sainfo_statement
	:	SAINFO
		{
			cur_sainfo = newsainfo();
			if (cur_sainfo == NULL) {
				yyerror("failed to allocate sainfo");
				return -1;
			}
		}
		sainfo_name BOC sainfo_specs
		{
			struct sainfo *check;

			/* default */
			if (cur_sainfo->algs[algclass_ipsec_enc] == 0) {
				yyerror("no encryption algorithm at %s",
					sainfo2str(cur_sainfo));
				return -1;
			}
			if (cur_sainfo->algs[algclass_ipsec_auth] == 0) {
				yyerror("no authentication algorithm at %s",
					sainfo2str(cur_sainfo));
				return -1;
			}
			if (cur_sainfo->algs[algclass_ipsec_comp] == 0) {
				yyerror("no compression algorithm at %s",
					sainfo2str(cur_sainfo));
				return -1;
			}

			/* duplicate check */
			check = getsainfo(cur_sainfo->idsrc, cur_sainfo->iddst);
			if (check && (!check->idsrc && !cur_sainfo->idsrc)) {
				yyerror("duplicated sainfo: %s",
					sainfo2str(cur_sainfo));
				return -1;
			}
			inssainfo(cur_sainfo);
		}
		EOC
	;
sainfo_name
	:	ANONYMOUS
		{
			cur_sainfo->idsrc = NULL;
			cur_sainfo->iddst = NULL;
		}
	|	sainfo_id sainfo_id
		{
			cur_sainfo->idsrc = $1;
			cur_sainfo->iddst = $2;
		}
	;
sainfo_id
	:	IDENTIFIERTYPE ADDRSTRING prefix port ul_proto
		{
			char portbuf[10];
			struct sockaddr *saddr;

			if (($5 == IPPROTO_ICMP || $5 == IPPROTO_ICMPV6)
			 && ($4 != IPSEC_PORT_ANY || $4 != IPSEC_PORT_ANY)) {
				yyerror("port number must be \"any\".");
				return -1;
			}

			snprintf(portbuf, sizeof(portbuf), "%lu", $4);
			saddr = str2saddr($2->v, portbuf);
			vfree($2);
			if (saddr == NULL)
				return -1;

			switch (saddr->sa_family) {
			case AF_INET:
				if ($5 == IPPROTO_ICMPV6) {
					yyerror("upper layer protocol mismatched.\n");
					racoon_free(saddr);
					return -1;
				}
				$$ = ipsecdoi_sockaddr2id(saddr,
					$3 == ~0 ? (sizeof(struct in_addr) << 3): $3,
					$5);
				break;
#ifdef INET6
			case AF_INET6:
				if ($5 == IPPROTO_ICMP) {
					yyerror("upper layer protocol mismatched.\n");
					racoon_free(saddr);
					return -1;
				}
				$$ = ipsecdoi_sockaddr2id(saddr,
					$3 == ~0 ? (sizeof(struct in6_addr) << 3) : $3,
					$5);
				break;
#endif
			default:
				yyerror("invalid family: %d", saddr->sa_family);
				break;
			}
			racoon_free(saddr);
			if ($$ == NULL)
				return -1;
		}
	|	IDENTIFIERTYPE QUOTEDSTRING
		{
			struct ipsecdoi_id_b *id_b;

			if ($1 == IDTYPE_ASN1DN) {
				yyerror("id type forbidden: %d", $1);
				return -1;
			}

			$2->l--;

			$$ = vmalloc(sizeof(*id_b) + $2->l);
			if ($$ == NULL) {
				yyerror("failed to allocate identifier");
				return -1;
			}

			id_b = (struct ipsecdoi_id_b *)$$->v;
			id_b->type = idtype2doi($1);

			id_b->proto_id = 0;
			id_b->port = 0;

			memcpy($$->v + sizeof(*id_b), $2->v, $2->l);
		}
	;
sainfo_specs
	:	/* nothing */
	|	sainfo_specs sainfo_spec
	;
sainfo_spec
	:	PFS_GROUP dh_group_num
		{
			cur_sainfo->pfs_group = $2;
		}
		EOS
	|	LIFETIME LIFETYPE_TIME NUMBER unittype_time
		{
			cur_sainfo->lifetime = $3 * $4;
		}
		EOS
	|	LIFETIME LIFETYPE_BYTE NUMBER unittype_byte
		{
#if 1
			yyerror("byte lifetime support is deprecated");
			return -1;
#else
			cur_sainfo->lifebyte = fix_lifebyte($3 * $4);
			if (cur_sainfo->lifebyte == 0)
				return -1;
#endif
		}
		EOS
	|	ALGORITHM_CLASS {
			cur_algclass = $1;
		}
		algorithms EOS
	|	IDENTIFIER IDENTIFIERTYPE
		{
			yyerror("it's deprecated to specify a identifier in phase 2");
		}
		EOS
	|	MY_IDENTIFIER IDENTIFIERTYPE QUOTEDSTRING
		{
			yyerror("it's deprecated to specify a identifier in phase 2");
		}
		EOS
	;

algorithms
	:	algorithm
		{
			inssainfoalg(&cur_sainfo->algs[cur_algclass], $1);
		}
	|	algorithm
		{
			inssainfoalg(&cur_sainfo->algs[cur_algclass], $1);
		}
		COMMA algorithms
	;
algorithm
	:	ALGORITHMTYPE keylength
		{
			int defklen;

			$$ = newsainfoalg();
			if ($$ == NULL) {
				yyerror("failed to get algorithm allocation");
				return -1;
			}

			$$->alg = algtype2doi(cur_algclass, $1);
			if ($$->alg == -1) {
				yyerror("algorithm mismatched");
				racoon_free($$);
				return -1;
			}

			defklen = default_keylen(cur_algclass, $1);
			if (defklen == 0) {
				if ($2) {
					yyerror("keylen not allowed");
					racoon_free($$);
					return -1;
				}
			} else {
				if ($2 && check_keylen(cur_algclass, $1, $2) < 0) {
					yyerror("invalid keylen %d", $2);
					racoon_free($$);
					return -1;
				}
			}

			if ($2)
				$$->encklen = $2;
			else
				$$->encklen = defklen;

			/* check if it's supported algorithm by kernel */
			if (!(cur_algclass == algclass_ipsec_auth && $1 == algtype_non_auth)
			 && pk_checkalg(cur_algclass, $1, $$->encklen)) {
				int a = algclass2doi(cur_algclass);
				int b = algtype2doi(cur_algclass, $1);
				if (a == IPSECDOI_ATTR_AUTH)
					a = IPSECDOI_PROTO_IPSEC_AH;
				yyerror("algorithm %s not supported",
					s_ipsecdoi_trns(a, b));
				racoon_free($$);
				return -1;
			}
		}
	;
prefix
	:	/* nothing */ { $$ = ~0; }
	|	PREFIX { $$ = $1; }
	;
port
	:	/* nothing */ { $$ = IPSEC_PORT_ANY; }
	|	PORT { $$ = $1; }
	|	PORTANY { $$ = IPSEC_PORT_ANY; }
	;
ul_proto
	:	NUMBER { $$ = $1; }
	|	UL_PROTO { $$ = $1; }
	|	ANY { $$ = IPSEC_ULPROTO_ANY; }
	;
keylength
	:	/* nothing */ { $$ = 0; }
	|	NUMBER { $$ = $1; }
	;

	/* remote */
remote_statement
	:	REMOTE remote_index
		{
			struct remoteconf *new;
			struct proposalspec *prspec;
			
			new = newrmconf();
			if (new == NULL) {
				yyerror("failed to get new remoteconf.");
				return -1;
			}

			new->remote = $2;
			cur_rmconf = new;

			prspec = newprspec();
			if (prspec == NULL)
				return -1;
			prspec->lifetime = oakley_get_defaultlifetime();
			insprspec(prspec, &prhead);
		}
		BOC remote_specs
		{
			/* check a exchange mode */
			if (cur_rmconf->etypes == NULL) {
				yyerror("no exchange mode specified.\n");
				return -1;
			}

			if (cur_rmconf->idvtype == IDTYPE_ASN1DN
			 && cur_rmconf->mycertfile == NULL
			 && cur_rmconf->identity_in_keychain == 0) {
				yyerror("id type mismatched due to "
					"no CERT defined.\n");
				return -1;
			}
			
			if (cur_rmconf->cert_verification_option == VERIFICATION_OPTION_PEERS_IDENTIFIER
				&& cur_rmconf->idv_p == NULL) {
				yyerror("peers_identifier required for specified certificate "
					"verification option.\n");
					return -1;
			}

			if (set_isakmp_proposal(cur_rmconf, prhead) != 0)
				return -1;

			/* DH group settting if aggressive mode is there. */
			if (check_etypeok(cur_rmconf, ISAKMP_ETYPE_AGG) != NULL) {
				struct isakmpsa *p;
				int b = 0;

				/* DH group */
				for (p = cur_rmconf->proposal; p; p = p->next) {
					if (b == 0 || (b && b == p->dh_group)) {
						b = p->dh_group;
						continue;
					}
					yyerror("DH group must be equal "
						"to each proposals's "
						"when aggressive mode is "
						"used.\n");
					return -1;
				}
				cur_rmconf->dh_group = b;

				if (cur_rmconf->dh_group == 0) {
					yyerror("DH group must be required.\n");
					return -1;
				}

				/* DH group settting if PFS is required. */
				if (oakley_setdhgroup(cur_rmconf->dh_group,
						&cur_rmconf->dhgrp) < 0) {
					yyerror("failed to set DH value.\n");
					return -1;
				}
			}

			insrmconf(cur_rmconf);

			cleanprhead();
		}
		EOC
	;
remote_index
	:	ANONYMOUS ike_port
		{
			$$ = newsaddr(sizeof(struct sockaddr *));
			$$->sa_family = AF_UNSPEC;
			((struct sockaddr_in *)$$)->sin_port = htons($2);
		}
	|	ike_addrinfo_port
		{
			$$ = $1;
			if ($$ == NULL) {
				yyerror("failed to allocate sockaddr");
				return -1;
			}
		}
	;
remote_specs
	:	/* nothing */
	|	remote_specs remote_spec
	;
remote_spec
	:	EXCHANGE_MODE exchange_types EOS
	|	DOI DOITYPE { cur_rmconf->doitype = $2; } EOS
	|	SITUATION SITUATIONTYPE { cur_rmconf->sittype = $2; } EOS
	|	CERTIFICATE_TYPE cert_spec
	|	PEERS_CERTFILE QUOTEDSTRING
		{
#ifdef HAVE_SIGNING_C
			cur_rmconf->getcert_method = ISAKMP_GETCERT_LOCALFILE;
			cur_rmconf->peerscertfile = strdup($2->v);
			vfree($2);
#else
			yyerror("directive not supported");
			return -1;
#endif
		}
		EOS
	|	PEERS_CERTFILE DNSSEC
		{
#ifdef HAVE_SIGNING_C
			cur_rmconf->getcert_method = ISAKMP_GETCERT_DNS;
			cur_rmconf->peerscertfile = NULL;
#else
			yyerror("directive not supported");
			return -1;
#endif
		}
		EOS
	|	VERIFY_CERT SWITCH { cur_rmconf->verify_cert = $2; } EOS
	|	SEND_CERT SWITCH { cur_rmconf->send_cert = $2; } EOS
	|	SEND_CR SWITCH { cur_rmconf->send_cr = $2; } EOS
	|	CERTIFICATE_VERIFICATION VERIFICATION_MODULE { cur_rmconf->cert_verification = $2; } EOS
	|	CERTIFICATE_VERIFICATION VERIFICATION_MODULE VERIFICATION_OPTION
		{
			cur_rmconf->cert_verification = $2;
			cur_rmconf->cert_verification_option = $3;
		}
		EOS
	|	OPEN_DIR_AUTH_GROUP QUOTEDSTRING { cur_rmconf->open_dir_auth_group = $2; } EOS
	|	IDENTIFIER IDENTIFIERTYPE
		{
			/*XXX to be deleted */
			cur_rmconf->idvtype = $2;
		}
		EOS
	|	MY_IDENTIFIER IDENTIFIERTYPE identifierstring
		{
			if (set_identifier(&cur_rmconf->idv, $2, $3) != 0) {
				yyerror("failed to set identifer.\n");
				vfree($3);
				return -1;
			}
			vfree($3);
			cur_rmconf->idvtype = $2;
		}
		EOS
	|	PEERS_IDENTIFIER IDENTIFIERTYPE identifierstring
		{
			if (set_identifier(&cur_rmconf->idv_p, $2, $3) != 0) {
				yyerror("failed to set identifer.\n");
				vfree($3);
				return -1;
			}
			vfree($3);
			cur_rmconf->idvtype_p = $2;
		}
		EOS
	|	VERIFY_IDENTIFIER SWITCH { cur_rmconf->verify_identifier = $2; } EOS
	|	SHARED_SECRET SECRETTYPE QUOTEDSTRING { cur_rmconf->secrettype = $2; cur_rmconf->shared_secret = $3; } EOS
	|	NONCE_SIZE NUMBER { cur_rmconf->nonce_size = $2; } EOS
	|	DH_GROUP
		{
			yyerror("dh_group cannot be defined here.");
			return -1;
		}
		dh_group_num EOS
	|	KEEPALIVE { cur_rmconf->keepalive = TRUE; } EOS
	|	PASSIVE SWITCH { cur_rmconf->passive = $2; } EOS
	|	GENERATE_POLICY SWITCH { cur_rmconf->gen_policy = $2; } EOS
	|	SUPPORT_MIP6 SWITCH { cur_rmconf->support_mip6 = $2; } EOS
	|	INITIAL_CONTACT SWITCH { cur_rmconf->ini_contact = $2; } EOS
	|	PROPOSAL_CHECK PROPOSAL_CHECK_LEVEL { cur_rmconf->pcheck_level = $2; } EOS
	|	LIFETIME LIFETYPE_TIME NUMBER unittype_time
		{
			prhead->lifetime = $3 * $4;
		}
		EOS
	|	LIFETIME LIFETYPE_BYTE NUMBER unittype_byte
		{
#if 1
			yyerror("byte lifetime support is deprecated");
			return -1;
#else
			yywarn("the lifetime of bytes in phase 1 "
				"will be ignored at the moment.");
			prhead->lifebyte = fix_lifebyte($3 * $4);
			if (prhead->lifebyte == 0)
				return -1;
#endif
		}
		EOS
	|	PROPOSAL
		{
			struct secprotospec *spspec;

			spspec = newspspec();
			if (spspec == NULL)
				return -1;
			insspspec(spspec, &prhead);
		}
		BOC isakmpproposal_specs EOC
	;
exchange_types
	:	/* nothing */
	|	exchange_types EXCHANGETYPE
		{
			struct etypes *new;
			new = racoon_malloc(sizeof(struct etypes));
			if (new == NULL) {
				yyerror("filed to allocate etypes");
				return -1;
			}
			new->type = $2;
			new->next = NULL;
			if (cur_rmconf->etypes == NULL)
				cur_rmconf->etypes = new;
			else {
				struct etypes *p;
				for (p = cur_rmconf->etypes;
				     p->next != NULL;
				     p = p->next)
					;
				p->next = new;
			}
		}
	;
cert_spec
	:	CERT_X509 QUOTEDSTRING QUOTEDSTRING
		{
#ifdef HAVE_SIGNING_C
			cur_rmconf->certtype = $1;
			cur_rmconf->mycertfile = strdup($2->v);
			vfree($2);
			cur_rmconf->myprivfile = strdup($3->v);
			vfree($3);
#else
			yyerror("directive not supported");
			return -1;
#endif
		}
		EOS
	|	CERT_X509 IN_KEYCHAIN
		{
			cur_rmconf->certtype = $1;
			cur_rmconf->identity_in_keychain = 1;
			cur_rmconf->keychainCertRef = 0;
		}
		EOS
	;
	|	CERT_X509 IN_KEYCHAIN QUOTEDSTRING
		{
			int result;
			
			cur_rmconf->certtype = $1;
			cur_rmconf->identity_in_keychain = 1;
			result = base64toCFData($3, &cur_rmconf->keychainCertRef);
			vfree($3);
			if (result)
				return -1;			
		}
		EOS
	;

dh_group_num
	:	ALGORITHMTYPE
		{
			$$ = algtype2doi(algclass_isakmp_dh, $1);
			if ($$ == -1) {
				yyerror("must be DH group");
				return -1;
			}
		}
	|	NUMBER
		{
			if (ARRAYLEN(num2dhgroup) > $1 && num2dhgroup[$1] != 0) {
				$$ = num2dhgroup[$1];
			} else {
				yyerror("must be DH group");
				return -1;
			}
		}
	;
identifierstring
	:	/* nothing */ { $$ = NULL; }
	|	ADDRSTRING { $$ = $1; }
	|	QUOTEDSTRING { $$ = $1; }
	;
isakmpproposal_specs
	:	/* nothing */
	|	isakmpproposal_specs isakmpproposal_spec
	;
isakmpproposal_spec
	:	STRENGTH
		{
			yyerror("strength directive is obsoleted.");
		} STRENGTHTYPE EOS
	|	LIFETIME LIFETYPE_TIME NUMBER unittype_time
		{
			prhead->spspec->lifetime = $3 * $4;
		}
		EOS
	|	LIFETIME LIFETYPE_BYTE NUMBER unittype_byte
		{
#if 1
			yyerror("byte lifetime support is deprecated");
			return -1;
#else
			prhead->spspec->lifebyte = fix_lifebyte($3 * $4);
			if (prhead->spspec->lifebyte == 0)
				return -1;
#endif
		}
		EOS
	|	DH_GROUP dh_group_num
		{
			prhead->spspec->algclass[algclass_isakmp_dh] = $2;
		}
		EOS
	|	GSSAPI_ID QUOTEDSTRING
		{
			if (prhead->spspec->vendorid != VENDORID_GSSAPI) {
				yyerror("wrong Vendor ID for gssapi_id");
				return -1;
			}
			prhead->spspec->gssid = strdup($2->v);
		}
		EOS
	|	ALGORITHM_CLASS ALGORITHMTYPE keylength
		{
			int doi;
			int defklen;

			doi = algtype2doi($1, $2);
			if (doi == -1) {
				yyerror("algorithm mismatched 1");
				return -1;
			}

			switch ($1) {
			case algclass_isakmp_enc:
			/* reject suppressed algorithms */
#ifndef HAVE_OPENSSL_RC5_H
				if ($2 == algtype_rc5) {
					yyerror("algorithm %s not supported",
					    s_attr_isakmp_enc(doi));
					return -1;
				}
#endif
#ifndef HAVE_OPENSSL_IDEA_H
				if ($2 == algtype_idea) {
					yyerror("algorithm %s not supported",
					    s_attr_isakmp_enc(doi));
					return -1;
				}
#endif

				prhead->spspec->algclass[algclass_isakmp_enc] = doi;
				defklen = default_keylen($1, $2);
				if (defklen == 0) {
					if ($3) {
						yyerror("keylen not allowed");
						return -1;
					}
				} else {
					if ($3 && check_keylen($1, $2, $3) < 0) {
						yyerror("invalid keylen %d", $3);
						return -1;
					}
				}
				if ($3)
					prhead->spspec->encklen = $3;
				else
					prhead->spspec->encklen = defklen;
				break;
			case algclass_isakmp_hash:
				prhead->spspec->algclass[algclass_isakmp_hash] = doi;
				break;
			case algclass_isakmp_ameth:
				prhead->spspec->algclass[algclass_isakmp_ameth] = doi;
				/*
				 * We may have to set the Vendor ID for the
				 * authentication method we're using.
				 */
				switch ($2) {
				case algtype_gssapikrb:
					if (prhead->spspec->vendorid !=
					    VENDORID_UNKNOWN) {
						yyerror("Vendor ID mismatch "
						    "for auth method");
						return -1;
					}
					/*
					 * For interoperability with Win2k,
					 * we set the Vendor ID to "GSSAPI".
					 */
					prhead->spspec->vendorid =
					    VENDORID_GSSAPI;
					break;
				default:
					break;
				}
				break;
			default:
				yyerror("algorithm mismatched 2");
				return -1;
			}
		}
		EOS
	;

unittype_time
	:	UNITTYPE_SEC	{ $$ = 1; }
	|	UNITTYPE_MIN	{ $$ = 60; }
	|	UNITTYPE_HOUR	{ $$ = (60 * 60); }
	;
unittype_byte
	:	UNITTYPE_BYTE	{ $$ = 1; }
	|	UNITTYPE_KBYTES	{ $$ = 1024; }
	|	UNITTYPE_MBYTES	{ $$ = (1024 * 1024); }
	|	UNITTYPE_TBYTES	{ $$ = (1024 * 1024 * 1024); }
	;
%%

static struct proposalspec *
newprspec()
{
	struct proposalspec *new;

	new = racoon_calloc(1, sizeof(*new));
	if (new == NULL)
		yyerror("failed to allocate proposal");

	return new;
}

static void
cleanprhead()
{
	struct proposalspec *p, *next;

	if (prhead == NULL)
		return;

	for (p = prhead; p != NULL; p = next) {
		struct secprotospec *psp, *nextsp;
		for (psp = p->spspec; psp; psp = nextsp) {
			nextsp = psp->next;
			racoon_free(p->spspec);
		}
		next = p->next;
		racoon_free(p);
	}

	prhead = NULL;
}

/*
 * insert into head of list.
 */
static void
insprspec(prspec, head)
	struct proposalspec *prspec;
	struct proposalspec **head;
{
	if (*head != NULL)
		(*head)->prev = prspec;
	prspec->next = *head;
	*head = prspec;
}

static struct secprotospec *
newspspec()
{
	struct secprotospec *new;

	new = racoon_calloc(1, sizeof(*new));
	if (new == NULL) {
		yyerror("failed to allocate spproto");
		return NULL;
	}

	new->encklen = 0;	/*XXX*/

	/*
	 * Default to "uknown" vendor -- we will override this
	 * as necessary.  When we send a Vendor ID payload, an
	 * "unknown" will be translated to a KAME/racoon ID.
	 */
	new->vendorid = VENDORID_UNKNOWN;

	return new;
}

/*
 * insert into head of list.
 */
static void
insspspec(spspec, head)
	struct secprotospec *spspec;
	struct proposalspec **head;
{
	spspec->back = *head;

	if ((*head)->spspec != NULL)
		(*head)->spspec->prev = spspec;
	spspec->next = (*head)->spspec;
	(*head)->spspec = spspec;
}

/* set final acceptable proposal */
static int
set_isakmp_proposal(rmconf, prspec)
	struct remoteconf *rmconf;
	struct proposalspec *prspec;
{
	struct proposalspec *p;
	struct secprotospec *s;
	int prop_no = 1; 
	int trns_no = 1;
	u_int32_t types[MAXALGCLASS];

	p = prspec;
	if (p->next != 0) {
		plog(LLV_ERROR, LOCATION, NULL,
			"multiple proposal definition.\n");
		return -1;
	}

	/* mandatory check */
	if (p->spspec == NULL) {
		yyerror("no remote specification found: %s.\n",
			rm2str(rmconf));
		return -1;
	}
	for (s = p->spspec; s != NULL; s = s->next) {
		/* XXX need more to check */
		if (s->algclass[algclass_isakmp_enc] == 0) {
			yyerror("encryption algorithm required.");
			return -1;
		}
		if (s->algclass[algclass_isakmp_hash] == 0) {
			yyerror("hash algorithm required.");
			return -1;
		}
		if (s->algclass[algclass_isakmp_dh] == 0) {
			yyerror("DH group required.");
			return -1;
		}
		if (s->algclass[algclass_isakmp_ameth] == 0) {
			yyerror("authentication method required.");
			return -1;
		}
	}

	/* skip to last part */
	for (s = p->spspec; s->next != NULL; s = s->next)
		;

	while (s != NULL) {
		plog(LLV_DEBUG2, LOCATION, NULL,
			"lifetime = %ld\n", (long)
			(s->lifetime ? s->lifetime : p->lifetime));
		plog(LLV_DEBUG2, LOCATION, NULL,
			"lifebyte = %d\n",
			s->lifebyte ? s->lifebyte : p->lifebyte);
		plog(LLV_DEBUG2, LOCATION, NULL,
			"encklen=%d\n", s->encklen);

		memset(types, 0, ARRAYLEN(types));
		types[algclass_isakmp_enc] = s->algclass[algclass_isakmp_enc];
		types[algclass_isakmp_hash] = s->algclass[algclass_isakmp_hash];
		types[algclass_isakmp_dh] = s->algclass[algclass_isakmp_dh];
		types[algclass_isakmp_ameth] =
		    s->algclass[algclass_isakmp_ameth];

		/* expanding spspec */
		clean_tmpalgtype();
		trns_no = expand_isakmpspec(prop_no, trns_no, types,
				algclass_isakmp_enc, algclass_isakmp_ameth + 1,
				s->lifetime ? s->lifetime : p->lifetime,
				s->lifebyte ? s->lifebyte : p->lifebyte,
				s->encklen, s->vendorid, s->gssid,
				rmconf);
		if (trns_no == -1) {
			plog(LLV_ERROR, LOCATION, NULL,
				"failed to expand isakmp proposal.\n");
			return -1;
		}

		s = s->prev;
	}

	if (rmconf->proposal == NULL) {
		plog(LLV_ERROR, LOCATION, NULL,
			"no proposal found.\n");
		return -1;
	}

	return 0;
}

static void
clean_tmpalgtype()
{
	int i;
	for (i = 0; i < MAXALGCLASS; i++)
		tmpalgtype[i] = 0;	/* means algorithm undefined. */
}

static int
expand_isakmpspec(prop_no, trns_no, types,
		class, last, lifetime, lifebyte, encklen, vendorid, gssid,
		rmconf)
	int prop_no, trns_no;
	int *types, class, last;
	time_t lifetime;
	int lifebyte;
	int encklen;
	int vendorid;
	char *gssid;
	struct remoteconf *rmconf;
{
	struct isakmpsa *new;

	/* debugging */
    {
	int j;
	char tb[10];
	plog(LLV_DEBUG2, LOCATION, NULL,
		"p:%d t:%d\n", prop_no, trns_no);
	for (j = class; j < MAXALGCLASS; j++) {
		snprintf(tb, sizeof(tb), "%d", types[j]);
		plog(LLV_DEBUG2, LOCATION, NULL,
			"%s%s%s%s\n",
			s_algtype(j, types[j]),
			types[j] ? "(" : "",
			tb[0] == '0' ? "" : tb,
			types[j] ? ")" : "");
	}
	plog(LLV_DEBUG2, LOCATION, NULL, "\n");
    }

#define TMPALGTYPE2STR(n) \
	s_algtype(algclass_isakmp_##n, types[algclass_isakmp_##n])
		/* check mandatory values */
		if (types[algclass_isakmp_enc] == 0
		 || types[algclass_isakmp_ameth] == 0
		 || types[algclass_isakmp_hash] == 0
		 || types[algclass_isakmp_dh] == 0) {
			yyerror("few definition of algorithm "
				"enc=%s ameth=%s hash=%s dhgroup=%s.\n",
				TMPALGTYPE2STR(enc),
				TMPALGTYPE2STR(ameth),
				TMPALGTYPE2STR(hash),
				TMPALGTYPE2STR(dh));
			return -1;
		}
#undef TMPALGTYPE2STR

	/* set new sa */
	new = newisakmpsa();
	if (new == NULL) {
		yyerror("failed to allocate isakmp sa");
		return -1;
	}
	new->prop_no = prop_no;
	new->trns_no = trns_no++;
	new->lifetime = lifetime;
	new->lifebyte = lifebyte;
	new->enctype = types[algclass_isakmp_enc];
	new->encklen = encklen;
	new->authmethod = types[algclass_isakmp_ameth];
	new->hashtype = types[algclass_isakmp_hash];
	new->dh_group = types[algclass_isakmp_dh];
	new->vendorid = vendorid;
#ifdef HAVE_GSSAPI
	if (gssid != NULL) {
		new->gssid = vmalloc(strlen(gssid) + 1);
		memcpy(new->gssid->v, gssid, new->gssid->l);
		racoon_free(gssid);
	} else
		new->gssid = NULL;
#endif
	insisakmpsa(new, rmconf);

	return trns_no;
}

#if 0
/*
 * fix lifebyte.
 * Must be more than 1024B because its unit is kilobytes.
 * That is defined RFC2407.
 */
static int
fix_lifebyte(t)
	unsigned long t;
{
	if (t < 1024) {
		yyerror("byte size should be more than 1024B.");
		return 0;
	}

	return(t / 1024);
}
#endif

extern int yyparse(void);

int
cfparse()
{
	int error;

	yycf_init_buffer();

	if (yycf_switch_buffer(lcconf->racoon_conf) != 0)
		return -1;

	prhead = NULL;

	error = yyparse();
	if (error != 0) {
		if (yyerrorcount) {
			plog(LLV_ERROR, LOCATION, NULL,
				"fatal parse failure (%d errors)\n",
				yyerrorcount);
		} else {
			plog(LLV_ERROR, LOCATION, NULL,
				"fatal parse failure.\n");
		}
		return -1;
	}

	if (error == 0 && yyerrorcount) {
		plog(LLV_ERROR, LOCATION, NULL,
			"parse error is nothing, but yyerrorcount is %d.\n",
				yyerrorcount);
		exit(1);
	}

	yycf_clean_buffer();

	plog(LLV_DEBUG2, LOCATION, NULL, "parse successed.\n");

	return 0;
}

int
cfreparse()
{
	flushph2();
	flushph1();
	flushrmconf();
	flushsainfo();
	cleanprhead();
	clean_tmpalgtype();

	return(cfparse());
}


/* -----------------------------------------------------------------------------
The base-64 encoding packs three 8-bit bytes into four 7-bit ASCII
characters.  If the number of bytes in the original data isn't divisable
by three, "=" characters are used to pad the encoded data.  The complete
set of characters used in base-64 are:
     'A'..'Z' => 00..25
     'a'..'z' => 26..51
     '0'..'9' => 52..61
     '+'      => 62
     '/'      => 63
     '='      => pad

----------------------------------------------------------------------------- */
static const signed char base64_DecodeTable[128] = {
    /* 000 */ -1, -1, -1, -1, -1, -1, -1, -1,
    /* 010 */ -1, -1, -1, -1, -1, -1, -1, -1,
    /* 020 */ -1, -1, -1, -1, -1, -1, -1, -1,
    /* 030 */ -1, -1, -1, -1, -1, -1, -1, -1,
    /* ' ' */ -1, -1, -1, -1, -1, -1, -1, -1,
    /* '(' */ -1, -1, -1, 62, -1, -1, -1, 63,
    /* '0' */ 52, 53, 54, 55, 56, 57, 58, 59,
    /* '8' */ 60, 61, -1, -1, -1,  0, -1, -1,
    /* '@' */ -1,  0,  1,  2,  3,  4,  5,  6,
    /* 'H' */  7,  8,  9, 10, 11, 12, 13, 14,
    /* 'P' */ 15, 16, 17, 18, 19, 20, 21, 22,
    /* 'X' */ 23, 24, 25, -1, -1, -1, -1, -1,
    /* '`' */ -1, 26, 27, 28, 29, 30, 31, 32,
    /* 'h' */ 33, 34, 35, 36, 37, 38, 39, 40,
    /* 'p' */ 41, 42, 43, 44, 45, 46, 47, 48,
    /* 'x' */ 49, 50, 51, -1, -1, -1, -1, -1
};

static int base64toCFData(vchar_t *textin, CFDataRef *dataRef)
{
    uint8_t 	*tmpbuf;
    uint8_t		c;
    int 		tmpbufpos = 0;
    int 		numeq = 0;
    int 		acc = 0;
    int 		cntr = 0;
    uint8_t		*textcur = textin->v;
    int			len = textin->l;
    int 		i;

    tmpbuf = malloc(len);		// len of result will be less than encoded len
    if (tmpbuf == NULL) {
    	yyerror("memory error - could not allocate buffer for certificate reference conversion from base-64.");
    	return -1;
    }
    	
    for (i = 0; i < len; i++) {
        c = *(textcur++);
        if (c == '=')
            numeq++;
        else if (!isspace(c))
            numeq = 0;
        if (base64_DecodeTable[c] < 0)
            continue;
        cntr++;
        acc <<= 6;
        acc += base64_DecodeTable[c];
        if (0 == (cntr & 0x3)) {
            tmpbuf[tmpbufpos++] = (acc >> 16) & 0xff;
            if (numeq < 2)
                tmpbuf[tmpbufpos++] = (acc >> 8) & 0xff;
            if (numeq < 1)
                tmpbuf[tmpbufpos++] = acc & 0xff;
        }
    }
    *dataRef = CFDataCreate(NULL, tmpbuf, tmpbufpos);
    free(tmpbuf);
    return 0;
  
}

