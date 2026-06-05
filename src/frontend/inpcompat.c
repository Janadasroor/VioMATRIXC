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
    char *new_str = copy(".param temp = 27");
    newcard = insert_new_line(NULL, new_str, 1, 0, "internal");
    new_str = copy(".param vt = 0.025865");
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


    /* Inject .param temp = 27 inside each subcircuit so that {temp} from
     * LTspice resolves to the default simulation temperature. */
    for (card = newcard; card; card = card->nextcard) {
        char *cut_line = card->line;
        if (ciprefix(".subckt", cut_line)) {
            new_str = copy(".param temp = 27");
            nextcard = insert_new_line(card, new_str, 0, card->linenum_orig, card->linesource);
            new_str = copy(".param vt = 0.025865");
            nextcard = insert_new_line(nextcard, new_str, 1, card->linenum_orig, card->linesource);
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
            /* Convert to ngspice SW model with VT/VH arithmetic:
             *   VT = (VON + VOFF) / 2, VH = |VON - VOFF| */
            else if (strstr(str, "von=") || strstr(str, "voff=")) {
                char *lstr = copy(str);
                double ron_val = 1.0, roff_val = 1.0e9;
                double von_val = 1.0, voff_val = 0.0;
                double vt_val, vh_val;
                char *p;
                char ron_str[64], roff_str[64], vt_str[64], vh_str[64];
                int has_paren;

                has_paren = (lstr[strlen(lstr) - 1] == ')');

                /* extract ron */
                if ((p = strstr(lstr, "ron="))) {
                    char *val = p + 4;
                    char *end = val;
                    while (*end && *end != ' ' && *end != '\t' && *end != ')' && *end != '\n') end++;
                    if (end > val) {
                        char saved = *end;
                        *end = '\0';
                        ron_val = atof(val);
                        *end = saved;
                    }
                }
                /* extract roff */
                if ((p = strstr(lstr, "roff="))) {
                    char *val = p + 5;
                    char *end = val;
                    while (*end && *end != ' ' && *end != '\t' && *end != ')' && *end != '\n') end++;
                    if (end > val) {
                        char saved = *end;
                        *end = '\0';
                        roff_val = atof(val);
                        *end = saved;
                    }
                }
                /* extract von */
                if ((p = strstr(lstr, "von="))) {
                    char *val = p + 4;
                    char *end = val;
                    while (*end && *end != ' ' && *end != '\t' && *end != ')' && *end != '\n') end++;
                    if (end > val) {
                        char saved = *end;
                        *end = '\0';
                        von_val = atof(val);
                        *end = saved;
                    }
                }
                /* extract voff */
                if ((p = strstr(lstr, "voff="))) {
                    char *val = p + 5;
                    char *end = val;
                    while (*end && *end != ' ' && *end != '\t' && *end != ')' && *end != '\n') end++;
                    if (end > val) {
                        char saved = *end;
                        *end = '\0';
                        voff_val = atof(val);
                        *end = saved;
                    }
                }

                vt_val = (von_val + voff_val) / 2.0;
                vh_val = (von_val > voff_val ? von_val - voff_val : voff_val - von_val);
                if (vh_val < 1e-12) vh_val = 1e-6;

                snprintf(ron_str, sizeof(ron_str), "%.15g", ron_val);
                snprintf(roff_str, sizeof(roff_str), "%.15g", roff_val);
                snprintf(vt_str, sizeof(vt_str), "%.15g", vt_val);
                snprintf(vh_str, sizeof(vh_str), "%.15g", vh_val);

                tfree(lstr);
                tfree(card->line);
                if (has_paren)
                    card->line = tprintf(".model %s sw (ron=%s roff=%s vt=%s vh=%s)", modname, ron_str, roff_str, vt_str, vh_str);
                else
                    card->line = tprintf(".model %s sw ron=%s roff=%s vt=%s vh=%s", modname, ron_str, roff_str, vt_str, vh_str);
                tfree(modname);
                /* Not added to modelsfound — SW model is natively supported by S-devices */
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

        /* Locate the '(' after "table".  Two formats:
         *   table=(x0 y0, x1 y1, ...)   — with '=', or
         *   table(x0 y0 x1 y1 ...)      — bare table( */
        char *s_data;
        char *eq = strchr(tablepos, '=');
        if (eq) {
            s_data = eq + 1;
            while (*s_data && (isspace_c(*s_data) || *s_data == '{'))
                s_data++;
        } else {
            s_data = tablepos + 5;
            while (*s_data && isspace_c(*s_data))
                s_data++;
        }
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


/* Replace LTspice/PSpice TBL(...) syntax in I-sources with ngspice
 * B-source table(time, ...) function.
 *
 * LTspice format:   Ixxx n+ n- TBL(t1 i1 t2 i2 ...)
 * Each is converted to a B-source:
 *   Bxxx n+ n- I = table(time, t1, i1, t2, i2, ...)
 */
static void ltspice_tbl_transform(struct card *startcard)
{
    struct card *card;
    for (card = startcard; card; card = card->nextcard) {
        char *cut_line = card->line;
        if (!cut_line) continue;
        if (tolower_c(*cut_line) != 'i')
            continue;

        char *tblpos = NULL;
        for (char *cp = cut_line; *cp; cp++) {
            if (tolower_c(cp[0]) == 't' && tolower_c(cp[1]) == 'b' &&
                tolower_c(cp[2]) == 'l') {
                tblpos = cp;
                break;
            }
        }
        if (!tblpos) continue;

        char *s_data = tblpos + 3;
        while (*s_data && isspace_c(*s_data)) s_data++;
        if (*s_data != '(') continue;

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
        if (!*close_paren || paren_count != 0 || close_paren <= open_paren)
            continue;

        size_t datalen = (size_t)(close_paren - open_paren - 1);
        char *data = tmalloc(datalen + 1);
        memcpy(data, open_paren + 1, datalen);
        data[datalen] = '\0';

        for (char *p = data; *p; p++)
            if (*p == ',') *p = ' ';

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
        if (idx < 2) continue;

        char *linecopy = copy(cut_line);
        char *ls = linecopy;
        char *name_tok = gettok(&ls);
        char *node1 = gettok(&ls);
        char *node2 = gettok(&ls);
        if (!name_tok || !node1 || !node2) {
            tfree(name_tok); tfree(node1); tfree(node2);
            tfree(linecopy);
            continue;
        }

        char *newsrc = tprintf("B%s %s %s I = table(time, %s)",
                               name_tok, node1, node2, table_args);
        tfree(card->line);
        card->line = newsrc;

        tfree(name_tok); tfree(node1); tfree(node2);
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

    /* handle TBL(...) functions in I-sources */
    ltspice_tbl_transform(oldcard);

    /* Replace tbl( with table( in ANY line (not just I-sources).
     * LTspice uses tbl() as a function in B-source expressions
     * ("Bi ... I = V(drv)*I(Vo)+tbl(V(Vcc,Com),0,0,1,.2m)").
     * ngspice's table() uses identical argument syntax. */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        size_t len = strlen(cl);
        char *result = tmalloc(len + 1);
        char *dst = result;
        char *src = cl;
        int changed = 0;
        while (*src) {
            if ((tolower_c(src[0]) == 't' && tolower_c(src[1]) == 'b' &&
                 tolower_c(src[2]) == 'l' && src[3] == '(') &&
                (src == cl || !isalnum_c((unsigned char)src[-1]))) {
                memcpy(dst, "table", 5);
                dst += 5;
                src += 3;
                changed = 1;
            } else {
                *dst++ = *src++;
            }
        }
        *dst = '\0';
        if (changed) {
            tfree(card->line);
            card->line = result;
        } else {
            tfree(result);
        }
    }

    /* Replace buf(...) with u(...-0.5) in B-source expressions.
     * LTspice's buf() returns 1 when input > 0.5, 0 otherwise.
     * ngspice's u() returns 1 when input > 0.  So buf(x) = u(x-0.5). */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (strstr(cl, "buf(") == NULL && strstr(cl, "BUF(") == NULL &&
            strstr(cl, "Buf(") == NULL) continue;
        size_t len = strlen(cl);
        char *result = tmalloc(len * 2 + 20);
        char *dst = result;
        char *src = cl;
        while (*src) {
            if ((src[0] == 'b' || src[0] == 'B') &&
                (src[1] == 'u' || src[1] == 'U') &&
                (src[2] == 'f' || src[2] == 'F') &&
                src[3] == '(' &&
                (src == cl || !isalnum_c((unsigned char)src[-1]))) {
                /* Find matching close paren */
                char *arg_start = src + 4;
                char *arg_end = arg_start;
                int depth = 1;
                while (*arg_end && depth > 0) {
                    if (*arg_end == '(') depth++;
                    else if (*arg_end == ')') depth--;
                    if (depth > 0) arg_end++;
                }
                if (depth == 0 && arg_end > arg_start) {
                    size_t arg_len = arg_end - arg_start;
                    *dst++ = 'u';
                    *dst++ = '(';
                    memcpy(dst, arg_start, arg_len);
                    dst += arg_len;
                    memcpy(dst, "-0.5)", 5);
                    dst += 5;
                    src = arg_end + 1;
                } else {
                    *dst++ = *src++;
                }
            } else {
                *dst++ = *src++;
            }
        }
        *dst = '\0';
        tfree(card->line);
        card->line = result;
    }

    /* Replace non-ASCII dash/minus characters with ASCII hyphen-minus.
     * LTspice subcircuits sometimes contain EN DASH (U+2013) or other
     * Unicode dash characters used as minus signs in numeric values,
     * which ngspice cannot parse (e.g. spice2poly "Bad real value"). */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (strstr(cl, "\xe2\x80\x93") || strstr(cl, "\xe2\x80\x94") || strstr(cl, "\xe2\x88\x92")) {
            size_t len = strlen(cl);
            char *result = tmalloc(len + 1);
            char *dst = result;
            char *src = cl;
            while (*src) {
                if ((unsigned char)src[0] == 0xe2 && (unsigned char)src[1] == 0x80 &&
                    (unsigned char)src[2] == 0x93) {
                    *dst++ = '-';
                    src += 3;
                } else if ((unsigned char)src[0] == 0xe2 && (unsigned char)src[1] == 0x80 &&
                           (unsigned char)src[2] == 0x94) {
                    *dst++ = '-';
                    src += 3;
                } else if ((unsigned char)src[0] == 0xe2 && (unsigned char)src[1] == 0x88 &&
                           (unsigned char)src[2] == 0x92) {
                    *dst++ = '-';
                    src += 3;
                } else {
                    *dst++ = *src++;
                }
            }
            *dst = '\0';
            tfree(card->line);
            card->line = result;
        }
    }

    /* Uncomment *param lines and inject case-variant aliases.
     * LTspice authors sometimes comment out .param lines (*param vcc=5...)
     * because LTspice resolves parameters case-insensitively. ngspice needs
     * the .param active AND needs alias cards for case variants.
     * Example: *param vcc=5 with {VCC} references
     *   -> .param vcc=5  and  .param VCC=vcc (alias card) */
    {
        /* Pass 1: uncomment *param lines */
        for (card = oldcard; card; card = card->nextcard) {
            char *cl = card->line;
            if (!cl) continue;

            /* Check for *param or * param (with optional leading whitespace) */
            char *lp = cl;
            while (*lp == ' ' || *lp == '\t') lp++;
            if (lp[0] != '*' || lp[1] == '\0') continue;
            char *pp = lp + 1;
            while (*pp == ' ' || *pp == '\t') pp++;
            if (strncasecmp(pp, "param", 5) != 0) continue;
            if (pp[5] != ' ' && pp[5] != '\t' && pp[5] != '=') continue;

            /* Uncomment by stripping the * (and any space before it) and adding .
             * *param vcc=5 -> .param vcc=5 */
            {
            size_t off = (pp - 1) - cl;  /* offset to the * character */
            if (off > 0 && (cl[off-1] == ' ' || cl[off-1] == '\t'))
                off--;  /* also remove trailing whitespace before * */
            char *result = tmalloc(strlen(cl + off + 1) + 2);
            result[0] = '.';
            strcpy(result + 1, cl + off + 1);
            tfree(card->line);
            card->line = result;
            }
        }

        /* Pass 2: inject case-variant alias cards */
        for (card = oldcard; card; card = card->nextcard) {
            char *cl = card->line;
            if (!cl) continue;

            /* Check current card for .param declaration */
            if (cl[0] == '.' && strncasecmp(cl + 1, "param", 5) == 0) {
                /* Check if any {identifier} in later body cards of this subcircuit
                 * matches one of these params case-insensitively */
                char *ps_copy = copy(cl + 7);
                char *ps = ps_copy;
                char *tok = gettok(&ps);
                while (tok) {
                    char *eq = strchr(tok, '=');
                    if (!eq) { tfree(tok); tok = gettok(&ps); continue; }
                    *eq = '\0';

                    /* Scan forward for {identifier} case variants */
                    struct card *scan = card->nextcard;
                    while (scan && scan->line) {
                        if (strncasecmp(scan->line, ".ends", 5) == 0) break;
                        char *scl = scan->line;
                        char found_ident[256];
                        bool found = false;
                        for (char *p = scl; *p; p++) {
                            if (*p == '{') {
                                char *end = strchr(p + 1, '}');
                                if (!end) continue;
                                size_t nlen = end - (p + 1);
                                if (nlen == 0) continue;

                                /* Check if the entire {content} is a simple identifier */
                                if (nlen < sizeof(found_ident)) {
                                    strncpy(found_ident, p + 1, nlen);
                                    found_ident[nlen] = '\0';
                                    if (strcasecmp(tok, found_ident) == 0 && strcmp(tok, found_ident) != 0) {
                                        found = true;
                                        break;
                                    }
                                }

                                /* Or tokenize the expression inside {} to find embedded identifiers */
                                {
                                    char expr[1024];
                                    if (nlen < sizeof(expr)) {
                                        strncpy(expr, p + 1, nlen);
                                        expr[nlen] = '\0';
                                        char *xp = expr;
                                        while (*xp) {
                                            while (*xp && !isalnum_c(*xp) && *xp != '_') xp++;
                                            if (!*xp) break;
                                            char *tok_start = xp;
                                            while (*xp && (isalnum_c(*xp) || *xp == '_')) xp++;
                                            size_t tlen = xp - tok_start;
                                            char scratch[256];
                                            if (tlen >= sizeof(scratch)) continue;
                                            strncpy(scratch, tok_start, tlen);
                                            scratch[tlen] = '\0';
                                            if (strcasecmp(tok, scratch) == 0 && strcmp(tok, scratch) != 0) {
                                                strncpy(found_ident, scratch, sizeof(found_ident)-1);
                                                found_ident[sizeof(found_ident)-1] = '\0';
                                                found = true;
                                                break;
                                            }
                                        }
                                        if (found) break;
                                    }
                                }
                            }
                        }
                        if (found) {
                            /* Inject alias card right before scan */
                            struct card *alias = TMALLOC(struct card, 1);
                            alias->line = tprintf(".param %s=%s", found_ident, tok);
                            struct card *prev = card;
                            while (prev && prev->nextcard != scan)
                                prev = prev->nextcard;
                            if (prev) {
                                alias->nextcard = prev->nextcard;
                                prev->nextcard = alias;
                            }
                        }
                        scan = scan->nextcard;
                    }
                    tfree(tok);
                    tok = gettok(&ps);
                }
                tfree(ps_copy);
            }
        }
    }

    /* Replace 1/Gmin (case-insensitive) in .param lines with 1e12.
     * Gmin is a SPICE engine constant (minimum conductance, default 1e-12)
     * but ngspice does not expose it as a user-accessible .param variable.
     * LTspice subcircuits like Lrememb use 1/Gmin for high-impedance bias
     * resistors. Replacing with 1e12 (= 1/1e-12) preserves the intent. */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (cl[0] != '.' || strncasecmp(cl + 1, "param", 5) != 0) continue;
        if (!strstr(cl, "/Gmin") && !strstr(cl, "/gmin") && !strstr(cl, "/GMIN")) continue;
        char *result = tmalloc(strlen(cl) + 1);
        char *dst = result;
        char *src = cl;
        while (*src) {
            if ((src[0] == '/' || src[0] == '1') &&
                strncasecmp(src[0] == '/' ? src + 1 : src, "Gmin", 4) == 0 &&
                !isalnum_c(src[5])) {
                /* Check for 1/Gmin pattern */
                if (src[0] == '1' && src[1] == '/' &&
                    strncasecmp(src + 2, "Gmin", 4) == 0) {
                    memcpy(dst, "1e12", 4);
                    dst += 4;
                    src += 6;
                } else if (src[0] == '/' &&
                    strncasecmp(src + 1, "Gmin", 4) == 0) {
                    *dst++ = '/';
                    memcpy(dst, "1e12", 4);
                    dst += 4;
                    src += 5;
                } else {
                    *dst++ = *src++;
                }
            } else {
                *dst++ = *src++;
            }
        }
        *dst = '\0';
        tfree(card->line);
        card->line = result;
    }

    /* Pass 3: inject case-variant aliases for subcircuit header parameters.
     * Subcircuit params like D=0 in ".subckt Rwire n1 n2 D=0" are case-sensitive
     * in ngspice but LTspice resolves case-insensitively. Scan body cards for
     * case-variant identifiers in {expressions} and inject .param alias cards. */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (!ciprefix(".subckt", cl)) continue;
        /* Tokenize the .subckt line: .subckt name pin1 pin2 ... param=val ... */
        char *scopy = copy(cl);
        char *sp = scopy;
        gettok(&sp); /* skip .subckt */
        gettok(&sp); /* skip subckt name */
        /* Collect pin names (tokens without '=') until we hit param=val pairs */
        char *pin;
        while ((pin = gettok(&sp)) != NULL) {
            if (strchr(pin, '=')) {
                /* This is a subcircuit parameter */
                char *eq = strchr(pin, '=');
                *eq = '\0';
                char *pname = pin;
                /* Scan body cards until .ends for case-variant {ident} or expression ident */
                struct card *scan = card->nextcard;
                while (scan && scan->line) {
                    if (strncasecmp(scan->line, ".ends", 5) == 0) break;
                    char *scl = scan->line;
                    char found_ident[256];
                    bool found = false;
                    for (char *cp = scl; *cp; cp++) {
                        if (*cp == '{') {
                            char *end = strchr(cp + 1, '}');
                            if (!end) continue;
                            size_t nlen = end - (cp + 1);
                            if (nlen == 0) continue;
                            /* Check entire {content} as identifier */
                            if (nlen < sizeof(found_ident)) {
                                strncpy(found_ident, cp + 1, nlen);
                                found_ident[nlen] = '\0';
                                if (strcasecmp(pname, found_ident) == 0 && strcmp(pname, found_ident) != 0) {
                                    found = true;
                                    break;
                                }
                            }
                            /* Tokenize expression for embedded identifiers */
                            {
                                char expr[1024];
                                if (nlen < sizeof(expr)) {
                                    strncpy(expr, cp + 1, nlen);
                                    expr[nlen] = '\0';
                                    char *xp = expr;
                                    while (*xp) {
                                        while (*xp && !isalnum_c(*xp) && *xp != '_') xp++;
                                        if (!*xp) break;
                                        char *tok_start = xp;
                                        while (*xp && (isalnum_c(*xp) || *xp == '_')) xp++;
                                        size_t tlen = xp - tok_start;
                                        char scratch[256];
                                        if (tlen >= sizeof(scratch)) continue;
                                        strncpy(scratch, tok_start, tlen);
                                        scratch[tlen] = '\0';
                                        if (strcasecmp(pname, scratch) == 0 && strcmp(pname, scratch) != 0) {
                                            strncpy(found_ident, scratch, sizeof(found_ident)-1);
                                            found_ident[sizeof(found_ident)-1] = '\0';
                                            found = true;
                                            break;
                                        }
                                    }
                                    if (found) break;
                                }
                            }
                        }
                    }
                    if (found) {
                        struct card *alias = TMALLOC(struct card, 1);
                        alias->line = tprintf(".param %s=%s", found_ident, pname);
                        struct card *prev = card;
                        while (prev && prev->nextcard != scan)
                            prev = prev->nextcard;
                        if (prev) {
                            alias->nextcard = prev->nextcard;
                            prev->nextcard = alias;
                        }
                    }
                    scan = scan->nextcard;
                }
            }
            tfree(pin);
        }
        tfree(scopy);
    }

    /* Pass 4: inject default .param values for undeclared {ident} references in
     * subcircuit bodies. LTspice treats undeclared params as having implicit
     * defaults (0 or 1 depending on context), but ngspice errors on undefined
     * parameters. Scan each subcircuit's body for {ident} patterns, collect
     * declared param names (from .subckt header params and .param cards), and
     * inject .param ident=1 for any undeclared reference. */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (!ciprefix(".subckt", cl)) continue;

        /* Collect declared param names from this subcircuit */
        char *scopy = copy(cl);
        char *sp = scopy;
        gettok(&sp); /* skip .subckt */
        gettok(&sp); /* skip name */

        /* Use a simple fixed-size array for param names (max 64 params) */
        char *declared[64];
        int nd = 0;

        /* Collect from .subckt header */
        char *tok;
        while ((tok = gettok(&sp)) != NULL && nd < 64) {
            if (strchr(tok, '=')) {
                char *eq = strchr(tok, '=');
                *eq = '\0';
                declared[nd++] = copy(tok);
            }
            tfree(tok);
        }
        tfree(scopy);

        /* Also scan body .param cards for additional declared params */
        struct card *scanp = card->nextcard;
        while (scanp && scanp->line) {
            if (strncasecmp(scanp->line, ".ends", 5) == 0) break;
            char *scl = scanp->line;
            if (scl[0] == '.' && strncasecmp(scl + 1, "param", 5) == 0) {
                char *pcopy = copy(scl + 7);
                char *pp = pcopy;
                char *ptok;
                while ((ptok = gettok(&pp)) != NULL && nd < 64) {
                    char *eq = strchr(ptok, '=');
                    if (eq) {
                        *eq = '\0';
                        bool already = false;
                        for (int i = 0; i < nd; i++) {
                            if (strcasecmp(declared[i], ptok) == 0) {
                                already = true;
                                break;
                            }
                        }
                        if (!already)
                            declared[nd++] = copy(ptok);
                    }
                    tfree(ptok);
                }
                tfree(pcopy);
            }
            scanp = scanp->nextcard;
        }

        /* Now scan body for {ident} patterns where ident is a simple identifier */
        scanp = card->nextcard;
        while (scanp && scanp->line) {
            if (strncasecmp(scanp->line, ".ends", 5) == 0) break;
            char *scl = scanp->line;
            for (char *cp = scl; *cp; cp++) {
                if (*cp == '{') {
                    char *end = strchr(cp + 1, '}');
                    if (!end) continue;
                    size_t nlen = end - (cp + 1);
                    if (nlen == 0 || nlen > 255) continue;

                    /* Check if it's a simple identifier */
                    bool simple = true;
                    for (size_t i = 0; i < nlen; i++) {
                        char c = cp[1 + i];
                        if (!isalnum_c(c) && c != '_') { simple = false; break; }
                    }
                    /* Extract simple identifier or tokenize expression */
                    {
                    char content[256];
                    strncpy(content, cp + 1, nlen);
                    content[nlen] = '\0';

                    if (simple) {
                        bool found = false;
                        for (int i = 0; i < nd; i++) {
                            if (strcasecmp(declared[i], content) == 0) {
                                found = true;
                                break;
                            }
                        }
                        if (!found && nd < 64) {
                            declared[nd++] = copy(content);
                            const char *defval = (strcasecmp(content, "temp") == 0) ? "27" : "1";
                            char *pline = tprintf(".param %s=%s", content, defval);
                            insert_new_line(card, pline, 0, card->linenum_orig, card->linesource);
                        }
                    } else {
                        /* Expression - tokenize to find embedded identifiers */
                        char *xp = content;
                        while (*xp) {
                            while (*xp && !isalnum_c(*xp) && *xp != '_') xp++;
                            if (!*xp) break;
                            char *tok_start = xp;
                            while (*xp && (isalnum_c(*xp) || *xp == '_')) xp++;
                            size_t tlen = xp - tok_start;
                            char scratch[256];
                            if (tlen >= sizeof(scratch)) continue;
                            strncpy(scratch, tok_start, tlen);
                            scratch[tlen] = '\0';

                            /* Skip if it starts with a digit or '.' (number or scaled number like 1u, 10k) */
                            if (isdigit_c(scratch[0]) || scratch[0] == '.') continue;

                            bool found = false;
                            for (int i = 0; i < nd; i++) {
                                if (strcasecmp(declared[i], scratch) == 0) {
                                    found = true;
                                    break;
                                }
                            }
                            if (strcasecmp(scratch, "vt") == 0 ||
                                strcasecmp(scratch, "pi") == 0 ||
                                strcasecmp(scratch, "v") == 0) found = true;

                            if (!found && nd < 64) {
                                declared[nd++] = copy(scratch);
                                const char *defval = (strcasecmp(scratch, "temp") == 0) ? "27" : "1";
                                char *pline = tprintf(".param %s=%s", scratch, defval);
                            insert_new_line(card, pline, 0, card->linenum_orig, card->linesource);
                            }
                        }
                    }
                    }

                    cp = end;
                }
            }
            scanp = scanp->nextcard;
        }

        for (int i = 0; i < nd; i++)
            tfree(declared[i]);
    }

    /* Pass 5: fix divide-by-zero in R/L/C element expressions.
     * When a .subckt header param defaults to 0 and is used in a
     * denominator in an R/L/C body card {expr}, the expression
     * evaluates to inf. Inject .param <ident>=1u to provide a
     * small non-zero default. */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (!ciprefix(".subckt", cl)) continue;

        /* Collect header params with =0 default */
        char scopy[1024];
        if (strlen(cl) >= sizeof(scopy)) continue;
        strcpy(scopy, cl);
        char *sp = scopy;
        gettok(&sp); /* .subckt */
        gettok(&sp); /* name */
        char *zero_params[64];
        int nz = 0;
        char *tok;
        while ((tok = gettok(&sp)) != NULL && nz < 64) {
            char *eq = strchr(tok, '=');
            if (eq) {
                *eq = '\0';
                char *val = eq + 1;
                if (strcmp(val, "0") == 0 || strcmp(val, "0.0") == 0) {
                    zero_params[nz++] = copy(tok);
                }
            }
            tfree(tok);
        }

        if (nz == 0) continue; /* no zero-valued params to fix */

        /* Scan body for R/L/C cards with {expr} containing / */
        struct card *scan = card->nextcard;
        while (scan && scan->line) {
            if (strncasecmp(scan->line, ".ends", 5) == 0) break;
            char c0 = tolower_c(scan->line[0]);
            if (c0 != 'r' && c0 != 'l' && c0 != 'c') { scan = scan->nextcard; continue; }
            char *scl = scan->line;
            char *brace_start = strchr(scl, '{');
            if (!brace_start) { scan = scan->nextcard; continue; }
            char *brace_end = strchr(brace_start + 1, '}');
            if (!brace_end) { scan = scan->nextcard; continue; }
            if (!strchr(brace_start, '/')) { scan = scan->nextcard; continue; }

            size_t nlen = brace_end - (brace_start + 1);
            if (nlen == 0 || nlen > 255) { scan = scan->nextcard; continue; }
            char content[256];
            strncpy(content, brace_start + 1, nlen);
            content[nlen] = '\0';

            /* Tokenize expression to find identifiers */
            char *xp = content;
            while (*xp) {
                while (*xp && !isalnum_c(*xp) && *xp != '_') xp++;
                if (!*xp) break;
                char *tokstart = xp;
                while (*xp && (isalnum_c(*xp) || *xp == '_')) xp++;
                size_t tlen = xp - tokstart;
                char scratch[256];
                if (tlen >= sizeof(scratch)) break;
                strncpy(scratch, tokstart, tlen);
                scratch[tlen] = '\0';

                /* Skip if it starts with a digit or is a reserved name */
                if (isdigit_c(scratch[0]) || scratch[0] == '.') continue;
                if (strcasecmp(scratch, "temp") == 0 ||
                    strcasecmp(scratch, "vt") == 0 ||
                    strcasecmp(scratch, "pi") == 0 ||
                    strcasecmp(scratch, "v") == 0) continue;

                /* Check if this identifier matches a zero-valued header param */
                for (int zi = 0; zi < nz; zi++) {
                    if (strcasecmp(zero_params[zi], scratch) == 0) {
                        /* Inject .param with small non-zero default */
                        char *pline = tprintf(".param %s=1u", zero_params[zi]);
                        insert_new_line(card, pline, 0, card->linenum_orig, card->linesource);
                        /* Remove from zero_params to avoid duplicate injection */
                        tfree(zero_params[zi]);
                        zero_params[zi] = zero_params[--nz];
                        break;
                    }
                }
            }
            scan = scan->nextcard;
        }

        for (int i = 0; i < nz; i++)
            tfree(zero_params[i]);
    }

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

    /* Strip/replace {braces} from .param value expressions: LTspice allows
     * {identifier} and {expression} inside .param statements, but ngspice does
     * not accept curly braces in .param values. For simple identifiers, strip
     * the braces entirely. For expressions with operators, replace {} with ()
     * to preserve operator precedence.
     * Example: .param Vh0=({ion}-{ioff})/({ion}+{ioff})/2
     *   ->   .param Vh0=(ion-ioff)/(ion+ioff)/2
     * Example: .param ron=x*(14.3/{vdd-0.7})**0.75
     *   ->   .param ron=x*(14.3/(vdd-0.7))**0.75 */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        size_t llen = strlen(cl);
        if (llen < 7) continue;
        if (cl[0] != '.') continue;
        if (strncasecmp(cl + 1, "param", 5) != 0) continue;
        if (cl[6] != ' ' && cl[6] != '\t') continue;

        int has_open = 0;
        for (int i = 0; cl[i]; i++) {
            if (cl[i] == '{') { has_open = 1; break; }
        }
        if (!has_open) continue;

        /* Result may be up to original length + 1 per brace pair unchanged */
        char *result = tmalloc(llen * 2 + 1);
        char *dst = result;
        char *src = cl;

        while (*src) {
            if (*src == '{') {
                char *end = strchr(src + 1, '}');
                if (end && end > src + 1) {
                    int is_ident = 1;
                    for (char *p = src + 1; p < end; p++) {
                        if (!isalnum_c((unsigned char)*p) && *p != '_') {
                            is_ident = 0;
                            break;
                        }
                    }
                    if (is_ident) {
                        size_t ident_len = end - (src + 1);
                        memcpy(dst, src + 1, ident_len);
                        dst += ident_len;
                        src = end + 1;
                        continue;
                    } else {
                        /* Expression with operators: replace {} with () */
                        size_t expr_len = end - (src + 1);
                        *dst++ = '(';
                        memcpy(dst, src + 1, expr_len);
                        dst += expr_len;
                        *dst++ = ')';
                        src = end + 1;
                        continue;
                    }
                }
            }
            *dst++ = *src++;
        }
        *dst = '\0';
        /* Only update if something changed */
        if (strcmp(result, cl) != 0) {
            tfree(card->line);
            card->line = result;
        } else {
            tfree(result);
        }
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
                    /* Check context: scan backwards to find start of the token.
                     * If the token starts with a letter or '_', it's a name (skip).
                     * If it starts with a digit or '=', it's a value (transform). */
                    char *tok_start = src;
                    while (tok_start > cl && (isdigit_c(*(tok_start-1)) || *(tok_start-1) == '.'))
                        tok_start--;
                    if (tok_start > cl && (isalpha_c(*(tok_start-1)) || *(tok_start-1) == '_')) {
                        /* Part of a name - do not transform */
                        *dst++ = *src++;
                        continue;
                    }
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

    /* Strip LTspice-only Rpar=, Cpar=, tripdt=, tripdv= from B-sources 
     * and add parallel R/C components if present to avoid singular matrix. */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (cl[0] != 'b' && cl[0] != 'B') continue;
        if (cl[1] == '.' || cl[1] == '\0') continue;

        char bname[128], n1[128], n2[128];
        if (sscanf(cl, "%127s %127s %127s", bname, n1, n2) < 3) continue;

        char rpar_str[64] = "", cpar_str[64] = "";
        int any_stripped = 0;
        char *result = tmalloc(strlen(cl) + 1);
        char *dst = result;
        char *src = cl;
        while (*src) {
            bool is_rpar = (tolower_c(src[0]) == 'r' && tolower_c(src[1]) == 'p' &&
                            tolower_c(src[2]) == 'a' && tolower_c(src[3]) == 'r' && src[4] == '=');
            bool is_cpar = (tolower_c(src[0]) == 'c' && tolower_c(src[1]) == 'p' &&
                            tolower_c(src[2]) == 'a' && tolower_c(src[3]) == 'r' && src[4] == '=');
            bool is_tripdt = (tolower_c(src[0]) == 't' && tolower_c(src[1]) == 'r' &&
                              tolower_c(src[2]) == 'i' && tolower_c(src[3]) == 'p' &&
                              tolower_c(src[4]) == 'd' && tolower_c(src[5]) == 't' && src[6] == '=');
            bool is_tripdv = (tolower_c(src[0]) == 't' && tolower_c(src[1]) == 'r' &&
                              tolower_c(src[2]) == 'i' && tolower_c(src[3]) == 'p' &&
                              tolower_c(src[4]) == 'd' && tolower_c(src[5]) == 'v' && src[6] == '=');

            if (is_rpar || is_cpar || is_tripdt || is_tripdv) {
                int kwlen = (is_tripdt || is_tripdv) ? 7 : 5;
                const char *val_start = src + kwlen;
                char val_buf[64];
                int k = 0;
                while (val_start[k] && val_start[k] != ' ' && val_start[k] != '\t' && val_start[k] != ';') {
                    if (k < 63) val_buf[k] = val_start[k];
                    k++;
                }
                val_buf[k] = '\0';
                
                if (is_rpar) strcpy(rpar_str, val_buf);
                else if (is_cpar) strcpy(cpar_str, val_buf);

                src += kwlen + k;
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
            
            struct card *insert_pos = card;
            if (rpar_str[0]) {
                char *rline = tprintf("R_lt_%s_rpar %s %s %s", bname+1, n1, n2, rpar_str);
                insert_pos = insert_new_line(insert_pos, rline, card->linenum, card->linenum_orig, card->linesource);
            }
            if (cpar_str[0]) {
                char *cline = tprintf("C_lt_%s_cpar %s %s %s", bname+1, n1, n2, cpar_str);
                insert_pos = insert_new_line(insert_pos, cline, card->linenum, card->linenum_orig, card->linesource);
            }
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
        int has_val2 = 0;
        bool skip_val2_as_keyword = 0;
        int has_parens = 0;
        char *val2_end = NULL;
        /* If the "second value" contains an '=' it is actually a separate
         * keyword=value pair (e.g. TC=9m, TC2=5.5u) — don't consume it. */
        if (has_comma && strchr(val2_start, '=')) {
            skip_val2_as_keyword = 1;
            has_val2 = 0;
            has_comma = 0;
        } else {
            if (*val2_start == '(') {
                has_parens = 1;
                val2_start++;
                while (*val2_start == ' ' || *val2_start == '\t') val2_start++;
            }
            val2_end = val2_start;
            if (*val2_end) {
                while (*val2_end && *val2_end != ' ' && *val2_end != '\t' && *val2_end != ')' &&
                       *val2_end != ';' && *val2_end != ',') val2_end++;
            }
            has_val2 = (val2_end > val2_start);
        }
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
            if (skip_val2_as_keyword) {
                /* Skip comma and whitespace after val1 when the "second value"
                 * was actually a separate keyword (e.g. TC2=val) */
                while (*suf_start == ' ' || *suf_start == '\t' || *suf_start == ',') suf_start++;
            }
        }
        if (*suf_start) {
            if (!has_val2) *d++ = ' ';
            size_t suf = strlen(suf_start);
            memcpy(d, suf_start, suf + 1);
        } else {
            *d = '\0';
        }
        tfree(card->line);
        card->line = result;
    }

    /* Convert TC1=value, TC2=value to tc1=value, tc2=value on R/L/C lines */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        char c0 = cl[0];
        if (c0 != 'r' && c0 != 'R' && c0 != 'c' && c0 != 'C' &&
            c0 != 'l' && c0 != 'L') continue;
        if (cl[1] == '.' || cl[1] == '\0') continue;
        /* Replace TC1= with tc1= */
        char *p = cl;
        int replaced = 0;
        while ((p = strstr(p, "TC1=")) != NULL) {
            if (p == cl || (p[-1] == ' ' || p[-1] == '\t')) {
                p[0] = 't'; p[1] = 'c'; p[2] = '1';
                replaced = 1;
                p += 4;
            } else {
                p += 1;
            }
        }
        p = cl;
        while ((p = strstr(p, "TC2=")) != NULL) {
            if (p == cl || (p[-1] == ' ' || p[-1] == '\t')) {
                p[0] = 't'; p[1] = 'c'; p[2] = '2';
                replaced = 1;
                p += 4;
            } else {
                p += 1;
            }
        }
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

    /* Convert LTspice F-source value={expr} to B-source I = expr.
     * LTspice syntax: F<name> <n+> <n-> value={<expr>}
     * ngspice:         B<name> <n+> <n-> I = <expr>
     * The B-source runtime evaluator handles the expression correctly,
     * while the ngspice F-source parser cannot handle value={} syntax. */
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        if (cl[0] != 'f' && cl[0] != 'F') continue;
        if (cl[1] == '.' || cl[1] == '\0') continue;
        char *value_kw = strstr(cl + 1, "value=");
        if (!value_kw) continue;
        /* Find the opening brace after value= */
        char *brace = value_kw + 6;
        while (*brace == ' ' || *brace == '\t') brace++;
        if (*brace != '{') continue;
        char *brace_start = brace;
        char *brace_end = brace + 1;
        int depth = 1;
        while (*brace_end && depth > 0) {
            if (*brace_end == '{') depth++;
            else if (*brace_end == '}') depth--;
            if (depth > 0) brace_end++;
        }
        if (depth != 0) continue;
        /* Extract nodes: <name> <n+> <n-> */
        char *cut = cl;
        char *name_tok = gettok(&cut);
        char *n1_tok = gettok(&cut);
        char *n2_tok = gettok(&cut);
        if (!name_tok || !n1_tok || !n2_tok) {
            tfree(name_tok); tfree(n1_tok); tfree(n2_tok);
            continue;
        }
        /* Extract expression inside braces */
        size_t expr_len = brace_end - brace_start - 1;
        char *expr = tmalloc(expr_len + 1);
        memcpy(expr, brace_start + 1, expr_len);
        expr[expr_len] = '\0';
        /* Build B-source line: B<name> <n+> <n-> I = <expr> */
        char *bsrc = tprintf("B%s %s %s I = %s", name_tok, n1_tok, n2_tok, expr);
        tfree(card->line);
        card->line = bsrc;
        tfree(name_tok); tfree(n1_tok); tfree(n2_tok); tfree(expr);
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
    /* add predefined params TEMP, VT, GMIN to beginning of deck */
    new_str = copy(".param temp = 27");
    nextcard = insert_new_line(nextcard, new_str, 6, 0, "internal");
    new_str = copy(".param vt = 0.025865");
    nextcard = insert_new_line(nextcard, new_str, 7, 0, "internal");
    new_str = copy(".param gmin = 1e-12");
    nextcard = insert_new_line(nextcard, new_str, 8, 0, "internal");
    /* add funcs limit, pwr, pwrs, stp, if, int */
    new_str = copy(".func limit(x, a, b) { ternary_fcn(a > b, max(min(x, a), b), max(min(x, b), a)) }");
    nextcard = insert_new_line(nextcard, new_str, 9, 0, "internal");
    new_str = copy(".func pwr(x, a) { pow(x, a) }");
    nextcard = insert_new_line(nextcard, new_str, 10, 0, "internal");
    new_str = copy(".func pwrs(x, a) { sgn(x) * pow(x, a) }");
    nextcard = insert_new_line(nextcard, new_str, 11, 0, "internal");
    new_str = copy(".func stp(x) { u(x) }");
    nextcard = insert_new_line(nextcard, new_str, 12, 0, "internal");
    new_str = copy(".func inv(x) { (!(x)) }");
    nextcard = insert_new_line(nextcard, new_str, 13, 0, "internal");
    new_str = copy(".func if(a, b, c) {ternary_fcn( a , b , c )}");
    nextcard = insert_new_line(nextcard, new_str, 13, 0, "internal");
    new_str = copy(".func int(x) { sgn(x)*floor(abs(x)) }");
    nextcard = insert_new_line(nextcard, new_str, 14, 0, "internal");
    nextcard->nextcard = oldcard;

    /* Inject .param temp = 27 inside each subcircuit so that {temp} from
     * LTspice resolves to the default simulation temperature. */
    for (card = nextcard; card; card = card->nextcard) {
        char *cut_line = card->line;
        if (ciprefix(".subckt", cut_line)) {
            new_str = copy(".param temp = 27");
            nextcard = insert_new_line(card, new_str, 0, card->linenum_orig, card->linesource);
            new_str = copy(".param vt = 0.025865");
            insert_new_line(nextcard, new_str, 1, card->linenum_orig, card->linesource);
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

/* ---- LTspice device type helpers ---- */

static int ltspice_device_pin_count(const char *devtype)
{
    char buf[64];
    int i;
    for (i = 0; i < (int)strlen(devtype) && i < 63; i++)
        buf[i] = toupper_c(devtype[i]);
    buf[i] = '\0';
    const char *upper = buf;

    if (strcmp(upper, "COUNTER") == 0) return 4;
    if (strcmp(upper, "SCHMITT") == 0 || strcmp(upper, "SCHMITT_BUF") == 0) return 2;
    if (strcmp(upper, "BUF") == 0 || strcmp(upper, "BUFFER") == 0) return 2;
    if (strcmp(upper, "NOT") == 0 || strcmp(upper, "INV") == 0 || strcmp(upper, "INVERTER") == 0) return 2;
    if (strcmp(upper, "AND") == 0) return 3;
    if (strcmp(upper, "OR") == 0) return 3;
    if (strcmp(upper, "NAND") == 0) return 3;
    if (strcmp(upper, "NOR") == 0) return 3;
    if (strcmp(upper, "XOR") == 0) return 3;
    if (strcmp(upper, "XNOR") == 0) return 3;
    if (strcmp(upper, "DFF") == 0 || strcmp(upper, "D_FF") == 0 || strcmp(upper, "DFLOP") == 0) return 4;
    if (strcmp(upper, "JKFF") == 0 || strcmp(upper, "JK_FF") == 0) return 4;
    if (strcmp(upper, "SRFF") == 0 || strcmp(upper, "SR_FF") == 0 || strcmp(upper, "SRFLOP") == 0) return 4;
    if (strcmp(upper, "DLATCH") == 0 || strcmp(upper, "D_LATCH") == 0) return 3;
    if (strcmp(upper, "SRLATCH") == 0 || strcmp(upper, "SR_LATCH") == 0) return 3;
    if (strcmp(upper, "TFF") == 0 || strcmp(upper, "T_FF") == 0) return 3;
    if (strcmp(upper, "MODULATOR") == 0) return 4;
    if (strcmp(upper, "SAMPLEHOLD") == 0) return 3;
    if (strcmp(upper, "VARISTOR") == 0) return 4;
    if (strcmp(upper, "PHASEDET") == 0 || strcmp(upper, "PHIDET") == 0) return 4;
    if (strcmp(upper, "TRISTATE") == 0) return 3;

    return 0;
}

static int ltspice_pin_index(const char *devtype, int ngspice_port)
{
    char buf[64];
    int i;
    for (i = 0; i < (int)strlen(devtype) && i < 63; i++)
        buf[i] = toupper_c(devtype[i]);
    buf[i] = '\0';
    const char *upper = buf;

    /* SRFLOP uses LTspice pins 2(S)/3(R)/6(Q)/7(/Q), not 0/1/6/7 */
    bool is_srflop = (strcmp(upper, "SRFLOP") == 0 ||
                      strcmp(upper, "SRFF") == 0 ||
                      strcmp(upper, "SR_FF") == 0);

    if (is_srflop) {
        /* d_srlatch ports: S(0), R(1), enable(2), aset(3), areset(4), Q(5), /Q(6)
         * LTspice SRFLOP: S on pin 0 (A), R on pin 1 (B), Q on pin 6, /Q on pin 7.
         * Remaining LTspice pins 2-5 are unused in SRFLOP (tie to GND). */
        if (ngspice_port == 0) return 0;  /* S → LTspice pin 0 (A) */
        if (ngspice_port == 1) return 1;  /* R → LTspice pin 1 (B) */
        if (ngspice_port == 2) return -1; /* enable → unused (B-source injected) */
        if (ngspice_port == 3) return -1; /* aset → unused */
        if (ngspice_port == 4) return -1; /* areset → unused */
        if (ngspice_port == 5) return 6;  /* Q → LTspice pin 6 */
        if (ngspice_port == 6) return 7;  /* /Q → LTspice pin 7 */
        return -1;
    }

    int pin_count = ltspice_device_pin_count(devtype);

    if (pin_count == 2) {
        if (ngspice_port == 0) return 0;
        if (ngspice_port == 1) return 6;
    } else if (pin_count == 3) {
        if (ngspice_port == 0) return 0;
        if (ngspice_port == 1) return 1;
        if (ngspice_port == 2) return 6;
    } else if (pin_count == 4) {
        if (ngspice_port < 2) return ngspice_port;
        if (ngspice_port == 2) return 6;
        if (ngspice_port == 3) return 7;
    }

    return ngspice_port;
}

/* ---- LTspice inline parameter parsing ---- */

typedef struct {
    bool present;
    double vhigh, vlow, trise, tfall, ref, vt, td;
    double cycles;
    double mark, space;
    double vref, roff, rclamp;
    double vh;
    bool has_vhigh, has_vlow, has_trise, has_tfall, has_ref, has_vt, has_td;
    bool has_cycles;
    bool has_mark, has_space;
    bool has_vref, has_roff, has_rclamp;
    bool has_vh;
} LtspiceInlineParams;

static bool parse_eng_number(const char *str, double *result)
{
    char *end;
    *result = strtod(str, &end);
    if (end == str) return false;
    if (*end == '\0') return true;

    int consumed = 0;
    double scale = 1.0;

    switch (*end) {
    case 'T': case 't': scale = 1e12; consumed = 1; break;
    case 'G': case 'g': scale = 1e9; consumed = 1; break;
    case 'K': case 'k': scale = 1e3; consumed = 1; break;
    case 'M': case 'm':
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

    if (*end != '\0') {
        char c = *end;
        if (c == 'V' || c == 'v' || c == 'A' || c == 'a' ||
            c == 'W' || c == 'w') {
            end++;
        } else if (c == 'H' || c == 'h') {
            end++;
            if (toupper_c(*end) == 'Z') end++;
        } else if (c == 'F' || c == 'f') {
            if (consumed == 0) end++;
        } else if (c == 'O') {
            end++;
            if (toupper_c(end[0]) == 'H' && toupper_c(end[1]) == 'M') end += 3;
        } else if (c == 'S' || c == 's') {
            end++;
        }
    }

    return true;
}

static char *ltspice_map_a_model(const char *upper)
{
    if (strcmp(upper, "AND") == 0) return "d_and";
    if (strcmp(upper, "OR") == 0) return "d_or";
    if (strcmp(upper, "NAND") == 0) return "d_nand";
    if (strcmp(upper, "NOR") == 0) return "d_nor";
    if (strcmp(upper, "XOR") == 0) return "d_xor";
    if (strcmp(upper, "XNOR") == 0) return "d_xnor";
    if (strcmp(upper, "SRFLOP") == 0) return "d_srlatch";
    if (strcmp(upper, "SCHMITT") == 0 || strcmp(upper, "SCHMITT_BUF") == 0) return "adc_bridge";
    if (strcmp(upper, "BUF") == 0 || strcmp(upper, "BUFFER") == 0) return "d_buffer";
    if (strcmp(upper, "NOT") == 0 || strcmp(upper, "INV") == 0 || strcmp(upper, "INVERTER") == 0) return "d_inverter";
    if (strcmp(upper, "DFF") == 0 || strcmp(upper, "D_FF") == 0 || strcmp(upper, "DFLOP") == 0) return "d_dff";
    if (strcmp(upper, "JKFF") == 0 || strcmp(upper, "JK_FF") == 0) return "d_jkff";
    if (strcmp(upper, "SRFF") == 0 || strcmp(upper, "SR_FF") == 0) return "d_srff";
    if (strcmp(upper, "DLATCH") == 0 || strcmp(upper, "D_LATCH") == 0) return "d_dlatch";
    if (strcmp(upper, "SRLATCH") == 0 || strcmp(upper, "SR_LATCH") == 0) return "d_srlatch";
    if (strcmp(upper, "TFF") == 0 || strcmp(upper, "T_FF") == 0) return "d_tff";
    if (strcmp(upper, "COUNTER") == 0) return "d_fdiv";
    if (strcmp(upper, "MODULATOR") == 0) return "modulator";
    if (strcmp(upper, "SAMPLEHOLD") == 0) return "samplehold";
    if (strcmp(upper, "VARISTOR") == 0) return "varistor";
    return NULL;
}

static LtspiceInlineParams ltspice_parse_inline_params(const char *s)
{
    LtspiceInlineParams p = {0};
    char *tmp = copy(s);
    char *ptr = tmp;
    char *tok;

    p.vhigh = 5.0;
    p.vlow = 0.0;
    p.trise = 1e-9;
    p.tfall = 1e-9;
    p.ref = 2.5;
    p.vt = 2.5;
    p.vh = 0.001;
    p.td = 0.0;

    while ((tok = gettok(&ptr)) != NULL) {
        char *eq = strchr(tok, '=');
        if (!eq) { tfree(tok); continue; }
        *eq = '\0';
        const char *key = tok;
        const char *val = eq + 1;
        double num = 0.0;
        bool valid = parse_eng_number(val, &num);

        char keyup[64];
        int ki;
        for (ki = 0; ki < (int)sizeof(keyup)-1 && key[ki]; ki++)
            keyup[ki] = toupper_c(key[ki]);
        keyup[ki] = '\0';

        if (strcmp(keyup, "VHIGH") == 0 && valid) {
            p.vhigh = num; p.has_vhigh = true; p.present = true;
        } else if (strcmp(keyup, "VLOW") == 0 && valid) {
            p.vlow = num; p.has_vlow = true; p.present = true;
        } else if (strcmp(keyup, "TRISE") == 0 && valid) {
            p.trise = num; p.has_trise = true; p.present = true;
        } else if (strcmp(keyup, "TFALL") == 0 && valid) {
            p.tfall = num; p.has_tfall = true; p.present = true;
        } else if (strcmp(keyup, "REF") == 0 && valid) {
            p.ref = num; p.has_ref = true; p.present = true;
        } else if (strcmp(keyup, "VT") == 0 && valid) {
            p.vt = num; p.has_vt = true; p.present = true;
        } else if (strcmp(keyup, "VH") == 0 && valid) {
            p.vh = num; p.has_vh = true;
        } else if (strcmp(keyup, "TD") == 0 && valid) {
            p.td = num; p.has_td = true; p.present = true;
        } else if (strcmp(keyup, "CYCLES") == 0 && valid) {
            p.cycles = num; p.has_cycles = true; p.present = true;
        } else if (strcmp(keyup, "MARK") == 0 && valid) {
            p.mark = num; p.has_mark = true;
        } else if (strcmp(keyup, "SPACE") == 0 && valid) {
            p.space = num; p.has_space = true;
        } else if (strcmp(keyup, "VREF") == 0 && valid) {
            p.vref = num; p.has_vref = true;
        } else if (strcmp(keyup, "ROFF") == 0 && valid) {
            p.roff = num; p.has_roff = true;
        } else if (strcmp(keyup, "RCLAMP") == 0 && valid) {
            p.rclamp = num; p.has_rclamp = true;
        }
        tfree(tok);
    }
    tfree(tmp);
    return p;
}

static bool ltspice_pin_is_input(int lt_pin)
{
    return lt_pin >= 0 && lt_pin <= 4;
}

static bool ltspice_pin_is_output(int lt_pin)
{
    return lt_pin == 5 || lt_pin == 6 || lt_pin == 7;
}

/* In LTspice subcircuits, unused A-device pins are tied to a numeric GND
 * reference ("0" for global GND, or the subcircuit's GND pin like "1").
 * Heuristic: purely numeric nodes are treated as GND (inactive). */
static bool is_gnd_node(const char *node)
{
    if (!node) return true;
    if (strcmp(node, "0") == 0) return true;
    for (const char *p = node; *p; p++)
        if (!isdigit_c(*p)) return false;
    return true;
}

/* ---- LTspice A-device transform ---- */

/* do not modify oldcard address, insert everything after first line only */
void ltspice_compat_a(struct card *oldcard)
{
    struct card *card, *prev_card = NULL;
    int skip_control = 0;

    /* First pass: register model cards */
    struct vsmodels *modelsfound = NULL;
    for (card = oldcard; card; card = card->nextcard) {
        char *cl = card->line;
        if (!cl) continue;
        while (*cl == ' ' || *cl == '\t') cl++;
        if (*cl == '*' || *cl == '\0') continue;
        if (ciprefix(".control", cl)) { skip_control++; continue; }
        if (ciprefix(".endc", cl)) { skip_control--; continue; }
        if (skip_control > 0) continue;
        if (ciprefix(".model", cl)) {
            char *tmp = copy(cl);
            char *str = tmp;
            str = nexttok(str);
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
                        /* For space-separated format .model NAME type param=val ...
                         * the remaining tokens in str are the params. Read them all. */
                        char *rest = str;
                        while (*rest == ' ' || *rest == '\t') rest++;
                        if (*rest) {
                            char *end = rest + strlen(rest) - 1;
                            while (end > rest && (*end == ' ' || *end == '\t')) end--;
                            *(end + 1) = '\0';
                            params = copy(rest);
                        }
                    }
                    struct vsmodels *m = insert_new_model(modelsfound, modname, type, params, "top");
                    while (m && m->nextmodel) m = m->nextmodel;
                    if (m) m->cardptr = card;
                    /* Transform LTspice-specific D model params
                     *   Ron=value → RS=value   (series resistance)
                     *   Vrev=value → BV=value  (reverse breakdown voltage)
                     *   Roff=value → stripped  (off resistance, no ngspice equivalent)
                     *   Vfwd=value → stripped  (forward voltage drop, no ngspice equivalent)
                     *   Epsilon=value → stripped */
                    if (strcasecmp(type, "D") == 0 && params) {
                        double ron = 0, bv = 0;
                        bool needs_fix = false;
                        bool keep_other = true;
                        char parbuf[256];
                        snprintf(parbuf, sizeof(parbuf), "%s", params);
                        char *p = parbuf;
                        while (*p) {
                            while (*p == ' ' || *p == '\t' || *p == ',') p++;
                            if (strncasecmp(p, "Ron=", 4) == 0) {
                                p += 4;
                                if (!parse_eng_number(p, &ron)) ron = 0;
                                needs_fix = true;
                                while (*p && *p != ' ' && *p != '\t' && *p != ',') p++;
                            } else if (strncasecmp(p, "Vrev=", 5) == 0) {
                                p += 5;
                                if (!parse_eng_number(p, &bv)) bv = 0;
                                needs_fix = true;
                                while (*p && *p != ' ' && *p != '\t' && *p != ',') p++;
                            } else if (strncasecmp(p, "Roff=", 5) == 0 ||
                                       strncasecmp(p, "Vfwd=", 5) == 0 ||
                                       strncasecmp(p, "Epsilon=", 8) == 0 ||
                                       strncasecmp(p, "revEpsilon=", 11) == 0) {
                                needs_fix = true;
                                while (*p && *p != ' ' && *p != '\t' && *p != ',') p++;
                            } else p++;
                        }
                        if (needs_fix) {
                            char new_params[256] = "";
                            if (ron > 0)
                                snprintf(new_params + strlen(new_params), sizeof(new_params) - strlen(new_params),
                                         "RS=%.15g ", ron);
                            if (bv > 0)
                                snprintf(new_params + strlen(new_params), sizeof(new_params) - strlen(new_params),
                                         "BV=%.15g ", bv);
                            /* Trim trailing space */
                            size_t nl = strlen(new_params);
                            if (nl > 0 && new_params[nl-1] == ' ') new_params[nl-1] = '\0';
                            char new_model[512];
                            if (new_params[0])
                                snprintf(new_model, sizeof(new_model), ".model %s D(%s)", modname, new_params);
                            else
                                snprintf(new_model, sizeof(new_model), ".model %s D()", modname);
                            tfree(card->line);
                            card->line = copy(new_model);
                        }
                    }
                    /* Transform SW model Vh parameter: ngspice uses Vt±Vh,
                     * LTspice uses Vt±Vh/2.  Divide Vh by 2 for compatibility. */
                    if (strcasecmp(type, "SW") == 0 && params) {
                        double vh_val = 0;
                        bool has_vh = false;
                        char parbuf[512];
                        snprintf(parbuf, sizeof(parbuf), "%s", params);
                        char *scan = parbuf;
                        while (*scan) {
                            while (*scan == ' ' || *scan == '\t' || *scan == ',') scan++;
                            if (strncasecmp(scan, "Vh=", 3) == 0 ||
                                strncasecmp(scan, "VH=", 3) == 0) {
                                scan += 3;
                                if (parse_eng_number(scan, &vh_val)) {
                                    has_vh = true;
                                }
                                break;
                            }
                            while (*scan && *scan != ' ' && *scan != '\t' && *scan != ',') scan++;
                        }
                        if (has_vh) {
                            vh_val /= 2.0;
                            /* Rebuild the model line checking each param */
                            char new_params[512] = "";
                            scan = parbuf;
                            while (*scan) {
                                while (*scan == ' ' || *scan == '\t' || *scan == ',') {
                                    scan++;
                                }
                                if (!*scan) break;
                                const char *start = scan;
                                while (*scan && *scan != ' ' && *scan != '\t' && *scan != ',')
                                    scan++;
                                size_t len = (size_t)(scan - start);
                                if (len > 0) {
                                    char pname[128];
                                    size_t ncopy = len < sizeof(pname)-1 ? len : sizeof(pname)-1;
                                    memcpy(pname, start, ncopy);
                                    pname[ncopy] = '\0';
                                    /* Replace Vh with new value */
                                    if (strncasecmp(pname, "Vh=", 3) == 0) {
                                        if (new_params[0]) strcat(new_params, " ");
                                        char valbuf[64];
                                        snprintf(valbuf, sizeof(valbuf), "%.15g", vh_val);
                                        strcat(new_params, "Vh=");
                                        strcat(new_params, valbuf);
                                    } else {
                                        if (new_params[0]) strcat(new_params, " ");
                                        strncat(new_params, pname, sizeof(new_params)-strlen(new_params)-1);
                                    }
                                }
                            }
                            char new_model[1024];
                            snprintf(new_model, sizeof(new_model),
                                     ".model %s SW(%s)", modname, new_params);
                            tfree(card->line);
                            card->line = copy(new_model);
                        }
                    }
                    /* VSWITCH→SW model card transform */
                    if (strcasecmp(type, "VSWITCH") == 0 && params) {
                        double ron_val = 1.0, roff_val = 1.0e9;
                        double von_val = 1.0, voff_val = 0.0;
                        double vt_val = 0.0, vh_val = 0.001;
                        bool has_vt_vh = false, has_von_voff = false;
                        char parbuf[512];

                        snprintf(parbuf, sizeof(parbuf), "%s", params);

                        {
                            char *sp = parbuf;
                            while (*sp) {
                                while (*sp == ' ' || *sp == '\t' || *sp == ',') sp++;
                                if (strncasecmp(sp, "Ron=", 4) == 0) {
                                    sp += 4;
                                    parse_eng_number(sp, &ron_val);
                                    while (*sp && *sp != ' ' && *sp != '\t' && *sp != ',') sp++;
                                } else if (strncasecmp(sp, "Roff=", 5) == 0) {
                                    sp += 5;
                                    parse_eng_number(sp, &roff_val);
                                    while (*sp && *sp != ' ' && *sp != '\t' && *sp != ',') sp++;
                                } else if (strncasecmp(sp, "Vt=", 3) == 0) {
                                    sp += 3;
                                    parse_eng_number(sp, &vt_val);
                                    has_vt_vh = true;
                                    while (*sp && *sp != ' ' && *sp != '\t' && *sp != ',') sp++;
                                } else if (strncasecmp(sp, "Vh=", 3) == 0) {
                                    sp += 3;
                                    parse_eng_number(sp, &vh_val);
                                    has_vt_vh = true;
                                    while (*sp && *sp != ' ' && *sp != '\t' && *sp != ',') sp++;
                                } else if (strncasecmp(sp, "Von=", 4) == 0) {
                                    sp += 4;
                                    parse_eng_number(sp, &von_val);
                                    has_von_voff = true;
                                    while (*sp && *sp != ' ' && *sp != '\t' && *sp != ',') sp++;
                                } else if (strncasecmp(sp, "Voff=", 5) == 0) {
                                    sp += 5;
                                    parse_eng_number(sp, &voff_val);
                                    has_von_voff = true;
                                    while (*sp && *sp != ' ' && *sp != '\t' && *sp != ',') sp++;
                                } else {
                                    sp++;
                                }
                            }
                        }

                        if (has_von_voff) {
                            vt_val = (von_val + voff_val) / 2.0;
                            vh_val = (von_val > voff_val) ? (von_val - voff_val) : (voff_val - von_val);
                            if (vh_val < 1e-12) vh_val = 1e-6;
                        } else if (!has_vt_vh) {
                            vt_val = 0.0;
                            vh_val = 0.001;
                        }

                        {
                            char new_model[512];
                            snprintf(new_model, sizeof(new_model),
                                     ".model %s SW (RON=%.15g ROFF=%.15g VT=%.15g VH=%.15g)",
                                     modname, ron_val, roff_val, vt_val, vh_val);
                            tfree(card->line);
                            card->line = copy(new_model);
                    }
                    tfree(type); tfree(params);
                }
                tfree(modeltype_full);
            }
            tfree(modname);
            tfree(tmp);
        }
    }
    }

    /* Second pass: transform A-devices */
    skip_control = 0;
    for (card = oldcard; card; ) {
        char *cl = card->line;
        if (!cl) { prev_card = card; card = card->nextcard; continue; }
        char *cut_line = cl;
        while (*cut_line == ' ' || *cut_line == '\t') cut_line++;
        if (*cut_line == '*' || *cut_line == '\0') { prev_card = card; card = card->nextcard; continue; }
        if (ciprefix(".control", cut_line)) { skip_control++; prev_card = card; card = card->nextcard; continue; }
        if (ciprefix(".endc", cut_line)) { skip_control--; prev_card = card; card = card->nextcard; continue; }
        if (skip_control > 0) { prev_card = card; card = card->nextcard; continue; }

        if ((*cut_line == 'a' || *cut_line == 'A') && !ciprefix(".model", cut_line)) {
            char *linecopy = copy(cut_line);
            char *s = linecopy;
            char *instname = gettok_node(&s);
            if (!instname) { tfree(linecopy); prev_card = card; card = card->nextcard; continue; }

            char *pins[8] = {NULL}, *devtype = NULL;
            char *toks[16] = {NULL}; int nt = 0;
            char param_buf[1024] = "";
            while ((toks[nt] = gettok_node(&s)) != NULL && nt < 16) nt++;
            /* Find devtype (last token without '=') */
            int dev_idx = -1;
            for (int ti = nt - 1; ti >= 0; ti--) {
                if (strchr(toks[ti], '=') == NULL) { devtype = toks[ti]; dev_idx = ti; break; }
            }
            if (!devtype) {
                for (int fi = 0; fi < nt; fi++) tfree(toks[fi]);
                tfree(instname); tfree(linecopy);
                prev_card = card; card = card->nextcard; continue;
            }
            /* Extract pins and params */
            for (int ti = 0; ti < dev_idx && ti < 8; ti++) pins[ti] = toks[ti];
            for (int fi = dev_idx + 1; fi < nt; fi++) {
                if (strlen(param_buf) + strlen(toks[fi]) + 2 < sizeof(param_buf)) {
                    if (fi > dev_idx + 1) strcat(param_buf, " ");
                    strcat(param_buf, toks[fi]);
                }
                tfree(toks[fi]);
            }

            int pin_count = ltspice_device_pin_count(devtype);
            /* Try uppercase if zero */
            char dev_up[64]; int di;
            for (di = 0; di < 63 && devtype[di]; di++) dev_up[di] = toupper_c(devtype[di]);
            dev_up[di] = '\0';
            if (pin_count == 0) {
                pin_count = ltspice_device_pin_count(dev_up);
            }
            /* Check for model-card form */
            bool from_model_card = false;
            char *model_card_params = NULL;
            struct card *model_card_ptr = NULL;
            if (pin_count == 0) {
                struct vsmodels *mdl = find_model_by_name(modelsfound, devtype, "top");
                if (!mdl) mdl = find_model_by_name(modelsfound, dev_up, "top");
                if (mdl) {
                    char *mtup = copy(mdl->modeltype);
                    for (int mi = 0; mtup[mi]; mi++) mtup[mi] = toupper_c(mtup[mi]);
                    if (strcmp(mtup, "COUNTER") == 0) {
                        model_card_params = mdl->params ? copy(mdl->params) : NULL;
                        model_card_ptr = mdl->cardptr;
                        tfree(devtype);
                        devtype = copy("COUNTER");
                        pin_count = ltspice_device_pin_count(devtype);
                        from_model_card = true;
                    }
                    tfree(mtup);
                }
            }
            if (pin_count == 0) {
                for (int fi = 0; fi < dev_idx; fi++) tfree(toks[fi]);
                tfree(toks[dev_idx]); tfree(instname); tfree(linecopy);
                prev_card = card; card = card->nextcard; continue;
            }

            /* Uppercase devtype for lookup */
            char *upper = copy(devtype);
            for (int ui = 0; upper[ui]; ui++) upper[ui] = toupper_c(upper[ui]);
            char *ng_type = ltspice_map_a_model(upper);

            if (!ng_type) {
                tfree(upper);
                for (int fi = 0; fi < nt; fi++) tfree(toks[fi]);
                tfree(instname); tfree(linecopy);
                prev_card = card; card = card->nextcard; continue;
            }

            bool use_vector = (strcmp(upper, "AND") == 0 || strcmp(upper, "OR") == 0 ||
                               strcmp(upper, "NAND") == 0 || strcmp(upper, "NOR") == 0 ||
                               strcmp(upper, "XOR") == 0 || strcmp(upper, "XNOR") == 0);

            /* Parse inline params and set up bridge flag */
            LtspiceInlineParams iparams = {0};
            bool do_bridge = false;
            char *bridge_node[8] = {NULL};
            char *analog_node[8] = {NULL};
            int bridge_dir[8] = {0};
            int in_count = 0, out_count = 0;

            if (param_buf[0]) {
                iparams = ltspice_parse_inline_params(param_buf);
                do_bridge = iparams.present;
            }

            /* Suppress bridges for analog codemodels.
             * adc_bridge excluded: needs DAC bridge on output so
             * digital signal drives analog node in subcircuits. */
            bool is_analog = (strcmp(ng_type, "modulator") == 0 ||
                              strcmp(ng_type, "samplehold") == 0 ||
                              strcmp(ng_type, "varistor") == 0);
            if (is_analog) do_bridge = false;

            /* Model-card form COUNTER: suppress bridges, extract cycles */
            if (from_model_card && model_card_params) {
                char pc[256]; int pci;
                for (pci = 0; pci < 255 && model_card_params[pci]; pci++)
                    pc[pci] = toupper_c(model_card_params[pci]);
                pc[pci] = '\0';
                char *eqs = strstr(pc, "CYCLES=");
                if (!eqs) eqs = strstr(pc, "CYCLES =");
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

            /* Build bridge nodes for digital gates that need them */
            if (do_bridge) {
                for (int lt = 0; lt < 8; lt++) {
                    char *pv = pins[lt];
                    if (!pv) continue;
                    if (strcmp(pv, "0") == 0) continue;
                    if (ltspice_pin_is_input(lt) && (lt < pin_count || use_vector)) {
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

            /* For adc_bridge (SCHMITT), Q is always on LTspice pin 6,
             * /Q on LTspice pin 7, inputs on pins 0 and 1.
             * Clear all bridges that don't match these fixed positions.
             * This avoids the broken forward-scan heuristic on devices
             * with many non-GND pins (e.g. NE555.sub's 8-pin SCHMITT). */
            int adc_nq_pos = -1;
            if (strcmp(ng_type, "adc_bridge") == 0) {
                for (int lt = 0; lt < 8; lt++) {
                    bool keep = false;
                    if (bridge_dir[lt] == 1 && (lt == 0 || lt == 1)) {
                        keep = true;  /* ADC on input pins 0,1 */
                    } else if (bridge_dir[lt] == 2 && lt == 6) {
                        keep = true;  /* DAC on Q output pin 6 only */
                    }
                    if (!keep && bridge_dir[lt] != 0) {
                        bridge_dir[lt] = 0;
                        tfree(bridge_node[lt]); bridge_node[lt] = NULL;
                        tfree(analog_node[lt]); analog_node[lt] = NULL;
                    }
                }
                out_count = (bridge_dir[6] == 2) ? 1 : 0;
                in_count = 0;
                if (pins[7] && !is_gnd_node(pins[7]))
                    adc_nq_pos = 7;
            }

            /* Build model card */
            char model_name[256];
            snprintf(model_name, sizeof(model_name), "a%s_%s", instname + 1, ng_type);
            char model_line[512];
            if (strcmp(upper, "SCHMITT") == 0 || strcmp(upper, "SCHMITT_BUF") == 0) {
                double vt_val = iparams.has_vt ? iparams.vt : 2.5;
                double vh_val = iparams.has_vh ? iparams.vh : 0.001;
                double in_low = vt_val - vh_val / 2.0;
                double in_high = vt_val + vh_val / 2.0;
                snprintf(model_line, sizeof(model_line),
                         ".model %s adc_bridge(in_low=%.15g in_high=%.15g)",
                         model_name, in_low, in_high);
            } else if (strcmp(upper, "COUNTER") == 0 && iparams.has_cycles) {
                snprintf(model_line, sizeof(model_line),
                         ".model %s d_fdiv(div_factor=%d, high_cycles=%.15g)",
                         model_name, (int)(2 * iparams.cycles), iparams.cycles);
            } else if (strcmp(upper, "SRFLOP") == 0 || strcmp(upper, "SRFF") == 0 || strcmp(upper, "SR_FF") == 0) {
                snprintf(model_line, sizeof(model_line), ".model %s %s (ic=1)", model_name, ng_type);
            } else if (strcmp(upper, "VARISTOR") == 0) {
                char params[256] = "";
                char tmp[64];
                if (iparams.has_vref) { snprintf(tmp, sizeof(tmp), "vref=%.15g ", iparams.vref); strcat(params, tmp); }
                if (iparams.has_roff) { snprintf(tmp, sizeof(tmp), "roff=%.15g ", iparams.roff); strcat(params, tmp); }
                if (iparams.has_rclamp) { snprintf(tmp, sizeof(tmp), "rclamp=%.15g ", iparams.rclamp); strcat(params, tmp); }
                snprintf(model_line, sizeof(model_line), ".model %s %s (%s)", model_name, ng_type, params);
            } else if (strcmp(upper, "SAMPLEHOLD") == 0) {
                char params[256] = "";
                char tmp[64];
                if (iparams.has_vt) { snprintf(tmp, sizeof(tmp), "vt=%.15g ", iparams.vt); strcat(params, tmp); }
                if (iparams.has_vhigh) { snprintf(tmp, sizeof(tmp), "vhigh=%.15g ", iparams.vhigh); strcat(params, tmp); }
                if (iparams.has_vlow) { snprintf(tmp, sizeof(tmp), "vlow=%.15g ", iparams.vlow); strcat(params, tmp); }
                snprintf(model_line, sizeof(model_line), ".model %s %s (%s)", model_name, ng_type, params);
            } else if (strcmp(upper, "MODULATOR") == 0) {
                char params[256] = "";
                char tmp[64];
                if (iparams.has_mark) { snprintf(tmp, sizeof(tmp), "mark=%.15g ", iparams.mark); strcat(params, tmp); }
                if (iparams.has_space) { snprintf(tmp, sizeof(tmp), "space=%.15g ", iparams.space); strcat(params, tmp); }
                snprintf(model_line, sizeof(model_line), ".model %s %s (%s)", model_name, ng_type, params);
            } else {
                snprintf(model_line, sizeof(model_line), ".model %s %s ()", model_name, ng_type);
            }
            struct card *insert_pos = card;
            insert_pos = insert_new_line(insert_pos, copy(model_line), card->linenum, card->linenum_orig, card->linesource);

            /* SCHMITT comparator: generate B-source for V(pin0)-V(pin1) */
            bool need_diff = false;
            char diff_node[64] = "";
            if ((strcmp(upper, "SCHMITT") == 0 || strcmp(upper, "SCHMITT_BUF") == 0) && pin_count == 2) {
                if (pins[0] && pins[1] && !is_gnd_node(pins[0]) &&
                    !is_gnd_node(pins[1]) && strcmp(pins[0], pins[1]) != 0) {
                    need_diff = true;
                    snprintf(diff_node, sizeof(diff_node), "_%s_diff", instname);
                    char bsource[512];
                    snprintf(bsource, sizeof(bsource), "B%s_diff %s 0 V=V(%s)-V(%s)",
                             instname, diff_node, pins[0], pins[1]);
                    insert_pos = insert_new_line(insert_pos, copy(bsource),
                                                  card->linenum, card->linenum_orig, card->linesource);
                }
            }

            /* d_srlatch has 7 ports: S(0), R(1), enable(2), aset(3), areset(4), Q(5), /Q(6).
             * Enable must be ONE for S/R to take effect. LTspice SRFLOP is set-dominant
             * (S=1,R=1→Q=1), while d_srlatch is reset-dominant. Fix both issues:
             * - Inject 5V on the enable pin
             * - Force R to 0 when S is active for set-dominant behavior */
            bool srflop_set_dominant = (strcmp(upper, "SRFLOP") == 0 ||
                                        strcmp(upper, "SRFF") == 0 ||
                                        strcmp(upper, "SR_FF") == 0);
            char sr_r_fix[64] = "", sr_en_node[64] = "";
            if (srflop_set_dominant && pins[0] && pins[1]) {
                /* Enable: 5V pull-up */
                snprintf(sr_en_node, sizeof(sr_en_node), "_%s_en", instname);
                char en_bs[512];
                snprintf(en_bs, sizeof(en_bs), "B%s_en %s 0 V=5", instname, sr_en_node);
                insert_pos = insert_new_line(insert_pos, copy(en_bs),
                                              card->linenum, card->linenum_orig, card->linesource);
                /* Set-dominant: R' = S ? 0 : R */
                snprintf(sr_r_fix, sizeof(sr_r_fix), "_%s_rfix", instname);
                char fix_bs[512];
                snprintf(fix_bs, sizeof(fix_bs), "B%s_srfix %s 0 V=V(%s)*(V(%s)<2.5)",
                         instname, sr_r_fix, pins[1], pins[0]);
                insert_pos = insert_new_line(insert_pos, copy(fix_bs),
                                              card->linenum, card->linenum_orig, card->linesource);
            }

            /* Build node string with proper port mapping */
            bool is_adc_bridge = (strcmp(ng_type, "adc_bridge") == 0);

            int ngspice_ports = pin_count;
            int active_inputs[6], num_active_inputs = 0;
            int vector_out_pin = -1;
            bool use_dynamic = false;
            if (use_vector) {
                use_dynamic = true;
            } else if (dev_idx > pin_count && strcmp(ng_type, "adc_bridge") != 0) {
                /* Device has more nodes on the line than its standard pin count.
                 * Likely using 8-pin LTspice convention with non-standard pinout.
                 * Use dynamic output detection to find the real output pin.
                 * Exclude adc_bridge (SCHMITT) which uses fixed pin positions
                 * and has its own bridge cleanup logic. */
                use_dynamic = true;
            }
            if (use_dynamic) {
                /* Determine output pin: scan from last pin backwards,
                 * skip numeric-only subcircuit pin refs (is_gnd_node)
                 * as well as literal "0" (global GND).
                 * This handles non-standard pinouts where output is
                 * on pin 5 instead of 6 (e.g. NE555.sub A4). */
                for (int lt = dev_idx - 1; lt >= 0; lt--) {
                    if (pins[lt] && !is_gnd_node(pins[lt])) {
                        vector_out_pin = lt;
                        break;
                    }
                }
                if (vector_out_pin < 0)
                    vector_out_pin = dev_idx - 1;
                /* Build active inputs from pins before output pin,
                 * skipping literal "0" (global GND) and NULL. */
                for (int lt = 0; lt < vector_out_pin && lt < dev_idx; lt++) {
                    if (!pins[lt]) continue;
                    if (strcmp(pins[lt], "0") == 0) continue;
                    char *pv = bridge_node[lt] ? bridge_node[lt] : pins[lt];
                    if (pv) active_inputs[num_active_inputs++] = lt;
                }
                ngspice_ports = num_active_inputs + 1;
                /* Vector gate bridge cleanup: clear DAC bridges for unused
                 * output positions (LTspice pins 6,7 when they map to
                 * inactive subcircuit pins like "1"). Only keep the
                 * dynamically-detected output pin. */
                if (do_bridge) {
                    for (int lt = 0; lt < 8; lt++) {
                        if (bridge_dir[lt] == 2 && lt != vector_out_pin) {
                            bridge_dir[lt] = 0;
                            tfree(bridge_node[lt]); bridge_node[lt] = NULL;
                            tfree(analog_node[lt]); analog_node[lt] = NULL;
                            out_count--;
                        }
                    }
                }
                /* Also clear ADC bridge entries for pins that are not active inputs */
                if (do_bridge) {
                    for (int lt = 0; lt < 8; lt++) {
                        if (bridge_dir[lt] != 1) continue;
                        bool is_active = false;
                        for (int ai = 0; ai < num_active_inputs; ai++)
                            if (active_inputs[ai] == lt) { is_active = true; break; }
                        if (!is_active) {
                            bridge_dir[lt] = 0;
                            tfree(bridge_node[lt]); bridge_node[lt] = NULL;
                            tfree(analog_node[lt]); analog_node[lt] = NULL;
                            in_count--;
                        }
                    }
                }
            } else if (strcmp(upper, "DLATCH") == 0 || strcmp(upper, "D_LATCH") == 0) ngspice_ports = 6;
            else if (strcmp(upper, "DFF") == 0 || strcmp(upper, "D_FF") == 0 || strcmp(upper, "DFLOP") == 0) ngspice_ports = 6;
            else if (strcmp(upper, "JKFF") == 0 || strcmp(upper, "JK_FF") == 0) ngspice_ports = 7;
            else if (strcmp(upper, "SRFF") == 0 || strcmp(upper, "SR_FF") == 0 || strcmp(upper, "SRFLOP") == 0) ngspice_ports = 7;
            else if (strcmp(upper, "COUNTER") == 0) ngspice_ports = 2;
            else if (strcmp(upper, "SRLATCH") == 0 || strcmp(upper, "SR_LATCH") == 0) ngspice_ports = 7;
            else if (strcmp(upper, "SAMPLEHOLD") == 0) ngspice_ports = 3;
            else if (strcmp(upper, "VARISTOR") == 0) ngspice_ports = 4;
            else if (strcmp(upper, "MODULATOR") == 0) ngspice_ports = 4;

            char node_str[1024] = "";
            if (use_vector) strcat(node_str, "[");

            /* Build port→bridge mapping for vector gates */
            char *port_bridge[8] = {NULL};
            int bridge_input_idx = 0, bridge_output_idx = 0;
            bool bridge_map_done = false;
            if (do_bridge && use_dynamic) {
                for (int lt = 0; lt < 8; lt++) {
                    if (bridge_dir[lt] == 1 && bridge_input_idx < pin_count - 1)
                        port_bridge[bridge_input_idx++] = bridge_node[lt];
                    else if (bridge_dir[lt] == 2 && bridge_output_idx < 2)
                        port_bridge[pin_count - 1 + bridge_output_idx++] = bridge_node[lt];
                }
                bridge_map_done = true;
            }

            for (int i = 0; i < ngspice_ports; i++) {
                int lt_pin;
                /* Specific device pin mapping */
                if (strcmp(upper, "DLATCH") == 0 || strcmp(upper, "D_LATCH") == 0) {
                    if (i == 0) lt_pin = 0;
                    else if (i == 1) lt_pin = 1;
                    else if (i == 2 || i == 3) lt_pin = -1;
                    else if (i == 4) lt_pin = 6;
                    else lt_pin = 7;
                } else if (strcmp(upper, "DFF") == 0 || strcmp(upper, "D_FF") == 0 || strcmp(upper, "DFLOP") == 0) {
                    if (i == 0) lt_pin = 0;
                    else if (i == 1) lt_pin = 1;
                    else if (i == 2 || i == 3) lt_pin = -1;
                    else if (i == 4) lt_pin = 6;
                    else lt_pin = 7;
                } else if (strcmp(upper, "JKFF") == 0 || strcmp(upper, "JK_FF") == 0) {
                    if (i == 0) lt_pin = 0;
                    else if (i == 1) lt_pin = 1;
                    else if (i == 2) lt_pin = 2;
                    else if (i == 3 || i == 4) lt_pin = -1;
                    else if (i == 5) lt_pin = 6;
                    else lt_pin = 7;
                } else if (strcmp(upper, "SRFF") == 0 || strcmp(upper, "SR_FF") == 0 || strcmp(upper, "SRFLOP") == 0) {
                    if (i == 0) lt_pin = 0;
                    else if (i == 1) lt_pin = 1;
                    else if (i == 2) lt_pin = -1;
                    else if (i == 3 || i == 4) lt_pin = -1;
                    else if (i == 5) lt_pin = 6;
                    else lt_pin = 7;
                } else if (strcmp(upper, "SRLATCH") == 0 || strcmp(upper, "SR_LATCH") == 0) {
                    if (i == 0) lt_pin = 0;
                    else if (i == 1) lt_pin = 1;
                    else if (i == 2) lt_pin = -1;
                    else if (i == 3 || i == 4) lt_pin = -1;
                    else if (i == 5) lt_pin = 6;
                    else lt_pin = 7;
                } else if (strcmp(upper, "TFF") == 0 || strcmp(upper, "T_FF") == 0) {
                    if (i == 0) lt_pin = 0;
                    else if (i == 1) lt_pin = 1;
                    else if (i == 2 || i == 3) lt_pin = -1;
                    else if (i == 4) lt_pin = 6;
                    else lt_pin = 7;
                } else if (strcmp(upper, "COUNTER") == 0) {
                    if (i == 0) lt_pin = 0;
                    else lt_pin = 6;
                } else if (strcmp(upper, "SAMPLEHOLD") == 0) {
                    if (i == 0) lt_pin = 0;
                    else if (i == 1) lt_pin = 2;
                    else lt_pin = 6;
                } else if (strcmp(upper, "VARISTOR") == 0) {
                    if (i == 0) lt_pin = 0;
                    else if (i == 1) lt_pin = 1;
                    else if (i == 2) lt_pin = 2;
                    else lt_pin = 3;
                } else if (strcmp(upper, "MODULATOR") == 0) {
                    if (i == 0) lt_pin = 0;
                    else if (i == 1) lt_pin = 1;
                    else if (i == 2) lt_pin = 6;
                    else lt_pin = 7;
                } else if (use_vector) {
                    if (i < ngspice_ports - 1)
                        lt_pin = active_inputs[i];
                    else
                        lt_pin = vector_out_pin;
                } else if (use_dynamic) {
                    if (i < ngspice_ports - 1)
                        lt_pin = active_inputs[i];
                    else
                        lt_pin = vector_out_pin;
                } else {
                    lt_pin = ltspice_pin_index(upper, i);
                    /* Dynamic pin detection for 2-pin digital devices.
                     * Some subcircuits (e.g. MIC4606-1) use non-standard
                     * pin assignments where Q output appears at a position
                     * other than 6. Scan all pins: first non-"0" = input,
                     * second non-"0" after input = Q output. */
                    if (pin_count == 2 && !use_vector &&
                        (strcmp(upper, "SCHMITT") == 0 || strcmp(upper, "SCHMITT_BUF") == 0 ||
                         strcmp(upper, "BUF") == 0 || strcmp(upper, "BUFFER") == 0 ||
                         strcmp(upper, "NOT") == 0 || strcmp(upper, "INV") == 0 ||
                         strcmp(upper, "INVERTER") == 0)) {
                        int nongnd_pos[8], nn = 0;
                for (int pi = 0; pi < dev_idx; pi++) {
                    if (pins[pi] && !is_gnd_node(pins[pi]))
                        nongnd_pos[nn++] = pi;
                }
                        if (nn >= 2) {
                            if (i == 0) lt_pin = nongnd_pos[0];  /* input */
                            else lt_pin = nongnd_pos[1];         /* Q */
                        }
                    }
                }

                char *port_val = NULL;
                if (need_diff && i == 0) {
                    port_val = diff_node;
                } else if (srflop_set_dominant && i == 2 && sr_en_node[0]) {
                    port_val = sr_en_node;
                } else if (srflop_set_dominant && i == 1 && sr_r_fix[0]) {
                    port_val = sr_r_fix;
                } else if (bridge_map_done && port_bridge[i]) {
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

                if (is_adc_bridge) {
                    strcat(node_str, " [ ");
                    strcat(node_str, port_val);
                    strcat(node_str, " ]");
                } else if (use_vector && i == ngspice_ports - 1) {
                    strcat(node_str, "] ");
                    strcat(node_str, port_val);
                } else if (i > 0) {
                    strcat(node_str, " ");
                    strcat(node_str, port_val);
                } else {
                    strcat(node_str, port_val);
                }
            }

            /* Build the complete new A-device line */
            tfree(card->line);
            card->line = tprintf("%s %s %s", instname, node_str, model_name);

            /* Insert bridge cards */
            if (do_bridge) {
                if (in_count > 0) {
                    char adc_an[256] = "", adc_dig[256] = "";
                    int first = 1;
                    for (int lt = 0; lt < 8; lt++) {
                        if (bridge_dir[lt] == 1) {
                            if (!first) { strcat(adc_an, " "); strcat(adc_dig, " "); }
                            strcat(adc_an, analog_node[lt]);
                            strcat(adc_dig, bridge_node[lt]);
                            first = 0;
                        }
                    }
                    char buf[1024];
                    snprintf(buf, sizeof(buf), ".model _lt_m%s_adc adc_bridge(in_low=%.15g in_high=%.15g)",
                             instname, iparams.ref - 0.1, iparams.ref + 0.1);
                    insert_pos = insert_new_line(insert_pos, copy(buf), card->linenum, card->linenum_orig, card->linesource);
                    snprintf(buf, sizeof(buf), "a_lt%s_adc [ %s ] [ %s ] _lt_m%s_adc",
                             instname, adc_an, adc_dig, instname);
                    insert_pos = insert_new_line(insert_pos, copy(buf), card->linenum, card->linenum_orig, card->linesource);
                }
                if (out_count > 0) {
                    char dac_an[256] = "", dac_dig[256] = "";
                    int first = 1;
                    for (int lt = 0; lt < 8; lt++) {
                        if (bridge_dir[lt] == 2) {
                            if (!first) { strcat(dac_an, " "); strcat(dac_dig, " "); }
                            strcat(dac_an, analog_node[lt]);
                            strcat(dac_dig, bridge_node[lt]);
                            first = 0;
                        }
                    }
                    char buf[1024];
                    double out_low = iparams.has_vlow ? iparams.vlow : 0.0;
                    double out_high = iparams.has_vhigh ? iparams.vhigh
                                     : (iparams.has_vlow ? 1.0 : 5.0);
                    double tr = iparams.has_trise ? iparams.trise : 1e-9;
                    double tf = iparams.has_tfall ? iparams.tfall : 1e-9;
                    snprintf(buf, sizeof(buf),
                             ".model _lt_m%s_dac dac_bridge(out_low=%.15g out_high=%.15g "
                             "out_undef=%.15g t_rise=%.15g t_fall=%.15g)",
                             instname, out_low, out_high, out_high, tr, tf);
                    insert_pos = insert_new_line(insert_pos, copy(buf), card->linenum, card->linenum_orig, card->linesource);
                    snprintf(buf, sizeof(buf), "a_lt%s_dac [ %s ] [ %s ] _lt_m%s_dac",
                             instname, dac_dig, dac_an, instname);
                    insert_pos = insert_new_line(insert_pos, copy(buf), card->linenum, card->linenum_orig, card->linesource);
                }
            }

            /* For adc_bridge (SCHMITT) with active /Q node on LTspice pin 7:
             * generate B-source inverter: V(/Q) = vlow+vhigh - V(Q) where Q is on pin 6. */
            if (strcmp(ng_type, "adc_bridge") == 0 && adc_nq_pos >= 0) {
                char *nq_node = pins[7];
                char *q_node = pins[6];
                if (nq_node && !is_gnd_node(nq_node) &&
                    q_node && !is_gnd_node(q_node)) {
                    double out_low = iparams.has_vlow ? iparams.vlow : 0.0;
                    double out_high = iparams.has_vhigh ? iparams.vhigh : 5.0;
                    double sum = out_low + out_high;
                    char inv_bs[512];
                    snprintf(inv_bs, sizeof(inv_bs),
                             "B%s_nq %s 0 V=%.15g - V(%s)",
                             instname, nq_node, sum, q_node);
                    insert_pos = insert_new_line(insert_pos, copy(inv_bs),
                                                  card->linenum, card->linenum_orig, card->linesource);
                }
            }

            /* Cleanup */
            for (int fi = 0; fi < dev_idx; fi++) tfree(toks[fi]);
            tfree(toks[dev_idx]);
            for (int bi = 0; bi < 8; bi++) { tfree(bridge_node[bi]); tfree(analog_node[bi]); }
            tfree(upper);
            tfree(instname);
            tfree(linecopy);
            tfree(model_card_params);
            prev_card = card;
            card = card->nextcard;
            continue;
        }
        prev_card = card;
        card = card->nextcard;
    }

    /* Cleanup model registration */
    while (modelsfound) {
        struct vsmodels *next = modelsfound->nextmodel;
        tfree(modelsfound->modelname);
        tfree(modelsfound->modeltype);
        tfree(modelsfound->params);
        tfree(modelsfound);
        modelsfound = next;
    }
}
