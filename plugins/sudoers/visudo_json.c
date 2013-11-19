/*
 * Copyright (c) 2013 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <config.h>

#include <sys/types.h>
#include <stdio.h>
#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# ifdef HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif /* STDC_HEADERS */
#ifdef HAVE_STRING_H
# include <string.h>
#endif /* HAVE_STRING_H */
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif /* HAVE_STRINGS_H */
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <stdarg.h>
#include <ctype.h>

#include "sudoers.h"
#include "parse.h"
#include "gettext.h"
#include <gram.h>

/*
 * External globals exported by the parser
 */
extern FILE *sudoersin;
extern char *sudoers, *errorfile;
extern int errorlineno;
extern bool parse_error;

/*
 * JSON values may be of the following types.
 */
enum json_value_type {
    JSON_STRING,
    JSON_NUMBER,
    JSON_OBJECT,
    JSON_ARRAY,
    JSON_BOOL,
    JSON_NULL
};

/*
 * JSON value suitable for printing.
 * Note: this does not support object or array values.
 */
struct json_value {
    enum json_value_type type;
    union {
	char *string;
	int number;
	bool boolean;
    } u;
};

/*
 * Closure used to store state when iterating over all aliases.
 */
struct json_alias_closure {
    const char *title;
    unsigned int count;
    int alias_type;
    int indent;
    bool need_comma;
};

/*
 * Type values used to disambiguate the generic WORD and ALIAS types.
 */
enum word_type {
    TYPE_COMMAND,
    TYPE_HOSTNAME,
    TYPE_RUNASGROUP,
    TYPE_RUNASUSER,
    TYPE_USERNAME
};

/*
 * SUDO_DIGEST_* number to name mapping.
 * XXX - should go somewhere else.
 */
static const char *digest_names[] = {
    "sha224",
    "sha256",
    "sha384",
    "sha512",
    "invalid digest"
};

/*
 * Print "indent" number of blank characters.
 */
static void
print_indent(int indent)
{
    while (indent--)
	putchar(' ');
}

/*
 * Print a JSON string, escaping special characters.
 * Does not support unicode escapes.
 */
static void
print_string_json_unquoted(const char *str)
{
    char ch;

    while ((ch = *str++) != '\0') {
	switch (ch) {
	case '"':
	case '\\':
	case '/':
	    putchar('\\');
	    break;
	case '\b':
	    ch = 'b';
	    putchar('\\');
	    break;
	case '\f':
	    ch = 'f';
	    putchar('\\');
	    break;
	case '\n':
	    ch = 'n';
	    putchar('\\');
	    break;
	case '\r':
	    ch = 'r';
	    putchar('\\');
	    break;
	case '\t':
	    ch = 't';
	    putchar('\\');
	    break;
	}
	putchar(ch);
    }
}

/*
 * Print a quoted JSON string, escaping special characters.
 * Does not support unicode escapes.
 */
static void
print_string_json(const char *str)
{
    putchar('\"');
    print_string_json_unquoted(str);
    putchar('\"');
}

/*
 * Print a JSON name: value pair with proper quoting and escaping.
 */
static void
print_pair_json(const char *pre, const char *name,
    const struct json_value *value, const char *post, int indent)
{
    debug_decl(print_pair_json, SUDO_DEBUG_UTIL)

    print_indent(indent);

    /* prefix */
    if (pre != NULL)
	fputs(pre, stdout);

    /* name */
    print_string_json(name);
    putchar(':');
    putchar(' ');

    /* value */
    switch (value->type) {
    case JSON_STRING:
	print_string_json(value->u.string);
	break;
    case JSON_NUMBER:
	printf("%d", value->u.number);
	break;
    case JSON_NULL:
	fputs("null", stdout);
	break;
    case JSON_BOOL:
	fputs(value->u.boolean ? "true" : "false", stdout);
	break;
    case JSON_OBJECT:
	fatalx("internal error: can't print JSON_OBJECT");
	break;
    case JSON_ARRAY:
	fatalx("internal error: can't print JSON_ARRAY");
	break;
    }

    /* postfix */
    if (post != NULL)
	fputs(post, stdout);

    debug_return;
}

/*
 * Print a JSON string with optional prefix and postfix to stdout.
 * Strings are not quoted but are escaped as per the JSON spec.
 */
static void
printstr_json(const char *pre, const char *str, const char *post, int indent)
{
    debug_decl(printstr_json, SUDO_DEBUG_UTIL)

    print_indent(indent);
    if (pre != NULL)
	fputs(pre, stdout);
    if (str != NULL) {
	print_string_json_unquoted(str);
    }
    if (post != NULL)
	fputs(post, stdout);
    debug_return;
}

/*
 * Print struct sudo_command in JSON format, with specified indentation.
 * If last_one is false, a comma will be printed before the newline
 * that closes the object.
 */
static void
print_command_json(struct sudo_command *c, int indent, bool last_one)
{
    struct json_value value;
    const char *digest_type;
    debug_decl(print_command_json, SUDO_DEBUG_UTIL)

    printstr_json("{", NULL, NULL, indent);
    if (c->digest != NULL) {
	putchar('\n');
	indent += 4;
	if (c->digest->digest_type < SUDO_DIGEST_INVALID) {
	    digest_type = digest_names[c->digest->digest_type];
	} else {
	    digest_type = digest_names[SUDO_DIGEST_INVALID];
	}
	value.type = JSON_STRING;
	value.u.string = c->digest->digest_str;
	print_pair_json(NULL, digest_type, &value, ",\n", indent);
    } else {
	putchar(' ');
	indent = 0;
    }
    if (c->args != NULL) {
	printstr_json("\"", "command", "\": ", indent);
	printstr_json("\"", c->cmnd, " ", 0);
	printstr_json(NULL, c->args, "\"", 0);
    } else {
	value.type = JSON_STRING;
	value.u.string = c->cmnd;
	print_pair_json(NULL, "command", &value, NULL, indent);
    }
    if (c->digest != NULL) {
	indent -= 4;
	putchar('\n');
	print_indent(indent);
    } else {
	putchar(' ');
    }
    putchar('}');
    if (!last_one)
	putchar(',');
    putchar('\n');

    debug_return;
}

/*
 * Map an alias type to enum word_type.
 */
static enum word_type
alias_to_word_type(int alias_type)
{
    switch (alias_type) {
    case CMNDALIAS:
	return TYPE_COMMAND;
    case HOSTALIAS:
	return TYPE_HOSTNAME;
    case RUNASALIAS:
	return TYPE_RUNASUSER;
    case USERALIAS:
	return TYPE_USERNAME;
    default:
	fatalx_nodebug("unexpected alias type %d", alias_type);
    }
}

/*
 * Map a Defaults type to enum word_type.
 */
static enum word_type
defaults_to_word_type(int defaults_type)
{
    switch (defaults_type) {
    case DEFAULTS_CMND:
	return TYPE_COMMAND;
    case DEFAULTS_HOST:
	return TYPE_HOSTNAME;
    case DEFAULTS_RUNAS:
	return TYPE_RUNASUSER;
    case DEFAULTS_USER:
	return TYPE_USERNAME;
    default:
	fatalx_nodebug("unexpected defaults type %d", defaults_type);
    }
}

/*
 * Print struct member in JSON format, with specified indentation.
 * If last_one is false, a comma will be printed before the newline
 * that closes the object.
 */
static void
print_member_json(struct member *m, enum word_type word_type, bool last_one,
     int indent)
{
    struct json_value value;
    const char *typestr;
    debug_decl(print_member_json, SUDO_DEBUG_UTIL)

    /* Most of the time we print a string. */
    value.type = JSON_STRING;
    value.u.string = m->name;

    switch (m->type) {
    case USERGROUP:
	value.u.string++; /* skip leading '%' */
	if (*value.u.string == '#') {
	    value.type = JSON_NUMBER;
	    value.u.number = atoi(m->name + 2); /* XXX - use atoid? */
	    typestr = "usergid";
	} else {
	    typestr = "usergroup";
	}
	break;
    case NETGROUP:
	typestr = "netgroup";
	value.u.string++; /* skip leading '+' */
	break;
    case NTWKADDR:
	typestr = "networkaddr";
	break;
    case COMMAND:
	print_command_json((struct sudo_command *)m->name, indent, last_one);
	debug_return;
    case WORD:
	switch (word_type) {
	case TYPE_HOSTNAME:
	    typestr = "hostname";
	    break;
	case TYPE_RUNASGROUP:
	    typestr = "runasgroup";
	    break;
	case TYPE_RUNASUSER:
	    typestr = "runasuser";
	    break;
	case TYPE_USERNAME:
	    if (*value.u.string == '#') {
		value.type = JSON_NUMBER;
		value.u.number = atoi(m->name + 1); /* XXX - use atoid? */
		typestr = "userid";
	    } else {
		typestr = "username";
	    }
	    break;
	default:
	    fatalx("unexpected word type %d", word_type);
	}
	break;
    case ALL:
	value.u.string = "ALL";
	/* FALLTHROUGH */
    case ALIAS:
	switch (word_type) {
	case TYPE_COMMAND:
	    typestr = "cmndalias";
	    break;
	case TYPE_HOSTNAME:
	    typestr = "hostalias";
	    break;
	case TYPE_RUNASGROUP:
	case TYPE_RUNASUSER:
	    typestr = "runasalias";
	    break;
	case TYPE_USERNAME:
	    typestr = "useralias";
	    break;
	default:
	    fatalx("unexpected word type %d", word_type);
	}
	break;
    default:
	fatalx("unexpected member type %d", m->type);
    }
    print_pair_json("{ ", typestr, &value, " }", indent);
    if (!last_one)
	putchar(',');
    putchar('\n');

    debug_return;
}

/*
 * Callback for alias_apply() to print an alias entry if it matches
 * the type specified in the closure.
 */
int
print_alias_json(void *v1, void *v2)
{
    struct alias *a = v1;
    struct json_alias_closure *closure = v2;
    struct member *m;
    debug_decl(print_alias_json, SUDO_DEBUG_UTIL)

    if (a->type != closure->alias_type)
	debug_return_int(0);

    /* Open the aliases object or close the last entry, then open new one. */
    if (closure->count++ == 0) {
	printf("%s\n%*s\"%s\": {\n", closure->need_comma ? "," : "",
	    closure->indent, "", closure->title);
	closure->indent += 4;
    } else {
	printf("%*s],\n", closure->indent, "");
    }
    printstr_json("\"", a->name, "\": [\n", closure->indent);

    closure->indent += 4;
    TAILQ_FOREACH(m, &a->members, entries) {
	print_member_json(m, alias_to_word_type(closure->alias_type),
	    TAILQ_NEXT(m, entries) == NULL, closure->indent);
    }
    closure->indent -= 4;
    debug_return_int(0);
}

/*
 * Print the binding for a Defaults entry of the specified type.
 */
static void
print_binding_json(struct member_list *binding, int type, int indent)
{
    struct member *m;
    debug_decl(print_binding_json, SUDO_DEBUG_UTIL)

    if (TAILQ_EMPTY(binding))
	debug_return;

    printf("%*s\"Binding\": [\n", indent, "");
    indent += 4;

    /* Print each member object in binding. */
    TAILQ_FOREACH(m, binding, entries) {
	print_member_json(m, defaults_to_word_type(type),
	     TAILQ_NEXT(m, entries) == NULL, indent);
    }

    indent -= 4;
    printf("%*s],\n", indent, "");

    debug_return;
}

/*
 * Print a Defaults list JSON format.
 */
static void
print_defaults_list_json(struct defaults *def, int indent)
{
    char savech, *start, *end = def->val;
    struct json_value value;
    debug_decl(print_defaults_list_json, SUDO_DEBUG_UTIL)

    printf("%*s{\n", indent, "");
    indent += 4;
    value.type = JSON_STRING;
    switch (def->op) {
    case '+':
	value.u.string = "list_add";
	break;
    case '-':
	value.u.string = "list_remove";
	break;
    case true:
	value.u.string = "list_assign";
	break;
    default:
	warningx("internal error: unexpected list op %d", def->op);
	value.u.string = "unsupported";
	break;
    }
    print_pair_json(NULL, "operation", &value, ",\n", indent);
    value.u.string = def->var;
    print_pair_json(NULL, "name", &value, ",\n", indent);
    printstr_json("\"", "value", "\": [\n", indent);
    indent += 4;
    print_indent(indent);
    /* Split value into multiple space-separated words. */
    do {
	/* Remove leading blanks, must have a non-empty string. */
	for (start = end; isblank((unsigned char)*start); start++)
	    ;
	if (*start == '\0')
	    break;

	/* Find the end and print it. */
	for (end = start; *end && !isblank((unsigned char)*end); end++)
	    ;
	savech = *end;
	*end = '\0';
	print_string_json(start);
	if (savech != '\0')
	    putchar(',');
	*end = savech;
    } while (*end++ != '\0');
    putchar('\n');
    indent -= 4;
    printf("%*s]\n", indent, "");
    indent -= 4;
    printf("%*s}", indent, "");

    debug_return;
}

static int
get_defaults_type(struct defaults *def)
{
    struct sudo_defs_types *cur;

    /* Look up def in table to find its type. */
    for (cur = sudo_defs_table; cur->name; cur++) {
	if (strcmp(def->var, cur->name) == 0)
	    return cur->type;
    }
    return -1;
}

/*
 * Export all Defaults in JSON format.
 */
static bool
print_defaults_json(int indent, bool need_comma)
{
    struct json_value value;
    struct defaults *def, *next;
    int type;
    debug_decl(print_defaults_json, SUDO_DEBUG_UTIL)

    if (TAILQ_EMPTY(&defaults))
	debug_return_bool(need_comma);

    printf("%s\n%*s\"Defaults\": [\n", need_comma ? "," : "", indent, "");

    TAILQ_FOREACH_SAFE(def, &defaults, entries, next) {
	type = get_defaults_type(def);
	if (type == -1) {
	    warningx(U_("unknown defaults entry `%s'"), def->var);
	    /* XXX - just pass it through as a string anyway? */
	    continue;
	}

	/* Found it, print object container and binding (if any). */
	indent = 8;
	printf("%*s{\n", indent, "");
	indent = 12;
	print_binding_json(def->binding, def->type, indent);

	/* Validation checks. */
	/* XXX - validate values in addition to names? */

	/* Print options, merging ones with the same binding. */
	printf("%*s\"Options\": [\n", indent, "");
	indent += 4;
	for (;;) {
	    next = TAILQ_NEXT(def, entries);
	    /* XXX - need to update cur too */
	    if ((type & T_MASK) == T_FLAG || def->val == NULL) {
		value.type = JSON_BOOL;
		value.u.boolean = def->op;
		print_pair_json("{ ", def->var, &value, " }", indent);
	    } else if ((type & T_MASK) == T_LIST) {
		print_defaults_list_json(def, indent);
	    } else {
		value.type = JSON_STRING;
		value.u.string = def->val;
		print_pair_json("{ ", def->var, &value, " }", indent);
	    }
	    if (next == NULL || def->binding != next->binding)
		break;
	    def = next;
	    type = get_defaults_type(def);
	    if (type == -1) {
		warningx(U_("unknown defaults entry `%s'"), def->var);
		/* XXX - just pass it through as a string anyway? */
		break;;
	    }
	    fputs(",\n", stdout);
	}
	putchar('\n');
	indent -= 4;
	print_indent(indent);
	fputs("]\n", stdout);
	indent -= 4;
	print_indent(indent);
	printf("}%s\n", next != NULL ? "," : "");
    }

    /* Close Defaults array; comma (if any) & newline will be printer later. */
    indent -= 4;
    print_indent(indent);
    fputs("]", stdout);

    debug_return_bool(true);
}

/*
 * Export all aliases of the specified type in JSON format.
 * Iterates through the entire aliases tree.
 */
static bool
print_aliases_by_type_json(int alias_type, const char *title, int indent,
    bool need_comma)
{
    struct json_alias_closure closure;
    debug_decl(print_aliases_by_type_json, SUDO_DEBUG_UTIL)

    closure.indent = indent;
    closure.count = 0;
    closure.alias_type = alias_type;
    closure.title = title;
    closure.need_comma = need_comma;
    alias_apply(print_alias_json, &closure);
    if (closure.count != 0) {
	print_indent(closure.indent);
	fputs("]\n", stdout);
	closure.indent -= 4;
	print_indent(closure.indent);
	putchar('}');
	need_comma = true;
    }

    debug_return_bool(need_comma);
}

/*
 * Export all aliases in JSON format.
 */
static bool
print_aliases_json(int indent, bool need_comma)
{
    debug_decl(print_aliases_json, SUDO_DEBUG_UTIL)

    need_comma = print_aliases_by_type_json(USERALIAS, "User_Aliases",
	indent, need_comma);
    need_comma = print_aliases_by_type_json(RUNASALIAS, "Runas_Aliases",
	indent, need_comma);
    need_comma = print_aliases_by_type_json(HOSTALIAS, "Host_Aliases",
	indent, need_comma);
    need_comma = print_aliases_by_type_json(CMNDALIAS, "Command_Aliases",
	indent, need_comma);

    debug_return_bool(need_comma);
}

/* XXX these are all duplicated w/ parse.c */
#define RUNAS_CHANGED(cs1, cs2) \
	(cs1 == NULL || cs2 == NULL || \
	 cs1->runasuserlist != cs2->runasuserlist || \
	 cs1->runasgrouplist != cs2->runasgrouplist)

#define TAG_SET(tt) \
	((tt) != UNSPEC && (tt) != IMPLIED)

#define TAGS_CHANGED(ot, nt) \
	((TAG_SET((nt).setenv) && (nt).setenv != (ot).setenv) || \
	 (TAG_SET((nt).noexec) && (nt).noexec != (ot).noexec) || \
	 (TAG_SET((nt).nopasswd) && (nt).nopasswd != (ot).nopasswd) || \
	 (TAG_SET((nt).log_input) && (nt).log_input != (ot).log_input) || \
	 (TAG_SET((nt).log_output) && (nt).log_output != (ot).log_output))

/*
 * Print a Cmnd_Spec in JSON format at the specified indent level.
 * A pointer to the next Cmnd_Spec is passed in to make it possible to
 * merge adjacent entries that are identical in all but the command.
 */
static void
print_cmndspec_json(struct cmndspec *cs, struct cmndspec **nextp, int indent)
{
    struct cmndspec *next = *nextp;
    struct json_value value;
    struct member *m;
    bool last_one;
    debug_decl(print_cmndspec_json, SUDO_DEBUG_UTIL)

    /* Open Cmnd_Spec object. */
    printf("%*s{\n", indent, "");
    indent += 4;

    /* Print runasuserlist */
    if (cs->runasuserlist != NULL) {
	printf("%*s\"runasusers\": [\n", indent, "");
	indent += 4;
	TAILQ_FOREACH(m, cs->runasuserlist, entries) {
	    print_member_json(m, TYPE_RUNASUSER,
		TAILQ_NEXT(m, entries) == NULL, indent);
	}
	indent -= 4;
	printf("%*s],\n", indent, "");
    }

    /* Print runasgrouplist */
    if (cs->runasgrouplist != NULL) {
	printf("%*s\"runasgroups\": [\n", indent, "");
	indent += 4;
	TAILQ_FOREACH(m, cs->runasgrouplist, entries) {
	    print_member_json(m, TYPE_RUNASGROUP,
		TAILQ_NEXT(m, entries) == NULL, indent);
	}
	indent -= 4;
	printf("%*s],\n", indent, "");
    }

    /* Print tags */
    if (cs->tags.nopasswd != UNSPEC || cs->tags.noexec != UNSPEC ||
	cs->tags.setenv != UNSPEC || cs->tags.log_input != UNSPEC ||
	cs->tags.log_output != UNSPEC) {
	printf("%*s\"Options\": {\n", indent, "");
	indent += 4;
	if (cs->tags.nopasswd != UNSPEC) {
	    value.type = JSON_BOOL;
	    value.u.boolean = !cs->tags.nopasswd;
	    last_one = cs->tags.noexec == UNSPEC &&
		cs->tags.setenv == UNSPEC && cs->tags.log_input == UNSPEC &&
		cs->tags.log_output == UNSPEC;
	    print_pair_json(NULL, "authenticate", &value,
		last_one ? "\n" : ",\n", indent);
	}
	if (cs->tags.noexec != UNSPEC) {
	    value.type = JSON_BOOL;
	    value.u.boolean = cs->tags.noexec;
	    last_one = cs->tags.setenv == UNSPEC &&
		cs->tags.log_input == UNSPEC && cs->tags.log_output == UNSPEC;
	    print_pair_json(NULL, "noexec", &value,
		last_one ? "\n" : ",\n", indent);
	}
	if (cs->tags.setenv != UNSPEC) {
	    value.type = JSON_BOOL;
	    value.u.boolean = cs->tags.setenv;
	    last_one = cs->tags.log_input == UNSPEC &&
		cs->tags.log_output == UNSPEC;
	    print_pair_json(NULL, "setenv", &value,
		last_one ? "\n" : ",\n", indent);
	}
	if (cs->tags.log_input != UNSPEC) {
	    value.type = JSON_BOOL;
	    value.u.boolean = cs->tags.log_input;
	    last_one = cs->tags.log_output == UNSPEC;
	    print_pair_json(NULL, "log_input", &value,
		last_one ? "\n" : ",\n", indent);
	}
	if (cs->tags.log_output != UNSPEC) {
	    value.type = JSON_BOOL;
	    value.u.boolean = cs->tags.log_output;
	    print_pair_json(NULL, "log_output", &value, "\n", indent);
	}
	indent -= 4;
	printf("%*s},\n", indent, "");
    }

#ifdef HAVE_SELINUX
    /* Print SELinux role/type */
    if (cs->role != NULL && cs->type != NULL) {
	printf("%*s\"SELinux_Spec\": [\n", indent, "");
	indent += 4;
	value.type = JSON_STRING;
	value.u.string = cs->role;
	print_pair_json(NULL, "role", &value, ",\n", indent);
	value.u.string = cs->type;
	print_pair_json(NULL, "type", &value, "\n", indent);
	indent -= 4;
	printf("%*s],\n", indent, "");
    }
#endif /* HAVE_SELINUX */

#ifdef HAVE_PRIV_SET
    /* Print Solaris privs/limitprivs */
    if (cs->privs != NULL || cs->limitprivs != NULL) {
	printf("%*s\"Solaris_Priv_Spec\": [\n", indent, "");
	indent += 4;
	value.type = JSON_STRING;
	if (cs->privs != NULL) {
	    value.u.string = cs->privs;
	    print_pair_json(NULL, "privs", &value,
		cs->limitprivs != NULL ? ",\n" : "\n", indent);
	}
	if (cs->limitprivs != NULL) {
	    value.u.string = cs->limitprivs;
	    print_pair_json(NULL, "limitprivs", &value, "\n", indent);
	}
	indent -= 4;
	printf("%*s],\n", indent, "");
    }
#endif /* HAVE_PRIV_SET */

    /*
     * Merge adjacent commands with matching tags, runas, SELinux
     * role/type and Solaris priv settings.
     */
    printf("%*s\"Commands\": [\n", indent, "");
    indent += 4;
    for (;;) {
	/* Does the next entry differ only in the command itself? */
	/* XXX - move into a function that returns bool */
	last_one = next == NULL ||
	    RUNAS_CHANGED(cs, next) || TAGS_CHANGED(cs->tags, next->tags)
#ifdef HAVE_PRIV_SET
	    || cs->privs != next->privs || cs->limitprivs != next->limitprivs
#endif /* HAVE_PRIV_SET */
#ifdef HAVE_SELINUX
	    || cs->role != next->role || cs->type != next->type
#endif /* HAVE_SELINUX */
	    ;

	print_member_json(cs->cmnd, TYPE_COMMAND, last_one, indent);
	if (last_one)
	    break;
	cs = next;
	next = TAILQ_NEXT(cs, entries);
    }
    indent -= 4;
    printf("%*s]\n", indent, "");

    /* Close Cmnd_Spec object. */
    indent -= 4;
    printf("%*s}%s\n", indent, "", TAILQ_NEXT(cs, entries) != NULL ? "," : "");

    *nextp = next;

    debug_return;
}

/*
 * Print a User_Spec in JSON format at the specified indent level.
 */
static void
print_userspec_json(struct userspec *us, int indent)
{
    struct privilege *priv;
    struct member *m;
    struct cmndspec *cs, *next;
    debug_decl(print_userspec_json, SUDO_DEBUG_UTIL)

    /*
     * Each userspec struct may contain multiple privileges for
     * a user.  We export each privilege as a separate User_Spec
     * object for simplicity's sake.
     */
    TAILQ_FOREACH(priv, &us->privileges, entries) {
	/* Open User_Spec object. */
	printf("%*s{\n", indent, "");
	indent += 4;

	/* Print users list. */
	printf("%*s\"User_List\": [\n", indent, "");
	indent += 4;
	TAILQ_FOREACH(m, &us->users, entries) {
	    print_member_json(m, TYPE_USERNAME,
		TAILQ_NEXT(m, entries) == NULL, indent);
	}
	indent -= 4;
	printf("%*s],\n", indent, "");

	/* Print hosts list. */
	printf("%*s\"Host_List\": [\n", indent, "");
	indent += 4;
	TAILQ_FOREACH(m, &priv->hostlist, entries) {
	    print_member_json(m, TYPE_HOSTNAME,
		TAILQ_NEXT(m, entries) == NULL, indent);
	}
	indent -= 4;
	printf("%*s],\n", indent, "");

	/* Print commands. */
	printf("%*s\"Cmnd_Specs\": [\n", indent, "");
	indent += 4;
	TAILQ_FOREACH_SAFE(cs, &priv->cmndlist, entries, next) {
	    print_cmndspec_json(cs, &next, indent);
	}
	indent -= 4;
	printf("%*s]\n", indent, "");

	/* Close User_Spec object. */
	indent -= 4;
	printf("%*s}%s\n", indent, "", TAILQ_NEXT(priv, entries) != NULL ||
	    TAILQ_NEXT(us, entries) != NULL ? "," : "");
    }

    debug_return;
}

static bool
print_userspecs_json(int indent, bool need_comma)
{
    struct userspec *us;
    debug_decl(print_userspecs_json, SUDO_DEBUG_UTIL)

    if (TAILQ_EMPTY(&userspecs))
	debug_return_bool(need_comma);

    printf("%s\n%*s\"User_Specs\": [\n", need_comma ? "," : "", indent, "");
    indent += 4;
    TAILQ_FOREACH(us, &userspecs, entries) {
	print_userspec_json(us, indent);
    }
    indent -= 4;
    printf("%*s]", indent, "");

    debug_return_bool(true);
}

/*
 * Export the parsed sudoers file in JSON format.
 * XXX - ignores strict flag and doesn't pass through quiet flag
 * XXX - pass indent=4 to other json functions
 */
bool
export_sudoers(char *sudoers_path, bool quiet, bool strict)
{
    bool ok = false, need_comma = false;
    const int indent = 4;
    debug_decl(export_sudoers, SUDO_DEBUG_UTIL)

    if (strcmp(sudoers_path, "-") == 0) {
	sudoersin = stdin;
	sudoers_path = "stdin";
    } else if ((sudoersin = fopen(sudoers_path, "r")) == NULL) {
	if (!quiet)
	    warning(U_("unable to open %s"), sudoers_path);
	goto done;
    }
    init_parser(sudoers_path, quiet);
    if (sudoersparse() && !parse_error) {
	if (!quiet)
	    warningx(U_("failed to parse %s file, unknown error"), sudoers_path);
	parse_error = true;
	errorfile = sudoers_path;
    }
    ok = !parse_error;

    if (parse_error) {
	if (!quiet) {
	    if (errorlineno != -1)
		printf(_("parse error in %s near line %d\n"),
		    errorfile, errorlineno);
	    else if (errorfile != NULL)
		printf(_("parse error in %s\n"), errorfile);
	}
	goto done;
    }

    /* Open JSON output. */
    putchar('{');

    /* Dump Defaults in JSON format. */
    need_comma = print_defaults_json(indent, need_comma);

    /* Dump Aliases in JSON format. */
    need_comma = print_aliases_json(indent, need_comma);

    /* Dump User_Specs in JSON format. */
    print_userspecs_json(indent, need_comma);

    /* Close JSON output. */
    puts("\n}");

done:
    debug_return_bool(ok);
}