//
// binary data output for spin2cpp
//
// Copyright 2012-2016 Total Spectrum Software Inc.
// see the file COPYING for conditions of redistribution
//
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "spinc.h"
#include "outcpp.h"

void
OutputGasFile(const char *fname, Module *P)
{
    FILE *f = NULL;
    Module *save;
    Flexbuf fb;
    
    save = current;
    current = P;

    f = fopen(fname, "wb");
    if (!f) {
        perror(fname);
        exit(1);
    }

    flexbuf_init(&fb, BUFSIZ);
    PrintDataBlockForGas(&fb, P, 0 /* inline asm */);
    fwrite(flexbuf_peek(&fb), flexbuf_curlen(&fb), 1, f);
    fclose(f);
    flexbuf_delete(&fb);
    
    current = save;
}

/*
 * functions for output of DAT sections
 */
/*
 * some defines for inline asm
 */
#define INLINE_ASM_LINELEN 70

/*
 * data block printing functions
 */
#define BYTES_PER_LINE 16  /* must be at least 4 */
static int datacount = 0;

static void
startLine(Flexbuf *f, int inlineAsm)
{
    if (inlineAsm) {
        flexbuf_printf(f, "_dat_(");
    }
}

static void
endLine(Flexbuf *f, int inlineAsm)
{
    if (inlineAsm) {
        // look back and see how many spaces we need to include to line everything up
        const char *here;
        size_t count;
        size_t linelen = 0;
        count = flexbuf_curlen(f);
        here = flexbuf_peek(f);
        while (count > 0) {
            --count;
            if (here[count] == '\n') break;
            linelen++;
        }
        if (linelen < INLINE_ASM_LINELEN) {
            linelen = INLINE_ASM_LINELEN - linelen;
            flexbuf_printf(f, "%*s", linelen, " ");
        }
        flexbuf_printf(f, ");");
    }
    flexbuf_printf(f, "\n");
}

static void
forceAlign(Flexbuf *f, int size, int inlineAsm)
{
    if ( (datacount % size) != 0 ) {
        startLine(f, inlineAsm);
        flexbuf_printf(f, "%11s %-7s %d", " ", ".balign", size);
        endLine(f, inlineAsm);
        datacount = (datacount + size - 1) & ~(size-1);
    }
}

static void
PrintQuotedChar(Flexbuf *f, int val)
{
    switch (val) {
    case '"':
    case '\'':
    case '\\':
        flexbuf_printf(f, "\\%c", val);
        break;
    case 0:
        flexbuf_printf(f, "\\0");
        break;
    case 10:
        flexbuf_printf(f, "\\n");
        break;
    case 13:
        flexbuf_printf(f, "\\r");
        break;
    default:
        flexbuf_printf(f, "%c", val);
        break;
    }
}

static bool
ShouldPrintAsString(AST *ast)
{
    AST *sub;
    int val;
    while (ast) {
        sub = ast->left;
        if (sub->kind == AST_ARRAYDECL || sub->kind == AST_ARRAYREF) {
            return false;
        } else if (sub->kind == AST_STRING) {
            // OK
        } else {
            if (!IsConstExpr(sub)) {
                return false;
            }
            val = EvalConstExpr(sub);
            if (!(isprint(val) || val == 10 || val == 13 || val == 0)) {
                return false;
            }
        }
        ast = ast->right;
    }
    return true;
}

static void
outputGasDataList(Flexbuf *f, const char *prefix, AST *ast, int size, int inlineAsm)
{
    int reps;
    AST *sub;
    char *comma = "";
    AST *origval = NULL;
    bool isString = false;
    
    if (size == 1 && ShouldPrintAsString(ast)) {
        isString = true;
        prefix = ".ascii";
    }
    forceAlign(f, size, inlineAsm);
    startLine(f, inlineAsm);
    flexbuf_printf(f, "%11s %-7s ", " ", prefix);
    if (isString) {
        flexbuf_printf(f, "\"");
    }
    while (ast) {
        sub = ast->left;
        if (sub->kind == AST_ARRAYDECL || sub->kind == AST_ARRAYREF) {
            origval = ast->left->left;
            reps = EvalPasmExpr(ast->left->right);
        } else if (sub->kind == AST_STRING) {
            const char *ptr = sub->d.string;
            unsigned val;
            while (*ptr) {
                val = (*ptr++) & 0xff;
                if (isString) {
                    PrintQuotedChar(f, val);
                } else {
                    flexbuf_printf(f, "%s%d", comma, val);
                }
                comma = ", ";
                datacount += size;
            }
            reps = 0;
        } else {
            origval = ast->left;
            reps = 1;
        }
        while (reps > 0) {
            if (isString) {
                unsigned val = EvalConstExpr(origval);
                PrintQuotedChar(f, val);
            } else {
                flexbuf_printf(f, "%s", comma);
                PrintGasExpr(f, origval);
                comma = ", ";
            }
            --reps;
            datacount += size;
        }
        ast = ast->right;
    }
    if (isString) {
        flexbuf_printf(f, "\"");
    }
    endLine(f, inlineAsm);
}

static void
outputGasDirective(Flexbuf *f, const char *prefix, AST *expr, int inlineAsm)
{
    startLine(f, inlineAsm);
    flexbuf_printf(f, "%11s %-7s ", " ", prefix);
    if (expr)
        PrintExpr(f, expr, PRINTEXPR_GAS);
    else
        flexbuf_printf(f, "0");
    endLine(f, inlineAsm);
}

static void
outputGasComment(Flexbuf *f, AST *ast, int inlineAsm)
{
    const char *string;
    if (!ast || ast->kind != AST_COMMENT) return;
    string = ast->d.string;
    if (!string) return;

    PrintCommentString(f, string, 0);
}

#define GAS_WZ 1
#define GAS_WC 2
#define GAS_NR 4
#define GAS_WR 8
#define MAX_OPERANDS 2

static void
outputGasInstruction(Flexbuf *f, AST *ast, int inlineAsm)
{
    Instruction *instr;
    AST *operand[MAX_OPERANDS];
    AST *sub;
    int immflag = 0;
    int effects = 0;
    int i;
    int numoperands = 0;
    int printed_if = 0;
    int printFlags;
    const char *opcode;

    forceAlign(f, 4, inlineAsm);
    startLine(f, inlineAsm);
    instr = (Instruction *)ast->d.ptr;
    /* print modifiers */
    sub = ast->right;
    while (sub != NULL) {
        if (sub->kind == AST_INSTRMODIFIER) {
            InstrModifier *mod = (InstrModifier *)sub->d.ptr;
            if (!strncmp(mod->name, "if_", 3)) {
                flexbuf_printf(f, "  %-9s ", mod->name);
                printed_if = 1;
            } else if (!strcmp(mod->name, "wz")) {
                effects |= GAS_WZ;
            } else if (!strcmp(mod->name, "wc")) {
                effects |= GAS_WC;
            } else if (!strcmp(mod->name, "wr")) {
                effects |= GAS_WR;
            } else if (!strcmp(mod->name, "nr")) {
                effects |= GAS_NR;
            } else if (!strcmp(mod->name, "#")) {
                immflag = 1;
            } else {
                ERROR(sub, "unknown modifier %s", mod->name);
            }
        } else if (sub->kind == AST_EXPRLIST) {
            if (numoperands >= MAX_OPERANDS) {
                ERROR(ast, "Too many operands to instruction");
                return;
            }
            operand[numoperands++] = sub->left;
        } else {
            ERROR(ast, "Internal error parsing instruction");
        }
        sub = sub->right;
    }

    /* print instruction opcode */
    if (instr->opc == OPC_CALL) {
        opcode = "jmpret";
    } else {
        opcode = instr->name;
    }
    if (!printed_if) {
        flexbuf_printf(f, "%11s ", " ");
    }
    flexbuf_printf(f, "%-7s", opcode);
    datacount += 4;
    /* now print the operands */
    for (i = 0; i < numoperands; i++) {
        printFlags = PRINTEXPR_GAS | PRINTEXPR_GASOP;
        if (i == 0)
            flexbuf_printf(f, " ");
        else
            flexbuf_printf(f, ", ");
        if (immflag) {
            switch (instr->ops) {
            case CALL_OPERAND:
                if (i == 0) {
                    AST *ast = operand[i];
                    Symbol *sym;
                    char *retname;
                    if (ast->kind != AST_IDENTIFIER) {
                        ERROR(ast, "call instruction must be to identifier");
                        continue;
                    }
                    retname = alloca(strlen(ast->d.string) + 6);
                    sprintf(retname, "%s_ret", ast->d.string);
                    sym = LookupSymbol(retname);
                    if (!sym || sym->type != SYM_LABEL) {
                        ERROR(ast, "cannot find return label %s", retname);
                        return;
                    }
                    PrintSymbol(f, sym, printFlags);
                    flexbuf_printf(f, ", #");
                    immflag = 0;
                }
                break;
            case SRC_OPERAND_ONLY:
                if (i == 0) {
                    flexbuf_printf(f, "#");
                    if (instr->opc != OPC_JUMP) printFlags |= PRINTEXPR_GASIMM;
                    immflag = 0;
                }
                break;
            case JMPRET_OPERANDS:
                if (i == 1) {
                    flexbuf_printf(f, "#");
                    immflag = 0;
                }
                break;
            default:
                if (i == 1) {
                    flexbuf_printf(f, "#");
                    immflag = 0;
                    printFlags |= PRINTEXPR_GASIMM;
                }
                break;
            }
        }
        PrintExpr(f, operand[i], printFlags);
    }
    if (effects) {
        const char *comma = "";
        const char *effnames[] = { "wz", "wc", "nr", "wr" };
        flexbuf_printf(f, "    ");
        for (i = 0; i < 4; i++) {
            if (effects & (1<<i)) {
                flexbuf_printf(f, "%s%s", comma, effnames[i]);
                comma = ", ";
            }
        }
    }
    endLine(f, inlineAsm);
}

static void
outputGasLabel(Flexbuf *f, AST *id, int inlineAsm)
{
    const char *name = id->d.string;
    Symbol *sym = LookupSymbol(name);
    int align = 1;
    
    if (sym) {
        Label *lab;
        if (sym->type != SYM_LABEL) {
            ERROR(id, "expected label symbol");
        } else {
            lab = (Label *)sym->val;
            align = TypeAlignment(lab->type);
        }
    }
    forceAlign(f, align, inlineAsm);
    startLine(f, inlineAsm);
    flexbuf_printf(f, "  %s:", name);
    endLine(f, inlineAsm);
}

static void
DeclareLabelsGas(Flexbuf *f, Module *P, int inlineAsm)
{
    AST *ast, *top;
    Symbol *sym;
    Label *lab;
   
    if (!inlineAsm) return; // only need this in inline
    for (top = P->datblock; top; top = top->right) {
        ast = top;
        while (ast && ast->kind == AST_COMMENTEDNODE) {
            ast = ast->left;
        }
       if (ast && ast->kind == AST_IDENTIFIER) {
           const char *name = ast->d.string;
           // GAS label declaration
           sym = LookupSymbol(name);
           if (!sym) continue;
           if (sym->type != SYM_LABEL) continue;
           lab = (Label *)sym->val;
           if (lab->flags & LABEL_USED_IN_SPIN) {
                flexbuf_printf(f, "extern ");
                PrintType(f, lab->type);
                flexbuf_printf(f, " %s[] __asm__(\"%s\");\n", name, name);
           }
       }
   }
}

static void
PrintGasConstantDecl(Flexbuf *f, AST *ast, int inlineAsm)
{
    startLine(f, inlineAsm);
    flexbuf_printf(f, "%11s .equ    %s, ", " ", ast->d.string);
    PrintInteger(f, EvalConstExpr(ast), PRINTEXPR_DEFAULT);
    endLine(f, inlineAsm);
}

void
PrintConstantsGas(Flexbuf *f, Module *P, int inlineAsm)
{
    AST *upper, *ast;

    if (inlineAsm) {
        flexbuf_printf(f, "#define _tostr__(...) #__VA_ARGS__\n");
        flexbuf_printf(f, "#define _tostr_(...) _tostr__(__VA_ARGS__)\n");
        flexbuf_printf(f, "#define _dat_(...) __asm__(_tostr_(__VA_ARGS__) \"\\n\")\n");
        flexbuf_printf(f, "#define _lbl_(x) (x - _org_)\n");
        flexbuf_printf(f, "#define _org_ ..dat_start\n");
        return;
    }
    for (upper = P->conblock; upper; upper = upper->right) {
        ast = upper->left;
        while (ast) {
            switch (ast->kind) {
            case AST_IDENTIFIER:
                PrintGasConstantDecl(f, ast, inlineAsm);
                ast = ast->right;
                break;
            case AST_ASSIGN:
                PrintGasConstantDecl(f, ast->left, inlineAsm);
                ast = NULL;
                break;
            case AST_ENUMSKIP:
                PrintGasConstantDecl(f, ast->left, inlineAsm);
                break;
            case AST_COMMENTEDNODE:
                // FIXME: these nodes are backwards, the rest of the list is on the left
                // also, should we print the comment here?
                ast = ast->left;
                break;
            default:
                /* do nothing */
                ast = ast->right;
                break;
            }
        }
    }
}

static void
outputGasOrg(Flexbuf *f, AST *ast, int inlineAsm)
{
    int val = 0;
    Symbol *sym;
    if (!inlineAsm) {
        outputGasDirective(f, ".org", ast->left, inlineAsm);
        return;
    }
    if (ast->left) {
        val = EvalConstExpr(ast->left);
    }
    sym = (Symbol *)ast->d.ptr;
    flexbuf_printf(f, "\n#undef _org_\n");
    flexbuf_printf(f, "#define _org_ %s\n", sym->name);
    startLine(f, inlineAsm);
    flexbuf_printf(f, "%s_base = . + 0x%x", sym->name, val);
    endLine(f, inlineAsm);
}

static void
outputFinalOrgs(Flexbuf *f, AST *ast[], int count, int inlineAsm)
{
    int i;
    Symbol *sym;
    if (count == 0 || inlineAsm == 0) return;
    flexbuf_printf(f, "//\n");
    flexbuf_printf(f, "// due to a gas bug, we need the .org constants to be unknown during the first pass\n");
    flexbuf_printf(f, "// so they have to be defined here, after all asm is done\n");
    flexbuf_printf(f, "//\n");
    for (i = 0; i < count; i++) {
        sym = (Symbol *)ast[i]->d.ptr;
        startLine(f, inlineAsm);
        flexbuf_printf(f, "  .equ %s, %s_base", sym->name, sym->name);
        endLine(f, inlineAsm);
    }
}
#define MAX_ORG_COUNT 80

void
PrintDataBlockForGas(Flexbuf *f, Module *P, int inlineAsm)
{
    AST *ast, *top;
    int saveState;
    AST *saveOrgs[MAX_ORG_COUNT];
    int orgCount = 0;
    
    if (gl_errors != 0)
        return;
    saveState = P->pasmLabels;
    P->pasmLabels = 1;

    /* print constant declarations */
    PrintConstantsGas(f, P, inlineAsm);
    DeclareLabelsGas(f, P, inlineAsm);
    
    if (inlineAsm) {
        startLine(f, inlineAsm);
        flexbuf_printf(f, "%11s .section .%s.dat,\"ax\"", " ", P->classname);
        endLine(f, inlineAsm);
        startLine(f, inlineAsm);
        flexbuf_printf(f, "%11s .compress off", " ");
        endLine(f, inlineAsm);
        startLine(f, inlineAsm);
        flexbuf_printf(f, "  ..dat_start:");
        endLine(f, inlineAsm);
    }
    for (top = P->datblock; top; top = top->right) {
        ast = top;

        while (ast->kind == AST_COMMENTEDNODE) {
            outputGasComment(f, ast->right, inlineAsm);
            ast = ast->left;
        }
        switch (ast->kind) {
        case AST_BYTELIST:
            outputGasDataList(f, ".byte", ast->left, 1, inlineAsm);
            break;
        case AST_WORDLIST:
            outputGasDataList(f, ".word", ast->left, 2, inlineAsm);
            break;
        case AST_LONGLIST:
            outputGasDataList(f, ".long", ast->left, 4, inlineAsm);
            break;
        case AST_INSTRHOLDER:
            outputGasInstruction(f, ast->left, inlineAsm);
            break;
        case AST_LINEBREAK:
            break;
        case AST_IDENTIFIER:
            // FIXME: need to handle labels not on lines (type and alignment)
            outputGasLabel(f, ast, inlineAsm);
            break;
        case AST_FILE:
            ERROR(ast, "File directive not supported in GAS output");
            break;
        case AST_ORG:
            if (orgCount == MAX_ORG_COUNT) {
                ERROR(ast, "too many .org directives in GAS output");
            } else {
                saveOrgs[orgCount++] = ast;
            }
            outputGasOrg(f, ast, inlineAsm);
            break;
        case AST_RES:
            if (0 && inlineAsm && ast->left) {
                outputGasDirective(f," . = . + 4*", ast->left, inlineAsm);
            } else {
                outputGasDirective(f, ".res", ast->left, inlineAsm);
            }
            break;
        case AST_FIT:
            outputGasDirective(f, ".fit", ast->left ? ast->left : AstInteger(496), inlineAsm);
            break;
        case AST_COMMENT:
//            outputGasComment(f, ast, inlineAsm); // printed above already
            break;
        default:
            ERROR(ast, "unknown element in data block");
            break;
        }
    }
    outputFinalOrgs(f, saveOrgs, orgCount, inlineAsm);
    if (inlineAsm) {
        startLine(f, inlineAsm);
        flexbuf_printf(f, "%11s .compress default", " ");
        endLine(f, inlineAsm);
        startLine(f, inlineAsm);
        flexbuf_printf(f, "%11s .text", " ");
        endLine(f, inlineAsm);
        flexbuf_printf(f, "\n");
    }
    P->pasmLabels = saveState;
}
