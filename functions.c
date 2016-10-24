/*
 * Spin to C/C++ converter
 * Copyright 2011-2016 Total Spectrum Software Inc.
 * See the file COPYING for terms of use
 *
 * code for handling functions
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spinc.h"

Function *curfunc;
static int visitPass = 1;

Function *
NewFunction(void)
{
    Function *f;
    Function *pf;

    f = (Function *)calloc(1, sizeof(*f));
    if (!f) {
        fprintf(stderr, "FATAL ERROR: Out of memory!\n");
        exit(1);
    }
    /* now link it into the current object */
    if (current->functions == NULL) {
        current->functions = f;
    } else {
        pf = current->functions;
        while (pf->next != NULL)
            pf = pf->next;
        pf->next = f;
    }
    f->module = current;
    f->localsyms.next = &current->objsyms;
    return f;
}

static Symbol *
EnterVariable(int kind, SymbolTable *stab, const char *name, AST *type)
{
    Symbol *sym;

    sym = AddSymbol(stab, name, kind, (void *)type);
    return sym;
}

int
EnterVars(int kind, SymbolTable *stab, AST *symtype, AST *varlist, int offset)
{
    AST *lower;
    AST *ast;
    Symbol *sym;
    int size;
    int typesize = EvalConstExpr(symtype->left);

    for (lower = varlist; lower; lower = lower->right) {
        if (lower->kind == AST_LISTHOLDER) {
            ast = lower->left;
            switch (ast->kind) {
            case AST_IDENTIFIER:
                sym = EnterVariable(kind, stab, ast->d.string, symtype);
                sym->offset = offset;
                offset += typesize;
                break;
            case AST_ARRAYDECL:
                sym = EnterVariable(kind, stab, ast->left->d.string, NewAST(AST_ARRAYTYPE, symtype, ast->right));
                size = EvalConstExpr(ast->right);
                sym->offset = offset;
                offset += size * typesize;
                break;
            case AST_ANNOTATION:
                /* just ignore it */
                break;
            default:
                ERROR(ast, "Internal error: bad AST value %d", ast->kind);
                break;
            }
        } else {
            ERROR(lower, "Expected list of variables, found %d instead", lower->kind);
            return offset;
        }
    }
    return offset;
}

/*
 * check for a variable having its address taken
 */
static bool
IsAddrRef(AST *body, Symbol *sym)
{
    if (body->kind == AST_ADDROF)
        return true;
    if (body->kind == AST_ARRAYREF && !IsArraySymbol(sym))
        return true;
    return false;
}

/*
 * scan a function body for various special conditions
 */
static void
ScanFunctionBody(Function *fdef, AST *body, AST *upper)
{
    AST *ast;
    Symbol *sym;

    if (!body)
        return;
    switch(body->kind) {
        // note that below we are making assumptions that we're still in the original function
        // these assumptions fail if we encounter a methodref or constref
    case AST_CONSTREF:
    case AST_METHODREF:
        return;
    case AST_ADDROF:
    case AST_ABSADDROF:
    case AST_ARRAYREF:
        /* see if it's a parameter whose address is being taken */
        ast = body->left;
        if (ast->kind == AST_IDENTIFIER) {
            sym = FindSymbol(&fdef->localsyms, ast->d.string);
            if (sym) {
                if (sym->type == SYM_PARAMETER) {
                    if (!fdef->parmarray) {
                        fdef->parmarray = NewTemporaryVariable("_parm_");
                    }
                    fdef->localarray = fdef->parmarray;
                } else if (sym->type == SYM_LOCALVAR && IsAddrRef(body, sym) ) {
                    if (!fdef->localarray) {
                        fdef->localarray = NewTemporaryVariable("_local_");
                    }
                }
            } else {
                /* Taking the address of an object variable? That will make the object volatile. */
                sym = LookupSymbol(ast->d.string);
                if (sym && sym->type == SYM_VARIABLE && IsAddrRef(body, sym)) {
                    current->volatileVariables = 1;
                } else if (sym && sym->type == SYM_LABEL && upper->kind == AST_MEMREF) {
                    Label *lab = (Label *)sym->val;
                    int refalign = TypeAlignment(upper->left);
                    int labalign = TypeAlignment(lab->type);
                    if (refalign > labalign) {
                        lab->flags |= (LABEL_NEEDS_EXTRA_ALIGN|LABEL_USED_IN_SPIN);
                        WARNING(body, "Label is dereferenced with greater alignment than it was declared with");
                    }

                }
            }
        } else if (ast->kind == AST_RESULT) {
            /* hack to make F32.spin work: it expects all variables
               to be arranged as result,parameters,locals
               so force all of those into the _parm_ array
            */
            if (!fdef->parmarray) {
                fdef->parmarray = NewTemporaryVariable("_parm_");
            }
            fdef->localarray = fdef->parmarray;
            if (!fdef->result_in_parmarray) {
                fdef->result_in_parmarray = 1;
                fdef->resultexpr = NewAST(AST_RESULT, NULL, NULL);
                fdef->result_used = 1;
            }
        }
        break;
    case AST_IDENTIFIER:
        sym = FindSymbol(&fdef->localsyms,  body->d.string);
        if (!sym) {
            sym = LookupSymbol(body->d.string);
        }
        if (sym) {
            if (sym->type == SYM_LABEL) {
                Label *L = (Label *)sym->val;
                L->flags |= LABEL_USED_IN_SPIN;
            }
            // convert plain foo into foo[0] if foo is an array
            if (IsArraySymbol(sym) && (sym->type == SYM_VARIABLE || sym->type == SYM_LOCALVAR || sym->type == SYM_LABEL))
            {
                if (upper && !(upper->kind == AST_ARRAYREF && upper->left == body)) {
                    AST *deref = NewAST(AST_ARRAYREF, body, AstInteger(0));
                    deref->line = upper->line;
                    if (body == upper->left) {
                        upper->left = deref;
                    } else if (body == upper->right) {
                        upper->right = deref;
                    } else {
                        ERROR(body, "failed to dereference %s", body->d.string); 
                    }
                }
            }
        }
        break;
    default:
        break;
    }
    ScanFunctionBody(fdef, body->left, body);
    ScanFunctionBody(fdef, body->right, body);
}

/*
 * declare a function and create a Function structure for it
 */

void
DeclareFunction(int is_public, AST *funcdef, AST *body, AST *annotation, AST *comment)
{
    AST *funcblock;
    AST *holder;

    holder = NewAST(AST_FUNCHOLDER, funcdef, body);
    funcblock = NewAST(is_public ? AST_PUBFUNC : AST_PRIFUNC, holder, annotation);
    funcblock->d.ptr = (void *)comment;
    funcblock = NewAST(AST_LISTHOLDER, funcblock, NULL);

    current->funcblock = AddToList(current->funcblock, funcblock);
}

static void
doDeclareFunction(AST *funcblock)
{
    AST *holder;
    AST *funcdef;
    AST *body;
    AST *annotation;
    Function *fdef;
    AST *vars;
    AST *src;
    AST *comment;
    const char *resultname;
    int is_public;
    
    is_public = (funcblock->kind == AST_PUBFUNC);
    holder = funcblock->left;
    annotation = funcblock->right;
    funcdef = holder->left;
    body = holder->right;
    comment = (AST *)funcblock->d.ptr;

    if (funcdef->kind != AST_FUNCDEF || funcdef->left->kind != AST_FUNCDECL) {
        ERROR(funcdef, "Internal error: bad function definition");
        return;
    }
    src = funcdef->left;
    if (src->left->kind != AST_IDENTIFIER) {
        ERROR(funcdef, "Internal error: no function name");
        return;
    }
    fdef = NewFunction();
    fdef->name = src->left->d.string;
    fdef->annotations = annotation;
    fdef->decl = funcdef;
    if (comment) {
        if (comment->kind != AST_COMMENT) {
            ERROR(comment, "Internal error: expected comment");
            abort();
        }
        fdef->doccomment = comment;
    }
    if (src->right && src->right->kind == AST_IDENTIFIER)
        resultname = src->right->d.string;
    else
        resultname = "result";
    fdef->resultexpr = AstIdentifier(resultname);
    fdef->is_public = is_public;
    fdef->rettype = ast_type_generic;

    vars = funcdef->right;
    if (vars->kind != AST_FUNCVARS) {
        ERROR(vars, "Internal error: bad variable declaration");
    }

    /* enter the variables into the local symbol table */
    fdef->params = vars->left;
    fdef->locals = vars->right;

    fdef->numparams = EnterVars(SYM_PARAMETER, &fdef->localsyms, ast_type_long, fdef->params, 0) / LONG_SIZE;
    fdef->numlocals = EnterVars(SYM_LOCALVAR, &fdef->localsyms, ast_type_long, fdef->locals, 0) / LONG_SIZE;

    AddSymbol(&fdef->localsyms, resultname, SYM_RESULT, ast_type_long);

    fdef->body = body;

    /* define the function itself */
    AddSymbol(&current->objsyms, fdef->name, SYM_FUNCTION, fdef);

#if 0
    /* check for special conditions */
    ScanFunctionBody(fdef, fdef->body, NULL);

    /* if we put the locals into an array, record the size of that array */
    if (fdef->localarray) {
        fdef->localarray_len += fdef->numlocals;
    }
#endif
}

void
DeclareFunctions(Module *P)
{
    AST *ast;

    ast = P->funcblock;
    while (ast) {
        doDeclareFunction(ast->left);
        ast = ast->right;
    }
}

/*
 * try to optimize lookup on constant arrays
 * if we can, modifies ast and returns any
 * declarations needed
 */
static AST *
ModifyLookup(AST *top)
{
    int len = 0;
    AST *ast;
    AST *expr;
    AST *ev;
    AST *table;
    AST *id;
    AST *decl;

    ev = top->left;
    table = top->right;
    if (table->kind == AST_TEMPARRAYUSE) {
        /* already modified, skip it */
        return NULL;
    }
    if (ev->kind != AST_LOOKEXPR || table->kind != AST_EXPRLIST) {
        ERROR(ev, "Internal error in lookup");
        return NULL;
    }
    /* see if the table is constant, and count the number of elements */
    ast = table;
    while (ast) {
        int c, d;
        expr = ast->left;
        ast = ast->right;

        if (expr->kind == AST_RANGE) {
            c = EvalConstExpr(expr->left);
            d = EvalConstExpr(expr->right);
            len += abs(d - c) + 1;
        } else if (expr->kind == AST_STRING) {
            len += strlen(expr->d.string);
        } else {
            if (IsConstExpr(expr))
                len++;
            else
                return NULL;
        }
    }

    /* if we get here, the array is constant of length "len" */
    /* create a temporary identifier for it */
    id = AstTempVariable("look_");
    /* replace the table in the top expression */
    top->right = NewAST(AST_TEMPARRAYUSE, id, AstInteger(len));

    /* create a declaration */
    decl = NewAST(AST_TEMPARRAYDECL, NewAST(AST_ARRAYDECL, id, AstInteger(len)), table);

    /* put it in a list holder */
    decl = NewAST(AST_LISTHOLDER, decl, NULL);

    return decl;
}

/*
 * Normalization of function and expression structures
 *
 * there are a number of simple optimizations we can perform on a function
 * (1) Convert lookup/lookdown into constant array references
 * (2) Eliminate unused result variables
 *
 * Called recursively; the top level call has ast = func->body
 * Returns an AST that should be printed before the function body, e.g.
 * to declare temporary arrays.
 */
static AST *
NormalizeFunc(AST *ast, Function *func)
{
    AST *ldecl;
    AST *rdecl;

    if (!ast)
        return NULL;

    switch (ast->kind) {
    case AST_RETURN:
        if (ast->left) {
            return NormalizeFunc(ast->left, func);
        }
        return NULL;
    case AST_RESULT:
        func->result_used = 1;
        return NULL;
    case AST_IDENTIFIER:
        rdecl = func->resultexpr;
        if (rdecl && AstMatch(rdecl, ast))
            func->result_used = 1;
        return NULL;
    case AST_INTEGER:
    case AST_FLOAT:
    case AST_STRING:
    case AST_STRINGPTR:
    case AST_CONSTANT:
    case AST_HWREG:
    case AST_CONSTREF:
        return NULL;
    case AST_LOOKUP:
    case AST_LOOKDOWN:
        return ModifyLookup(ast);
    default:
        ldecl = NormalizeFunc(ast->left, func);
        rdecl = NormalizeFunc(ast->right, func);
        return AddToList(ldecl, rdecl);
    }
}

/*
 * find the variable symbol in an array declaration or identifier
 */
Symbol *VarSymbol(Function *func, AST *ast)
{
    if (ast && ast->kind == AST_ARRAYDECL)
        ast = ast->left;
    if (ast == 0 || ast->kind != AST_IDENTIFIER) {
        ERROR(ast, "internal error: expected variable name\n");
        return NULL;
    }
    return FindSymbol(&func->localsyms, ast->d.string);
}

/*
 * add a local variable to a function
 */
void
AddLocalVariable(Function *func, AST *var, int type)
{
    AST *varlist = NewAST(AST_LISTHOLDER, var, NULL);
    EnterVars(type, &func->localsyms, ast_type_long, varlist, func->numlocals * LONG_SIZE);
    func->locals = AddToList(func->locals, NewAST(AST_LISTHOLDER, var, NULL));
    func->numlocals++;
    if (func->localarray) {
        func->localarray_len++;
    }
}

AST *
AstTempLocalVariable(const char *prefix)
{
    char *name;
    AST *ast = NewAST(AST_IDENTIFIER, NULL, NULL);

    name = NewTemporaryVariable(prefix);
    ast->d.string = name;
    AddLocalVariable(curfunc, ast, SYM_TEMPVAR);
    return ast;
}

/*
 * transform a case expression list into a single boolean test
 * "var" is the variable the case is testing against (may be
 * a temporary)
 */
AST *
TransformCaseExprList(AST *var, AST *ast)
{
    AST *listexpr = NULL;
    AST *node;
    while (ast) {
        if (ast->kind == AST_OTHER) {
            return AstInteger(1);
        } else {
            if (ast->left->kind == AST_RANGE) {
                node = NewAST(AST_ISBETWEEN, var, ast->left);
            } else {
                node = AstOperator(T_EQ, var, ast->left);
            }
        }
        if (listexpr) {
            listexpr = AstOperator(T_OR, listexpr, node);
        } else {
            listexpr = node;
        }
        ast = ast->right;
    }
    return listexpr;
}

/*
 * print a counting repeat loop
 */
AST *
TransformCountRepeat(AST *ast)
{
    AST *origast = ast;
    AST *fromval, *toval;
    AST *stepval;
    AST *limit;
    AST *step;
    AST *loop_le_limit, *loop_ge_limit;
    AST *loopvar = NULL;
    AST *loopleft, *loopright;
    AST *initstmt;
    AST *condtest;
    AST *stepstmt;
    AST *forast;
    AST *body;
    AST *initvar = NULL;
    
    int negstep = 0;
    int needsteptest = 1;
    int deltaknown = 0;
    int32_t delta = 0;
    int useLt = 0;
    int loopkind = AST_FOR;
    
    if (ast->left) {
        if (ast->left->kind == AST_IDENTIFIER) {
            loopvar = ast->left;
        } else if (ast->left->kind == AST_RESULT) {
            loopvar = ast->left;
        } else {
            ERROR(ast, "Need a variable name for the loop");
            return origast;
        }
    }
    ast = ast->right;
    if (ast->kind != AST_FROM) {
        ERROR(ast, "expected FROM");
        return origast;
    }
    fromval = ast->left;
    ast = ast->right;
    if (ast->kind != AST_TO) {
        ERROR(ast, "expected TO");
        return origast;
    }
    toval = ast->left;
    ast = ast->right;
    if (ast->kind != AST_STEP) {
        ERROR(ast, "expected STEP");
        return origast;
    }
    if (ast->left) {
        stepval = ast->left;
    } else {
        stepval = AstInteger(1);
    }
    body = ast->right;

    /* create new ast elements using this ast's line info */
    AstReportAs(toval ? toval : origast);

    /* for fixed counts (like "REPEAT expr") we get a NULL value
       for fromval; this signals that we should be counting
       from 0 to toval - 1 (in C) or from toval down to 1 (in asm)
    */
    if (fromval == NULL) {
        needsteptest = 0;
        if (gl_output == OUTPUT_C || gl_output == OUTPUT_CPP) {
            useLt = 1;
            fromval = AstInteger(0);
            negstep = 0;
        } else {
            fromval = toval;
            toval = AstInteger(1);
            negstep = 1;
        }
    } else if (IsConstExpr(fromval) && IsConstExpr(toval)) {
        int32_t fromi, toi;

        fromi = EvalConstExpr(fromval);
        toi = EvalConstExpr(toval);
        needsteptest = 0;
        negstep = (fromi > toi);
    }

    /* set the loop variable */
    if (!loopvar) {
        loopvar = AstTempLocalVariable("_idx_");
    }

    if (!IsConstExpr(fromval)) {
        initvar = AstTempLocalVariable("_start_");
        initstmt = AstAssign(T_ASSIGN, loopvar, AstAssign(T_ASSIGN, initvar, fromval));
    } else {
        initstmt = AstAssign(T_ASSIGN, loopvar, fromval);
        initvar = fromval;
    }
    /* set the limit variable */
    if (IsConstExpr(toval)) {
        if (gl_expand_constants) {
            limit = AstInteger(EvalConstExpr(toval));
        } else {
            limit = toval;
        }
    } else {
        limit = AstTempLocalVariable("_limit_");
        initstmt = NewAST(AST_SEQUENCE, initstmt, AstAssign(T_ASSIGN, limit, toval));
    }
    /* set the step variable */
    if (IsConstExpr(stepval) && !needsteptest) {
        delta = EvalConstExpr(stepval);
        if (negstep) delta = -delta;
        step = AstInteger(delta);
        deltaknown = 1;
    } else {
        if (negstep) stepval = AstOperator(T_NEGATE, NULL, stepval);
        step = AstTempLocalVariable("_step_");
        initstmt = NewAST(AST_SEQUENCE, initstmt, AstAssign(T_ASSIGN, step, stepval));
    }

    if (deltaknown && delta == 1) {
        stepstmt = AstOperator(T_INCREMENT, loopvar, NULL);
    } else if (deltaknown && delta == -1) {
        stepstmt = AstOperator(T_DECREMENT, NULL, loopvar);
    } else {
        stepstmt = AstAssign('+', loopvar, step);
    }
    
    /* want to do:
     * if (loopvar > limit) step = -step;
     * do {
     *   body
     * } while ( (step > 0 && loopvar <= limit) || (step < 0 && loopvar >= limit));
     */

    loop_ge_limit = AstOperator(T_GE, loopvar, limit);

    /* try to make things a bit more idiomatic; if the limit is N - 1,
       change the ge_limit to actually be "< N" rather than "<= N - 1"
    */
    if (!useLt && limit->kind == AST_OPERATOR && limit->d.ival == '-'
        && limit->right->kind == AST_INTEGER
        && limit->right->d.ival == 1)
    {
        loop_le_limit = AstOperator('<', loopvar, limit->left);
    } else if (useLt) {
        loop_le_limit = AstOperator('<', loopvar, limit);
    } else {
        loop_le_limit = AstOperator(T_LE, loopvar, limit);
    }
    if (needsteptest) {
        AST *fixstep;
        fixstep = NewAST( AST_CONDRESULT, loop_ge_limit,
                          NewAST(AST_THENELSE,
                                 AstOperator(T_NEGATE, NULL, step),
                                 step) );
        initstmt = NewAST(AST_SEQUENCE, initstmt,
                          AstAssign(T_ASSIGN, step, fixstep));
    }

    if (deltaknown) {
        if (delta > 0) {
            loopleft = loop_le_limit;
            loopright = AstInteger(0);
        } else if (delta < 0) {
            loopleft = AstInteger(0);
            loopright = loop_ge_limit;
        } else {
            loopleft = loopright = AstInteger(0);
        }
    } else {
        loopleft = AstOperator(T_AND, AstOperator('>', step, AstInteger(0)), loop_le_limit);
        loopright = AstOperator(T_AND, AstOperator('<', step, AstInteger(0)), loop_ge_limit);
    }

    if (IsConstExpr(loopleft)) {
        condtest = loopright;
    } else if (IsConstExpr(loopright)) {
        condtest = loopleft;
    } else {
        // condtest = AstOperator(T_OR, loopleft, loopright);
        // use the AST_BETWEEN operator for better code
        condtest = NewAST(AST_ISBETWEEN, loopvar, NewAST(AST_RANGE, initvar, limit));
        /* the loop has to execute at least once */
        if (gl_output == OUTPUT_C || gl_output == OUTPUT_CPP) {
            condtest = AstOperator(T_OR, condtest, AstOperator(T_EQ, loopvar, fromval));
        } else {
            loopkind = AST_FORATLEASTONCE;
        }
    }

    // optimize counting down to 1; x != 0 is much faster than x >= 1
    if (deltaknown && delta == -1) {
        if (condtest->kind == AST_OPERATOR && condtest->d.ival == T_GE
            && IsConstExpr(condtest->right)
            && 1 == EvalConstExpr(condtest->right))
        {
            AST *lhs = condtest->left;
            condtest = AstOperator(T_NE, lhs, AstInteger(0));
            /* if we know we will execute at least once, optimize */
            if (IsConstExpr(fromval) && EvalConstExpr(fromval) >= 1) {
                loopkind = AST_FORATLEASTONCE;
            }
        }
    }
    stepstmt = NewAST(AST_STEP, stepstmt, body);
    condtest = NewAST(AST_TO, condtest, stepstmt);
    forast = NewAST((enum astkind)loopkind, initstmt, condtest);
    forast->line = origast->line;
    return forast;
}

/*
 * parse annotation directives
 */
int match(const char *str, const char *pat)
{
    return !strncmp(str, pat, strlen(pat));
}

static void
ParseDirectives(const char *str)
{
    if (match(str, "nospin"))
        gl_nospin = 1;
    else if (match(str, "ccode")) {
        if (gl_output == OUTPUT_CPP)
            gl_output = OUTPUT_C;
    }
}

/*
 * an annotation just looks like a function with no name or body
 */
void
DeclareToplevelAnnotation(AST *anno)
{
    Function *f;
    const char *str;

    /* check the annotation string; some of them are special */
    str = anno->d.string;
    if (*str == '!') {
        /* directives, not code */
        str += 1;
        ParseDirectives(str);
    } else {
        f = NewFunction();
        f->annotations = anno;
    }
}

/*
 * type inference
 */

/* check for static member functions, i.e. ones that do not
 * use any variables in the object, and hence can be called
 * without the object itself as an implicit parameter
 */
static void
CheckForStatic(Function *fdef, AST *body)
{
    Symbol *sym;
    if (!body || !fdef->is_static) {
        return;
    }
    switch(body->kind) {
    case AST_IDENTIFIER:
        sym = FindSymbol(&fdef->localsyms, body->d.string);
        if (!sym) {
            sym = LookupSymbol(body->d.string);
        }
        if (sym) {
            if (sym->type == SYM_VARIABLE || sym->type == SYM_OBJECT) {
                fdef->is_static = 0;
                return;
            }
            if (sym->type == SYM_FUNCTION) {
                Function *func = (Function *)sym->val;
                if (func) {
                    fdef->is_static = fdef->is_static && func->is_static;
                } else {
                    fdef->is_static = 0;
                }
            }
        } else {
            // assume this is an as-yet-undefined member
            fdef->is_static = 0;
        }
        break;
    default:
        CheckForStatic(fdef, body->left);
        CheckForStatic(fdef, body->right);
    }
}

/*
 * Check for function return type
 * This returns 1 if we see a return statement, 0 if not
 */
static int CheckRetStatement(Function *func, AST *ast);

static int
CheckRetStatementList(Function *func, AST *ast)
{
    int sawreturn = 0;
    while (ast) {
        if (ast->kind != AST_STMTLIST) {
            ERROR(ast, "Internal error: expected statement list, got %d",
                  ast->kind);
            return 0;
        }
        sawreturn |= CheckRetStatement(func, ast->left);
        ast = ast->right;
    }
    return sawreturn;
}

static bool
IsResultVar(Function *func, AST *lhs)
{
    if (lhs->kind == AST_RESULT) {
        return true;
    }
    if (lhs->kind == AST_IDENTIFIER) {
        return AstMatch(lhs, func->resultexpr);
    }
    return false;
}

static int
CheckRetCaseMatchList(Function *func, AST *ast)
{
    AST *item;
    int sawReturn = 1;
    while (ast) {
        if (ast->kind != AST_LISTHOLDER) {
            ERROR(ast, "Internal error, expected list holder");
            return sawReturn;
        }
        item = ast->left;
        ast = ast->right;
        if (item->kind != AST_CASEITEM) {
            ERROR(item, "Internal error, expected case item");
            return sawReturn;
        }
        sawReturn = CheckRetStatementList(func, item->right) && sawReturn;
    }
    return sawReturn;
}

static int
CheckRetStatement(Function *func, AST *ast)
{
    int sawreturn = 0;
    AST *lhs, *rhs;
    
    if (!ast) return 0;
    switch (ast->kind) {
    case AST_COMMENTEDNODE:
        sawreturn = CheckRetStatement(func, ast->left);
        break;
    case AST_RETURN:
        if (ast->left) {
            SetFunctionType(func, ExprType(ast->left));
        }
        sawreturn = 1;
        break;
    case AST_ABORT:
        if (ast->left) {
            (void)CheckRetStatement(func, ast->left);
            SetFunctionType(func, ExprType(ast->left));
        }
        break;
    case AST_IF:
        ast = ast->right;
        if (ast->kind == AST_COMMENTEDNODE)
            ast = ast->left;
        sawreturn = CheckRetStatementList(func, ast->left);
        sawreturn = CheckRetStatementList(func, ast->right) && sawreturn;
        break;
    case AST_CASE:
        sawreturn = CheckRetCaseMatchList(func, ast->right);
        break;
    case AST_WHILE:
    case AST_DOWHILE:
        sawreturn = CheckRetStatementList(func, ast->right);
        break;
    case AST_COUNTREPEAT:
        lhs = ast->left; // count variable
        if (lhs) {
            if (IsResultVar(func, lhs)) {
                SetFunctionType(func, ast_type_long);
            }
        }
        ast = ast->right; // from value
        ast = ast->right; // to value
        ast = ast->right; // step value
        ast = ast->right; // body
        sawreturn = CheckRetStatementList(func, ast);
        break;
    case AST_STMTLIST:
        sawreturn = CheckRetStatementList(func, ast);
        break;
    case AST_ASSIGN:
        lhs = ast->left;
        rhs = ast->right;
        if (IsResultVar(func, lhs)) {
            SetFunctionType(func, ExprType(rhs));
        }
        sawreturn = 0;
        break;
    default:
        sawreturn = 0;
        break;
    }
    return sawreturn;
}

/*
 * check function calls for correct number of arguments
 */
void
CheckFunctionCalls(AST *ast)
{
    int expectArgs;
    int gotArgs;
    Symbol *sym;
    const char *fname = "function";
    
    if (!ast) {
        return;
    }
    if (ast->kind == AST_FUNCCALL) {
        AST *a;
        sym = FindFuncSymbol(ast, NULL, NULL);
        expectArgs = 0;
        if (sym) {
            fname = sym->name;
            if (sym->type == SYM_BUILTIN) {
                Builtin *b = (Builtin *)sym->val;
                expectArgs = b->numparameters;
            } else if (sym->type == SYM_FUNCTION) {
                Function *f = (Function *)sym->val;
                expectArgs = f->numparams;
            } else {
                ERROR(ast, "Unexpected function type");
                return;
            }
        }
        gotArgs = 0;
        for (a = ast->right; a; a = a->right) {
            gotArgs++;
        }
        if (gotArgs != expectArgs) {
            ERROR(ast, "Bad number of parameters in call to %s: expected %d found %d", fname, expectArgs, gotArgs);
        }
    }
    CheckFunctionCalls(ast->left);
    CheckFunctionCalls(ast->right);
}

/*
 * do basic processing of functions
 */
void
ProcessFuncs(Module *P)
{
    Function *pf;
    int sawreturn = 0;

    current = P;
    for (pf = P->functions; pf; pf = pf->next) {
        CheckRecursive(pf);  /* check for recursive functions */
        pf->extradecl = NormalizeFunc(pf->body, pf);

        CheckFunctionCalls(pf->body);
        
        /* check for void functions */
        pf->rettype = NULL;
        sawreturn = CheckRetStatementList(pf, pf->body);
        if (pf->rettype == NULL && pf->result_used) {
            /* there really is a return type */
            pf->rettype = ast_type_generic;
        }
        if (pf->rettype == NULL) {
            pf->rettype = ast_type_void;
            pf->resultexpr = NULL;
        } else {
            if (!pf->result_used) {
                pf->resultexpr = AstInteger(0);
                pf->result_used = 1;
            }
            if (!sawreturn) {
                AST *retstmt;

                retstmt = NewAST(AST_STMTLIST, NewAST(AST_RETURN, pf->resultexpr, NULL), NULL);
                pf->body = AddToList(pf->body, retstmt);
            }
        }
    }
}

/*
 * main entry for type checking
 */
int
InferTypes(Module *P)
{
    Function *pf;
    int changes = 0;
    
    /* scan for static definitions */
    current = P;
    for (pf = P->functions; pf; pf = pf->next) {
        if (pf->is_static) {
            continue;
        }
        pf->is_static = 1;
        CheckForStatic(pf, pf->body);
        if (pf->is_static) {
            changes++;
        }
    }
    return changes;
}

static void
MarkUsedBody(AST *body)
{
    Symbol *sym, *objsym;
    AST *objref;
    
    if (!body) return;
    switch(body->kind) {
    case AST_IDENTIFIER:
        sym = LookupSymbol(body->d.string);
        if (sym && sym->type == SYM_FUNCTION) {
            Function *func = (Function *)sym->val;
            MarkUsed(func);
        }
        break;
    case AST_METHODREF:
        objref = body->left;
        objsym = LookupAstSymbol(objref, "object reference");
        if (!objsym) return;
        if (objsym->type != SYM_OBJECT) {
            ERROR(body, "%s is not an object", objsym->name);
            return;
        }
        sym = LookupObjSymbol(body, objsym, body->right->d.string);
        if (!sym || sym->type != SYM_FUNCTION) {
            return;
        }
        MarkUsed((Function *)sym->val);
        break;
    default:
        MarkUsedBody(body->left);
        MarkUsedBody(body->right);
        break;
    }
}

#define CALLSITES_MANY 10

void
MarkUsed(Function *f)
{
    Module *oldcurrent;
    if (!f || f->callSites > CALLSITES_MANY) {
        return;
    }
    f->callSites++;
    oldcurrent = current;
    current = f->module;
    MarkUsedBody(f->body);
    current = oldcurrent;
}

void
SetFunctionType(Function *f, AST *typ)
{
    f->rettype = typ;
}

/*
 * check to see if function ref may be called from AST body
 * also clears the is_leaf flag on the function if we see any
 * calls within it
 */
bool
IsCalledFrom(Function *ref, AST *body, int visitRef)
{
    Module *oldState;
    Symbol *sym;
    Function *func;
    bool result;
    
    if (!body) return false;
    switch(body->kind) {
    case AST_FUNCCALL:
        ref->is_leaf = 0;
        sym = FindFuncSymbol(body, NULL, NULL);
        if (!sym) return false;
        if (sym->type != SYM_FUNCTION) return false;
        func = (Function *)sym->val;
        if (ref == func) return true;
        if (func->visitFlag == visitRef) {
            // we've been here before
            return false;
        }
        func->visitFlag = visitRef;
        oldState = current;
        current = func->module;
        result = IsCalledFrom(ref, func->body, visitRef);
        current = oldState;
        return result;
    default:
        return IsCalledFrom(ref, body->left, visitRef)
            || IsCalledFrom(ref, body->right, visitRef);
        break;
    }
    return false;
}

void
CheckRecursive(Function *f)
{
    visitPass++;
    f->is_leaf = 1; // possibly a leaf function
    f->is_recursive = IsCalledFrom(f, f->body, visitPass);
}

/*
 * change a small longmove call into a series of assignments
 * returns true if transform done
 */
#define LONGMOVE_THRESHOLD 4

static bool
TransformLongMove(AST **astptr, AST *ast)
{
    AST *dst;
    AST *src;
    AST *count;
    AST *sequence;
    AST *assign;
    Symbol *syms, *symd;
    int srcoff, dstoff;
    int n;
    SymbolTable *srctab, *dsttab;
    
    ast = ast->right; // ast->left is the longmove function
    dst = ast->left; ast = ast->right;
    if (!ast || !dst) return false;
    src = ast->left; ast = ast->right;
    if (!ast || !src) return false;
    count = ast->left; ast = ast->right;
    if (ast || !count) return false;
    if (!IsConstExpr(count)) return false;
    n = EvalConstExpr(count);
    if (n > LONGMOVE_THRESHOLD || n <= 0) return false;

    // check src and dst
    if (src->kind != AST_ADDROF && src->kind != AST_ABSADDROF) return false;
    src = src->left;
    if (src->kind != AST_IDENTIFIER) return false;
    if (dst->kind != AST_ADDROF && dst->kind != AST_ABSADDROF) return false;
    dst = dst->left;
    if (dst->kind != AST_IDENTIFIER) return false;

    // ok, we have the pattern we like
    syms = LookupSymbol(src->d.string);
    symd = LookupSymbol(dst->d.string);
    if (!syms || !symd) return false;

    switch(syms->type) {
    case SYM_VARIABLE:
        srctab = &current->objsyms;
        break;
    case SYM_PARAMETER:
    case SYM_LOCALVAR:
        srctab = &curfunc->localsyms;
        break;
    default:
        return false;
    }
    switch(symd->type) {
    case SYM_VARIABLE:
        dsttab = &current->objsyms;
        break;
    case SYM_PARAMETER:
    case SYM_LOCALVAR:
        dsttab = &curfunc->localsyms;
        break;
    default:
        return false;
    }
    AstReportAs(dst);
    srcoff = syms->offset;
    dstoff = symd->offset;
    sequence = NULL;
    for(;;) {
        assign = AstAssign(T_ASSIGN,
                           AstIdentifier(symd->name),
                           AstIdentifier(syms->name));
        sequence = AddToList(sequence, NewAST(AST_SEQUENCE, assign, NULL));
        --n;
        if (n == 0) break;
        srcoff += 4;
        dstoff += 4;
        symd = FindSymbolByOffset(dsttab, dstoff);
        syms = FindSymbolByOffset(srctab, srcoff);
        if (!symd || !syms) {
            return false;
        }
    }
    *astptr = sequence;
    /* the longmove probably indicates a COG will be reading these
       variables */
    current->volatileVariables = 1;
    return true;
}

static bool
IsLocalVariable(AST *ast) {
    Symbol *sym;
    switch (ast->kind) {
    case AST_IDENTIFIER:
        sym = LookupSymbol(ast->d.string);
        if (!sym) return false;
        switch (sym->type) {
        case SYM_RESULT:
        case SYM_LOCALVAR:
        case SYM_PARAMETER:
            return true;
        default:
            return false;
        }
        break;
    case AST_ARRAYREF:
        return IsLocalVariable(ast->left);
    default:
        return false;
    }
}

/*
 * SpinTransform
 * transform AST to reflect some oddities of the Spin language:
 * (1) It's legal to call a void function, just substitute 0 for the result
 * (2) Certain operators used at top level are changed into assignments
 * (3) Validate parameters to some builtins
 * (4) Turn AST_COUNTREPEAT into AST_FOR
 * (5) make sure expression in a case statement is a variable
 */
/* if level is 0, we are inside an expression
 * level == 1 at top level
 * level == 2 inside a coginit
 */
static void
doSpinTransform(AST **astptr, int level)
{
    AST *ast = *astptr;
    Symbol *sym;
    Function *func;
    
    while (ast && ast->kind == AST_COMMENTEDNODE) {
        astptr = &ast->left;
        ast = *astptr;
    }
    if (!ast) return;
    switch (ast->kind) {
    case AST_EXPRLIST:
    case AST_THENELSE:
        doSpinTransform(&ast->left, level);
        doSpinTransform(&ast->right, level);
        break;
    case AST_RETURN:
    case AST_ABORT:
        doSpinTransform(&ast->left, 0);
        break;
    case AST_IF:
    case AST_WHILE:
    case AST_DOWHILE:
        doSpinTransform(&ast->left, 0);
        doSpinTransform(&ast->right, level);
        break;
    case AST_COUNTREPEAT:
        ast = ast->right; // from value
        doSpinTransform(&ast->left, 0);
        ast = ast->right; // to value
        doSpinTransform(&ast->left, 0);
        ast = ast->right; // step value
        doSpinTransform(&ast->left, 0);
        doSpinTransform(&ast->right, level);

        /* now fix it up */
        *astptr = TransformCountRepeat(*astptr);
        break;
    case AST_STMTLIST:
        doSpinTransform(&ast->left, level);
        doSpinTransform(&ast->right, level);
        break;
    case AST_CASE:
    {
        AST *list = ast->right;
        doSpinTransform(&ast->left, 0);
        if (ast->left->kind != AST_IDENTIFIER && ast->left->kind != AST_ASSIGN) {
            AST *var = AstTempLocalVariable("_tmp_");
            ast->left = AstAssign(T_ASSIGN, var, ast->left);
        }
        while (list) {
            doSpinTransform(&list->left->left, 0);
            doSpinTransform(&list->left->right, level);
            list = list->right;
        }
        break;
    }
    case AST_COGINIT:
        if (0 != (func = IsSpinCoginit(ast))) {
            current->needsCoginit = 1;
            func->cog_task = 1;
            if (!func->is_static) {
                func->force_static = 1;
                func->is_static = 1;
            }
        }
        doSpinTransform(&ast->left, 2);
        doSpinTransform(&ast->right, 2);
        break;
    case AST_FUNCCALL:
        if (level == 0) {
            /* check for void functions here; if one is called,
               pretend it returned 0 */
            sym = FindFuncSymbol(ast, NULL, NULL);
            if (sym && sym->type == SYM_FUNCTION) {
                Function *f = (Function *)sym->val;
                if (f->rettype == ast_type_void) {
                    AST *seq = NewAST(AST_SEQUENCE, ast, AstInteger(0));
                    *astptr = seq;
                }
            }
        }
        /* check for longmove(@x, @y, n) where n is a small
           number */
        if (level == 1 && ast->left && ast->left->kind == AST_IDENTIFIER
            && !strcasecmp(ast->left->d.string, "longmove"))
        {
            TransformLongMove(astptr, ast);
            ast = *astptr;
        }
        doSpinTransform(&ast->left, 0);
        doSpinTransform(&ast->right, 0);
        break;
    case AST_POSTEFFECT:
    {
        /* x~ is the same as (tmp = x, x = 0, tmp) */
        /* x~~ is (tmp = x, x = -1, tmp) */
        AST *target;
        AST *tmp;
        AST *seq1, *seq2;
        if (ast->d.ival == '~') {
            target = AstInteger(0);
        } else if (ast->d.ival == T_DOUBLETILDE) {
            target = AstInteger(-1);
        } else {
            ERROR(ast, "bad posteffect operator %d", ast->d.ival);
            target = AstInteger(0);
        }
         if (ast->right != NULL) {
            ERROR(ast, "Expected NULL on right of posteffect");
        }
        if (level == 1) {
            // at toplevel we can ignore the old result
            *astptr = AstAssign(T_ASSIGN, ast->left, target);
        } else {
            tmp = AstTempLocalVariable("_tmp_");

            seq1 = NewAST(AST_SEQUENCE,
                          AstAssign(T_ASSIGN, tmp, ast->left),
                          AstAssign(T_ASSIGN, ast->left, target));
            seq2 = NewAST(AST_SEQUENCE, seq1, tmp);
            *astptr = seq2;
        }
        // we may have a range reference in here, so do the
        // transform on the result
        doSpinTransform(astptr, level);
	break;
    }
    case AST_ASSIGN:
        if (ast->left && ast->left->kind == AST_RANGEREF) {
            *astptr = ast = TransformRangeAssign(ast->left, ast->right, level == 1);
        }
        doSpinTransform(&ast->left, 0);
        doSpinTransform(&ast->right, 0);
        break;
    case AST_RANGEREF:
        *astptr = ast = TransformRangeUse(ast);
        break;
    case AST_ADDROF:
    case AST_ABSADDROF:
        doSpinTransform(&ast->left, 0);
        if (IsLocalVariable(ast->left)) {
            curfunc->local_address_taken = 1;
        }
        break;
    case AST_OPERATOR:
        if (level == 1) {
            AST *lhsast;
            int line = ast->line;
            switch (ast->d.ival) {
            case T_NEGATE:
            case T_ABS:
            case T_SQRT:
            case T_BIT_NOT:
            case T_DECODE:
            case T_ENCODE:
                lhsast = DupAST(ast->right);
                *astptr = ast = AstAssign(T_ASSIGN, lhsast, ast);
                ast->line = lhsast->line = line;
                doSpinTransform(astptr, level);
                break;
            }
        } else {
            AST *lhsast;
            switch (ast->d.ival) {
            case T_DECODE:
                lhsast = AstOperator(T_SHL, AstInteger(1), ast->right);
                lhsast->line = ast->line;
                *astptr = ast = lhsast;
                break;
            }
        }
        /* fall through */
    default:
        doSpinTransform(&ast->left, 0);
        doSpinTransform(&ast->right, 0);
        break;
    }
}

void
SpinTransform(Module *Q)
{
    Module *savecur = current;
    Function *func;
    Function *savefunc = curfunc;
    current = Q;
    for (func = Q->functions; func; func = func->next) {
        curfunc = func;
        doSpinTransform(&func->body, 1);

        // ScanFunctionBody is left over from older code
        // it should probably be merged in with doSpinTransform
        /* check for special conditions */
        ScanFunctionBody(func, func->body, NULL);

        /* if we put the locals into an array, record the size of that array */
        if (func->localarray) {
            func->localarray_len += func->numlocals;
        }
    }
    curfunc = savefunc;
    current = savecur;
}
