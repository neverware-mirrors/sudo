%{

/*
 *  CU sudo version 1.4
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 1, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  Please send bugs, changes, problems to sudo-bugs@cs.colorado.edu
 *
 *******************************************************************
 *
 * parse.yacc -- yacc parser and alias manipulation routines for sudo.
 *
 * Chris Jepeway <jepeway@cs.utk.edu>
 */

#ifndef lint
static char rcsid[] = "$Id$";
#endif /* lint */

#include "config.h"
#include <stdio.h>
#ifdef STDC_HEADERS
#include <stdlib.h>
#endif /* STDC_HEADERS */
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <pwd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <netinet/in.h>
#ifdef HAVE_STRING_H
#include <string.h>
#endif /* HAVE_STRING_H */
#if defined(HAVE_MALLOC_H) && !defined(STDC_HEADERS)
#include <malloc.h>
#endif /* HAVE_MALLOC_H && !STDC_HEADERS */
#ifdef HAVE_LSEARCH
#include <search.h>
#endif /* HAVE_LSEARCH */

#include <options.h>
#include "sudo.h"

#ifndef HAVE_LSEARCH
#include "emul/search.h"
#endif /* HAVE_LSEARCH */

#ifndef HAVE_STRCASECMP
#define strcasecmp(a,b)		strcmp(a,b)
#endif /* !HAVE_STRCASECMP */

/*
 * Globals
 */
extern int sudolineno, parse_error;
int errorlineno = -1;
int clearaliases = 1;
int printmatches = 0;

/*
 * Alias types
 */
#define HOST			 1
#define CMND			 2
#define USER			 3

/*
 * The matching stack, we should not have to initialize this,
 * since it is global but some compilers are just too braindamaged...
 */
struct matchstack match[MATCHSTACKSIZE] = { FALSE };
int top = 0;

#define push \
    if (top > MATCHSTACKSIZE) \
	yyerror("matching stack overflow"); \
    else { \
	match[top].user   = -1; \
	match[top].cmnd   = -1; \
	match[top].host   = -1; \
	match[top].runas  = -1; \
	match[top].nopass = -1; \
	top++; \
    }
#define pop \
    if (top == 0) \
	yyerror("matching stack underflow"); \
    else \
	top--;

/*
 * The stack for printmatches.  A list of allowed commands for the user.
 * Space for cmndstack is malloc'd in parse.c
 */
struct sudo_match *matches;
int nummatches;

#define cmndpush \
    if (nummatches++ > MATCHSTACKSIZE) \
	yyerror("cmnd stack overflow"); \
    else { \
	matches[nummatches].runas = matches[nummatches].cmnd = NULL; \
	matches[nummatches].nopasswd = FALSE; \
    }

/*
 * The list of cmndaliases for MODE_LIST
 */
int in_alias = FALSE, num_command_aliases = 0;
/* XXX - dynamically size me! */
struct command_alias command_aliases[1024] = { NULL, NULL, 0, 0 };

#define command_alias_push \
    if (num_command_aliases++ > 1024) \
	yyerror("command_alias stack overflow"); \
    else \
	command_aliases[num_command_aliases].entries = NULL;

/*
 * Protoypes
 */
extern int  command_matches	__P((char *, char *, char *, char *));
extern int  addr_matches	__P((char *));
extern int  netgr_matches	__P((char *, char *, char *));
extern int  usergr_matches	__P((char *, char *));
static int  find_alias		__P((char *, int));
static int  add_alias		__P((char *, int));
static int  more_aliases	__P((size_t));
static void append		__P((char *, char **, size_t *, size_t *, int));
       void yyerror		__P((char *));

void yyerror(s)
    char *s;
{
    /* save the line the first error occured on */
    if (errorlineno == -1)
	errorlineno = sudolineno;
#ifndef TRACELEXER
    (void) fprintf(stderr, ">>> sudoers file: %s, line %d <<<\n", s, sudolineno);
#else
    (void) fprintf(stderr, "<*> ");
#endif
    parse_error = TRUE;
}
%}

%union {
    char *string;
    int BOOLEAN;
    struct sudo_command command;
    int tok;
}


%start file				/* special start symbol */
%token <string>  ALIAS			/* an UPPERCASE alias name */
%token <string>  NTWKADDR		/* w.x.y.z */
%token <string>  FQHOST			/* foo.bar.com */
%token <string>  NETGROUP		/* a netgroup (+NAME) */
%token <string>  USERGROUP		/* a usergroup (%NAME) */
%token <string>  NAME			/* a mixed-case name */
%token <tok> 	 RUNAS			/* a mixed-case runas name */
%token <tok> 	 NOPASSWD		/* no passwd req for command*/
%token <command> COMMAND		/* an absolute pathname */
%token <tok>	 COMMENT		/* comment and/or carriage return */
%token <tok>	 ALL			/* ALL keyword */
%token <tok>	 HOSTALIAS		/* Host_Alias keyword */
%token <tok>	 CMNDALIAS		/* Cmnd_Alias keyword */
%token <tok>	 USERALIAS		/* User_Alias keyword */
%token <tok>	 ':' '=' ',' '!' '.'	/* union member tokens */
%token <tok>	 ERROR

%type <BOOLEAN>	 cmnd
%type <BOOLEAN>	 opcmnd
%type <BOOLEAN>	 runasspec
%type <BOOLEAN>	 runaslist
%type <BOOLEAN>	 runasuser
%type <BOOLEAN>	 nopasswd

%%

file		:	entry
		|	file entry
		;

entry		:	COMMENT
			    { ; }
                |       error COMMENT
			    { yyerrok; }
		|	{ push; } user privileges {
			    while (top && user_matches != TRUE) {
				pop;
			    }
			}
		|	USERALIAS useraliases
			    { ; }
		|	HOSTALIAS hostaliases
			    { ; }
		|	CMNDALIAS cmndaliases
			    { ; }
		;
		

privileges	:	privilege
		|	privileges ':' privilege
		;

privilege	:	hostspec '=' cmndspeclist {
			    if (user_matches == TRUE) {
				push;
				user_matches = TRUE;
			    } else {
				no_passwd = -1;
				runas_matches = -1;
			    }
			}
		;

hostspec	:	ALL {
			    host_matches = TRUE;
			}
		|	NTWKADDR {
			    if (addr_matches($1))
				host_matches = TRUE;
			    (void) free($1);
			}
		|	NETGROUP {
			    if (netgr_matches($1, host, NULL))
				host_matches = TRUE;
			    (void) free($1);
			}
		|	NAME {
			    if (strcasecmp(shost, $1) == 0)
				host_matches = TRUE;
			    (void) free($1);
			}
		|	FQHOST {
			    if (strcasecmp(host, $1) == 0)
				host_matches = TRUE;
			    (void) free($1);
			}
		|	ALIAS {
			    /* could be an all-caps hostname */
			    if (find_alias($1, HOST) || !strcasecmp(shost, $1))
				host_matches = TRUE;
			    (void) free($1);
			}
		;

cmndspeclist	:	cmndspec
		|	cmndspeclist ',' cmndspec
		;

cmndspec	:	runasspec nopasswd opcmnd {
			    if ($1 > 0 && $3 == TRUE) {
				runas_matches = TRUE;
				if ($2 == TRUE)
				    no_passwd = TRUE;
			    } else if (printmatches) {
				matches[nummatches].runas_len = 0;
				matches[nummatches].cmnd_len = 0;
				matches[nummatches].nopasswd = FALSE;
			    }
			}
		;

opcmnd		:	cmnd { ; }
		|	'!' {
			    if (printmatches == TRUE && in_alias == TRUE) {
				/* XXX - want a space before first '!' */
				append("!", &command_aliases[num_command_aliases].entries,
				       &command_aliases[num_command_aliases].entries_len,
				       &command_aliases[num_command_aliases].entries_size, 0);
			    }
			    if (printmatches == TRUE && host_matches == TRUE &&
				user_matches == TRUE) {
				append("!", &matches[nummatches].cmnd,
				       &matches[nummatches].cmnd_len,
				       &matches[nummatches].cmnd_size, 0);
				push;
				user_matches = TRUE;
				host_matches = TRUE;
			    } else {
				push;
			    }
			} opcmnd {
			    int cmnd_matched = cmnd_matches;
			    pop;
			    if (cmnd_matched == TRUE)
				cmnd_matches = FALSE;
			    else if (cmnd_matched == FALSE)
				cmnd_matches = TRUE;
			    $$ = cmnd_matches;
			}
		;

runasspec	:	/* empty */ {
			    $$ = (strcmp("root", runas_user) == 0);
			}
		|	RUNAS runaslist {
			    $$ = $2;
			}
		;

runaslist	:	runasuser {
			    $$ = $1;
			}
		|	runaslist ',' runasuser	{
			    $$ = $1 + $3;
			}
		;


runasuser	:	NAME {
			    $$ = (strcmp($1, runas_user) == 0);
			    if (printmatches == TRUE && host_matches == TRUE &&
				user_matches == TRUE)
				append($1, &matches[nummatches].runas,
				       &matches[nummatches].runas_len,
				       &matches[nummatches].runas_size, ':');
			    (void) free($1);
			}
		|	USERGROUP {
			    $$ = usergr_matches($1, runas_user);
			    if (printmatches == TRUE && host_matches == TRUE &&
				user_matches == TRUE) {
				append("%", &matches[nummatches].runas,
				       &matches[nummatches].runas_len,
				       &matches[nummatches].runas_size, ':');
				append($1, &matches[nummatches].runas,
				       &matches[nummatches].runas_len,
				       &matches[nummatches].runas_size, 0);
			    }
			    (void) free($1);
			}
		|	NETGROUP {
			    $$ = netgr_matches($1, NULL, runas_user);
			    if (printmatches == TRUE && host_matches == TRUE &&
				user_matches == TRUE) {
				append("+", &matches[nummatches].runas,
				       &matches[nummatches].runas_len,
				       &matches[nummatches].runas_size, ':');
				append($1, &matches[nummatches].runas,
				       &matches[nummatches].runas_len,
				       &matches[nummatches].runas_size, 0);
			    }
			    (void) free($1);
			}
		|	ALIAS {
			    /* could be an all-caps username */
			    if (find_alias($1, USER) || !strcmp($1, runas_user))
				$$ = TRUE;
			    else
				$$ = FALSE;
			    if (printmatches == TRUE && host_matches == TRUE &&
				user_matches == TRUE)
				append($1, &matches[nummatches].runas,
				       &matches[nummatches].runas_len,
				       &matches[nummatches].runas_size, ':');
			    (void) free($1);
			}
		|	ALL {
			    $$ = TRUE;
			    if (printmatches == TRUE && host_matches == TRUE &&
				user_matches == TRUE)
				append("ALL", &matches[nummatches].runas,
				       &matches[nummatches].runas_len,
				       &matches[nummatches].runas_size, ':');
			}
		;

nopasswd	:	/* empty */ {
			    $$ = FALSE;
			}
		|	NOPASSWD {
			    $$ = TRUE;
			    if (printmatches == TRUE && host_matches == TRUE &&
				user_matches == TRUE)
				matches[nummatches].nopasswd = TRUE;
			}
		;

cmnd		:	ALL {
			    if (printmatches == TRUE && in_alias == TRUE) {
				append("ALL", &command_aliases[num_command_aliases].entries,
				       &command_aliases[num_command_aliases].entries_len,
				       &command_aliases[num_command_aliases].entries_size, ',');
			    }
			    if (printmatches == TRUE && host_matches == TRUE &&
				user_matches == TRUE) {
				append("ALL", &matches[nummatches].cmnd,
				       &matches[nummatches].cmnd_len,
				       &matches[nummatches].cmnd_size, 0);
				cmndpush;
			    }

			    cmnd_matches = TRUE;
			    $$ = TRUE;
			}
		|	ALIAS {
			    if (printmatches == TRUE && in_alias == TRUE) {
				append($1, &command_aliases[num_command_aliases].entries,
				       &command_aliases[num_command_aliases].entries_len,
				       &command_aliases[num_command_aliases].entries_size, ',');
			    }
			    if (printmatches == TRUE && host_matches == TRUE &&
				user_matches == TRUE) {
				append($1, &matches[nummatches].cmnd,
				       &matches[nummatches].cmnd_len,
				       &matches[nummatches].cmnd_size, 0);
				cmndpush;
			    }
			    if (find_alias($1, CMND)) {
				cmnd_matches = TRUE;
				$$ = TRUE;
			    }
			    (void) free($1);
			}
		|	 COMMAND {
			    if (printmatches == TRUE && in_alias == TRUE) {
				append($1.cmnd, &command_aliases[num_command_aliases].entries,
				       &command_aliases[num_command_aliases].entries_len,
				       &command_aliases[num_command_aliases].entries_size, ',');
				/* XXX - ignores args */
			    }
			    if (printmatches == TRUE && host_matches == TRUE &&
				user_matches == TRUE)  {
				append($1.cmnd, &matches[nummatches].cmnd,
				       &matches[nummatches].cmnd_len,
				       &matches[nummatches].cmnd_size, 0);
				if ($1.args) {
				    append($1.args, &matches[nummatches].cmnd,
					   &matches[nummatches].cmnd_len,
					   &matches[nummatches].cmnd_size, ' ');
				}
				cmndpush;
			    }

			    /* if NewArgc > 1 pass ptr to 1st arg, else NULL */
			    if (command_matches(cmnd, (NewArgc > 1) ?
				    cmnd_args : NULL, $1.cmnd, $1.args)) {
				cmnd_matches = TRUE;
				$$ = TRUE;
			    }

			    (void) free($1.cmnd);
			    if ($1.args)
				(void) free($1.args);
			}
		;

hostaliases	:	hostalias
		|	hostaliases ':' hostalias
		;

hostalias	:	ALIAS { push; } '=' hostlist {
			    if (host_matches == TRUE && !add_alias($1, HOST))
				YYERROR;
			    pop;
			}
		;

hostlist	:	hostspec
		|	hostlist ',' hostspec
		;

cmndaliases	:	cmndalias
		|	cmndaliases ':' cmndalias
		;

cmndalias	:	ALIAS {
			    push;
			    if (printmatches) {
				in_alias = TRUE;
				if ((command_aliases[num_command_aliases].alias
				     = strdup($1)) == NULL) {

				    perror("malloc");
				    (void) fprintf(stderr,
					"%s: cannot allocate memory!\n", Argv[0]);
				    exit(1);
				 }
			     }
			} '=' cmndlist {
			    if (cmnd_matches == TRUE && !add_alias($1, CMND))
				YYERROR;
			    pop;
			    (void) free($1);

			    if (printmatches) {
				in_alias = FALSE;
				command_alias_push;
			    }
			}
		;

cmndlist	:	cmnd
			    { ; }
		|	cmndlist ',' cmnd
		;

useraliases	:	useralias
		|	useraliases ':' useralias
		;

useralias	:	ALIAS { push; }	'=' userlist {
			    if (user_matches == TRUE && !add_alias($1, USER))
				YYERROR;
			    pop;
			    (void) free($1);
			}
		;

userlist	:	user
			    { ; }
		|	userlist ',' user
		;

user		:	NAME {
			    if (strcmp($1, user_name) == 0)
				user_matches = TRUE;
			    (void) free($1);
			}
		|	USERGROUP {
			    if (usergr_matches($1, user_name))
				user_matches = TRUE;
			    (void) free($1);
			}
		|	NETGROUP {
			    if (netgr_matches($1, NULL, user_name))
				user_matches = TRUE;
			    (void) free($1);
			}
		|	ALIAS {
			    /* could be an all-caps username */
			    if (find_alias($1, USER) || !strcmp($1, user_name))
				user_matches = TRUE;
			    (void) free($1);
			}
		|	ALL {
			    user_matches = TRUE;
			}
		;

%%


typedef struct {
    int type;
    char name[BUFSIZ];
} aliasinfo;

#define MOREALIASES (32)
aliasinfo *aliases = NULL;
size_t naliases = 0;
size_t nslots = 0;


static int aliascmp(a1, a2)
    const VOID *a1, *a2;
{
    int r;
    aliasinfo *ai1, *ai2;

    ai1 = (aliasinfo *) a1;
    ai2 = (aliasinfo *) a2;
    r = strcmp(ai1->name, ai2->name);
    if (r == 0)
	r = ai1->type - ai2->type;

    return(r);
}


static int cmndaliascmp(entry, key)
    const VOID *entry, *key;
{
    struct command_alias *ca1 = (struct command_alias *) key;
    struct command_alias *ca2 = (struct command_alias *) entry;

    return(strcmp(ca1->alias, ca2->alias));
}


static int add_alias(alias, type)
    char *alias;
    int type;
{
    aliasinfo ai, *aip;
    char s[512];
    int ok;

    ok = FALSE;			/* assume failure */
    ai.type = type;
    (void) strcpy(ai.name, alias);
    if (lfind((VOID *)&ai, (VOID *)aliases, &naliases, sizeof(ai),
	aliascmp) != NULL) {
	(void) sprintf(s, "Alias `%s' already defined", alias);
	yyerror(s);
    } else {
	if (naliases == nslots && !more_aliases(nslots)) {
	    (void) sprintf(s, "Out of memory defining alias `%s'", alias);
	    yyerror(s);
	}

	aip = (aliasinfo *) lsearch((VOID *)&ai, (VOID *)aliases,
				    &naliases, sizeof(ai), aliascmp);

	if (aip != NULL) {
	    ok = TRUE;
	} else {
	    (void) sprintf(s, "Aliases corrupted defining alias `%s'", alias);
	    yyerror(s);
	}
    }

    return(ok);
}


static int find_alias(alias, type)
    char *alias;
    int type;
{
    aliasinfo ai;

    (void) strcpy(ai.name, alias);
    ai.type = type;

    return(lfind((VOID *)&ai, (VOID *)aliases, &naliases,
		 sizeof(ai), aliascmp) != NULL);
}


static int more_aliases(nslots)
    size_t nslots;
{
    aliasinfo *aip;

    if (nslots == 0)
	aip = (aliasinfo *) malloc(MOREALIASES * sizeof(*aip));
    else
	aip = (aliasinfo *) realloc(aliases,
				    (nslots + MOREALIASES) * sizeof(*aip));

    if (aip != NULL) {
	aliases = aip;
	nslots += MOREALIASES;
    }

    return(aip != NULL);
}


void dumpaliases()
{
    size_t n;

    for (n = 0; n < naliases; n++) {
	switch (aliases[n].type) {
	case HOST:
	    (void) puts("HOST");
	    break;

	case CMND:
	    (void) puts("CMND");
	    break;

	case USER:
	    (void) puts("USER");
	    break;
	}
	(void) printf("\t%s\n", aliases[n].name);
    }
}


void list_matches()
{
    int i; 
    char *p;
    struct command_alias *ca, key;

    for (i = 0; i < nummatches; i++) {

	/* Print the runas list. */
	if (matches[i].runas) {
	    (void) putchar('(');
	    if ((p = strtok(matches[i].runas, ":")))
		(void) fputs(p, stdout);
	    while ((p = strtok(NULL, ":"))) {
		(void) fputs(", ", stdout);
		(void) fputs(p, stdout);
	    }
	    (void) fputs(") ", stdout);
	} else {
	    (void) fputs("(root) ", stdout);
	}

	/* Is a password required? */
	if (matches[i].nopasswd == TRUE)
	    (void) fputs("NOPASSWD: ", stdout);

	/* XXX - this could be faster (check for all upcase) */
	/* Print the actual command or expanded Cmnd_Alias. */
	key.alias = matches[i].cmnd;
	if ((ca = lfind((VOID *)&key, (VOID *)&command_aliases[0],
	     &num_command_aliases, sizeof(struct command_alias), cmndaliascmp)))
	    (void) puts(ca->entries);
	else
	    (void) puts(matches[i].cmnd);
    }

    /* XXX - free up space */
}


static void append(src, dstp, dst_len, dst_size, separator)
    char *src, **dstp;
    size_t *dst_len, *dst_size;
    int separator;
{
    /* Only add the separator if *dstp is non-NULL. */
    size_t src_len = strlen(src) + ((separator && *dstp) ? 1 : 0);
    char *dst = *dstp;

    /* Assumes dst will be NULL if not set. */
    if (dst == NULL) {
	if ((dst = (char *) malloc(BUFSIZ)) == NULL) {
	    perror("malloc");
	    (void) fprintf(stderr, "%s: cannot allocate memory!\n", Argv[0]);
	    exit(1);
	}

	*dst_size = BUFSIZ;
	*dst_len = 0;
	*dstp = dst;
    }

    /* Allocate more space if necesary. */
    if (*dst_size <= *dst_len + src_len) {
	while (*dst_size <= *dst_len + src_len)
	    *dst_size += BUFSIZ;

	if (!(dst = (char *) realloc(dst, *dst_size))) {
	    perror("malloc");
	    (void) fprintf(stderr, "%s: cannot allocate memory!\n", Argv[0]);
	    exit(1);
	}
	*dstp = dst;
    }

    /* Copy src -> dst adding a separator char if appropriate and adjust len. */
    dst += *dst_len;
    if (separator && *dst_len)
	*dst++ = (char) separator;
    (void) strcpy(dst, src);
    *dst_len += src_len;
}


void reset_aliases()
{
    if (aliases)
	(void) free(aliases);
    naliases = nslots = 0;
}
