#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdio.h>
#include <errno.h>

#define _GNU_SOURCE
#include <string.h>

#include <murphy-db/assert.h>
#include <murphy-db/list.h>
#include <murphy-db/handle.h>
#include <murphy-db/hash.h>
#include <murphy-db/sequence.h>
#include "column.h"
#include "index.h"
#include "table.h"
#include "cond.h"


typedef struct {
    mqi_data_type_t  type;
    union {
        char        *varchar;
        int32_t      integer;
        uint32_t     unsignd;
        void        *blob;
        void        *data;
    };
} cond_data_t;

#define PRECEDENCE_DATA 256

typedef struct {
    int                precedence; /* 256 => data, precedence otherwise */
    union {
        cond_data_t    data;
        mqi_operator_t operator;
    };
} cond_stack_t;

static int cond_get_data(cond_stack_t*,mqi_cond_entry_t*,mdb_column_t*,void*);
static int cond_eval(cond_stack_t *, cond_stack_t *, int);
static int cond_relop(mqi_operator_t, cond_stack_t *, cond_stack_t *);
static int cond_binary_logicop(mqi_operator_t, cond_stack_t *,
                               cond_stack_t *);
static int cond_unary_logicop(mqi_operator_t, cond_stack_t *);

int mdb_cond_evaluate(mdb_table_t *tbl, mqi_cond_entry_t **cond_ptr,void *data)
{
    static int precedence[mqi_operator_max] = {
        [ mqi_done  ] = 0,
        [ mqi_begin ] = 1,
        [ mqi_and   ] = 2,
        [ mqi_or    ] = 3,
        [ mqi_less  ] = 4,
        [ mqi_leq   ] = 4,
        [ mqi_eq    ] = 4,
        [ mqi_geq   ] = 4,
        [ mqi_gt    ] = 4,
        [ mqi_not   ] = 5
    };

    mqi_cond_entry_t *cond       = *cond_ptr;
    cond_stack_t      stack[256] = { [0] = {precedence[mqi_begin],
                                            .operator = mqi_begin}
                                   };
    cond_stack_t     *sp         = stack + 1;
    cond_stack_t     *lastop     = stack;
    int               result;
    int               pr;

    MDB_CHECKARG(cond && data, -1);

    for (;;) {
        switch (cond->type) {

        case mqi_operator:
            pr  = precedence[cond->operator];
            sp += cond_eval(sp, lastop, pr);

            switch (cond->operator) {

            case mqi_begin:
                cond++;
                result = mdb_cond_evaluate(tbl, &cond, data);
                cond++;

                sp->data.integer = result >= 0 ? result : 0;
                sp->precedence   = PRECEDENCE_DATA;
                sp->data.type    = mqi_integer;
                sp++;
                break;
                
            case mqi_end:
                *cond_ptr = cond+1;
                sp--;
                if (sp->precedence < PRECEDENCE_DATA ||
                    sp->data.type != mqi_integer)
                {
                    errno = ENOENT;
                    return -1;
                }
                return sp->data.integer ? 1 : 0;

            case mqi_and:
            case mqi_or:
            case mqi_less:
            case mqi_leq:
            case mqi_eq:
            case mqi_geq:
            case mqi_gt:
            case mqi_not:
                lastop = sp++;
                lastop->precedence = pr;
                lastop->operator = cond->operator;
                cond++;
                break;

            default:
                break;
            }
            break;

        case mqi_variable:
        case mqi_column:
            sp += cond_get_data(sp, cond, tbl->columns, data);
            cond++;
            break;

        default:
            errno = EINVAL;
            return -1;
        }

    } /* for ;; */
}

static int cond_get_data(cond_stack_t     *sp,
                         mqi_cond_entry_t *cond,
                         mdb_column_t     *columns,
                         void             *data)
{
    mqi_column_desc_t  sp_desc[2];
    cond_data_t       *sd;
    mdb_column_t      *col_desc;
    mqi_variable_t    *var;
    int                ok;

    switch (cond->type) {

    case mqi_variable:
        sd  = &sp->data;
        var = &cond->variable;

        if (!var->generic)
            ok = 0;
        else {
            switch ((sp->data.type = var->type)) {
            case mqi_varchar:   sd->varchar = *var->varchar;  ok = 1;   break;
            case mqi_integer:   sd->integer = *var->integer;  ok = 1;   break;
            case mqi_unsignd:   sd->unsignd = *var->unsignd;  ok = 1;   break;
            case mqi_blob:      sd->blob    = *var->blob;     ok = 1;   break;
            default:                                          ok = 0;   break;
            }
        }
        break;

    case mqi_column: {
        col_desc = columns + cond->column;
        sp_desc[0].cindex  = cond->column;
        sp_desc[0].offset = 0;
        sp_desc[1].cindex  = -1;
        sp_desc[1].offset = -1;
        mdb_column_read(sp_desc, &sp->data.data, col_desc, data);
        sp->data.type = col_desc->type;
        ok = 1;
        }
        break;

    default:
        ok = 0;
        break;
    }

    sp->precedence = PRECEDENCE_DATA * ok;

    return ok;
}

static int cond_eval(cond_stack_t *sp,cond_stack_t *lastop,int new_precedence)
{
    cond_stack_t *result;
    cond_stack_t *newsp;
    int value;
    int stack_advance = 0;

    while (new_precedence < lastop->precedence) {
        switch (lastop->operator) {

        case mqi_begin:
            /* stack: (0)begin, (1)operand => (0)result */
            newsp = (result = lastop) + 1;
            value = (lastop+1)->data.integer;
            new_precedence = INT_MAX;
            goto store_on_stack;

        case mqi_and:
        case mqi_or:
            /* stack: (-1)operand1, (0)operator, (1)operand2 => (-1)result */
            result = (newsp = lastop) - 1;
            value = cond_binary_logicop(lastop->operator, lastop-1,lastop+1);
            goto find_new_lastop_and_store_on_stack;
            
        case mqi_less:
        case mqi_leq:
        case mqi_eq:
        case mqi_geq:
        case mqi_gt:
            /* stack: (-1)operand1, (0)operator, (1)operand2 => (-1)result */
            result = (newsp = lastop) - 1;
            value = cond_relop(lastop->operator, lastop-1,lastop+1);
            goto find_new_lastop_and_store_on_stack;

        case mqi_not:
            /* stack: (0)operator, (1)operand => (0)result */
            newsp = (result = lastop) + 1;
            value = cond_unary_logicop(lastop->operator, lastop+1);
            goto find_new_lastop_and_store_on_stack;
            
        find_new_lastop_and_store_on_stack:
            for (lastop--;  lastop->precedence >= PRECEDENCE_DATA;  lastop--)
                ;
            /* intentional fall over */
            
        store_on_stack:
            result->precedence   = PRECEDENCE_DATA;
            result->data.type    = mqi_integer;
            result->data.integer = value;
            /* intentional fall over */

        default:
            stack_advance = newsp - sp;
            break;
        }
    }

    return stack_advance;
}


static int cond_relop(mqi_operator_t op, cond_stack_t *v1, cond_stack_t *v2)
{
    cond_data_t *d1 = &v1->data;
    cond_data_t *d2 = &v2->data;
    int cmp;

    if (v1->precedence >= PRECEDENCE_DATA &&
        v2->precedence >= PRECEDENCE_DATA &&
        d1->type == d2->type)
    {
        switch (d1->type) {
        case mqi_varchar:
            if (!d1->varchar && !d2->varchar)
                cmp = 0;
            else if (!d1->varchar)
                cmp = -1;
            else if (!d2->varchar)
                cmp = 1;
            else
                cmp = strcmp(d1->varchar, d2->varchar);
            break;

        case mqi_integer:
            if (d1->integer > d2->integer)
                cmp = 1;
            else if (d1->integer == d2->integer)
                cmp = 0;
            else
                cmp = -1;
            break;

        case mqi_unsignd:
            if (d1->unsignd > d2->unsignd)
                cmp = 1;
            else if (d1->unsignd == d2->unsignd)
                cmp = 0;
            else
                cmp = -1;
            break;

        default:
            return 0;
        }

        switch (op) {
        case mqi_less:  return cmp <  0;
        case mqi_leq:   return cmp <= 0;
        case mqi_eq:    return cmp == 0;
        case mqi_geq:   return cmp >= 0;
        case mqi_gt:    return cmp >  0;
        default:        return 0;
        }
    }

    return 0;
}

static int cond_binary_logicop(mqi_operator_t op,
                               cond_stack_t  *v1,
                               cond_stack_t  *v2)
{
    cond_data_t *d1 = &v1->data;
    cond_data_t *d2 = &v2->data;

    if (v1->precedence >= PRECEDENCE_DATA &&
        v2->precedence >= PRECEDENCE_DATA &&
        d1->type == d2->type)
    {
        switch (op) {

        case mqi_and:
            switch (d1->type) {
            case mqi_integer:   return d1->integer && d2->integer;
            case mqi_unsignd:   return d1->unsignd && d2->unsignd;
            default:            return 0;
            }
            break;

        case mqi_or:
            switch (d1->type) {
            case mqi_integer:   return d1->integer || d2->integer;
            case mqi_unsignd:   return d1->unsignd || d2->unsignd;
            default:            return 0;
            }
            break;

        default:
            return 0;
        }
    }

    return 0;
}

static int cond_unary_logicop(mqi_operator_t op, cond_stack_t *v)
{
    cond_data_t *d = &v->data;

    if (v->precedence >= PRECEDENCE_DATA && op == mqi_not) {
        switch (d->type) {
        case mqi_varchar:  return d->varchar && d->varchar[0] ? 0 : 1;
        case mqi_integer:  return d->integer ? 0 : 1;
        case mqi_unsignd:  return d->unsignd ? 0 : 1;
        default:           return 0;
        }
    }

    return 0;
}

/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 *
 */
