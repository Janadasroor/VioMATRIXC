/**********
Copyright 2023 The ngspice team.  All rights reserved.
License: Three-clause BCD
Author: 2023 Holger Vogt
**********/

/*
  For dealing with compatibility transformations

  PSPICE, LTSPICE and others
*/

#include "ngspice/ngspice.h"

#include "ngspice/compatmode.h"
#include "ngspice/cpdefs.h"
#include "ngspice/dstring.h"
#include "ngspice/dvec.h"
#include "ngspice/ftedefs.h"
#include "ngspice/fteext.h"
#include "ngspice/fteinp.h"
#include "numparam/general.h"

#include <limits.h>
#include <stdlib.h>

#include <sys/stat.h>
#include <sys/types.h>

#if !defined(__MINGW32__) && !defined(_MSC_VER)
#include <unistd.h>
#endif

#include "../misc/util.h" /* ngdirname() */
#include "inpcom.h"
#include "ngspice/stringskip.h"
#include "ngspice/stringutil.h"
#include "ngspice/wordlist.h"
#include "subckt.h"
#include "variable.h"

#define INTEGRATE_UDEVICES
#ifdef INTEGRATE_UDEVICES
#include "ngspice/udevices.h"
#endif

void print_compat_mode(void);
void set_compat_mode(void);
struct card* pspice_compat(struct card* newcard);
void pspice_compat_a(struct card* oldcard);
struct card* ltspice_compat(struct card* oldcard);
void ltspice_compat_a(struct card* oldcard);

/* Replace LTspice IF(cond, tval, fval) with standard (...)?(...):(...) */
/* Returns a newly allocated string with all IF(...) replaced, or NULL if no change. */
static char *replace_if_ternary(const char *line)
{
    if (!line) return NULL;
    /* First pass: find leftmost IF( and analyze */
    const char *ifpos = strstr(line, "IF(");
    if (!ifpos) ifpos = strstr(line, "if(");
    if (!ifpos) ifpos = strstr(line, "If(");
    if (!ifpos) return NULL;

    /* We'll build the result by processing IF() calls left-to-right.
     * Use a dynamic buffer approach. */
    size_t cap = strlen(line) + 256;
    char *result = tmalloc(cap);
    char *d = result;
    const char *src = line;

    while (1) {
        /* Find next IF( */
        const char *p = strstr(src, "IF(");
        if (!p) p = strstr(src, "if(");
        if (!p) p = strstr(src, "If(");
        if (!p) break; /* No more IF() calls */

        /* Find the matching closing paren.
         * First try normal parsing (commas at depth=1).
         * If that fails, try extra wrapping (commas at depth=2): if((cond, tval, fval)). */
        const char *cp = p + 3;
        int depth = 1;
        int extra_wrap = 0;
        const char *comma1 = NULL, *comma2 = NULL;
        while (*cp && depth > 0) {
            if (*cp == '(') depth++;
            else if (*cp == ')') depth--;
            if (depth == 1) {
                if (*cp == ',' && !comma1) comma1 = cp;
                else if (*cp == ',' && comma1 && !comma2) comma2 = cp;
            }
            cp++;
        }

        if (!comma1 || !comma2 || cp[-1] != ')') {
            /* Normal parsing failed — try extra wrapping: if((cond, tval, fval)).
             * The extra pair wraps all three arguments, so commas are at depth=2.
             * Depth d2=1 represents the if() open paren, d2=2 is inside the wrapper. */
            const char *cp2 = p + 3;
            int d2 = 1;
            const char *cm1 = NULL, *cm2 = NULL;
            while (*cp2 && d2 > 0) {
                if (*cp2 == '(') d2++;
                else if (*cp2 == ')') d2--;
                if (d2 == 2) {
                    if (*cp2 == ',' && !cm1) cm1 = cp2;
                    else if (*cp2 == ',' && cm1 && !cm2) cm2 = cp2;
                }
                cp2++;
            }
            /* Verify: commas found inside the extra wrapper, and the if() close paren exists */
            if (cm1 && cm2 && cp2[-1] == ')') {
                extra_wrap = 1;
                comma1 = cm1;
                comma2 = cm2;
                cp = cp2;  /* cp points past the if()'s close paren */
            }
        }

        if (!comma1 || !comma2 || cp[-1] != ')') {
            /* Malformed — copy verbatim and continue */
            size_t skip = (size_t)(p + 3 - src);
            if (d - result + skip >= cap) {
                cap *= 2;
                size_t off = (size_t)(d - result);
                result = trealloc(result, cap);
                d = result + off;
            }
            memcpy(d, src, skip);
            d += skip;
            src = p + 3;
            continue;
        }

        /* Extract and strip the three arguments */
        const char *cond_start, *cond_end, *true_start, *true_end, *false_start, *false_end;
        if (extra_wrap) {
            cond_start = p + 4;
            cond_end = comma1;
            true_start = comma1 + 1;
            true_end = comma2;
            false_start = comma2 + 1;
            false_end = cp - 2;
        } else {
            cond_start = p + 3;
            cond_end = comma1;
            true_start = comma1 + 1;
            true_end = comma2;
            false_start = comma2 + 1;
            false_end = cp - 1;
        }

        while (cond_start < cond_end && (*cond_start == ' ' || *cond_start == '\t')) cond_start++;
        while (cond_end > cond_start && (cond_end[-1] == ' ' || cond_end[-1] == '\t')) cond_end--;
        while (true_start < true_end && (*true_start == ' ' || *true_start == '\t')) true_start++;
        while (true_end > true_start && (true_end[-1] == ' ' || true_end[-1] == '\t')) true_end--;
        while (false_start < false_end && (*false_start == ' ' || *false_start == '\t')) false_start++;
        while (false_end > false_start && (false_end[-1] == ' ' || false_end[-1] == '\t')) false_end--;

        size_t cond_len = (size_t)(cond_end - cond_start);
        size_t true_len = (size_t)(true_end - true_start);
        size_t false_len = (size_t)(false_end - false_start);
        size_t prefix_len = (size_t)(p - src);

        /* New segment length: prefix + (cond).?...?.(true).?...?.(false) */
        size_t seg_len = prefix_len + 1 + cond_len + 5 + true_len + 5 + false_len + 1;
        /* Ensure buffer has room */
        if (d - result + seg_len + strlen(cp) + 1 >= cap) {
            cap = d - result + seg_len + strlen(cp) + 256;
            size_t off = (size_t)(d - result);
            result = trealloc(result, cap);
            d = result + off;
        }

        /* Copy prefix */
        memcpy(d, src, prefix_len);
        d += prefix_len;

        /* Write (cond) ? (true) : (false) */
        *d++ = '(';
        memcpy(d, cond_start, cond_len);
        d += cond_len;
        *d++ = ')'; *d++ = ' '; *d++ = '?'; *d++ = ' '; *d++ = '(';
        memcpy(d, true_start, true_len);
        d += true_len;
        *d++ = ')'; *d++ = ' '; *d++ = ':'; *d++ = ' '; *d++ = '(';
        memcpy(d, false_start, false_len);
        d += false_len;
        *d++ = ')';

        /* Advance source past the original IF() call */
        src = cp; /* points to the char after ')' */

        /* Reset ifpos pointers for next iteration */
    }

    /* Copy remaining source */
    size_t rest = strlen(src);
    if (d - result + rest + 1 >= cap) {
        cap = d - result + rest + 1;
        size_t off = (size_t)(d - result);
        result = trealloc(result, cap);
        d = result + off;
    }
    memcpy(d, src, rest + 1);

    /* If no changes were made, return NULL */
    if (strcmp(result, line) == 0) {
        tfree(result);
        return NULL;
    }
    return result;
}


/* Set a compatibility flag.
Currently available are flags for:
- LTSPICE, HSPICE, Spice3, PSPICE, KiCad, Spectre, XSPICE
*/
struct compat newcompat;
void set_compat_mode(void)
{
    char behaviour[80];
    newcompat.hs = FALSE;
    newcompat.ps = FALSE;
    newcompat.xs = FALSE;
    newcompat.lt = FALSE;
    newcompat.ki = FALSE;
    newcompat.a = FALSE;
    newcompat.spe = FALSE;
    newcompat.isset = FALSE;
    newcompat.s3 = FALSE;
    newcompat.mc = FALSE;
#ifdef VICOMPAT_LTSPICE
    newcompat.isset = newcompat.lt = TRUE;
#endif
    if (cp_getvar("vicompat", CP_STRING, behaviour, sizeof(behaviour))) {
        if (strstr(behaviour, "hs"))
            newcompat.isset = newcompat.hs = TRUE; /*HSPICE*/
        if (strstr(behaviour, "ps"))
            newcompat.isset = newcompat.ps = TRUE; /*PSPICE*/
        if (strstr(behaviour, "xs"))
            newcompat.isset = newcompat.xs = TRUE; /*XSPICE*/
        if (strstr(behaviour, "lt"))
            newcompat.isset = newcompat.lt = TRUE; /*LTSPICE*/
        if (strstr(behaviour, "ki"))
            newcompat.isset = newcompat.ki = TRUE; /*KiCad*/
        if (strstr(behaviour, "a"))
            newcompat.isset = newcompat.a = TRUE; /*complete netlist, used in conjuntion with other mode*/
        if (strstr(behaviour, "ll"))
            newcompat.isset = newcompat.ll = TRUE; /*all (currently not used)*/
        if (strstr(behaviour, "s3"))
            newcompat.isset = newcompat.s3 = TRUE; /*spice3 only*/
        if (strstr(behaviour, "eg"))
            newcompat.isset = newcompat.eg = TRUE; /*EAGLE*/
        if (strstr(behaviour, "spe")) {
            newcompat.isset = newcompat.spe = TRUE; /*Spectre*/
            newcompat.ps = newcompat.lt = newcompat.ki = newcompat.eg = FALSE;
        }
        if (strstr(behaviour, "mc")) {
            newcompat.isset = FALSE;
            newcompat.mc = TRUE; /*make check*/
        }
    }
    /* Auto-set 'a' flag when LTspice compat is enabled, since we always process complete netlists */
    if (newcompat.lt && !newcompat.a)
        newcompat.a = TRUE;

    if (newcompat.hs && newcompat.ps) {
        fprintf(stderr, "Warning: hs and ps compatibility are mutually exclusive, switch to ps!\n");
        newcompat.hs = FALSE;
    }
    /* reset everything for 'make check' */
    if (newcompat.mc)
        newcompat.eg = newcompat.hs = newcompat.spe = newcompat.ps = newcompat.xs =
        newcompat.ll = newcompat.lt = newcompat.ki = newcompat.a = FALSE;
}

/* Print the compatibility flags */
void print_compat_mode(void) {
    if (newcompat.mc) /* make check */
        return;
    if (newcompat.isset) {
        fprintf(stdout, "\n");
        fprintf(stdout, "Note: Compatibility modes selected:");
        if (newcompat.hs)
            fprintf(stdout, " hs");
        if (newcompat.ps)
            fprintf(stdout, " ps");
        if (newcompat.xs)
            fprintf(stdout, " xs");
        if (newcompat.lt)
            fprintf(stdout, " lt");
        if (newcompat.ki)
            fprintf(stdout, " ki");
        if (newcompat.ll)
            fprintf(stdout, " ll");
        if (newcompat.s3)
            fprintf(stdout, " s3");
        if (newcompat.eg)
            fprintf(stdout, " eg");
        if (newcompat.spe)
            fprintf(stdout, " spe");
        if (newcompat.a)
            fprintf(stdout, " a");
        fprintf(stdout, "\n\n");
    }
    else {
        fprintf(stdout, "\n");
        fprintf(stdout, "Note: No compatibility mode selected!\n\n");
    }
}


/* replace the E and G source TABLE function by a B source pwl
 * (used by ST OpAmps and comparators of Infineon models).
 * E_RO_3 VB_3 VB_4  VALUE={ TABLE( V(VCCP,VCCN), 2 , 35 , 3.3 , 15 , 5 , 10
 *         )*I(VreadIo)}
 * will become
 * BE_RO_3_1 TABLE_NEW_1 0 v = pwl( V(VCCP,VCCN), 2 , 35 , 3.3 , 15 , 5 , 10) 
 * E_RO_3 VB_3 VB_4  VALUE={ V(TABLE_NEW_1)*I(VreadIo)}
 */
static void replace_table(struct card *startcard)
{
    struct card *card;
    static int numb = 0;
    for (card = startcard; card; card = card->nextcard) {
        char *cut_line = card->line;
        if (*cut_line == 'e' || *cut_line == 'E' ||
            *cut_line == 'g' || *cut_line == 'G') {
            char *valp = search_plain_identifier(cut_line, "value");
            char *valp2 = search_plain_identifier(cut_line, "cur");
            if (valp || (valp2 && (*cut_line == 'g' || *cut_line == 'G'))) {
                char *ftablebeg = strstr(cut_line, "table(");
                if (!ftablebeg)
                    ftablebeg = strstr(cut_line, "TABLE(");
                while (ftablebeg) {
                    /* get the beginning of the line */
                    char *begline = copy_substring(cut_line, ftablebeg);
                    /* get the table function */
                    char *tabfun = gettok_char(&ftablebeg, ')', TRUE, TRUE);
                    if (!tabfun || strlen(tabfun) < 6) {
                        tfree(tabfun);
                        tfree(begline);
                        break;
                    }
                    /* the new e, g line */
                    char *neweline = tprintf("%s v(table_new_%d)%s",
                            begline, numb, ftablebeg);
                    char *newbline =
                            tprintf("btable_new_%d table_new_%d 0 v=pwl%s",
                                    numb, numb, tabfun + 5);
                    numb++;
                    tfree(tabfun);
                    tfree(begline);
                    tfree(card->line);
                    card->line = cut_line = neweline;
                    insert_new_line(card, newbline, 0, card->linenum_orig, card->linesource);
                    /* read next TABLE/TABLE function in cut_line */
                    ftablebeg = strstr(cut_line, "table(");
                    if (!ftablebeg)
                        ftablebeg = strstr(cut_line, "TABLE(");
                }
                continue;
            }
        }
    }
}

/* find the model requested by ako:model and do the replacement */
static struct card *find_model(struct card *startcard,
        struct card *changecard, char *searchname, char *newmname,
        char *newmtype, char *endstr)
{
    struct card *nomod, *returncard = changecard;
    char *origmname, *origmtype;
    char *beginline = startcard->line;
    if (ciprefix(".subckt", beginline))
        startcard = startcard->nextcard;

    int nesting2 = 0;
    for (nomod = startcard; nomod; nomod = nomod->nextcard) {
        char *origmodline = nomod->line;
        if (ciprefix(".subckt", origmodline))
            nesting2++;
        if (ciprefix(".ends", origmodline))
            nesting2--;
        /* skip any subcircuit */
        if (nesting2 > 0)
            continue;
        if (nesting2 == -1) {
            returncard = changecard;
            break;
        }
        if (ciprefix(".model", origmodline)) {
            origmodline = nexttok(origmodline);
            origmname = gettok(&origmodline);
            origmtype = gettok_noparens(&origmodline);
            if (cieq(origmname, searchname)) {
                if (!eq(origmtype, newmtype)) {
                    fprintf(stderr,
                            "Error: Original (%s) and new (%s) type for AKO "
                            "model disagree\n",
                            origmtype, newmtype);
                    controlled_exit(1);
                }
                /* we have got it */
                char *newmodcard = tprintf(".model %s %s %s%s",
                        newmname, newmtype, origmodline, endstr);
                char *tmpstr = strstr(newmodcard, ")(");
                if (tmpstr) {
                    tmpstr[0] = ' ';
                    tmpstr[1] = ' ';
                }
                tfree(changecard->line);
                changecard->line = newmodcard;
                tfree(origmname);
                tfree(origmtype);
                returncard = NULL;
                break;
            }
            tfree(origmname);
            tfree(origmtype);
        }
        else
            returncard = changecard;
    }
    return returncard;
}

/* Process any .distribution cards for PSPICE's Monte-Carlo feature.
 * A .distribution card defines a probability distribution by a PWL
 * density function.  This could be rewritten as a function that
 * returns a random value following that distribution.
 * For now, just comment it away.
 */
static void do_distribution(struct card *oldcard) {
    while (oldcard) {
        char *line = oldcard->line;

        if (line && ciprefix(".distribution", line))
            *line = '*';
        oldcard = oldcard->nextcard;
    }
}

/* Do the .model replacement required by ako (a kind of)
 * PSPICE does not support nested .subckt definitions, so
 * a simple structure is needed: search for ako:modelname,
 * then for modelname in the subcircuit or in the top level.
 * .model qorig npn (BF=48 IS=2e-7)
 * .model qbip1 ako:qorig NPN (BF=60 IKF=45m)
 * after the replacement we have
 * .model qbip1 NPN (BF=48 IS=2e-7 BF=60 IKF=45m)
 * and we benefit from the fact that if parameters have
 * doubled, the last entry of a parameter (e.g. BF=60)
 * overwrites the previous one (BF=48).
 */
static struct card *ako_model(struct card *startcard)
{
    char *newmname, *newmtype;
    struct card *card, *returncard = NULL, *subcktcard = NULL;
    for (card = startcard; card; card = card->nextcard) {
        char *akostr, *searchname;
        char *cut_line = card->line;

        if (ciprefix(".subckt", cut_line))
            subcktcard = card;
        else if (ciprefix(".ends", cut_line))
            subcktcard = NULL;
        if (ciprefix(".model", cut_line)) {
            if ((akostr = strstr(cut_line, "ako:")) != NULL &&
                isspace_c(akostr[-1])) {
                akostr += 4;
                searchname = gettok(&akostr);
                cut_line = nexttok(cut_line);
                newmname = gettok(&cut_line);
                newmtype = gettok_noparens(&akostr);

                /* Find the model and do the replacement. */

                if (subcktcard)
                    returncard = find_model(subcktcard, card, searchname,
                                            newmname, newmtype, akostr);
                if (returncard || !subcktcard)
                    returncard = find_model(startcard, card, searchname,
                                            newmname, newmtype, akostr);
                tfree(searchname);
                tfree(newmname);
                tfree(newmtype);

                /* Replacement not possible, bail out. */

                if (returncard)
                    break;
            }
        }
    }
    return returncard;
}

struct vsmodels {
    char *modelname;
    char *modeltype;     /* e.g., "counter", "buf", etc. */
    char *params;        /* e.g., "cycles=3" */
    char *subcktline;
    struct card *cardptr; /* pointer to .model card for in-place modification */
    struct vsmodels *nextmodel;
};

/* insert a new model, just behind the given model */
static struct vsmodels *insert_new_model(
        struct vsmodels *vsmodel, char *name, char *type, char *params, char *subcktline)
{
    struct vsmodels *x = TMALLOC(struct vsmodels, 1);

    x->nextmodel = vsmodel ? vsmodel->nextmodel : NULL;
    x->modelname = copy(name);
    x->modeltype = type ? copy(type) : NULL;
    x->params = params ? copy(params) : NULL;
    x->subcktline = copy(subcktline);
    x->cardptr = NULL;
    if (vsmodel)
        vsmodel->nextmodel = x;
    else
        vsmodel = x;

    return vsmodel;
}

/* find the model by name */
static bool find_a_model(
        struct vsmodels *vsmodel, char *name, char *subcktline)
{
    struct vsmodels *x;
    for (x = vsmodel; x; x = x->nextmodel)
        if (eq(x->modelname, name) &&
                eq(x->subcktline, subcktline))
            return TRUE;
    return FALSE;
}

/* find model by name, returning the entry */
static struct vsmodels *find_model_by_name(
        struct vsmodels *vsmodel, char *name, char *subcktline)
{
    struct vsmodels *x;
    for (x = vsmodel; x; x = x->nextmodel)
        if (eq(x->modelname, name) &&
                eq(x->subcktline, subcktline))
            return x;
    return NULL;
}

/* delete the vsmodels list */
static bool del_models(struct vsmodels *vsmodel)
{
    struct vsmodels *x;

    if (!vsmodel)
        return FALSE;

    while (vsmodel) {
        x = vsmodel->nextmodel;
        tfree(vsmodel->modelname);
        tfree(vsmodel->modeltype);
        tfree(vsmodel->params);
        tfree(vsmodel->subcktline);
        tfree(vsmodel);
        vsmodel = x;
    }

    return TRUE;
}

/* Check for double '{', replace the inner '{', '}' by '(', ')'
   in .subckt, .model, or .param (which all three may stem from external sources) */
static void rem_double_braces(struct card* newcard)
{
    struct card* card;
    int slevel = 0;

    for (card = newcard; card; card = card->nextcard) {
        char* cut_line = card->line;
        if (ciprefix(".subckt", cut_line))
            slevel++;
        else if (ciprefix(".ends", cut_line))
            slevel--;
        if (ciprefix(".model", cut_line) || slevel > 0 || ciprefix(".param", cut_line)) {
            cut_line = strchr(cut_line, '{');
            if (cut_line) {
                int level = 1;
                cut_line++;
                while (*cut_line != '\0') {
                    if (*cut_line == '{') {
                        level++;
                        if (level > 1)
                            *cut_line = '(';
                    }
                    else if (*cut_line == '}') {
                        if (level > 1)
                            *cut_line = ')';
                        level--;
                    }
                    cut_line++;
                }
            }
        }
    }
}

#ifdef INTEGRATE_UDEVICES
static void list_the_cards(struct card *startcard, char *prefix)
{
    struct card *card;
    if (!startcard) { return; }
    for (card = startcard; card; card = card->nextcard) {
        char* cut_line = card->line;
        printf("%s %s\n", prefix, cut_line);
    }
}

static struct card *the_last_card(struct card *startcard)
{
    struct card *card, *lastcard = NULL;
    if (!startcard) { return NULL; }
    for (card = startcard; card; card = card->nextcard) {
        lastcard = card;
    }
    return lastcard;
}
 static void remove_old_cards(struct card *first, struct card *stop)
{
    struct card *x, *y, *next = NULL, *nexta = NULL;
    if (!first || !stop || (first == stop)) { return; }
    for (x = first; (x && (x != stop)); x = next) {
        if (x->line) { tfree(x->line); }
        if (x->error) { tfree(x->error); }
        for (y = x->actualLine; y; y = nexta) {
            if (y->line) { tfree(y->line); }
            if (y->error) { tfree(y->error); }
            nexta = y->nextcard;
            tfree(y);
        }
        next = x->nextcard;
        tfree(x);
    }

}

static struct card *u_instances(struct card *startcard)
{
    struct card *card, *returncard = NULL, *subcktcard = NULL;
    struct card *newcard = NULL, *last_newcard = NULL;
    int models_ok = 0, models_not_ok = 0;
    int udev_ok = 0, udev_not_ok = 0;
    bool create_called = FALSE, repeat_pass = FALSE;
    bool skip_next = FALSE;
    struct card *c = startcard;
    bool insub = FALSE;
    int ps_global_tmodels = 0;

    /* NOTE: PSpice ref. manual Product Version 16.5 page 105.
       Subcircuits can be nested. That is, an X device can appear between
       .SUBCKT and .ENDS commands. However, subcircuit definitions cannot
       be nested. That is, a .SUBCKT statement cannot appear in the
       statements between a .SUBCKT and a .ENDS.
    */
    if (!cp_getvar("ps_global_tmodels", CP_NUM, &ps_global_tmodels, 0)) {
        ps_global_tmodels = 0;
    }
    if (ps_global_tmodels) {
        initialize_udevice(NULL);
        /* First scan for global timing models */
        while (c) {
            char *line = c->line;
            if (ciprefix(".subckt", line)) {
                u_subckt_line(line);
                insub = TRUE;
            } else if (ciprefix(".ends", line)) {
                insub = FALSE;
            }
            if (!insub && ciprefix(".model", line)) {
                (void) u_process_model_line(line, TRUE);
            }
            c = c->nextcard;
        }
    }

    /* Now scan for subckts containing U* instances and local timing models */
    card = startcard;
    while (card) {
        char *cut_line = card->line;

        skip_next = FALSE;
        if (ciprefix(".subckt", cut_line)) {
            models_ok = models_not_ok = 0;
            udev_ok = udev_not_ok = 0;
            subcktcard = card;
            if (!repeat_pass) {
                if (create_called) {
                    cleanup_udevice(FALSE);
                }
                initialize_udevice(subcktcard->line);
                create_called = TRUE;
            }
        } else if (ciprefix(".ends", cut_line)) {
            if (repeat_pass) {
                newcard = replacement_udevice_cards();
                if (newcard) {
                    char *tmp = NULL, *pos, *posp, *new_str = NULL, *cl;
                    struct card* tmpc;
                    /* replace linenum_orig and linesource */
                    for (tmpc = newcard; tmpc; tmpc = tmpc->nextcard) {
                        tmpc->linenum_orig = subcktcard->linenum_orig;
                        tmpc->linesource = subcktcard->linesource;
                    }
                    DS_CREATE(ds_tmp, 128);
                    /* Pspice definition of .subckt card:
                       .SUBCKT <name> [node]*
                       + [OPTIONAL: < <interface node> = <default value> >*]
                       + [PARAMS: < <name> = <value> >* ]
                       + [TEXT: < <name> = <text value> >* ]
                       ...
                       .ENDS
                    */
                    cl = subcktcard->line;
                    tmp = TMALLOC(char, strlen(cl) + 1);
                    (void) memcpy(tmp, cl, strlen(cl) + 1);
                    pos = strstr(tmp, "optional:");
                    posp = strstr(tmp, "params:");
                    ds_clear(&ds_tmp);
                    /* If there is an optional: and a param: then posp > pos */
                    if (pos) {
                        /* Remove the optional: section if present */
                        *pos = '\0';
                        if (posp) {
                            ds_cat_str(&ds_tmp, tmp);
                            ds_cat_str(&ds_tmp, posp);
                            new_str = copy(ds_get_buf(&ds_tmp));
                        } else {
                            new_str = copy(tmp);
                        }
                    } else {
                        new_str = copy(tmp);
                    }
                    ds_free(&ds_tmp);
                    tfree(tmp);
                    remove_old_cards(subcktcard->nextcard, card);
                    subcktcard->nextcard = newcard;
                    tfree(subcktcard->line);
                    subcktcard->line = new_str;
                    if (ft_ngdebug) {
                        printf("%s\n", new_str);
                        list_the_cards(newcard, "Replacement:");
                    }
                    last_newcard = the_last_card(newcard);
                    if (last_newcard) {
                        last_newcard->nextcard = card; // the .ends card
                    }
                } else {
                    models_ok = models_not_ok = 0;
                    udev_ok = udev_not_ok = 0;
                }
            }
            if (models_not_ok > 0 || udev_not_ok > 0) {
                repeat_pass = FALSE;
                cleanup_udevice(FALSE);
                create_called = FALSE;
            } else if (udev_ok > 0) {
                repeat_pass = TRUE;
                card = subcktcard;
                skip_next = TRUE;
            } else {
                repeat_pass = FALSE;
                cleanup_udevice(FALSE);
                create_called = FALSE;
            }
            subcktcard = NULL;
        } else if (ciprefix(".model", cut_line)) {
            if (subcktcard && !repeat_pass) {
                // Add .model local to subckt
                if (!u_process_model_line(cut_line, FALSE)) {
                    models_not_ok++;
                } else {
                    models_ok++;
                }
            }
        } else if (ciprefix("u", cut_line) || ciprefix("x", cut_line)) {
            /* U* device instance or X* instance of a subckt */
            if (subcktcard) {
                if (repeat_pass) {
                    if (!u_process_instance(cut_line)) {
                        repeat_pass = FALSE;
                        cleanup_udevice(FALSE);
                        create_called = FALSE;
                        subcktcard = NULL;
                        models_ok = models_not_ok = 0;
                        udev_ok = udev_not_ok = 0;
                        skip_next = FALSE;
                    }
                } else {
                    if (u_check_instance(cut_line)) {
                        udev_ok++;
                    } else {
                        udev_not_ok++;
                    }
                }
            }
        } else {
            if (!ciprefix("*", cut_line)) {
                udev_not_ok++;
            }
        }

        if (!skip_next) {
            card = card->nextcard;
        }
    }
    if (create_called) {
        cleanup_udevice(FALSE);
    }
    cleanup_udevice(TRUE);
    return returncard;
}
#endif

/**** PSPICE to ngspice **************
* .model replacement in ako (a kind of) model descriptions
* replace the E source TABLE function by a B source pwl
* add predefined params TEMP, VT, GMIN to beginning of deck
* add predefined params TEMP, VT, GMIN to beginning of each .subckt call
* add .functions limit, pwr, pwrs, stp, if, int
* replace vswitch part S
  S1 D S DG GND SWN
 .MODEL SWN VSWITCH(VON = { 0.55 } VOFF = { 0.49 }
     RON = { 1 / (2 * M*(W / LE)*(KPN / 2) * 10) }  ROFF = { 1G })
* by
  as1 %vd(DG GND) % gd(D S) aswn
  .model aswn aswitch(cntl_off={0.49} cntl_on={0.55} r_off={1G}
  + r_on={ 1 / (2 * M*(W / LE)*(KPN / 2) * 10) } log = TRUE)
* replace vswitch part S_ST
  S1 D S DG GND S_ST
 .MODEL S_ST VSWITCH(VT = { 1.5 } VH = { 0.3 }
     RON = { 1 / (2 * M*(W / LE)*(KPN / 2) * 10) }  ROFF = { 1G })
* by the classical voltage controlled ngspice switch
  S1 D S DG GND SWN
 .MODEL S_ST SW(VT = { 1.5 } VH = { 0.3 }
     RON = { 1 / (2 * M*(W / LE)*(KPN / 2) * 10) }  ROFF = { 1G })
  switch parameter td is not yet supported
* replace & by &&
* replace | by ||
* in R instance, replace TC = xx1, xx2 by TC1=xx1 TC2=xx2
* replace T_ABS by temp and T_REL_GLOBAL by dtemp in .model cards
* get the area factor for diodes and bipolar devices
* in subcircuit .subckt and X lines with 'params:' statement
  replace comma separator by space. Do nothing if comma is inside of {}.
* in .model, if double curly braces {{}}, replace the inner by {()}  */
struct card *pspice_compat(struct card *oldcard)
{
    struct card *card, *newcard, *nextcard;
    struct vsmodels *modelsfound = NULL;
    int skip_control = 0;

    /* .model replacement in ako (a kind of) model descriptions
     * in first .subckt and top level only */
    struct card *errcard;
    if ((errcard = ako_model(oldcard)) != NULL) {
        fprintf(stderr, "Error: no model found for %s\n", errcard->line);
        controlled_exit(1);
    }

    /* Process .distribution cards. */
    do_distribution(oldcard);

    /* replace TABLE function in E source */
    replace_table(oldcard);

    /* remove double braces */
    rem_double_braces(oldcard);

    /* add predefined params TEMP, VT, GMIN to beginning of deck */
    char *new_str = copy(".param temp = 'temper'");
    newcard = insert_new_line(NULL, new_str, 1, 0, "internal");
    new_str = copy(".param vt = '(temper + 273.15) * 8.6173303e-5'");
    nextcard = insert_new_line(newcard, new_str, 2, 0, "internal");
    new_str = copy(".param gmin = 1e-12");
    nextcard = insert_new_line(nextcard, new_str, 3, 0, "internal");
    /* add funcs limit, pwr, pwrs, stp, if, int */
    /* LIMIT( Output Expression, Limit1, Limit2)
       Output will stay between the two limits given. */
    new_str = copy(".func limit(x, a, b) { ternary_fcn(a > b, max(min(x, a), b), max(min(x, b), a)) }");
    nextcard = insert_new_line(nextcard, new_str, 4, 0, "internal");
    new_str = copy(".func pwr(x, a) { pow(x, a) }");
    nextcard = insert_new_line(nextcard, new_str, 5, 0, "internal");
    new_str = copy(".func pwrs(x, a) { sgn(x) * pow(x, a) }");
    nextcard = insert_new_line(nextcard, new_str, 6, 0, "internal");
    new_str = copy(".func stp(x) { u(x) }");
    nextcard = insert_new_line(nextcard, new_str, 7, 0, "internal");
    new_str = copy(".func if(a, b, c) {ternary_fcn( a , b , c )}");
    nextcard = insert_new_line(nextcard, new_str, 8, 0, "internal");
    new_str = copy(".func int(x) { sgn(x)*floor(abs(x)) }");
    nextcard = insert_new_line(nextcard, new_str, 9, 0, "internal");
    nextcard->nextcard = oldcard;

#ifdef INTEGRATE_UDEVICES
    {
        struct card *ucard;
#ifdef TRACE
        list_the_cards(newcard, "Before udevices");
#endif
        ucard = u_instances(newcard);
#ifdef TRACE
        list_the_cards(newcard, "After udevices");
#endif
    }
#endif


    /* add predefined parameters TEMP, VT after each subckt call */
    /* FIXME: This should not be necessary if we had a better sense of
    hierarchy during the evaluation of TEMPER */
    for (card = newcard; card; card = card->nextcard) {
        char *cut_line = card->line;
        if (ciprefix(".subckt", cut_line)) {
            new_str = copy(".param temp = 'temper'");
            nextcard = insert_new_line(card, new_str, 0, card->linenum_orig, card->linesource);
            new_str = copy(".param vt = '(temper + 273.15) * 8.6173303e-5'");
            nextcard = insert_new_line(nextcard, new_str, 1, card->linenum_orig, card->linesource);
            /* params: replace comma separator by space.
               Do nothing if you are inside of { }. */
            char* parastr = strstr(cut_line, "params:");
            int brace = 0;
            if (parastr) {
                parastr += 8;
                while (*parastr) {
                    if (*parastr == '{')
                        brace++;
                    else if (*parastr == '}')
                        brace--;
                    if (brace == 0 && *parastr == ',')
                        *parastr = ' ';
                    parastr++;
                }
            }
        }
    }

    /* .model xxx NMOS/PMOS level=6 --> level = 8,  version=3.2.4
       .model xxx NMOS/PMOS level=7 --> level = 8,  version=3.2.4
       .model xxx NMOS/PMOS level=5 --> only available per Veriloga, OpenVAF and OSDI
       .model xxx NMOS/PMOS level=8 --> level = 14, version=4.5.0
       .model xxx NPN/PNP   level=2 --> only available per Veriloga, OpenVAF and OSDI
       .model xxx LPNP      level=n --> level = 1 subs=-1
       Remove any Monte - Carlo variation parameters from .model cards.*/
    for (card = newcard; card; card = card->nextcard) {
        char* cut_line = card->line;
        if (ciprefix(".model", cut_line)) {
            char* modname, *modtype, *curr_line;
            int i;
            char *cut_del = curr_line = cut_line = inp_remove_ws(copy(cut_line));
            cut_line = nexttok(cut_line); /* skip .model */
            modname = gettok(&cut_line); /* save model name */
            if (!modname) {
                fprintf(stderr, "Error: No model name given for %s\n", curr_line);
                controlled_exit(EXIT_BAD);
            }
            modtype = gettok_noparens(&cut_line); /* save model type */
            if (!modtype) {
                fprintf(stderr, "Error: No model type given for %s\n", curr_line);
                controlled_exit(EXIT_BAD);
            }
            if (cieq(modtype, "NMOS") || cieq(modtype, "PMOS")) {
                char* lv = strstr(cut_line, "level=");
                if (lv) {
                    int ll;
                    lv = lv + 6;
                    char* ntok = gettok(&lv);
                    ll = atoi(ntok);
                    switch (ll) {
                    case 5:
                        {
                        /* EKV 2.6 only per OSDI and OpenVAF */
                        fprintf(stderr, "Error: MOS model level 5, EKV 2.6, is not available as an intrinsic model.\n");
                        fprintf(stderr, "    Please consider using a Verilog-A model, OpenVAF compilation,\n");
                        fprintf(stderr, "    and the ngspice OSDI interface (see ngspice manual chapter 9).\n");
                        controlled_exit(EXIT_BAD);
                        }
                        break;
                    case 6:
                    case 7:
                        {
                        /* BSIM3 version 3.2.4 */
                        char* newline = tprintf(".model %s %s level=8 version=3.2.4 %s", modname, modtype, lv);
                        tfree(card->line);
                        card->line = curr_line = newline;
                        }
                        break;
                    case 8:
                        {
                        /* BSIM4 version 4.5.0 */
                        char* newline = tprintf(".model %s %s level=14 version=4.5.0 %s", modname, modtype, lv);
                        tfree(card->line);
                        card->line = curr_line = newline;
                        }
                        break;
                    default:
                        break;
                    }
                    tfree(ntok);
                }
            }
            else if (cieq(modtype, "NPN") || cieq(modtype, "PNP")) {
                char* lv = strstr(cut_line, "level=");
                if (lv) {
                    int ll;
                    lv = lv + 6;
                    char* ntok = gettok(&lv);
                    ll = atoi(ntok);
                    switch (ll) {
                    case 2:
                        {
                        /* MEXTRAM 504.12.1 only per OSDI and OpenVAF */
                        fprintf(stderr, "Error: Bipolar model level 2, MEXTRAM 504.12.1, is not available as an intrinsic model.\n");
                        fprintf(stderr, "    Please consider using a Verilog-A model, OpenVAF compilation,\n");
                        fprintf(stderr, "    and the ngspice OSDI interface (see ngspice manual chapter 9).\n");
                        controlled_exit(EXIT_BAD);
                        }
                        break;
                    default:
                        break;
                    }
                    tfree(ntok);
                }
            }
            else if (cieq(modtype, "LPNP")) {
                /* lateral PNP enabled */
                char* newline = tprintf(".model %s PNP level=1 subs=-1 %s", modname, cut_line);
                tfree(card->line);
                card->line = curr_line = newline;
            }
            tfree(modname);
            tfree(modtype);

            /* Remove any Monte-Carlo variation parameters. They qualify
             * a previous parameter, so there must be at least 3 tokens.
             * There are two keywords "dev" (different values for each device),
             * and "lot" (all devices of this model share a value).
             * The keyword may be optionally followed by '/' and
             * a probability distribution name, then there must be '=' and
             * a value, then an optional '%' indicating relative rather than
             * absolute variation. Allow muliple lot and dev on a single .model line.
             */
            bool remdevlot = FALSE;
            cut_line = curr_line;
            for (i = 0; i < 3; i++)
                cut_line = nexttok(cut_line);
            while (cut_line) {
                if (!strncmp(cut_line, "dev=", 4) ||
                    !strncmp(cut_line, "lot=", 4)) {
                    while (*cut_line && !isspace_c(*cut_line)) {
                        *cut_line++ = ' ';
                    }
                    remdevlot = TRUE;
                    cut_line = skip_ws(cut_line);
                    continue;
                }
                cut_line = nexttok(cut_line);
            }
            if (remdevlot) {
                tfree(card->line);
                card->line = curr_line;
            }
            else
                tfree(cut_del);
        } // if .model
    } // for loop through all cards

    /* x ... params: p1=val1, p2=val2 replace comma separator by space.
       Do nothing if you are inside of { }. */
    for (card = newcard; card; card = card->nextcard) {
        char* cut_line = card->line;
        if (ciprefix("x", cut_line)) {
            char* parastr = strstr(cut_line, "params:");
            int brace = 0;
            if (parastr) {
                parastr += 8;
                while (*parastr) {
                    if (*parastr == '{')
                        brace++;
                    else if (*parastr == '}')
                        brace--;
                    if (brace == 0 && *parastr == ',')
                        *parastr = ' ';
                    parastr++;
                }
            }
        }
    }

    /* in R instance, replace TC = xx1, xx2 by TC1=xx1 TC2=xx2 */
    for (card = newcard; card; card = card->nextcard) {
        char *cut_line = card->line;

        /* exclude any command inside .control ... .endc */
        if (ciprefix(".control", cut_line)) {
            skip_control++;
            continue;
        }
        else if (ciprefix(".endc", cut_line)) {
            skip_control--;
            continue;
        }
        else if (skip_control > 0) {
            continue;
        }

        if (*cut_line == 'r' || *cut_line == 'l' || *cut_line == 'c') {
            /* Skip name and two nodes */
            char *ntok = nexttok(cut_line);
            ntok = nexttok(ntok);
            ntok = nexttok(ntok);
            if (!ntok || *ntok == '\0') {
                fprintf(stderr, "Error: Missing token in line %d:\n%s\n",
                        card->linenum, cut_line);
                fprintf(stderr, "    Please correct the input file\n");
                controlled_exit(1);
            }
            char *tctok = search_plain_identifier(ntok, "tc");
            if (tctok) {
                char *tc1, *tc2;
                char *tctok1 = strchr(tctok, '=');
                if (tctok1)
                    /* skip '=' */
                    tctok1 += 1;
                else
                    /* no '=' found, skip 'tc' */
                    tctok1 = tctok + 2;
                /* tc1 may be an expression, enclosed in {} */
                if (*tctok1 == '{') {
                    tc1 = gettok_char(&tctok1, '}', TRUE, TRUE);
                }
                else {
                    tc1 = gettok_node(&tctok1);
                }
                /* skip spaces and commas */
                while (isspace_c(*tctok1) || (*tctok1 == ','))
                   tctok1++;
                /* tc2 may be an expression, enclosed in {} */
                if (*tctok1 == '{') {
                    tc2 = gettok_char(&tctok1, '}', TRUE, TRUE);
                }
                else {
                    tc2 = gettok_node(&tctok1);
                }
                tctok[-1] = '\0';
                char *newstring;
                if (tc1 && tc2)
                    newstring = tprintf("%s tc1=%s tc2=%s",
                            cut_line, tc1, tc2);
                else if (tc1)
                    newstring = tprintf("%s tc1=%s", cut_line, tc1);
                else {
                    fprintf(stderr,
                            "Warning: tc without parameters removed in line "
                            "\n   %s\n",
                            cut_line);
                    continue;
                }
                tfree(card->line);
                card->line = newstring;
                tfree(tc1);
                tfree(tc2);
            }
        }
    }

    /* replace & with && , | with || , *# with * # , and ~ with ! */
    for (card = newcard; card; card = card->nextcard) {
        char *t;
        char *cut_line = card->line;

        /* we don't have command lines in a PSPICE model */
        if (ciprefix("*#", cut_line)) {
            char *tmpstr = tprintf("* #%s", cut_line + 2);
            tfree(card->line);
            card->line = tmpstr;
            continue;
        }

        if (*cut_line == '*')
            continue;

        if (*cut_line == '\0')
            continue;

        /* exclude any command inside .control ... .endc */
        if (ciprefix(".control", cut_line)) {
            skip_control++;
            continue;
        }
        else if (ciprefix(".endc", cut_line)) {
            skip_control--;
            continue;
        }
        else if (skip_control > 0) {
            continue;
        }
        if ((t = strstr(card->line, "&")) != NULL) {
            while (t && (t[1] != '&')) {
                char *tt = NULL;
                char *tn = copy(t + 1); /*skip |*/
                char *strbeg = copy_substring(card->line, t);
                tfree(card->line);
                card->line = tprintf("%s&&%s", strbeg, tn);
                tfree(strbeg);
                tfree(tn);
                t = card->line;
                while ((t = strstr(t, "&&")) != NULL)
                    tt = t = t + 2;
                if (!tt)
                    break;
                else
                    t = strstr(tt, "&");
            }
        }
        if ((t = strstr(card->line, "|")) != NULL) {
            while (t && (t[1] != '|')) {
                char *tt = NULL;
                char *tn = copy(t + 1); /*skip |*/
                char *strbeg = copy_substring(card->line, t);
                tfree(card->line);
                card->line = tprintf("%s||%s", strbeg, tn);
                tfree(strbeg);
                tfree(tn);
                t = card->line;
                while ((t = strstr(t, "||")) != NULL)
                    tt = t = t + 2;
                if (!tt)
                    break;
                else
                    t = strstr(tt, "|");
            }
        }
        /* We may have '~' in path names or A devices */
        if (ciprefix(".inc", card->line) || ciprefix(".lib", card->line) ||
                ciprefix("A", card->line))
            continue;

        if ((t = strstr(card->line, "~")) != NULL) {
            while (t) {
                *t = '!';
                t = strstr(t, "~");
            }
        }
    }

    /* replace T_ABS by temp, T_REL_GLOBAL by dtemp, and T_MEASURED by TNOM
    in .model cards. What about T_REL_LOCAL ? T_REL_LOCAL is used in
    conjunction with AKO and is not yet implemented.  */
    for (card = newcard; card; card = card->nextcard) {
        char *cut_line = card->line;
        if (ciprefix(".model", cut_line)) {
            char *t_str;
            if ((t_str = strstr(cut_line, "t_abs")) != NULL)
                memcpy(t_str, " temp", 5);
            else if ((t_str = strstr(cut_line, "t_rel_global")) != NULL)
                memcpy(t_str, "       dtemp", 12);
            else if ((t_str = strstr(cut_line, "t_measured")) != NULL)
                memcpy(t_str, "      tnom", 10);
        }
    }

    /* get the area factor for diodes and bipolar devices
    d1 n1 n2 dmod 7 --> d1 n1 n2 dmod area=7
    q2 n1 n2 n3 [n4] bjtmod 1.35 --> q2 n1 n2 n3 n4 bjtmod area=1.35
    q3 1 2 3 4 bjtmod 1.45 --> q2 1 2 3 4 bjtmod area=1.45
    */
    for (card = newcard; card; card = card->nextcard) {
        char *cut_line = card->line;
        if (*cut_line == '*')
            continue;
        // exclude any command inside .control ... .endc
        if (ciprefix(".control", cut_line)) {
            skip_control++;
            continue;
        }
        else if (ciprefix(".endc", cut_line)) {
            skip_control--;
            continue;
        }
        else if (skip_control > 0) {
            continue;
        }
        if (*cut_line == 'q') {
            /* According to PSPICE Reference Guide the fourth (substrate) node
            has to be put into [] if it is not just a number */
            cut_line = nexttok(cut_line); //.model
            cut_line = nexttok(cut_line); // node1
            cut_line = nexttok(cut_line); // node2
            cut_line = nexttok(cut_line); // node3
            if (!cut_line || *cut_line == '\0') {
                fprintf(stderr, "Line no. %d, %s, missing tokens\n",
                        card->linenum_orig, card->line);
                if (ft_stricterror)
                    controlled_exit(1);
                else
                    continue;
            }
            if (*cut_line == '[') { // node4 not a number
                *cut_line = ' ';
                cut_line = strchr(cut_line, ']');
                *cut_line = ' ';
                cut_line = skip_ws(cut_line);
                cut_line = nexttok(cut_line); // model name
            }
            else { // if an integer number, it is node4
                bool is_node4 = TRUE;
                while (*cut_line && !isspace_c(*cut_line))
                    if (!isdigit_c(*cut_line++))
                        is_node4 = FALSE; // already model name
                if (is_node4) {
                    cut_line = nexttok(cut_line); // model name
                }
            }
            if (cut_line && *cut_line &&
                    atof(cut_line) > 0.0) { // size of area is a real number
                char *tmpstr1 = copy_substring(card->line, cut_line);
                char *tmpstr2 = tprintf("%s area=%s", tmpstr1, cut_line);
                tfree(tmpstr1);
                tfree(card->line);
                card->line = tmpstr2;
            }
            else if (cut_line && *cut_line &&
                    *(skip_ws(cut_line)) ==
                            '{') { // size of area is parametrized inside {}
                char *tmpstr1 = copy_substring(card->line, cut_line);
                char *tmpstr2 = gettok_char(&cut_line, '}', TRUE, TRUE);
                char *tmpstr3 =
                        tprintf("%s area=%s %s", tmpstr1, tmpstr2, cut_line);
                tfree(tmpstr1);
                tfree(tmpstr2);
                tfree(card->line);
                card->line = tmpstr3;
            }
        }
        else if (*cut_line == 'd') {
            cut_line = nexttok(cut_line); //.model
            cut_line = nexttok(cut_line); // node1
            cut_line = nexttok(cut_line); // node2
            if (!cut_line || *cut_line == '\0') {
                fprintf(stderr, "Line no. %d, %s, missing tokens\n",
                        card->linenum_orig, card->line);
                if (ft_stricterror)
                    controlled_exit(1);
                else
                    continue;
            }
            cut_line = nexttok(cut_line); // model name
            if (cut_line && *cut_line &&
                    atof(cut_line) > 0.0) { // size of area
                char *tmpstr1 = copy_substring(card->line, cut_line);
                char *tmpstr2 = tprintf("%s area=%s", tmpstr1, cut_line);
                tfree(tmpstr1);
                tfree(card->line);
                card->line = tmpstr2;
            }
        }
    }

    /* if vswitch part s, replace
     * S1 D S DG GND SWN
     * .MODEL SWN VSWITCH ( VON = {0.55} VOFF = {0.49}
     *         RON={1/(2*M*(W/LE)*(KPN/2)*10)}  ROFF={1G} )
     * by
     * a1 %v(DG) %gd(D S) swa
     * .MODEL SWA aswitch(cntl_off=0.49 cntl_on=0.55 r_off=1G
     *         r_on={1/(2*M*(W/LE)*(KPN/2)*10)} log=TRUE)
     *
     * if vswitch part s_st, don't replace instance, only model
     * replace
     * S1 D S DG GND S_ST
     * .MODEL S_ST VSWITCH(VT = { 1.5 } VH = { 0.s }
           RON = { 1 / (2 * M*(W / LE)*(KPN / 2) * 10) }  ROFF = { 1G })
     * by the classical voltage controlled ngspice switch
     * S1 D S DG GND S_ST
     * .MODEL S_ST SW(VT = { 1.5 } VH = { 0.s }
             RON = { 1 / (2 * M*(W / LE)*(KPN / 2) * 10) }  ROFF = { 1G })
     * vswitch delay parameter td is not yet supported

     * simple hierachy, as nested subcircuits are not allowed in PSPICE */

    /* first scan: find the vswitch models, transform them and put the S models
       into a list */
    for (card = newcard; card; card = card->nextcard) {
        char *str;
        static struct card *subcktline = NULL;
        static int nesting = 0;
        char *cut_line = card->line;
        if (ciprefix(".subckt", cut_line)) {
            subcktline = card;
            nesting++;
        }
        if (ciprefix(".ends", cut_line))
            nesting--;

        if (ciprefix(".model", card->line) && strstr(card->line, "vswitch")) {
            char *modname;

            str = card->line = inp_remove_ws(card->line);
            str = nexttok(str); /* throw away '.model' */
            INPgetNetTok(&str, &modname, 0); /* model name */
            if (!ciprefix("vswitch", str)) {
                tfree(modname);
                continue;
            }
            str = nexttok_noparens(str); /* throw away 'vswitch' */
            /* S_ST switch (parameters ron, roff, vt, vh)
             * we have to find 0 to 4 parameters, identified by 'vh=' etc.
             * Parameters not found have to be replaced by their default values. */
            if (strstr(str, "vt=") || strstr(str, "vh=")) {
                char* newstr;
                char* lstr = copy(str);
                char* partstr = strstr(lstr, "ron=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "ron=1.0", lstr);  //default value
                    tfree(lstr);
                    lstr = newstr;
                }
                partstr = strstr(lstr, "roff=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "roff=1.0e12", lstr);  //default value
                    tfree(lstr);
                    lstr = newstr;
                }
                partstr = strstr(lstr, "vt=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "vt=0", lstr);  //default value
                    tfree(lstr);
                    lstr = newstr;
                }
                partstr = strstr(lstr, "vh=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "vh=0", lstr);  //default value
                    tfree(lstr);
                    lstr = newstr;
                }
                tfree(card->line);
                if (lstr[strlen(lstr) - 1] == ')')
                    card->line = tprintf(".model %s sw ( %s", modname, lstr);
                else
                    card->line = tprintf(".model %s sw %s", modname, lstr);
                tfree(lstr);
                tfree(modname);
            }
            /* S vswitch  (parameters ron, roff, von, voff) */
            /* We have to find 0 to 4 parameters, identified by 'von=' etc. and
             * replace them by the pswitch code model parameters
             * replace VON by cntl_on, VOFF by cntl_off, RON by r_on, and ROFF by r_off.
             * Parameters not found have to be replaced by their default values. */
            else if (strstr(str, "von=") || strstr(str, "voff=")) {
                char* newstr, *begstr;
                char* lstr = copy(str);
                /* ron */
                char* partstr = strstr(lstr, "ron=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "r_on=1.0", lstr);  //default value
                }
                else {
                    begstr = copy_substring(lstr, partstr);
                    newstr = tprintf("%s r_on%s", begstr, partstr + 3);
                    tfree(begstr);
                }
                tfree(lstr);
                lstr = newstr;
                /* roff */
                partstr = strstr(lstr, "roff=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "r_off=1.0e6", lstr);  //default value
                }
                else {
                    begstr = copy_substring(lstr, partstr);
                    newstr = tprintf("%s r_off%s", begstr, partstr + 4);
                    tfree(begstr);
                }
                tfree(lstr);
                lstr = newstr;
                /* von */
                partstr = strstr(lstr, "von=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "cntl_on=1", lstr);  //default value
                    tfree(lstr);
                    lstr = newstr;
                }
                else {
                    begstr = copy_substring(lstr, partstr);
                    newstr = tprintf("%s cntl_on%s", begstr, partstr + 3);
                    tfree(begstr);
                }
                tfree(lstr);
                lstr = newstr;
                /* voff */
                partstr = strstr(lstr, "voff=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "cntl_off=0", lstr);  //default value
                    tfree(lstr);
                    lstr = newstr;
                }
                else {
                    begstr = copy_substring(lstr, partstr);
                    newstr = tprintf("%s cntl_off%s", begstr, partstr + 4);
                    tfree(begstr);
                }
                tfree(lstr);
                lstr = newstr;
                tfree(card->line);
                if (lstr[strlen(lstr) - 1] == ')')
                    card->line = tprintf(".model a%s pswitch( log=TRUE %s", modname, lstr);
                else
                    card->line = tprintf(".model a%s pswitch(%s log=TRUE)", modname, lstr);
                tfree(lstr);
                /* add to list, to change vswitch instance to code model line */
                if (nesting > 0)
                    modelsfound = insert_new_model(
                        modelsfound, modname, NULL, NULL, subcktline->line);
                else
                    modelsfound = insert_new_model(
                        modelsfound, modname, NULL, NULL, "top");
                tfree(modname);
            }
            else {
                fprintf(stderr, "Error: Bad switch model in line %s\n", card->line);
            }
        }
    }

    /* no need to continue if no vswitch is found */
    if (!modelsfound)
        goto iswi;

    /* second scan: find the switch instances s calling a vswitch model and
     * transform them */
    for (card = newcard; card; card = card->nextcard) {
        static struct card *subcktline = NULL;
        static int nesting = 0;
        char *cut_line = card->line;
        if (*cut_line == '*')
            continue;
        // exclude any command inside .control ... .endc
        if (ciprefix(".control", cut_line)) {
            skip_control++;
            continue;
        }
        else if (ciprefix(".endc", cut_line)) {
            skip_control--;
            continue;
        }
        else if (skip_control > 0) {
            continue;
        }
        if (ciprefix(".subckt", cut_line)) {
            subcktline = card;
            nesting++;
        }
        if (ciprefix(".ends", cut_line))
            nesting--;

        if (ciprefix("s", cut_line)) {
            /* check for the model name */
            int i;
            bool good = TRUE;
            char *stoks[6];
            for (i = 0; i < 6; i++) {
                stoks[i] = gettok_node(&cut_line);
                if (!stoks[i]) {
                    int ii;
                    fprintf(stderr,
                        "Error: bad syntax in line %d\n  %s\n"
                        "from file\n"
                        "  %s\n",
                        card->linenum_orig, card->line, card->linesource);
                    good = FALSE;
                    /* null the rest of stoks */
                    for (ii = i + 1; ii < 6; ii++) {
                        stoks[ii] = NULL;
                    }
                    break;
                }
            }
            if (!good) {
                for (i = 0; i < 6; i++)
                    tfree(stoks[i]);
                continue;
            }
            /* rewrite s line and replace it if a model is found */
            if ((nesting > 0) &&
                    find_a_model(modelsfound, stoks[5], subcktline->line)) {
                tfree(card->line);
                card->line = tprintf("a%s %%gd(%s %s) %%gd(%s %s) a%s",
                        stoks[0], stoks[3], stoks[4], stoks[1], stoks[2],
                        stoks[5]);
            }
            /* if model is not within same subcircuit, search at top level */
            else if (find_a_model(modelsfound, stoks[5], "top")) {
                tfree(card->line);
                card->line = tprintf("a%s %%gd(%s %s) %%gd(%s %s) a%s",
                        stoks[0], stoks[3], stoks[4], stoks[1], stoks[2],
                        stoks[5]);
            }
            for (i = 0; i < 6; i++)
                tfree(stoks[i]);
        }
    }
    del_models(modelsfound);
    modelsfound = NULL;

iswi:;

    /* if iswitch part s, replace
     * W1 D S VC SWN
     * .MODEL SWN ISWITCH ( ION = {0.55} IOFF = {0.49}
     *         RON={1/(2*M*(W/LE)*(KPN/2)*10)}  ROFF={1G} )
     * by
     * a1 %v(DG) %gd(D S) swa
     * .MODEL SWA aswitch(cntl_off=0.49 cntl_on=0.55 r_off=1G
     *         r_on={1/(2*M*(W/LE)*(KPN/2)*10)} log=TRUE)
     *
     * if iswitch part s_st (short transition), don't replace instance, but only model
     * replace
     * W1 D S VC S_ST
     * .MODEL S_ST ISWITCH(IT = { 1.5 } IH = { 0.2 }
           RON = { 1 / (2 * M*(W / LE)*(KPN / 2) * 10) }  ROFF = { 1G })
     * by the classical current controlled ngspice switch
     * W1 D S DG GND S_ST
     * .MODEL S_ST CSW(IT = { 1.5 } IH = { 0.2 }
             RON = { 1 / (2 * M*(W / LE)*(KPN / 2) * 10) }  ROFF = { 1G })
     * iswitch delay parameter td is not yet supported

     * simple hierachy, as nested subcircuits are not allowed in PSPICE */

     /* first scan: find the iswitch models, transform them and put them into a
      * list */
    for (card = newcard; card; card = card->nextcard) {
        char* str;
        static struct card* subcktline = NULL;
        static int nesting = 0;
        char* cut_line = card->line;
        if (ciprefix(".subckt", cut_line)) {
            subcktline = card;
            nesting++;
        }
        if (ciprefix(".ends", cut_line))
            nesting--;

        if (ciprefix(".model", card->line) && strstr(card->line, "iswitch")) {
            char* modname;

            card->line = str = inp_remove_ws(card->line);
            str = nexttok(str); /* throw away '.model' */
            INPgetNetTok(&str, &modname, 0); /* model name */
            if (!ciprefix("iswitch", str)) {
                tfree(modname);
                continue;
            }
            str = nexttok_noparens(str); /* throw away 'iswitch' */
            /* S_ST switch (parameters ron, roff, it, ih)
             * we have to find 0 to 4 parameters, identified by 'ih=' etc.
             * Parameters not found have to be replaced by their default values. */
            if (strstr(str, "it=") || strstr(str, "ih=")) {
                char* newstr;
                char* lstr = copy(str);
                char* partstr = strstr(lstr, "ron=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "ron=1.0", lstr);  //default value
                    tfree(lstr);
                    lstr = newstr;
                }
                partstr = strstr(lstr, "roff=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "roff=1.0e12", lstr);  //default value
                    tfree(lstr);
                    lstr = newstr;
                }
                partstr = strstr(lstr, "it=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "it=0", lstr);  //default value
                    tfree(lstr);
                    lstr = newstr;
                }
                partstr = strstr(lstr, "ih=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "ih=0", lstr);  //default value
                    tfree(lstr);
                    lstr = newstr;
                }
                tfree(card->line);
                if (lstr[strlen(lstr) - 1] == ')')
                    card->line = tprintf(".model %s csw ( %s", modname, lstr);
                else
                    card->line = tprintf(".model %s csw %s", modname, lstr);
                tfree(lstr);
                tfree(modname);
            }
            /* S vswitch  (parameters ron, roff, ion, ioff) */
            /* We have to find 0 to 4 parameters, identified by 'ion=' etc. and
             * replace them by the pswitch code model parameters
             * replace VON by cntl_on, VOFF by cntl_off, RON by r_on, and ROFF by r_off.
             * Parameters not found have to be replaced by their default values. */
            else if (strstr(str, "ion=") || strstr(str, "ioff=")) {
                char* newstr, * begstr;
                char* lstr = copy(str);
                /* ron */
                char* partstr = strstr(lstr, "ron=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "r_on=1.0", lstr);  //default value
                }
                else {
                    begstr = copy_substring(lstr, partstr);
                    newstr = tprintf("%s r_on%s", begstr, partstr + 3);
                }
                tfree(lstr);
                lstr = newstr;
                /* roff */
                partstr = strstr(lstr, "roff=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "r_off=1.0e6", lstr);  //default value
                }
                else {
                    begstr = copy_substring(lstr, partstr);
                    newstr = tprintf("%s r_off%s", begstr, partstr + 4);
                }
                tfree(lstr);
                lstr = newstr;
                /* von */
                partstr = strstr(lstr, "ion=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "cntl_on=1", lstr);  //default value
                    tfree(lstr);
                    lstr = newstr;
                }
                else {
                    begstr = copy_substring(lstr, partstr);
                    newstr = tprintf("%s cntl_on%s", begstr, partstr + 3);
                }
                tfree(lstr);
                lstr = newstr;
                /* voff */
                partstr = strstr(lstr, "ioff=");
                if (!partstr) {
                    newstr = tprintf("%s %s", "cntl_off=0", lstr);  //default value
                    tfree(lstr);
                    lstr = newstr;
                }
                else {
                    begstr = copy_substring(lstr, partstr);
                    newstr = tprintf("%s cntl_off%s", begstr, partstr + 4);
                }
                tfree(lstr);
                lstr = newstr;
                tfree(card->line);
                if (lstr[strlen(lstr) - 1] == ')')
                    card->line = tprintf(".model a%s aswitch( log=TRUE limit=TRUE %s", modname, lstr);
                else
                    card->line = tprintf(".model a%s aswitch(%s log=TRUE limit=TRUE)", modname, lstr);
                tfree(lstr);
                /* add to list, to change vswitch instance to code model line */
                if (nesting > 0)
                    modelsfound = insert_new_model(
                        modelsfound, modname, NULL, NULL, subcktline->line);
                else
                    modelsfound = insert_new_model(
                        modelsfound, modname, NULL, NULL, "top");
                tfree(modname);
            }
            else {
                fprintf(stderr, "Error: Bad switch model in line %s\n", card->line);
            }
        }
    }

    /* no need to continue if no vswitch is found */
    if (!modelsfound)
        return newcard;

    /* second scan: find the switch instances s calling an iswitch model and
     * transform them */
    for (card = newcard; card; card = card->nextcard) {
        static struct card* subcktline = NULL;
        static int nesting = 0;
        char* cut_line = card->line;
        if (*cut_line == '*')
            continue;
        // exclude any command inside .control ... .endc
        if (ciprefix(".control", cut_line)) {
            skip_control++;
            continue;
        }
        else if (ciprefix(".endc", cut_line)) {
            skip_control--;
            continue;
        }
        else if (skip_control > 0) {
            continue;
        }
        if (ciprefix(".subckt", cut_line)) {
            subcktline = card;
            nesting++;
        }
        if (ciprefix(".ends", cut_line))
            nesting--;

        if (ciprefix("w", cut_line)) {
            /* check for the model name */
            int i;
            char* stoks[5];
            for (i = 0; i < 5; i++)
                stoks[i] = gettok_node(&cut_line);
            /* rewrite w line and replace it if a model is found */
            if ((nesting > 0) &&
                find_a_model(modelsfound, stoks[4], subcktline->line)) {
                tfree(card->line);
                card->line = tprintf("a%s %%vnam(%s) %%gd(%s %s) a%s",
                    stoks[0], stoks[3], stoks[1], stoks[2],
                    stoks[4]);
            }
            /* if model is not within same subcircuit, search at top level */
            else if (find_a_model(modelsfound, stoks[4], "top")) {
                tfree(card->line);
                card->line = tprintf("a%s %%vnam(%s) %%gd(%s %s) a%s",
                    stoks[0], stoks[3], stoks[1], stoks[2],
                    stoks[4]);
            }
            for (i = 0; i < 5; i++)
                tfree(stoks[i]);
        }
    }
    del_models(modelsfound);

    return newcard;
}



/* do not modify oldcard address, insert everything after first line only */
void pspice_compat_a(struct card *oldcard)
{
    oldcard->nextcard = pspice_compat(oldcard->nextcard);
}


/**** LTSPICE to ngspice **************
 * add functions uplim, dnlim
 * Replace
 * D1 A K SDMOD
 * .MODEL SDMOD D (Roff=1000 Ron=0.7  Rrev=0.2  Vfwd=1  Vrev=10 Revepsilon=0.2
 *         Epsilon=0.2 Ilimit=7 Revilimit=7)
 * by
 * ad1 a k asdmod
 * .model asdmod sidiode(Roff=1000 Ron=0.7  Rrev=0.2  Vfwd=1  Vrev=10
 *         Revepsilon=0.2 Epsilon=0.2 Ilimit=7 Revilimit=7)
 * Remove '.backanno'
 */
/* Replace LTspice/PSpice table=(...) syntax in G and E devices with ngspice
 * native B-source table() function.
 *
 * LTspice format:   Gxxx n+ n- nc+ nc- table=(x0 y0, x1 y1, ...)
 *                   Exxx n+ n- nc+ nc- table=(x0,y0 x1,y1, ...)
 * Each is converted to a single B source using ngspice's table() function:
 *   G → Bxxx n+ n- i = table(V(nc+, nc-), x0, y0, x1, y1, ...)
 *   E → Bxxx n+ n- v = table(V(nc+, nc-), x0, y0, x1, y1, ...)
 */
static void ltspice_table_transform(struct card *startcard)
{
    struct card *card;
    for (card = startcard; card; card = card->nextcard) {
        char *cut_line = card->line;
        if (tolower_c(*cut_line) != 'g' && tolower_c(*cut_line) != 'e')
            continue;

        /* Look for "table" keyword (case-insensitive) followed by "=" */
        char *tablepos = NULL;
        size_t clen = strlen(cut_line);
        for (char *cp = cut_line; cp + 4 < cut_line + clen; cp++) {
            if (tolower_c(cp[0]) == 't' && tolower_c(cp[1]) == 'a' &&
                tolower_c(cp[2]) == 'b' && tolower_c(cp[3]) == 'l' &&
                tolower_c(cp[4]) == 'e') {
                tablepos = cp;
                break;
            }
        }
        if (!tablepos) continue;
        
        char *eq = strchr(tablepos, '=');
        if (!eq) continue;

        /* Skip over "table =" part and optional "{" */
        char *s_data = eq + 1;
        while (*s_data && (isspace_c(*s_data) || *s_data == '{')) s_data++;

        if (*s_data != '(') continue;

        /* Parse the line: title node1 node2 node3 node4 ... */
        char *linecopy = copy(cut_line);
        char *ls = linecopy;

        char *title_tok = gettok(&ls);
        char *node1 = gettok(&ls);
        char *node2 = gettok(&ls);
        char *node3 = gettok(&ls);
        char *node4 = gettok(&ls);

        if (!title_tok || !node1 || !node2 || !node3 || !node4) {
            tfree(title_tok); tfree(node1); tfree(node2);
            tfree(node3); tfree(node4);
            tfree(linecopy);
            continue;
        }

        /* Skip lines where node3 or node4 contain non-node characters */
        if (strpbrk(node3, "{}()=") || strpbrk(node4, "{}()=")) {
            tfree(title_tok); tfree(node1); tfree(node2);
            tfree(node3); tfree(node4);
            tfree(linecopy);
            continue;
        }

        /* Use bracket counting to find the matching ')' for the first '(' */
        char *open_paren = s_data;
        int paren_count = 0;
        char *close_paren = open_paren;
        for (; *close_paren; close_paren++) {
            if (*close_paren == '(') paren_count++;
            else if (*close_paren == ')') {
                paren_count--;
                if (paren_count == 0) break;
            }
        }
        if (!*close_paren || paren_count != 0 || close_paren <= open_paren) {
            tfree(title_tok); tfree(node1); tfree(node2);
            tfree(node3); tfree(node4);
            tfree(linecopy);
            continue;
        }

        size_t datalen = (size_t)(close_paren - open_paren - 1);
        char *data = tmalloc(datalen + 1);
        memcpy(data, open_paren + 1, datalen);
        data[datalen] = '\0';

        /* Check for multiple (...) groups by scanning for unescaped ')'(' */
        int multi_group = 0;
        for (char *p = data; *p; p++) {
            if (*p == ')' && *(p + 1) == '(') { multi_group = 1; break; }
        }
        if (multi_group) {
            tfree(data);
            tfree(title_tok); tfree(node1); tfree(node2);
            tfree(node3); tfree(node4);
            tfree(linecopy);
            continue;
        }

        /* Normalize data: replace commas with spaces */
        for (char *p = data; *p; p++)
            if (*p == ',') *p = ' ';

        /* Build table() argument string: x0, y0, x1, y1, ... */
        char table_args[4096] = "";
        int idx = 0;
        char *d = data;
        char *tok;
        while ((tok = gettok(&d)) != NULL) {
            if (idx > 0)
                strcat(table_args, ", ");
            strcat(table_args, tok);
            tfree(tok);
            idx++;
        }
        tfree(data);

        if (idx < 2) {
            tfree(title_tok); tfree(node1); tfree(node2);
            tfree(node3); tfree(node4);
            tfree(linecopy);
            continue;
        }

        /* Build the G/E source line using "value = table(V(nc+,nc-), ...)" format.
         * This will then be handled by replace_table() in pspice_compat.
         * Format: Gxxx n+ n- value = table(V(nc+, nc-), x1, y1, ...) */
        char *newsrc = tprintf("%s %s %s value = table(V(%s,%s), %s)",
                               title_tok, node1, node2,
                               node3, node4, table_args);

        /* Replace original line with the B-source */
        tfree(card->line);
        card->line = newsrc;

        tfree(title_tok); tfree(node1); tfree(node2);
        tfree(node3); tfree(node4);
        tfree(linecopy);
    }
}


struct card *ltspice_compat(struct card *oldcard)
{
    struct card *card, *newcard, *nextcard;
    struct vsmodels *modelsfound = NULL;
    int skip_control = 0;


    /* remove double braces only if not yet done in pspice_compat() */
    if (!newcompat.ps)
        rem_double_braces(oldcard);

    /* replace LTspice/PSpice table=(...) syntax in G and E devices */
    ltspice_table_transform(oldcard);

    /* handle TABLE functions in G and E sources */
    replace_table(oldcard);

    /* Replace PSpice/LTspice & (logical AND) with ngspice &&, and | (logical OR) with ||.
     * These operators are used in B-source, E-source, .param, .func expressions.
     * Safe: no lines in the library use leading &/| for continuation (only + is used). */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (cl[0] == '*' || cl[0] == '#') continue;  /* comment lines */
        /* Only process lines that likely contain expressions */
        char c0 = tolower_c(cl[0]);
        if (c0 != 'b' && c0 != 'e' && c0 != 'g' && c0 != '.' && c0 != 'h' && c0 != 'f')
            continue;

        size_t len = strlen(cl);
        /* Check if we actually need to modify anything */
        char *amp = strchr(cl, '&');
        char *bar = strchr(cl, '|');
        if (!amp && !bar) continue;

        char *result = tmalloc(len * 2 + 1);
        char *dst = result;
        char *src = cl;
        while (*src) {
            if (*src == '&' && src[1] != '&') {
                *dst++ = '&';
                *dst++ = '&';
                src++;
            } else if (*src == '|' && src[1] != '|') {
                *dst++ = '|';
                *dst++ = '|';
                src++;
            } else {
                *dst++ = *src++;
            }
        }
        *dst = '\0';
        tfree(card->line);
        card->line = result;
    }

    /* Replace LTspice scale notation: a digit after a scale suffix (u/m/k/M/G/T)
     * is treated as fractional part:  2u6 = 2.6e-6, 1m5 = 1.5e-3, 3k3 = 3.3e+3, etc.
     * ngspice needs the decimal point: 2.6u, 1.5m, 3.3k, etc. */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (cl[0] == '*' || cl[0] == '#') continue;
        int has_match = 0;
        char *p = cl;
        while (*p) {
            if (isdigit_c(*p) && *(p+1) && *(p+2)) {
                char scale = *(p+1);
                if (strchr("uUmMkKgGtT", scale) && isdigit_c(*(p+2))) {
                    has_match = 1;
                    break;
                }
            }
            p++;
        }
        if (!has_match) continue;
        size_t len = strlen(cl);
        char *result = tmalloc(len * 2 + 1);
        char *dst = result;
        char *src = cl;
        while (*src) {
            if (isdigit_c(*src) && *(src+1) && *(src+2)) {
                char scale = *(src+1);
                if (strchr("uUmMkKgGtT", scale) && isdigit_c(*(src+2))) {
                    while (isdigit_c(*src)) *dst++ = *src++;
                    char scale_ch = *src++;
                    *dst++ = '.';
                    while (isdigit_c(*src)) *dst++ = *src++;
                    *dst++ = scale_ch;
                    continue;
                }
            }
            *dst++ = *src++;
        }
        *dst = '\0';
        tfree(card->line);
        card->line = result;
    }

    /* UpLim/DnLim are handled by injected .func definitions below (line 2567). */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (cl[0] != 'i' && cl[0] != 'I') continue;
        size_t llen = strlen(cl);
        if (llen < 6) continue;
        if (cl[llen-1] != 'd' || cl[llen-2] != 'a' || cl[llen-3] != 'o' ||
            cl[llen-4] != 'l' || (cl[llen-5] != ' ' && cl[llen-5] != '\t'))
            continue;
        char *ls_copy = copy(cl);
        char *ls = ls_copy;
        char *tok1 = gettok(&ls);   /* instance */
        char *tok2 = gettok(&ls);   /* n+ */
        char *tok3 = gettok(&ls);   /* n- */
        char *tok4 = gettok(&ls);   /* may be "DC"/"AC" or value */
        char *tok5 = gettok(&ls);   /* may be value or "load" */
        if (!tok1 || !tok2 || !tok3 || !tok4) {
            tfree(tok1); tfree(tok2); tfree(tok3); tfree(tok4); tfree(tok5);
            tfree(ls_copy); continue;
        }
        /* Handle optional DC/AC keyword: I1 n+ n- DC <value> load */
        if (strcasecmp(tok4, "DC") == 0 || strcasecmp(tok4, "AC") == 0) {
            tfree(tok4);
            tok4 = tok5;
            tok5 = gettok(&ls);
        }
        if (!tok4 || !tok5) {
            tfree(tok1); tfree(tok2); tfree(tok3); tfree(tok4); tfree(tok5);
            tfree(ls_copy); continue;
        }
        if (strcasecmp(tok5, "load") != 0) {
            tfree(tok1); tfree(tok2); tfree(tok3); tfree(tok4); tfree(tok5);
            tfree(ls_copy); continue;
        }
        char *newline = tprintf("B%s %s %s I = %s / max(V(%s,%s), 1u)",
                                tok1, tok2, tok3, tok4, tok2, tok3);
        tfree(card->line);
        card->line = newline;
        tfree(tok1); tfree(tok2); tfree(tok3); tfree(tok4); tfree(tok5);
        tfree(ls_copy);
    }

    /* Convert table() to pwl() in B-source expressions (ngspice has no table() for B-sources) */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (tolower_c(cl[0]) != 'b') continue;
        for (char *p = cl; *p; p++) {
            if (tolower_c(p[0]) == 't' && tolower_c(p[1]) == 'a' &&
                tolower_c(p[2]) == 'b' && tolower_c(p[3]) == 'l' &&
                tolower_c(p[4]) == 'e' && p[5] == '(') {
                p[0] = 'p'; p[1] = 'w'; p[2] = 'l';
                memmove(p + 3, p + 5, strlen(p + 5) + 1);
            }
        }
    }

    /* Work around ngspice bug: subcircuit names containing hyphens are not
     * properly registered when the subcircuit has parameters (inline or .param).
     * Rename by replacing hyphens with underscores, and update X instance refs.
     * Also move inline default params from .subckt lines to .param cards. */
    {
        /* First pass: collect renames */
        struct {
            char oldname[256];
            char newname[256];
        } renames[64];
        int nrenames = 0;

        for (card = oldcard; card; card = card->nextcard) {
            char *cl = card->line;
            if (!cl) continue;
            if (!ciprefix(".subckt", cl) && !ciprefix(".macro", cl)) continue;

            char linebuf[4096];
            strncpy(linebuf, cl, sizeof(linebuf) - 1);
            linebuf[sizeof(linebuf) - 1] = '\0';

            int nt = 0;
            char *tokens[64];
            char *tok = strtok(linebuf, " \t");
            while (tok != NULL && nt < 64) {
                tokens[nt++] = tok;
                tok = strtok(NULL, " \t");
            }
            if (nt < 3) continue;

            char *subname = tokens[1];

            /* Check if name has hyphens */
            if (!strchr(subname, '-') && !strchr(subname, '~'))
                continue;

            if (nrenames >= 64) continue;

            strncpy(renames[nrenames].oldname, subname, 256);
            renames[nrenames].oldname[255] = '\0';

            /* Build new name: replace hyphens and tildes with underscores */
            int di = 0;
            for (int si = 0; renames[nrenames].oldname[si]; si++) {
                char c = renames[nrenames].oldname[si];
                if (c == '-' || c == '~')
                    c = '_';
                if (di < 255)
                    renames[nrenames].newname[di++] = c;
            }
            renames[nrenames].newname[di] = '\0';
            nrenames++;
        }

        /* Second pass: apply renames to .subckt/.macro lines */
        if (nrenames > 0) {
            for (card = oldcard; card; card = card->nextcard) {
                char *cl = card->line;
                if (!cl) continue;

                /* Check .subckt/.macro lines */
                if (ciprefix(".subckt", cl) || ciprefix(".macro", cl)) {
                    for (int i = 0; i < nrenames; i++) {
                        /* .subckt=7 chars, .macro=6 chars */
                    char *pos = cl + (ciprefix(".subckt", cl) ? 7 : 6);
                    /* skip whitespace */
                        /* skip whitespace */
                        while (*pos == ' ' || *pos == '\t') pos++;
                        /* Check if name matches */
                        size_t nlen = strlen(renames[i].oldname);
                        if (strncmp(pos, renames[i].oldname, nlen) == 0 &&
                            (pos[nlen] == ' ' || pos[nlen] == '\t' || pos[nlen] == '\0')) {
                            /* Replace oldname with newname */
                            char *newcl = tmalloc(strlen(cl) - nlen + strlen(renames[i].newname) + 1);
                            size_t prefix_len = pos - cl;
                            memcpy(newcl, cl, prefix_len);
                            strcpy(newcl + prefix_len, renames[i].newname);
                            strcat(newcl, pos + nlen);
                            tfree(card->line);
                            card->line = newcl;
                            break;
                        }
                    }
                }

                /* Check X instance lines */
                if (tolower_c(cl[0]) == 'x') {
                    for (int i = 0; i < nrenames; i++) {
                        /* Look for the subcircuit name as the last token */
                        char *last = cl + strlen(cl);
                        while (last > cl && (last[-1] == ' ' || last[-1] == '\t'))
                            last--;
                        char *start = last;
                        while (start > cl && start[-1] != ' ' && start[-1] != '\t')
                            start--;
                        size_t nlen = strlen(renames[i].oldname);
                        if ((size_t)(last - start) == nlen &&
                            strncmp(start, renames[i].oldname, nlen) == 0) {
                            char *newcl = tmalloc(strlen(cl) - nlen + strlen(renames[i].newname) + 1);
                            size_t prefix_len = start - cl;
                            memcpy(newcl, cl, prefix_len);
                            strcpy(newcl + prefix_len, renames[i].newname);
                            strcpy(newcl + prefix_len + strlen(renames[i].newname), last);
                            tfree(card->line);
                            card->line = newcl;
                            break;
                        }
                    }
                }
            }
        }
    }

    /* Move inline default params from .subckt/.macro lines to .param cards.
     * This avoids issues with ngspice's handling of inline params. */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (!ciprefix(".subckt", cl) && !ciprefix(".macro", cl)) continue;

        char linebuf[4096];
        strncpy(linebuf, cl, sizeof(linebuf) - 1);
        linebuf[sizeof(linebuf) - 1] = '\0';

        int nt = 0;
        char *tokens[64];
        char *tok = strtok(linebuf, " \t");
        while (tok != NULL && nt < 64) {
            tokens[nt++] = tok;
            tok = strtok(NULL, " \t");
        }
        if (nt < 3) continue;

        int first_param = -1;
        for (int i = 2; i < nt; i++) {
            if (strchr(tokens[i], '=')) {
                first_param = i;
                break;
            }
        }
        if (first_param < 0) continue;

        size_t newsize = strlen(tokens[0]) + 1 + strlen(tokens[1]) + 1;
        for (int i = 2; i < first_param; i++)
            newsize += strlen(tokens[i]) + 1;
        char *newsub = tmalloc(newsize);
        newsub[0] = '\0';
        strcat(newsub, tokens[0]);
        strcat(newsub, " ");
        strcat(newsub, tokens[1]);
        for (int i = 2; i < first_param; i++) {
            strcat(newsub, " ");
            strcat(newsub, tokens[i]);
        }

        size_t paramsize = 7;
        for (int i = first_param; i < nt; i++)
            paramsize += strlen(tokens[i]) + 1;
        char *paramline = tmalloc(paramsize);
        strcpy(paramline, ".param ");
        for (int i = first_param; i < nt; i++) {
            if (i > first_param) strcat(paramline, " ");
            strcat(paramline, tokens[i]);
        }

        tfree(card->line);
        card->line = newsub;

        struct card *newcard = insert_new_line(card, paramline, 0,
                                               card->linenum_orig, card->linesource);
        (void)newcard;
    }

    /* Strip LTspice-only ilimit=, Vser= parameters from SW models */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (cl[0] != '.' || !ciprefix(".model", cl)) continue;
        char *modline = cl + 6;
        while (*modline == ' ' || *modline == '\t') modline++;
        while (*modline && *modline != ' ' && *modline != '\t') modline++;
        while (*modline == ' ' || *modline == '\t') modline++;
        if (!modline[0]) continue;
        if (tolower_c(modline[0]) != 's' || tolower_c(modline[1]) != 'w') continue;
        char *result = tmalloc(strlen(cl) + 1);
        char *dst = result;
        char *paren = strchr(modline, '(');
        if (paren) {
            /* Parenthesized format: .model XXX SW(...) */
            char *src = cl;
            while (src < paren) *dst++ = *src++;
            *dst++ = '(';
            src = paren + 1;
            int depth = 1;
            while (*src && depth > 0) {
                if (*src == '(') depth++;
                else if (*src == ')') depth--;
                if (depth == 0) break;
                if ((tolower_c(src[0]) == 'i' &&
                     tolower_c(src[1]) == 'l' &&
                     tolower_c(src[2]) == 'i' &&
                     tolower_c(src[3]) == 'm' &&
                     tolower_c(src[4]) == 'i' &&
                     tolower_c(src[5]) == 't' &&
                     src[6] == '=') ||
                    (tolower_c(src[0]) == 'v' &&
                     tolower_c(src[1]) == 's' &&
                     tolower_c(src[2]) == 'e' &&
                     tolower_c(src[3]) == 'r' &&
                     src[4] == '=')) {
                    int kwlen = (tolower_c(src[0]) == 'i') ? 7 : 5;
                    src += kwlen;
                    while (*src && *src != ' ' && *src != '\t' && *src != ')' && *src != ',') src++;
                    if (*src == ',') { src++; }
                    while (*src == ' ' || *src == '\t') src++;
                    continue;
                }
                *dst++ = *src++;
            }
            while (*src) *dst++ = *src++;
        } else {
            /* Space-separated format: .model XXX SW ... */
            char *src = cl;
            while (*src) {
                if ((tolower_c(src[0]) == 'i' &&
                     tolower_c(src[1]) == 'l' &&
                     tolower_c(src[2]) == 'i' &&
                     tolower_c(src[3]) == 'm' &&
                     tolower_c(src[4]) == 'i' &&
                     tolower_c(src[5]) == 't' &&
                     src[6] == '=') ||
                    (tolower_c(src[0]) == 'v' &&
                     tolower_c(src[1]) == 's' &&
                     tolower_c(src[2]) == 'e' &&
                     tolower_c(src[3]) == 'r' &&
                     src[4] == '=')) {
                    int kwlen = (tolower_c(src[0]) == 'i') ? 7 : 5;
                    src += kwlen;
                    while (*src && *src != ' ' && *src != '\t')
                        src++;
                    while (*src == ' ' || *src == '\t') src++;
                    continue;
                }
                *dst++ = *src++;
            }
        }
        *dst = '\0';
        tfree(card->line);
        card->line = result;
    }

    /* Strip LTspice-only Rpar=, Cpar=, tripdt=, tripdv= from B-sources */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (cl[0] != 'b' && cl[0] != 'B') continue;
        if (cl[1] == '.' || cl[1] == '\0') continue;
        int any_stripped = 0;
        char *result = tmalloc(strlen(cl) + 1);
        char *dst = result;
        char *src = cl;
        while (*src) {
            if ((tolower_c(src[0]) == 'r' && tolower_c(src[1]) == 'p' &&
                 tolower_c(src[2]) == 'a' && tolower_c(src[3]) == 'r' && src[4] == '=') ||
                (tolower_c(src[0]) == 'c' && tolower_c(src[1]) == 'p' &&
                 tolower_c(src[2]) == 'a' && tolower_c(src[3]) == 'r' && src[4] == '=') ||
                (tolower_c(src[0]) == 't' && tolower_c(src[1]) == 'r' &&
                 tolower_c(src[2]) == 'i' && tolower_c(src[3]) == 'p' &&
                 tolower_c(src[4]) == 'd' && tolower_c(src[5]) == 't' && src[6] == '=') ||
                (tolower_c(src[0]) == 't' && tolower_c(src[1]) == 'r' &&
                 tolower_c(src[2]) == 'i' && tolower_c(src[3]) == 'p' &&
                 tolower_c(src[4]) == 'd' && tolower_c(src[5]) == 'v' && src[6] == '=')) {
                int kwlen = (tolower_c(src[0]) == 't') ? 7 : 5;
                src += kwlen;
                while (*src && *src != ' ' && *src != '\t' && *src != ';')
                    src++;
                while (*src == ' ' || *src == '\t') src++;
                any_stripped = 1;
                continue;
            }
            *dst++ = *src++;
        }
        *dst = '\0';
        if (any_stripped) {
            tfree(card->line);
            card->line = result;
        } else {
            tfree(result);
        }
    }

    /* Fix LTspice inv(expr) → (!(expr)) in B-source expressions.
     * inv() is logical NOT used in some LTspice subcircuits (e.g. MC33063). */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (cl[0] != 'b' && cl[0] != 'B') continue;
        if (cl[1] == '.' || cl[1] == '\0') continue;
        char *s = cl;
        /* Find each inv( in the line */
        while ((s = strstr(s, "inv(")) != NULL) {
            /* Track depth to find the matching ')' */
            int depth = 1;
            char *end = s + 4; /* after 'inv(' */
            for (; *end && depth > 0; end++) {
                if (*end == '(') depth++;
                else if (*end == ')') depth--;
            }
            if (depth != 0) break; /* malformed, stop searching */
            end--; /* point to the matching ')' */
            /* Now rewrite: inv( ... ) → (!( ... )) */
            size_t len = strlen(cl);
            char *result = tmalloc(len + 4); /* add up to 3 chars: '(!' + ')' */
            /* prefix before 'inv' */
            size_t pre = (size_t)(s - cl);
            memcpy(result, cl, pre);
            char *d = result + pre;
            *d++ = '('; *d++ = '!'; *d++ = '('; /* "!(" */
            /* inner content */
            size_t inner = (size_t)(end - (s + 4));
            memcpy(d, s + 4, inner);
            d += inner;
            *d++ = ')'; *d++ = ')'; /* "))" */
            /* suffix after original ')' */
            size_t suf = strlen(end + 1);
            memcpy(d, end + 1, suf + 1);
            tfree(card->line);
            card->line = result;
            s = result + pre + 1; /* continue scanning from after '!(' */
            cl = result;
        }
    }

    /* Strip LTspice-only laplace keyword from R-element lines */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (cl[0] != 'r' && cl[0] != 'R') continue;
        char *la = strstr(cl, "laplace");
        if (!la) continue;
        if (la > cl && !isspace_c(la[-1])) continue;
        /* Truncate at the laplace keyword */
        *la = '\0';
    }

    /* Convert LTspice TC= value1 (value2) to tc1=value1 tc2=value2 on R/L/C lines.
     * LTspice uses TC= tc1 (tc2) syntax; ngspice uses tc1= tc2= separately. */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        char c0 = cl[0];
        if (c0 != 'r' && c0 != 'R' && c0 != 'c' && c0 != 'C' &&
            c0 != 'l' && c0 != 'L') continue;
        if (cl[1] == '.' || cl[1] == '\0') continue;
        char *tc = strstr(cl, "tc=");
        if (!tc) tc = strstr(cl, "TC=");
        if (!tc) continue;
        /* Must be a standalone keyword (preceded by space/tab) */
        if (tc > cl && tc[-1] != ' ' && tc[-1] != '\t') continue;
        /* tc= value1 (value2) or tc=value1,value2 — extract value1 and optional value2.
         * LTspice accepts: TC=2.34M (-4.57U), TC=2.34M,-4.57U, TC=2.34M -4.57U */
        char *val = tc + 3;
        while (*val == ' ' || *val == '\t') val++;
        if (!*val || *val == ';') continue;
        char *val1_start = val;
        char *val1_end = val;
        while (*val1_end && *val1_end != ' ' && *val1_end != '\t' && *val1_end != ';' &&
               *val1_end != '(' && *val1_end != ',') val1_end++;
        if (val1_end == val1_start) continue;
        int has_comma = 0;
        char *val2_start = val1_end;
        while (*val2_start == ' ' || *val2_start == '\t') val2_start++;
        if (*val2_start == ',') {
            has_comma = 1;
            val2_start++;
            while (*val2_start == ' ' || *val2_start == '\t') val2_start++;
        }
        int has_parens = 0;
        if (*val2_start == '(') {
            has_parens = 1;
            val2_start++;
            while (*val2_start == ' ' || *val2_start == '\t') val2_start++;
        }
        char *val2_end = val2_start;
        if (*val2_end) {
            while (*val2_end && *val2_end != ' ' && *val2_end != '\t' && *val2_end != ')' &&
                   *val2_end != ';' && *val2_end != ',') val2_end++;
        }
        int has_val2 = (val2_end > val2_start);
        /* Build new line replacing TC= with tc1= tc2= */
        size_t len = strlen(cl);
        char *result = tmalloc(len + 64);
        /* Prefix up to 'tc=' */
        size_t pre = (size_t)(tc - cl);
        memcpy(result, cl, pre);
        char *d = result + pre;
        /* Write tc1=value1 */
        d += sprintf(d, "tc1=%.*s", (int)(val1_end - val1_start), val1_start);
        /* Optional tc2=value2 */
        if (has_val2)
            d += sprintf(d, " tc2=%.*s", (int)(val2_end - val2_start), val2_start);
        /* Suffix: what comes after the TC= value area */
        const char *suf_start;
        if (has_val2) {
            if (has_parens) {
                /* Skip past any trailing ')' after val2 */
                suf_start = val2_end;
                while (*suf_start == ' ' || *suf_start == '\t') suf_start++;
                if (*suf_start == ')') suf_start++;
            } else {
                suf_start = val2_end;
            }
        } else {
            suf_start = val1_end;
        }
        size_t suf = strlen(suf_start);
        memcpy(d, suf_start, suf + 1);
        tfree(card->line);
        card->line = result;
    }

    /* Add braces around unbraced R=value, L=value, C=value expressions.
     * LTspice allows R=expr without braces, but ngspice requires R={expr}. */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        char c0 = cl[0];
        if (c0 != 'r' && c0 != 'R' && c0 != 'c' && c0 != 'C' &&
            c0 != 'l' && c0 != 'L') continue;
        if (cl[1] == '.' || cl[1] == '\0') continue;
        /* Check if the line has R=, L=, or C= followed by an expression.
         * Must be a standalone keyword (preceded by space/tab or start-of-line),
         * not part of a longer keyword like TC= or VC=. */
        char *eq = strchr(cl, '=');
        if (!eq) continue;
        if (eq == cl) continue;  /* = at start of line */
        char key_char = tolower_c(*(eq - 1));
        if (key_char != 'r' && key_char != 'l' && key_char != 'c') continue;
        /* Ensure the key is preceded by a separator (space, tab, or start of value area),
         * not by another letter (which would mean it's part of a multi-letter keyword). */
        if (eq > cl + 1) {
            char prev = *(eq - 2);
            if (prev != ' ' && prev != '\t' && prev != '(' && prev != ',')
                continue;
        }
        char *val = eq + 1;
        /* Skip whitespace after = */
        while (*val == ' ' || *val == '\t') val++;
        /* Already braced or quoted? */
        if (*val == '{' || *val == '\'') continue;
        /* Check if it looks like an expression (contains operators or functions) */
        int is_expr = 0;
        for (char *p = val; *p && *p != ' ' && *p != '\t' && *p != ';'; p++) {
            if (*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '(') {
                is_expr = 1;
                break;
            }
        }
        if (!is_expr) continue;
        /* Find end of value */
        char *vend = val;
        while (*vend && *vend != ' ' && *vend != '\t' && *vend != ';')
            vend++;
        /* Build new line with braces around value */
        size_t prefix_len = val - cl;
        size_t suffix_len = strlen(vend);
        char *result = tmalloc(prefix_len + 3 + (vend - val) + suffix_len + 1);
        memcpy(result, cl, prefix_len);
        result[prefix_len] = '{';
        memcpy(result + prefix_len + 1, val, vend - val);
        result[prefix_len + 1 + (vend - val)] = '}';
        memcpy(result + prefix_len + 2 + (vend - val), vend, suffix_len + 1);
        tfree(card->line);
        card->line = result;
    }

    /* Strip LTspice-only Rpar=, Rser=, Lser=, Cpar= from passive C/L element lines */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        char c0 = cl[0];
        if (c0 != 'c' && c0 != 'C' && c0 != 'l' && c0 != 'L') continue;
        if (cl[1] == '.' || cl[1] == '\0') continue;
        int any_stripped = 0;
        char *result = tmalloc(strlen(cl) + 1);
        char *dst = result;
        char *src = cl;
        while (*src) {
            if ((tolower_c(src[0]) == 'r' && tolower_c(src[1]) == 'p' &&
                 tolower_c(src[2]) == 'a' && tolower_c(src[3]) == 'r' && src[4] == '=') ||
                (tolower_c(src[0]) == 'r' && tolower_c(src[1]) == 's' &&
                 tolower_c(src[2]) == 'e' && tolower_c(src[3]) == 'r' && src[4] == '=') ||
                (tolower_c(src[0]) == 'l' && tolower_c(src[1]) == 's' &&
                 tolower_c(src[2]) == 'e' && tolower_c(src[3]) == 'r' && src[4] == '=') ||
                (tolower_c(src[0]) == 'c' && tolower_c(src[1]) == 'p' &&
                 tolower_c(src[2]) == 'a' && tolower_c(src[3]) == 'r' && src[4] == '=')) {
                src += 5;
                while (*src && *src != ' ' && *src != '\t' && *src != ';')
                    src++;
                while (*src == ' ' || *src == '\t') src++;
                any_stripped = 1;
                continue;
            }
            *dst++ = *src++;
        }
        *dst = '\0';
        if (any_stripped) {
            tfree(card->line);
            card->line = result;
        } else {
            tfree(result);
        }
    }

    /* Convert LTspice TBL() on I-sources to B-source pwl() */
    /* Format: Ixxx n+ n- TBL(x0 y0 x1 y1 ...) → Bxxx n+ n- I=pwl(V(n+,n-), x0, y0, x1, y1, ...) */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (cl[0] != 'i' && cl[0] != 'I') continue;
        if (cl[1] == '.' || cl[1] == '\0') continue;
        char *tbl = strstr(cl, "tbl(");
        if (!tbl) tbl = strstr(cl, "TBL(");
        if (!tbl) tbl = strstr(cl, "Tbl(");
        if (!tbl) continue;
        /* Extract instance name and two nodes */
        char *copy_ls = copy(cl);
        char *ls = copy_ls;
        char *iname = gettok(&ls);
        char *nplus = gettok(&ls);
        char *nminus = gettok(&ls);
        if (iname && nplus && nminus) {
            /* Extract TBL arguments: everything between TBL( and the trailing ) */
            char *args = tbl + 4;
            char *end = args + strlen(args) - 1;
            while (end > args && *end != ')') end--;
            if (*end == ')') *end = '\0';
            /* Strip leading/trailing whitespace */
            while (*args == ' ' || *args == '\t') args++;
            char *arg_end = args + strlen(args) - 1;
            while (arg_end > args && (*arg_end == ' ' || *arg_end == '\t')) arg_end--;
            *(arg_end + 1) = '\0';
            /* Convert spaces to commas for pwl() syntax */
            for (char *p = args; *p; p++)
                if (*p == ' ' || *p == '\t') *p = ',';
            char *new_line = tprintf("B%s %s %s I=pwl(V(%s,%s), %s)",
                                     iname, nplus, nminus, nplus, nminus, args);
            tfree(card->line);
            card->line = new_line;
        }
        tfree(copy_ls);
        tfree(iname);
        tfree(nplus);
        tfree(nminus);
    }

    /* Replace LTspice OTA A-device with a B-source current source */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (cl[0] != 'a' && cl[0] != 'A') continue;
        char *ls_copy = copy(cl);
        char *ls = ls_copy;
        char *tok0 = gettok(&ls);        /* instance name */
        char *pin[8];
        int npin;
        for (npin = 0; npin < 8; npin++)
            pin[npin] = gettok(&ls);
        char *devtype = gettok(&ls);
        if (!tok0 || !pin[7] || !devtype) {
            tfree(tok0);
            for (int i = 0; i < npin; i++) tfree(pin[i]);
            tfree(devtype);
            tfree(ls_copy);
            continue;
        }
        int is_ota = (tolower_c(devtype[0]) == 'o' && tolower_c(devtype[1]) == 't' &&
                      tolower_c(devtype[2]) == 'a' && devtype[3] == '\0');
        if (!is_ota) {
            tfree(tok0);
            for (int i = 0; i < 8; i++) tfree(pin[i]);
            tfree(devtype);
            tfree(ls_copy);
            continue;
        }
        /* Parse parameters after OTA keyword (remaining text in ls) */
        char *out_pin = pin[6];   /* pin 7 = Iout output */
        char *in_p = pin[0];      /* pin 1 = In+ */
        char *in_m = pin[1];      /* pin 2 = In- */
        char *g_val = NULL, *iout_val = NULL;
        char *ref_val = NULL, *vhigh_val = NULL, *vlow_val = NULL;
        while (*ls == ' ' || *ls == '\t') ls++;
        if (*ls) {
            char *p_copy = copy(ls);
            char *tok;
            char *pp = p_copy;
            while ((tok = gettok(&pp)) != NULL) {
                if (ciprefix("g=", tok)) {
                    tfree(g_val);
                    g_val = copy(tok + 2);
                } else if (ciprefix("iout=", tok)) {
                    tfree(iout_val);
                    iout_val = copy(tok + 5);
                } else if (ciprefix("ref=", tok)) {
                    tfree(ref_val);
                    ref_val = copy(tok + 4);
                } else if (ciprefix("vhigh=", tok)) {
                    tfree(vhigh_val);
                    vhigh_val = copy(tok + 6);
                } else if (ciprefix("vlow=", tok)) {
                    tfree(vlow_val);
                    vlow_val = copy(tok + 5);
                }
                tfree(tok);
            }
            tfree(p_copy);
        }
        if (!g_val) g_val = copy("1");
        if (!iout_val) iout_val = copy("10u");
        if (strcmp(out_pin, "0") != 0) {
            /* Build inner: G * (V(in+) - V(in-) [- Ref]) */
            char *expr;
            if (ref_val)
                expr = tprintf("(%s * (V(%s) - V(%s) - %s))", g_val, in_p, in_m, ref_val);
            else
                expr = tprintf("(%s * (V(%s) - V(%s)))", g_val, in_p, in_m);
            /* Current clamp: min(max(expr, -Iout), Iout) */
            char *clamped = tprintf("min(max(%s, -%s), %s)", expr, iout_val, iout_val);
            tfree(expr);
            /* Voltage compliance: conductance-based soft clamp */
            char *final_expr;
            if (vlow_val && vhigh_val) {
                final_expr = tprintf("(%s) + (V(%s) - %s)*(V(%s) > %s) + (V(%s) - %s)*(V(%s) < %s)",
                                      clamped, out_pin, vhigh_val, out_pin, vhigh_val,
                                      out_pin, vlow_val, out_pin, vlow_val);
            } else if (vlow_val) {
                final_expr = tprintf("(%s) + (V(%s) - %s)*(V(%s) < %s)",
                                      clamped, out_pin, vlow_val, out_pin, vlow_val);
            } else if (vhigh_val) {
                final_expr = tprintf("(%s) + (V(%s) - %s)*(V(%s) > %s)",
                                      clamped, out_pin, vhigh_val, out_pin, vhigh_val);
            } else {
                final_expr = clamped;
                clamped = NULL;
            }
            tfree(clamped);
            char *newline = tprintf("B%s_ota %s 0 I = %s", tok0, out_pin, final_expr);
            tfree(final_expr);
            /* Comment out A1 line and add B-source after */
            {
                size_t llen = strlen(cl);
                char *comment = tmalloc(llen + 2);
                comment[0] = '*';
                memcpy(comment + 1, cl, llen + 1);
                tfree(card->line);
                card->line = comment;
            }
            insert_new_line(card, newline, card->linenum + 1,
                            card->linenum_orig, card->linesource);
        } else {
            size_t llen = strlen(cl);
            char *newline = tmalloc(llen + 2);
            newline[0] = '*';
            memcpy(newline + 1, cl, llen + 1);
            tfree(card->line);
            card->line = newline;
        }
        tfree(g_val);
        tfree(iout_val);
        tfree(ref_val);
        tfree(vhigh_val);
        tfree(vlow_val);
        tfree(tok0);
        for (int i = 0; i < 8; i++) tfree(pin[i]);
        tfree(devtype);
        tfree(ls_copy);
    }

    /* Lowercase UIC in .tran lines (ngspice 45+ rejects uppercase UIC)
     * Also convert .tran 0 tstop → .tran 1u tstop (ngspice requires tstep > 0) */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (!ciprefix(".tran", cl)) continue;
        char *uic = strstr(cl, "UIC");
        while (uic) {
            uic[0] = 'u'; uic[1] = 'i'; uic[2] = 'c';
            uic = strstr(uic + 3, "UIC");
        }
        /* Fix tstep=0: replace leading ... 0 with ... 1u after .tran */
        char *tstep = cl + 5;
        while (*tstep == ' ' || *tstep == '\t') tstep++;
        if (tstep[0] == '0' && (tstep[1] == ' ' || tstep[1] == '\t' || tstep[1] == '\0')) {
            /* Single "0" as tstep — change to 1u (ngspice requires tstep > 0) */
            size_t rest = strlen(tstep + 1);
            memmove(tstep + 2, tstep + 1, rest + 1);
            tstep[0] = '1';
            tstep[1] = 'u';
        }
    }

    /* Convert LTspice IF(cond, tval, fval) to standard (...)?(...):(...) */
    /* Handles IF() in E/G VALUE={} expressions, B-source expressions, params.
     * Iterates to handle nested IF() calls (inner ones become exposed after
     * outer ones are converted to ternary). */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (cl[0] == '*' || cl[0] == '\0') continue;
        while (1) {
            char *newl = replace_if_ternary(cl);
            if (!newl) break;
            if (newl != cl) {
                tfree(card->line);
                card->line = newl;
                cl = newl;
            }
        }
    }

    /* add funcs uplim, dnlim to beginning of deck */
    char *new_str =
            copy(".func uplim(x, pos, z) { min(x, pos - z) + (1 - "
                 "(min(max(0, x - pos + z), 2 * z) / 2 / z - 1)**2)*z }");
    newcard = insert_new_line(NULL, new_str, 1, 0, "internal");
    new_str = copy(".func dnlim(x, neg, z) { max(x, neg + z) - (1 - "
                   "(min(max(0, -x + neg + z), 2 * z) / 2 / z - 1)**2)*z }");
    nextcard = insert_new_line(newcard, new_str, 2, 0, "internal");
    new_str = copy(".func uplim_tanh(x, pos, z) { min(x, pos - z) + "
                   "tanh(max(0, x - pos + z) / z)*z }");
    nextcard = insert_new_line(nextcard, new_str, 3, 0, "internal");
    new_str = copy(".func dnlim_tanh(x, neg, z) { max(x, neg + z) - "
                   "tanh(max(0, neg + z - x) / z)*z }");
    nextcard = insert_new_line(nextcard, new_str, 4, 0, "internal");

    /* Inject .param pi=... so {PI} in subcircuit expressions resolves */
    new_str = copy(".param pi=3.141592653589793");
    nextcard = insert_new_line(nextcard, new_str, 5, 0, "internal");
    /* Inject .param temp = 'temper' so that {temp} (LTspice built-in global)
     * resolves to the simulation temperature in Celsius. */
    new_str = copy(".param temp = 'temper'");
    nextcard = insert_new_line(nextcard, new_str, 6, 0, "internal");
    /* Inject .param vt = ... (thermal voltage) for convenience */
    new_str = copy(".param vt = '(temper + 273.15) * 8.6173303e-5'");
    nextcard = insert_new_line(nextcard, new_str, 7, 0, "internal");
    nextcard->nextcard = oldcard;

    /* Inject .param temp and vt inside each subcircuit for scoped access */
    for (card = oldcard; card; card = card->nextcard) {
        if (ciprefix(".subckt", card->line)) {
            char *s1 = copy(".param temp = 'temper'");
            insert_new_line(card, s1, 0, card->linenum_orig, card->linesource);
            char *s2 = copy(".param vt = '(temper + 273.15) * 8.6173303e-5'");
            insert_new_line(card->nextcard, s2, 1, card->linenum_orig, card->linesource);
        }
    }

    /* remove .backanno, replace 'noiseless' by 'moisy=0' */
    for (card = nextcard; card; card = card->nextcard) {
        char* cut_line = card->line;
        if (ciprefix(".backanno", cut_line)) {
            *cut_line = '*';
        }
        else if (*cut_line == 'r') {
            char* noi = strstr(cut_line, "noiseless");
            /* only if 'noiseless' is an unconnected token */
            if (noi && isspace_c(noi[-1]) && (isspace_c(noi[9]) || !isprint_c(noi[9]))) {
                memcpy(noi, "noisy=0  ", 9);
            }
        }
    }

    /* replace
   * D1 A K SDMOD
   * .MODEL SDMOD D (Roff=1000 Ron=0.7  Rrev=0.2  Vfwd=1  Vrev=10
   *          Revepsilon=0.2 Epsilon=0.2 Ilimit=7 Revilimit=7)
   * by
   * a1 a k SDMOD
   * .model SDMOD sidiode(Roff=1000 Ron=0.7  Rrev=0.2  Vfwd=1  Vrev=10
   *            Revepsilon=0.2 Epsilon=0.2 Ilimit=7 Revilimit=7)
   * Do this if one of the parameters, which are uncommon to standard diode
   * model, has been found.

   * simple hierachy, as nested subcircuits are not allowed in PSPICE */

    /* first scan: find the d models, transform them and put them into a list
     */
    for (card = nextcard; card; card = card->nextcard) {
        char *str;
        static struct card *subcktline = NULL;
        static int nesting = 0;
        char *cut_line = card->line;
        if (*cut_line == '*' || *cut_line == '\0')
            continue;
        else if (ciprefix(".subckt", cut_line)) {
            subcktline = card;
            nesting++;
        }
        else if (ciprefix(".ends", cut_line))
            nesting--;

        else if (ciprefix(".model", card->line) &&
                search_plain_identifier(card->line, "d")) {
            /* Case-insensitive matching: lowercase a copy of the line */
            char *lower_line = copy(card->line);
            if (lower_line) {
                char *lp;
                for (lp = lower_line; *lp; lp++)
                    *lp = (char)tolower_c(*lp);
            }
            int has_ltspice_params = lower_line && (
                    search_plain_identifier(lower_line, "roff") ||
                    search_plain_identifier(lower_line, "ron") ||
                    search_plain_identifier(lower_line, "rrev") ||
                    search_plain_identifier(lower_line, "vfwd") ||
                    search_plain_identifier(lower_line, "vrev") ||
                    search_plain_identifier(lower_line, "revepsilon") ||
                    search_plain_identifier(lower_line, "epsilon") ||
                    search_plain_identifier(lower_line, "revilimit") ||
                    search_plain_identifier(lower_line, "ilimit"));
            tfree(lower_line);
            if (has_ltspice_params) {
                char *modname;

                /* remove parameter 'noiseless' (the model is noiseless anyway) */
                char *nonoise = search_plain_identifier(card->line, "noiseless");
                if (nonoise) {
                    size_t iii;
                    for (iii = 0; iii < 9; iii++)
                        nonoise[iii] = ' ';
                }
                card->line = str = inp_remove_ws(card->line);
                str = nexttok(str); /* throw away '.model' */
                INPgetNetTok(&str, &modname, 0); /* model name */
                if (!ciprefix("d", str)) {
                    tfree(modname);
                    continue;
                }
                /* skip d */
                str++;
                /* we take all the existing parameters */
                char *newstr = copy(str);
                tfree(card->line);
                card->line = tprintf(".model a%s sidiode%s", modname, newstr);
                if (nesting > 0)
                    modelsfound = insert_new_model(
                            modelsfound, modname, NULL, NULL, subcktline->line);
                else
                    modelsfound =
                            insert_new_model(modelsfound, modname, NULL, NULL, "top");
                tfree(modname);
                tfree(newstr);
            }
        }
        else
            continue;
    }

    /* no need to continue if no d is found */
    if (!modelsfound)
        return newcard;

    /* second scan: find the diode instances d calling a simple diode model
     * and transform them. Also build a list of renamed devices. */
    struct vsmodels *renamed_devices = NULL;
    for (card = nextcard; card; card = card->nextcard) {
        static struct card *subcktline = NULL;
        static int nesting = 0;
        char *cut_line = card->line;
        if (*cut_line == '*')
            continue;
        if (*cut_line == '\0')
            continue;
        // exclude any command inside .control ... .endc
        if (ciprefix(".control", cut_line)) {
            skip_control++;
            continue;
        }
        else if (ciprefix(".endc", cut_line)) {
            skip_control--;
            continue;
        }
        else if (skip_control > 0) {
            continue;
        }
        if (ciprefix(".subckt", cut_line)) {
            subcktline = card;
            nesting++;
        }
        if (ciprefix(".ends", cut_line))
            nesting--;

        if (ciprefix("d", cut_line)) {
            /* check for the model name */
            int i;
            char *stoks[4];
            for (i = 0; i < 4; i++) {
                stoks[i] = gettok_node(&cut_line);
                if (stoks[i] == NULL) {
                    fprintf(stderr, "Error in line %d: buggy diode instance line\n    %s\n", card->linenum_orig, card->linesource);
                    fprintf(stderr, "At least 'Dxx n1 n2 d' is required.\n");
                    controlled_exit(EXIT_BAD);
                }
            }
            /* rewrite d line and replace it if a model is found */
            if ((nesting > 0) &&
                    find_a_model(modelsfound, stoks[3], subcktline->line)) {
                tfree(card->line);
                card->line = tprintf("a%s %s %s a%s",
                    stoks[0], stoks[1], stoks[2], stoks[3]);
                renamed_devices = insert_new_model(renamed_devices, stoks[0], NULL, NULL, "");
            }
            /* if model is not within same subcircuit, search at top level */
            else if (find_a_model(modelsfound, stoks[3], "top")) {
                tfree(card->line);
                card->line = tprintf("a%s %s %s a%s",
                        stoks[0], stoks[1], stoks[2], stoks[3]);
                renamed_devices = insert_new_model(renamed_devices, stoks[0], NULL, NULL, "");
            }
            for (i = 0; i < 4; i++)
                tfree(stoks[i]);
        }
    }

    /* third scan: update I(Dxx) references in B-source and other cards
     * to I(adxx) so that inp_meas_current() can find the renamed devices.
     * Use lowercase i(...) because inp_modify_exp() only recognizes lowercase
     * 'i' for the I() current-sensing function pattern. */
    if (renamed_devices) {
        struct vsmodels *rd;
        for (rd = renamed_devices; rd; rd = rd->nextmodel) {
            char *oldname = rd->modelname;
            size_t oldlen = strlen(oldname);
            char *findpat = tprintf("i(%s)", oldname);
            char *findpat_upper = tprintf("I(%s)", oldname);
            for (card = nextcard; card; card = card->nextcard) {
                char *cut_line = card->line;
                if (!cut_line || *cut_line == '*' || *cut_line == '\0')
                    continue;
                if (strstr(cut_line, findpat) || strstr(cut_line, findpat_upper)) {
                    char *newline = tmalloc(strlen(cut_line) + oldlen + 4);
                    char *dst = newline;
                    char *src = cut_line;
                    while (*src) {
                        if ((src[0] == 'I' || src[0] == 'i') &&
                            src[1] == '(' &&
                            strncmp(src + 2, oldname, oldlen) == 0 &&
                            src[2 + oldlen] == ')') {
                            memcpy(dst, "i(a", 3);
                            dst += 3;
                            memcpy(dst, oldname, oldlen);
                            dst += oldlen;
                            *dst++ = ')';
                            src += 3 + oldlen;
                        } else {
                            *dst++ = *src++;
                        }
                    }
                    *dst = '\0';
                    tfree(card->line);
                    card->line = newline;
                }
            }
            tfree(findpat);
            tfree(findpat_upper);
        }
        del_models(renamed_devices);
    }

    del_models(modelsfound);

    return newcard;
}

/* Forward declaration */
static void ltspice_a_device_transform(struct card *oldcard);

/* Trim leading/trailing whitespace in-place */
static void trim_ws(char **s)
{
    char *p = *s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != *s) {
        size_t n = strlen(p);
        memmove(*s, p, n + 1);
    }
    size_t l = strlen(*s);
    while (l > 0 && ((*s)[l - 1] == ' ' || (*s)[l - 1] == '\t'))
        (*s)[--l] = '\0';
}

/* Add pull-down resistors to A-device DAC bridge output nodes that may float */
static void add_dac_pulldowns(struct card *start)
{
    struct card *card;
    for (card = start; card; card = card->nextcard) {
        if (!card->line) continue;
        if (card->line[0] != 'a') continue;
        char *model = strrchr(card->line, ' ');
        if (!model) continue;
        model++;
        /* Match model names ending in _dac */
        size_t mlen = strlen(model);
        if (mlen < 4) continue;
        if (strcmp(model + mlen - 4, "_dac") != 0)
            continue;

        /* Find the analog output port [...] */
        char *p = card->line;
        char *last_open = NULL;
        while ((p = strchr(p, '[')) != NULL) {
            last_open = p;
            p++;
        }
        if (!last_open) continue;
        char *close_bracket = strchr(last_open, ']');
        if (!close_bracket) continue;

        /* Extract nodes inside last [...] */
        char *nodes = tmalloc((size_t)(close_bracket - last_open));
        memcpy(nodes, last_open + 1, (size_t)(close_bracket - last_open - 1));
        nodes[close_bracket - last_open - 1] = '\0';
        trim_ws(&nodes);

        /* Extract the instance name (first token before [) for unique naming */
        char *instname = NULL;
        {
            char *linecopy = copy(card->line);
            char *ls = linecopy;
            instname = gettok(&ls);
            tfree(linecopy);
        }

        /* Add a pulldown for each node */
        char *tok;
        char *ncopy = copy(nodes);
        char *walk = ncopy;
        int pd_count = 0;
        while ((tok = gettok(&walk)) != NULL) {
            if (*tok == '~' || *tok == '[' || *tok == '%') {
                tfree(tok);
                continue;
            }
            char *pdline = tprintf("rpulldown_%s_%s_%d %s 0 10meg",
                                   instname ? instname : "dac",
                                   tok, pd_count, tok);
            insert_new_line(card, pdline, card->linenum + 1,
                            card->linenum_orig, card->linesource);
            pd_count++;
            tfree(tok);
        }
        tfree(instname);
        tfree(ncopy);
        tfree(nodes);
    }
}

/* do not modify oldcard address, insert everything after first line only */
void ltspice_compat_a(struct card *oldcard)
{
    /* First pass: Transform LTspice fixed 8-pin A-devices to ngspice format */
    ltspice_a_device_transform(oldcard);

    /* Add pulldowns on DAC output nodes that may lack DC paths */
    add_dac_pulldowns(oldcard);

    /* Then run existing compat */
    oldcard->nextcard = ltspice_compat(oldcard->nextcard);
}

/**** LTspice A-device transformation **************
 * Transform LTspice fixed 8-pin A-device format to ngspice XSPICE format.
 * LTspice syntax: A1 n1 n2 n3 n4 n5 n6 n7 n8 DEVICETYPE
 * where unused pins are tied to 0.
 *
 * ngspice syntax: A1 <connections> <modelname>
 * where connections depend on the code model's ifspec.ifs.
 *
 * Maps LTspice device types to ngspice equivalents:
 *   COUNTER  -> d_fdiv (frequency divider with internal counter)
 *   SCHMITT  -> d_buffer with vhigh/vlow thresholds
 *   BUF      -> d_buffer
 *   NOT      -> d_inverter
 *   AND      -> d_and (variable inputs)
 *   OR       -> d_or (variable inputs)
 *   NAND     -> d_nand (variable inputs)
 *   NOR      -> d_nor (variable inputs)
 *   XOR      -> d_xor (variable inputs)
 *   XNOR     -> d_xnor (variable inputs)
 *   MUX      -> d_mux (if available) or d_genlut
 *   DFF      -> d_dff
 *   JKFF     -> d_jkff
 *   SRFF     -> d_srff
 *   DLATCH   -> d_dlatch
 */

/* Count tokens in a string */
static int count_tokens_a(char *line)
{
    int count = 0;
    char *s = line;
    char c;

    while ((c = *s) != '\0') {
        if (c == ' ' || c == '\t') {
            s++;
            continue;
        }
        count++;
        while ((c = *s) != '\0' && c != ' ' && c != '\t') s++;
    }
    return count;
}

/* Map LTspice device type to ngspice model type */
static const char* ltspice_devtype_to_ngspice(const char *devtype, char *mapped_type, size_t mapped_size)
{
    const char *upper = devtype;
    char buf[64];
    int i;

    for (i = 0; i < (int)strlen(devtype) && i < 63; i++)
        buf[i] = toupper_c(devtype[i]);
    buf[i] = '\0';
    upper = buf;

    if (strcmp(upper, "COUNTER") == 0) {
        snprintf(mapped_type, mapped_size, "d_counter");
        return "d_counter";
    }
    if (strcmp(upper, "SCHMITT") == 0 || strcmp(upper, "SCHMITT_BUF") == 0) {
        snprintf(mapped_type, mapped_size, "d_buffer");
        return "d_buffer";
    }
    if (strcmp(upper, "BUF") == 0 || strcmp(upper, "BUFFER") == 0) {
        snprintf(mapped_type, mapped_size, "d_buffer");
        return "d_buffer";
    }
    if (strcmp(upper, "NOT") == 0 || strcmp(upper, "INV") == 0 || strcmp(upper, "INVERTER") == 0) {
        snprintf(mapped_type, mapped_size, "d_inverter");
        return "d_inverter";
    }
    if (strcmp(upper, "AND") == 0) {
        snprintf(mapped_type, mapped_size, "d_and");
        return "d_and";
    }
    if (strcmp(upper, "OR") == 0) {
        snprintf(mapped_type, mapped_size, "d_or");
        return "d_or";
    }
    if (strcmp(upper, "NAND") == 0) {
        snprintf(mapped_type, mapped_size, "d_nand");
        return "d_nand";
    }
    if (strcmp(upper, "NOR") == 0) {
        snprintf(mapped_type, mapped_size, "d_nor");
        return "d_nor";
    }
    if (strcmp(upper, "XOR") == 0) {
        snprintf(mapped_type, mapped_size, "d_xor");
        return "d_xor";
    }
    if (strcmp(upper, "XNOR") == 0) {
        snprintf(mapped_type, mapped_size, "d_xnor");
        return "d_xnor";
    }
    if (strcmp(upper, "DFF") == 0 || strcmp(upper, "D_FF") == 0 || strcmp(upper, "DFLOP") == 0) {
        snprintf(mapped_type, mapped_size, "d_dff");
        return "d_dff";
    }
    if (strcmp(upper, "JKFF") == 0 || strcmp(upper, "JK_FF") == 0) {
        snprintf(mapped_type, mapped_size, "d_jkff");
        return "d_jkff";
    }
    if (strcmp(upper, "SRFF") == 0 || strcmp(upper, "SR_FF") == 0 || strcmp(upper, "SRFLOP") == 0) {
        snprintf(mapped_type, mapped_size, "d_srff");
        return "d_srff";
    }
    if (strcmp(upper, "DLATCH") == 0 || strcmp(upper, "D_LATCH") == 0) {
        snprintf(mapped_type, mapped_size, "d_dlatch");
        return "d_dlatch";
    }
    if (strcmp(upper, "SRLATCH") == 0 || strcmp(upper, "SR_LATCH") == 0) {
        snprintf(mapped_type, mapped_size, "d_srlatch");
        return "d_srlatch";
    }
    if (strcmp(upper, "TFF") == 0 || strcmp(upper, "T_FF") == 0) {
        snprintf(mapped_type, mapped_size, "d_tff");
        return "d_tff";
    }
    if (strcmp(upper, "PHASEDET") == 0 || strcmp(upper, "PHIDET") == 0) {
        snprintf(mapped_type, mapped_size, "d_phasedet");
        return "d_phasedet";
    }
    if (strcmp(upper, "MODULATOR") == 0) {
        snprintf(mapped_type, mapped_size, "a_modulator");
        return "a_modulator";
    }
    if (strcmp(upper, "SAMPLEHOLD") == 0) {
        snprintf(mapped_type, mapped_size, "a_samplehold");
        return "a_samplehold";
    }
    if (strcmp(upper, "VARISTOR") == 0) {
        snprintf(mapped_type, mapped_size, "a_varistor");
        return "a_varistor";
    }

    /* Unknown device type - return as-is */
    snprintf(mapped_type, mapped_size, "%s", devtype);
    return devtype;
}

/* Get the number of pins for a given LTspice A-device type */
static int ltspice_device_pin_count(const char *devtype)
{
    const char *upper = devtype;
    char buf[64];
    int i;

    for (i = 0; i < (int)strlen(devtype) && i < 63; i++)
        buf[i] = toupper_c(devtype[i]);
    buf[i] = '\0';
    upper = buf;

    if (strcmp(upper, "COUNTER") == 0) return 4;   /* in, out, reset, enable */
    if (strcmp(upper, "SCHMITT") == 0 || strcmp(upper, "SCHMITT_BUF") == 0) return 2;  /* in, out */
    if (strcmp(upper, "BUF") == 0 || strcmp(upper, "BUFFER") == 0) return 2;  /* in, out */
    if (strcmp(upper, "NOT") == 0 || strcmp(upper, "INV") == 0 || strcmp(upper, "INVERTER") == 0) return 2;  /* in, out */
    if (strcmp(upper, "AND") == 0) return 3;   /* in1, in2, out */
    if (strcmp(upper, "OR") == 0) return 3;    /* in1, in2, out */
    if (strcmp(upper, "NAND") == 0) return 3;  /* in1, in2, out */
    if (strcmp(upper, "NOR") == 0) return 3;   /* in1, in2, out */
    if (strcmp(upper, "XOR") == 0) return 3;   /* in1, in2, out */
    if (strcmp(upper, "XNOR") == 0) return 3;  /* in1, in2, out */
    if (strcmp(upper, "DFF") == 0 || strcmp(upper, "D_FF") == 0 || strcmp(upper, "DFLOP") == 0) return 4;   /* d, clk, q, nq */
    if (strcmp(upper, "JKFF") == 0 || strcmp(upper, "JK_FF") == 0) return 4; /* j, k, clk, q */
    if (strcmp(upper, "SRFF") == 0 || strcmp(upper, "SR_FF") == 0 || strcmp(upper, "SRFLOP") == 0) return 4; /* s, r, q, nq */
    if (strcmp(upper, "DLATCH") == 0 || strcmp(upper, "D_LATCH") == 0) return 3; /* d, en, q */
    if (strcmp(upper, "SRLATCH") == 0 || strcmp(upper, "SR_LATCH") == 0) return 3; /* s, r, q */
    if (strcmp(upper, "TFF") == 0 || strcmp(upper, "T_FF") == 0) return 3; /* t, clk, q */
    if (strcmp(upper, "PHASEDET") == 0 || strcmp(upper, "PHIDET") == 0) return 4; /* A, B, Q, NQ */
    if (strcmp(upper, "TRISTATE") == 0) return 3; /* in, en, out */
    if (strcmp(upper, "RAM") == 0) return 5; /* addr, data_in, clk, we, data_out */
    if (strcmp(upper, "MODULATOR") == 0) return 4; /* fm, am, cos, sin */
    if (strcmp(upper, "SAMPLEHOLD") == 0) return 3; /* in, clk, out */
    if (strcmp(upper, "VARISTOR") == 0) return 4; /* in+, in-, out+, out- */

    return 0; /* Unknown - no transformation */
}

/* Get the LTspice pin index (0-based) for a given ngspice port index.
 * LTspice uses fixed 8-pin positions:
 *   Pins 0-4: Inputs (5 slots)
 *   Pin 5:    Reserved/unused
 *   Pins 6-7: Outputs (2 slots)
 * For simple 2-pin devices like BUF/NOT:
 *   ngspice port 0 (input)  -> LTspice pin 0
 *   ngspice port 1 (output) -> LTspice pin 6
 * For 3-pin devices like AND/OR: pins 0,1,6 (in1, in2, out)
 * For 4-pin devices: pins 0,1,6,7
 */
static int ltspice_pin_index(const char *devtype, int ngspice_port)
{
    const char *upper = devtype;
    char buf[64];
    int i;
    int pin_count = ltspice_device_pin_count(devtype);

    for (i = 0; i < (int)strlen(devtype) && i < 63; i++)
        buf[i] = toupper_c(devtype[i]);
    buf[i] = '\0';
    upper = buf;

    if (pin_count == 2) {
        /* 2-pin: input=pin0, output=pin6 */
        if (ngspice_port == 0) return 0;
        if (ngspice_port == 1) return 6;
    } else if (pin_count == 3) {
        /* 3-pin: in1=pin0, in2=pin1, out=pin6 */
        if (ngspice_port == 0) return 0;
        if (ngspice_port == 1) return 1;
        if (ngspice_port == 2) return 6;
    } else if (pin_count == 4) {
        /* 4-pin: p0=pin0, p1=pin1, p2=pin6, p3=pin7 */
        if (ngspice_port < 2) return ngspice_port;
        if (ngspice_port == 2) return 6;
        if (ngspice_port == 3) return 7;
    }

    return ngspice_port; /* Fallback: direct mapping */
}

/* ---- LTspice inline parameter parsing ---- */

typedef struct {
    bool present;   /* true if any bridge-relevant param was found */
    double vhigh, vlow, trise, tfall, ref, vt, td;
    double cycles;
    double mark, space;
    double vref, roff, rclamp;
    bool has_vhigh, has_vlow, has_trise, has_tfall, has_ref, has_vt, has_td;
    bool has_cycles;
    bool has_mark, has_space;
    bool has_vref, has_roff, has_rclamp;
} LtspiceInlineParams;

/* Parse a number with optional engineering suffix or trailing unit tag.
 * Engineering scale suffixes (lowercase, case-sensitive by convention):
 *   T=1e12, G=1e9, MEG=1e6, k=1e3, m=1e-3, u=1e-6, n=1e-9, p=1e-12, f=1e-15.
 * Trailing unit tags (V, A, W, Hz, F, H, Ohm, S) are silently stripped AFTER
 * the scale suffix has been applied.  If no scale suffix is found, a trailing
 * unit tag is stripped (e.g. "5V" -> 5.0, "10f" -> 10e-15, "100m" -> 0.1). */
static bool parse_eng_number(const char *str, double *result)
{
    char *end;
    *result = strtod(str, &end);
    if (end == str) return false;
    if (*end == '\0') return true;

    /* --- First: check for engineering scale suffix --- */
    int consumed = 0;
    double scale = 1.0;

    switch (*end) {
    case 'T': case 't': scale = 1e12; consumed = 1; break;
    case 'G': case 'g': scale = 1e9; consumed = 1; break;
    case 'K': case 'k': scale = 1e3; consumed = 1; break;
    case 'M': case 'm':
        /* MEG/meg = mega, otherwise milli */
        if ((end[1] == 'E' || end[1] == 'e') &&
            (end[2] == 'G' || end[2] == 'g')) {
            scale = 1e6; consumed = 3;
        } else {
            scale = 1e-3; consumed = 1;
        }
        break;
    case 'U': case 'u': scale = 1e-6; consumed = 1; break;
    case 'N': case 'n': scale = 1e-9; consumed = 1; break;
    case 'P': case 'p': scale = 1e-12; consumed = 1; break;
    case 'F': case 'f': scale = 1e-15; consumed = 1; break;
    default: break;
    }

    if (consumed > 0) {
        *result *= scale;
        end += consumed;
    }

    /* --- Second: strip trailing unit tag (V, A, W, Hz, F, H, Ohm, S) --- */
    if (*end != '\0') {
        char c = *end;
        if (c == 'V' || c == 'v' || c == 'A' || c == 'a' ||
            c == 'W' || c == 'w') {
            end++;
        } else if (c == 'H' || c == 'h') {
            end++;
            if (toupper_c(*end) == 'Z') end++;
        } else if (c == 'F' || c == 'f') {
            /* Only strip 'F' as unit if no scale was consumed above
             * (otherwise "10f" = femto, "10F" = femto, but
             *  value trailing bare "F" means Farad = as-is) */
            if (consumed == 0) end++;
        } else if (c == 'O') {
            end++;
            if (toupper_c(end[0]) == 'H' && toupper_c(end[1]) == 'M') end += 3;
        } else if (c == 'S' || c == 's') {
            end++;
        }
        /* Ignore remaining */
    }

    return true;
}

/* Parse key=value pairs from the inline parameter string.
 * String is the remainder after devtype extraction, e.g.:
 * "Vhigh=5V Vlow=0V Trise=10n Tfall=20n Ref=2.5V" */
static LtspiceInlineParams ltspice_parse_inline_params(const char *s)
{
    LtspiceInlineParams p = {0};
    char *tmp = copy(s);
    char *ptr = tmp;
    char *tok;

    /* Defaults (used if the corresponding param is absent) */
    p.vhigh = 5.0;
    p.vlow = 0.0;
    p.trise = 1e-9;
    p.tfall = 1e-9;
    p.ref = 2.5;
    p.vt = 2.5;
    p.td = 0.0;

    while ((tok = gettok(&ptr)) != NULL) {
        char *eq = strchr(tok, '=');
        if (!eq) { tfree(tok); continue; }
        *eq = '\0';
        const char *key = tok;
        const char *val = eq + 1;
        double num = 0.0;
        bool valid = parse_eng_number(val, &num);

        /* Case-insensitive key matching */
        char keyup[64], *kd = keyup;
        int ki;
        for (ki = 0; ki < (int)sizeof(keyup)-1 && key[ki]; ki++)
            kd[ki] = toupper_c(key[ki]);
        kd[ki] = '\0';

        if (strcmp(kd, "VHIGH") == 0 && valid) {
            p.vhigh = num; p.has_vhigh = true; p.present = true;
        } else if (strcmp(kd, "VLOW") == 0 && valid) {
            p.vlow = num; p.has_vlow = true; p.present = true;
        } else if (strcmp(kd, "TRISE") == 0 && valid) {
            p.trise = num; p.has_trise = true; p.present = true;
        } else if (strcmp(kd, "TFALL") == 0 && valid) {
            p.tfall = num; p.has_tfall = true; p.present = true;
        } else if (strcmp(kd, "REF") == 0 && valid) {
            p.ref = num; p.has_ref = true; p.present = true;
        } else if (strcmp(kd, "VT") == 0 && valid) {
            p.vt = num; p.has_vt = true; p.present = true;
        } else if (strcmp(kd, "TD") == 0 && valid) {
            p.td = num; p.has_td = true; p.present = true;
        } else if (strcmp(kd, "CYCLES") == 0 && valid) {
            p.cycles = num; p.has_cycles = true; p.present = true;
        } else if (strcmp(kd, "MARK") == 0 && valid) {
            p.mark = num; p.has_mark = true;
        } else if (strcmp(kd, "SPACE") == 0 && valid) {
            p.space = num; p.has_space = true;
        } else if (strcmp(kd, "VREF") == 0 && valid) {
            p.vref = num; p.has_vref = true;
        } else if (strcmp(kd, "ROFF") == 0 && valid) {
            p.roff = num; p.has_roff = true;
        } else if (strcmp(kd, "RCLAMP") == 0 && valid) {
            p.rclamp = num; p.has_rclamp = true;
        }
        tfree(tok);
    }
    tfree(tmp);
    return p;
}

/* Check if an LTspice pin index is an input pin.
 * LTspice mapping: pins 0-4 = inputs, pin 5 = reserved, pins 6-7 = outputs. */
static bool ltspice_pin_is_input(int lt_pin)
{
    return lt_pin >= 0 && lt_pin <= 4;
}

/* Check if an LTspice pin index is an output pin.
 * LTspice 8-pin fixed format:
 *   Pins 0-4: Inputs
 *   Pin 5:    Output (first/primary) for simple gates and DFF Q
 *   Pin 6:    Output (secondary) for DFF !Q, or VCC+ for some devices
 *   Pin 7:    Output (tertiary) or VCC-/GND for some devices */
static bool ltspice_pin_is_output(int lt_pin)
{
    return lt_pin == 5 || lt_pin == 6 || lt_pin == 7;
}

void ltspice_a_device_transform(struct card *oldcard)
{
    struct card *card;
    struct vsmodels *modelsfound = NULL;
    int skip_control = 0;

    /* First pass: find all A-device model types used and register them */
    for (card = oldcard; card; card = card->nextcard) {
        char *cut_line = card->line;
        if (*cut_line == '*' || *cut_line == '\0')
            continue;
        if (ciprefix(".control", cut_line)) {
            skip_control++;
            continue;
        }
        else if (ciprefix(".endc", cut_line)) {
            skip_control--;
            continue;
        }
        else if (skip_control > 0)
            continue;

        if (ciprefix(".model", cut_line)) {
            /* Register existing model - parse type and params */
            char *tmp = copy(cut_line);
            char *str = tmp;
            str = nexttok(str); /* skip .model */
            char *modname = gettok(&str);
            if (modname) {
                char *modeltype_full = gettok(&str);
                if (modeltype_full) {
                    char *type = NULL, *params = NULL;
                    char *paren = strchr(modeltype_full, '(');
                    if (paren) {
                        *paren = '\0';
                        type = copy(modeltype_full);
                        char *p = paren + 1;
                        char *end = strchr(p, ')');
                        if (end) *end = '\0';
                        params = copy(p);
                    } else {
                        type = copy(modeltype_full);
                    }
                    modelsfound = insert_new_model(
                        modelsfound, modname, type, params, "top");
                    /* Find the newly inserted node to set cardptr */
                    {
                        struct vsmodels *p = modelsfound;
                        while (p && p->nextmodel) p = p->nextmodel;
                        if (p) p->cardptr = card;
                    }
                    tfree(type);
                    tfree(params);
                    tfree(modeltype_full);
                }
                tfree(modname);
            }
            tfree(tmp);
        }
    }

    /* Second pass: transform A-device lines */
    skip_control = 0;
    for (card = oldcard; card; card = card->nextcard) {
        char *cut_line = card->line;
        if (*cut_line == '*' || *cut_line == '\0')
            continue;
        if (ciprefix(".control", cut_line)) {
            skip_control++;
            continue;
        }
        else if (ciprefix(".endc", cut_line)) {
            skip_control--;
            continue;
        }
        else if (skip_control > 0)
            continue;

        /* Check if this is an A-device line */
        if (*cut_line == 'a' || *cut_line == 'A') {
            /* Parse the line */
            char *linecopy = copy(cut_line);
            char *s = linecopy;

            /* Get instance name */
            char *instname = gettok_node(&s);
            if (!instname) { tfree(linecopy); continue; }

            /* Get pins and device type — read all remaining tokens, then
             * find the devtype as the last token without '='.
             * Params always contain '='; pins and devtype never do.
             * This handles A-devices with fewer than 8 LTspice pins
             * (e.g. VARISTOR with 4 pins). */
            char *pins[8] = {NULL};
            int i;
            char *devtype = NULL;
            char param_buf[1024] = "";
            {
                char *all_toks[16] = {NULL};
                int nt = 0;
                int dev_idx = -1;
                char *tok;
                while ((tok = gettok_node(&s)) != NULL && nt < 16) {
                    all_toks[nt++] = tok;
                }
                for (int ti = nt - 1; ti >= 0; ti--) {
                    if (strchr(all_toks[ti], '=') == NULL) {
                        devtype = all_toks[ti];
                        dev_idx = ti;
                        break;
                    }
                }
                if (!devtype) {
                    tfree(instname);
                    for (int fi = 0; fi < nt; fi++) tfree(all_toks[fi]);
                    tfree(linecopy);
                    continue;
                }
                int npins = dev_idx > 8 ? 8 : dev_idx;
                for (i = 0; i < npins; i++) pins[i] = all_toks[i];
                /* Build param string from tokens after devtype */
                for (int fi = dev_idx + 1; fi < nt; fi++) {
                    if (fi > dev_idx + 1 && strlen(param_buf) < sizeof(param_buf) - 2)
                        strcat(param_buf, " ");
                    if (strlen(param_buf) + strlen(all_toks[fi]) < sizeof(param_buf) - 1)
                        strcat(param_buf, all_toks[fi]);
                    tfree(all_toks[fi]);
                }
            }

            /* Map device type to ngspice equivalent */
            char mapped_type[64];
            int pin_count = ltspice_device_pin_count(devtype);
            bool from_model_card = false;
            char *model_card_params = NULL;
            struct card *model_card_ptr = NULL;

            if (pin_count == 0) {
                /* Check if devtype references a registered model card */
                struct vsmodels *mdl = find_model_by_name(modelsfound, devtype, "top");
                if (mdl && mdl->modeltype && strcmp(mdl->modeltype, "counter") == 0) {
                    /* Model-card form COUNTER: .model MYMODEL counter(cycles=N) */
                    model_card_params = mdl->params ? copy(mdl->params) : NULL;
                    model_card_ptr = mdl->cardptr;
                    tfree(devtype);
                    devtype = copy("counter");
                    pin_count = ltspice_device_pin_count(devtype);
                    from_model_card = true;
                }
            }

            if (pin_count == 0) {
                /* Not a recognized LTspice A-device — pass through unchanged */
                tfree(instname);
                for (i = 0; i < 8; i++) tfree(pins[i]);
                tfree(devtype);
                tfree(linecopy);
                continue;
            }
            const char *ngspice_type = ltspice_devtype_to_ngspice(devtype, mapped_type, sizeof(mapped_type));

            /* Inline parameter bridge setup (declared here for cleanup scope) */
            struct card *insert_pos = card;
            LtspiceInlineParams iparams = {0};
            bool do_bridge = false;
            char *bridge_node[8] = {NULL};
            char *analog_node[8] = {NULL};
            int bridge_dir[8] = {0}; /* 0=none, 1=adc (input), 2=dac (output) */
            int in_count = 0, out_count = 0;

            {
                char *pp = param_buf;
                while (*pp == ' ' || *pp == '\t') pp++;
                if (*pp != '\0') {
                    iparams = ltspice_parse_inline_params(param_buf);
                    do_bridge = iparams.present;
                }
            }

            /* Analog codemodels — no ADC/DAC bridges */
            if (strcmp(ngspice_type, "a_modulator") == 0 ||
                strcmp(ngspice_type, "a_samplehold") == 0 ||
                strcmp(ngspice_type, "a_varistor") == 0) {
                do_bridge = false;
            }

            /* For model-card form COUNTER, extract cycles from model card params */
            if (from_model_card && model_card_params) {
                char pcopy[256];
                int pc;
                for (pc = 0; pc < 255 && model_card_params[pc]; pc++)
                    pcopy[pc] = toupper_c(model_card_params[pc]);
                pcopy[pc] = '\0';
                char *eqs = strstr(pcopy, "CYCLES=");
                if (!eqs) eqs = strstr(pcopy, "CYCLES =");
                if (eqs) {
                    char *val = eqs;
                    while (*val && *val != '=') val++;
                    if (*val == '=') val++;
                    while (*val == ' ' || *val == '\t') val++;
                    char *end = NULL;
                    double cv = strtod(val, &end);
                    if (end != val && cv > 0) {
                        iparams.has_cycles = true;
                        iparams.cycles = (double)(int)cv;
                        iparams.present = false;
                        do_bridge = false;
                    }
                }
            }

            if (do_bridge) {
                int lt;
                for (lt = 0; lt < 8; lt++) {
                    if (!pins[lt] || strcmp(pins[lt], "0") == 0) continue;
                    if (ltspice_pin_is_input(lt)) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "_lt%s_in%d_d", instname, in_count);
                        bridge_node[lt] = copy(buf);
                        analog_node[lt] = copy(pins[lt]);
                        bridge_dir[lt] = 1;
                        in_count++;
                    } else if (ltspice_pin_is_output(lt)) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "_lt%s_out%d_d", instname, out_count);
                        bridge_node[lt] = copy(buf);
                        analog_node[lt] = copy(pins[lt]);
                        bridge_dir[lt] = 2;
                        out_count++;
                    }
                }
            }

            if (pin_count > 0 && pin_count < 8) {
                /* Build new A-device line with only the pins the model needs */
                char *new_line;
                char node_str[512] = "";

                /* XSPICE 2-input digital gates (AND, OR, NAND, NOR, XOR, XNOR) and
                   3-pin DLATCH expect vectorized inputs. Wrap input pins in [...]
                   brackets for native XSPICE compatibility.
                   Flip-flops (DFF, JKFF, SRFF) and COUNTER use separate named ports. */
                const char *upper = devtype;
                char upper_buf[64];
                int ui;
                for (ui = 0; ui < (int)strlen(devtype) && ui < 63; ui++)
                    upper_buf[ui] = toupper_c(devtype[ui]);
                upper_buf[ui] = '\0';
                upper = upper_buf;

                int use_vector = (strcmp(upper, "AND") == 0 || strcmp(upper, "OR") == 0 ||
                                  strcmp(upper, "NAND") == 0 || strcmp(upper, "NOR") == 0 ||
                                  strcmp(upper, "XOR") == 0 || strcmp(upper, "XNOR") == 0);

                /* For flip-flops and latches, ngspice expects more ports than the
                   LTspice pin_count reports. Map them to full ngspice port lists. */
                int ngspice_ports = pin_count;
                if (strcmp(upper, "DLATCH") == 0 || strcmp(upper, "D_LATCH") == 0) {
                    /* d_dlatch: data enable set reset out Nout (6 ports, last 4 optional) */
                    ngspice_ports = 6;
                } else if (strcmp(upper, "DFF") == 0 || strcmp(upper, "D_FF") == 0 || strcmp(upper, "DFLOP") == 0) {
                    /* d_dff: data clk set reset out Nout (6 ports, last 4 optional) */
                    ngspice_ports = 6;
                } else if (strcmp(upper, "JKFF") == 0 || strcmp(upper, "JK_FF") == 0) {
                    /* d_jkff: j k clk set reset out Nout — but LTspice only has 4 pins */
                    /* Map: pin0=j, pin1=k, pin2=clk, pin3=q, pin6=nq */
                    ngspice_ports = 7;
                } else if (strcmp(upper, "SRFF") == 0 || strcmp(upper, "SR_FF") == 0 || strcmp(upper, "SRFLOP") == 0) {
                    /* d_srff: s r clk set reset out Nout */
                    ngspice_ports = 7;
                } else if (strcmp(upper, "COUNTER") == 0) {
                    /* d_fdiv: freq_in, freq_out (2 ports only) */
                    ngspice_ports = 2;
                } else if (strcmp(upper, "SRLATCH") == 0 || strcmp(upper, "SR_LATCH") == 0) {
                    /* d_srlatch: s r enable set reset out Nout (7 ports) */
                    ngspice_ports = 7;
                } else if (strcmp(upper, "SAMPLEHOLD") == 0) {
                    /* a_samplehold: in, clk, out (3 ports) */
                    ngspice_ports = 3;
                } else if (strcmp(upper, "VARISTOR") == 0) {
                    /* a_varistor: 3 XSPICE ports (in+ in/v, in- in/v, out inout/gd).
                     * gd port uses 2 netlist nodes, so total nodes needed = 1+1+2 = 4.
                     * Map: port0=in+(pin0), port1=in-(pin1), port2=out+(pin2), port3=out-(pin3). */
                    ngspice_ports = 4;
                }

                if (use_vector) {
                    strcat(node_str, "[");
                }

                /* Collect pins — for vectorized gates, inputs go in [] and output after */
                /* When do_bridge is active, build a port→bridge mapping from the actual
                 * pin layout (scanning bridge_dir in order) instead of using hardcoded
                 * ltspice_pin_index which assumes fixed output positions. */
                char *port_bridge[8] = {NULL}; /* maps ngspice port index → bridge node */
                int bridge_input_idx = 0, bridge_output_idx = 0;
                bool bridge_map_done = false;
                if (do_bridge && use_vector) {
                    for (int lt = 0; lt < 8; lt++) {
                        if (bridge_dir[lt] == 1 && bridge_input_idx < pin_count - 1) {
                            port_bridge[bridge_input_idx] = bridge_node[lt];
                            bridge_input_idx++;
                        } else if (bridge_dir[lt] == 2 && bridge_output_idx < 2) {
                            port_bridge[pin_count - 1 + bridge_output_idx] = bridge_node[lt];
                            bridge_output_idx++;
                        }
                    }
                    bridge_map_done = true;
                }
                for (i = 0; i < ngspice_ports; i++) {
                    int lt_pin;
                    /* Map ngspice port index back to LTspice pin position */
                    if (strcmp(upper, "DLATCH") == 0 || strcmp(upper, "D_LATCH") == 0) {
                        /* LTspice: d=pin0, en=pin1, q=pin6, nq=pin7. ngspice: data,en,set,reset,out,Nout */
                        if (i == 0) lt_pin = 0;       /* data */
                        else if (i == 1) lt_pin = 1;   /* enable */
                        else if (i == 2) lt_pin = -1;  /* set (not in LTspice) */
                        else if (i == 3) lt_pin = -1;  /* reset (not in LTspice) */
                        else if (i == 4) lt_pin = 6;   /* out (Q at pin 6) */
                        else lt_pin = 7;                /* Nout (!Q at pin 7) */
                    } else if (strcmp(upper, "DFF") == 0 || strcmp(upper, "D_FF") == 0 || strcmp(upper, "DFLOP") == 0) {
                        /* LTspice: d=pin0, clk=pin1, q=pin6, nq=pin7. ngspice: data,clk,set,reset,out,Nout */
                        if (i == 0) lt_pin = 0;       /* data */
                        else if (i == 1) lt_pin = 1;   /* clk */
                        else if (i == 2) lt_pin = -1;  /* set */
                        else if (i == 3) lt_pin = -1;  /* reset */
                        else if (i == 4) lt_pin = 6;   /* out (Q at pin 6) */
                        else lt_pin = 7;                /* Nout (!Q at pin 7) */
                    } else if (strcmp(upper, "JKFF") == 0 || strcmp(upper, "JK_FF") == 0) {
                        /* LTspice: j=pin0, k=pin1, clk=pin2, q=pin6, nq=pin7. ngspice: j,k,clk,set,reset,out,Nout */
                        if (i == 0) lt_pin = 0;       /* j */
                        else if (i == 1) lt_pin = 1;   /* k */
                        else if (i == 2) lt_pin = 2;   /* clk */
                        else if (i == 3) lt_pin = -1;  /* set */
                        else if (i == 4) lt_pin = -1;  /* reset */
                        else if (i == 5) lt_pin = 6;   /* out (Q at pin 6) */
                        else lt_pin = 7;                /* Nout (!Q at pin 7) */
                    } else if (strcmp(upper, "SRFF") == 0 || strcmp(upper, "SR_FF") == 0 || strcmp(upper, "SRFLOP") == 0) {
                        /* LTspice: s=pin0, r=pin1, q=pin6, nq=pin7 */
                        if (i == 0) lt_pin = 0;       /* s */
                        else if (i == 1) lt_pin = 1;   /* r */
                        else if (i == 2) lt_pin = -1;  /* clk (not used by d_srff) */
                        else if (i == 3) lt_pin = -1;  /* set */
                        else if (i == 4) lt_pin = -1;  /* reset */
                        else if (i == 5) lt_pin = 6;   /* out (Q at pin 6) */
                        else lt_pin = 7;                /* Nout (!Q at pin 7) */
                    } else if (strcmp(upper, "SRLATCH") == 0 || strcmp(upper, "SR_LATCH") == 0) {
                        /* d_srlatch: s r enable set reset out Nout (7 ports) */
                        /* LTspice: s=pin0, r=pin1, q=pin6, nq=pin7 */
                        if (i == 0) lt_pin = 0;       /* s */
                        else if (i == 1) lt_pin = 1;   /* r */
                        else if (i == 2) lt_pin = -1;  /* enable (not in LTspice) */
                        else if (i == 3) lt_pin = -1;  /* set (not in LTspice) */
                        else if (i == 4) lt_pin = -1;  /* reset (not in LTspice) */
                        else if (i == 5) lt_pin = 6;   /* out (Q at pin 6) */
                        else lt_pin = 7;                /* Nout (!Q at pin 7) */
                    } else if (strcmp(upper, "TFF") == 0 || strcmp(upper, "T_FF") == 0) {
                        /* d_tff: t clk set reset out Nout (6 ports) */
                        /* LTspice: t=pin0, clk=pin1, q=pin6, nq=pin7 */
                        if (i == 0) lt_pin = 0;       /* t */
                        else if (i == 1) lt_pin = 1;   /* clk */
                        else if (i == 2) lt_pin = -1;  /* set (not in LTspice) */
                        else if (i == 3) lt_pin = -1;  /* reset (not in LTspice) */
                        else if (i == 4) lt_pin = 6;   /* out (Q at pin 6) */
                        else lt_pin = 7;                /* Nout (!Q at pin 7) */
                    } else if (strcmp(upper, "COUNTER") == 0) {
                        /* d_fdiv: freq_in, freq_out (2 ports) */
                        /* LTspice: in=pin0, out=pin6 */
                        if (i == 0) lt_pin = 0;       /* freq_in */
                        else lt_pin = 6;               /* freq_out */
                    } else if (strcmp(upper, "SAMPLEHOLD") == 0) {
                        /* a_samplehold: in, clk, out (3 ports) */
                        /* LTspice: in=pin0, clk=pin2, out=pin6 */
                        if (i == 0) lt_pin = 0;       /* in */
                        else if (i == 1) lt_pin = 2;   /* clk */
                        else lt_pin = 6;                /* out */
                    } else if (strcmp(upper, "VARISTOR") == 0) {
                        /* a_varistor: in+, in-, out+, out- (4 ports) */
                        /* LTspice: in+=pin0, in-=pin1, out+=pin2, out-=pin3 */
                        if (i == 0) lt_pin = 0;       /* in+ */
                        else if (i == 1) lt_pin = 1;   /* in- */
                        else if (i == 2) lt_pin = 2;   /* out+ */
                        else lt_pin = 3;                /* out- */
                    } else {
                        lt_pin = ltspice_pin_index(devtype, i);
                    }

                    /* Determine the node string for this port */
                    char *port_val = NULL;
                    if (bridge_map_done && port_bridge[i]) {
                        port_val = port_bridge[i];
                    } else if (lt_pin >= 0 && lt_pin < 8) {
                        if (bridge_node[lt_pin])
                            port_val = bridge_node[lt_pin];
                        else if (pins[lt_pin])
                            port_val = pins[lt_pin];
                        else
                            port_val = "0";
                    } else {
                        port_val = "0";
                    }

                    /* For vectorized gates, inputs go in [], output after */
                    if (use_vector && i == pin_count - 1) {
                        strcat(node_str, "] ");
                        strcat(node_str, port_val);
                    } else if (i > 0) {
                        strcat(node_str, " ");
                        strcat(node_str, port_val);
                    } else {
                        strcat(node_str, port_val);
                    }
                }

                /* Create model name: a<instance>_<type> */
                char model_name[256];
                snprintf(model_name, sizeof(model_name), "a%s_%s", instname + 1, ngspice_type);

                new_line = tprintf("%s %s %s", instname, node_str, model_name);

                /* Special handling for specific devices */
                if (strcmp(upper, "DLATCH") == 0 || strcmp(upper, "D_LATCH") == 0) {
                    /* XSPICE d_dlatch: data enable set reset out Nout */
                    new_line = tprintf("%s %s %s NULL NULL %s %s %s",
                             instname,
                             bridge_node[0] ? bridge_node[0] : pins[0],
                             bridge_node[1] ? bridge_node[1] : pins[1],
                             bridge_node[6] ? bridge_node[6] : pins[6],
                             bridge_node[7] ? bridge_node[7] : pins[7],
                             model_name);
                }
                else if (strcmp(upper, "TRISTATE") == 0) {
                    /* XSPICE d_tristate: in enable out */
                    new_line = tprintf("%s %s %s %s %s",
                             instname,
                             bridge_node[0] ? bridge_node[0] : pins[0],
                             bridge_node[1] ? bridge_node[1] : pins[1],
                             bridge_node[6] ? bridge_node[6] : pins[6],
                             model_name);
                }

                /* Replace the original line */
                tfree(card->line);
                card->line = new_line;
                /* Insert explicit bridge cards when inline params are present */
                if (do_bridge) {
                    /* Build ADC bridge (input pins: analog -> digital) */
                    if (in_count > 0) {
                        char adc_an[256] = "", adc_dig[256] = "";
                        int lt, first = 1;
                        for (lt = 0; lt < 8; lt++) {
                            if (bridge_dir[lt] == 1) {
                                if (!first) { strcat(adc_an, " "); strcat(adc_dig, " "); }
                                strcat(adc_an, analog_node[lt]);
                                strcat(adc_dig, bridge_node[lt]);
                                first = 0;
                            }
                        }
                        double thresh = iparams.has_ref ? iparams.ref :
                                        (iparams.has_vt ? iparams.vt :
                                         (iparams.vhigh + iparams.vlow) / 2.0);
                        char adc_model[512], adc_inst[512];
                        snprintf(adc_model, sizeof(adc_model),
                                 ".model _lt_m%s_adc adc_bridge(in_low = %.15g in_high = %.15g)",
                                 instname, thresh, thresh);
                        snprintf(adc_inst, sizeof(adc_inst),
                                 "a_lt%s_adc [ %s ] [ %s ] _lt_m%s_adc",
                                 instname, adc_an, adc_dig, instname);
                        insert_pos = insert_new_line(insert_pos, copy(adc_model), 0,
                                                     card->linenum_orig, card->linesource);
                        insert_pos = insert_new_line(insert_pos, copy(adc_inst), 0,
                                                     card->linenum_orig, card->linesource);
                    }

                    /* Build DAC bridge (output pins: digital -> analog) */
                    if (out_count > 0) {
                        char dac_dig[256] = "", dac_an[256] = "";
                        int lt, first = 1;
                        for (lt = 0; lt < 8; lt++) {
                            if (bridge_dir[lt] == 2) {
                                if (!first) { strcat(dac_dig, " "); strcat(dac_an, " "); }
                                strcat(dac_dig, bridge_node[lt]);
                                strcat(dac_an, analog_node[lt]);
                                first = 0;
                            }
                        }
                        char dac_model[512], dac_inst[512];
                        snprintf(dac_model, sizeof(dac_model),
                                 ".model _lt_m%s_dac dac_bridge(out_low = %.15g out_high = %.15g"
                                 " t_rise = %.15g t_fall = %.15g)",
                                 instname, iparams.vlow, iparams.vhigh,
                                 iparams.trise, iparams.tfall);
                        snprintf(dac_inst, sizeof(dac_inst),
                                 "a_lt%s_dac [ %s ] [ %s ] _lt_m%s_dac",
                                 instname, dac_dig, dac_an, instname);
                        insert_pos = insert_new_line(insert_pos, copy(dac_model), 0,
                                                     card->linenum_orig, card->linesource);
                        insert_pos = insert_new_line(insert_pos, copy(dac_inst), 0,
                                                     card->linenum_orig, card->linesource);
                    }
                }

                /* Add .model card if not already present */
                if (!find_a_model(modelsfound, model_name, "top")) {
                    /* Generate default model parameters based on device type */
                    char model_line[512];
                    const char *upper = devtype;
                    char buf[64];
                    int j;
                    for (j = 0; j < (int)strlen(devtype) && j < 63; j++)
                        buf[j] = toupper_c(devtype[j]);
                    buf[j] = '\0';
                    upper = buf;

                    if (strcmp(upper, "COUNTER") == 0) {
                        if (iparams.has_cycles) {
                            int c = (int)iparams.cycles;
                            snprintf(model_line, sizeof(model_line),
                                     ".model %s d_fdiv (div_factor = %d high_cycles = %d)",
                                     model_name, 2*c, c);
                        } else {
                            snprintf(model_line, sizeof(model_line),
                                     ".model %s d_fdiv (i_count = 0)", model_name);
                        }
                    }
                    else if (strcmp(upper, "SCHMITT") == 0 || strcmp(upper, "SCHMITT_BUF") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_buffer ()", model_name);
                    }
                    else if (strcmp(upper, "BUF") == 0 || strcmp(upper, "BUFFER") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_buffer ()", model_name);
                    }
                    else if (strcmp(upper, "NOT") == 0 || strcmp(upper, "INV") == 0 || strcmp(upper, "INVERTER") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_inverter ()", model_name);
                    }
                    else if (strcmp(upper, "AND") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_and ()", model_name);
                    }
                    else if (strcmp(upper, "OR") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_or ()", model_name);
                    }
                    else if (strcmp(upper, "NAND") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_nand ()", model_name);
                    }
                    else if (strcmp(upper, "NOR") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_nor ()", model_name);
                    }
                    else if (strcmp(upper, "XOR") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_xor ()", model_name);
                    }
                    else if (strcmp(upper, "XNOR") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_xnor ()", model_name);
                    }
                    else if (strcmp(upper, "DFF") == 0 || strcmp(upper, "D_FF") == 0 || strcmp(upper, "DFLOP") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_dff ()", model_name);
                    }
                    else if (strcmp(upper, "JKFF") == 0 || strcmp(upper, "JK_FF") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_jkff ()", model_name);
                    }
                    else if (strcmp(upper, "SRFF") == 0 || strcmp(upper, "SR_FF") == 0 || strcmp(upper, "SRFLOP") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_srff ()", model_name);
                    }
                    else if (strcmp(upper, "DLATCH") == 0 || strcmp(upper, "D_LATCH") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_dlatch ()", model_name);
                    }
                    else if (strcmp(upper, "SRLATCH") == 0 || strcmp(upper, "SR_LATCH") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_srlatch ()", model_name);
                    }
                    else if (strcmp(upper, "TFF") == 0 || strcmp(upper, "T_FF") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_tff ()", model_name);
                    }
                    else if (strcmp(upper, "PHASEDET") == 0 || strcmp(upper, "PHIDET") == 0) {
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_phasedet ()", model_name);
                    }
                    else if (strcmp(upper, "MODULATOR") == 0) {
                        double mk = iparams.has_mark ? iparams.mark : 1e6;
                        double sp = iparams.has_space ? iparams.space : 5e5;
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s modulator(mark=%.15g space=%.15g)",
                                 model_name, mk, sp);
                    }
                    else if (strcmp(upper, "SAMPLEHOLD") == 0) {
                        double thresh = iparams.has_vt ? iparams.vt : 2.5;
                        double hv = iparams.has_vhigh ? iparams.vhigh : 5.0;
                        double lv = iparams.has_vlow ? iparams.vlow : 0.0;
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s samplehold(vt=%.15g vhigh=%.15g vlow=%.15g)",
                                 model_name, thresh, hv, lv);
                    }
                    else if (strcmp(upper, "VARISTOR") == 0) {
                        double vr = iparams.has_vref ? iparams.vref : 1.0;
                        double ro = iparams.has_roff ? iparams.roff : 1e12;
                        double rc = iparams.has_rclamp ? iparams.rclamp : 1.0;
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s varistor(vref=%.15g roff=%.15g rclamp=%.15g)",
                                 model_name, vr, ro, rc);
                    }
                    else {
                        /* Fallback: generic buffer */
                        snprintf(model_line, sizeof(model_line),
                                 ".model %s d_buffer ()", model_name);
                    }

                    if (from_model_card && model_card_ptr) {
                        /* Replace user's .model card in-place */
                        tfree(model_card_ptr->line);
                        model_card_ptr->line = copy(model_line);
                    } else {
                        /* Insert model card after bridges (or after A-device line if no bridges) */
                        card = insert_new_line(insert_pos, copy(model_line), 0,
                                              card->linenum_orig, card->linesource);
                    }
                } else {
                    if (do_bridge) card = insert_pos;
                }
            }

            /* Cleanup */
            tfree(instname);
            tfree(model_card_params);
            for (i = 0; i < 8 && pins[i]; i++) tfree(pins[i]);
            for (i = 0; i < 8; i++) {
                if (bridge_node[i]) tfree(bridge_node[i]);
                if (analog_node[i]) tfree(analog_node[i]);
            }
            tfree(devtype);
            tfree(linecopy);
        }
    }

    del_models(modelsfound);
}
