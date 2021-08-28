#include "common.h"
#include "nuir.h"
#include <stdio.h>
#include "sys/nuinterp.spin.h"
#include "becommon.h"

#define DIRECT_BYTECODE 0
#define PUSHI_BYTECODE  1
#define PUSHA_BYTECODE  2
#define FIRST_BYTECODE  3
#define MAX_BYTECODE 0xf8

static const char *NuOpName[] = {
    #define X(m) #m,
    NU_OP_XMACRO
    #undef X
};

static const char *impl_ptrs[NU_OP_DUMMY];
static int impl_sizes[NU_OP_DUMMY];

static int usage_sortfunc(const void *Av, const void *Bv) {
    NuBytecode **A = (NuBytecode **)Av;
    NuBytecode **B = (NuBytecode **)Bv;
    /* sort with larger used coming first */
    return B[0]->usage - A[0]->usage;
}

static int codenum_sortfunc(const void *Av, const void *Bv) {
    NuBytecode **A = (NuBytecode **)Av;
    NuBytecode **B = (NuBytecode **)Bv;
    /* sort with smaller codes coming first */
    return A[0]->code - B[0]->code;
}

static int NuImplSize(const char *lineptr) {
    int c;
    int size = 0;
    if (!strncmp(lineptr, "impl_", 5)) {
        --size; // ignore label
    }
    for(;;) {
        c = *lineptr++;
        if (c == 0) break;
        if (c == '\n') {
            size++;
            if (*lineptr == '\n') break;
        }
        if (c == '#' && lineptr[0] == '#') {
            size++;
        }
    }
    return size;
}

void NuIrInit(NuContext *ctxt) {
    const char *ptr = (char *)sys_nuinterp_spin;
    const char *linestart;
    int c;
    int i;
    
    memset(ctxt, 0, sizeof(*ctxt));

    // scan for implementation of primitives
    // first, skip the initial interpreter
    for(;;) {
        c = *ptr++;
        if (!c || c == '\014') break;
    }
    // some functions are always built in to the interpreter
    impl_ptrs[NU_OP_DROP] = "";
    impl_ptrs[NU_OP_DROP2] = "";
    impl_ptrs[NU_OP_DUP] = "";
    impl_ptrs[NU_OP_SWAP] = "";
    impl_ptrs[NU_OP_OVER] = "";
    impl_ptrs[NU_OP_CALL] = "";
    impl_ptrs[NU_OP_CALLM] = "";
    impl_ptrs[NU_OP_GOSUB] = "";
    impl_ptrs[NU_OP_ENTER] = "";
    impl_ptrs[NU_OP_RET] = "";
    impl_ptrs[NU_OP_INLINEASM] = "";
    impl_ptrs[NU_OP_PUSHI] = "";
    impl_ptrs[NU_OP_PUSHA] = "";
    
    // find the other implementations that we may need
    while (c) {
        linestart = ptr;
        if (!strncmp(linestart, "impl_", 5)) {
            // find the opcode this is an implementation of
            ptr = linestart + 5;
            for (i = 0; i < NU_OP_DUMMY; i++) {
                int n = strlen(NuOpName[i]);
                if (!strncmp(ptr, NuOpName[i], n) && ptr[n] == '\n') {
                    if (impl_ptrs[i]) {
                        ERROR(NULL, "Duplicate definition for %s\n", NuOpName[i]);
                    }
                    impl_ptrs[i] = linestart;
                    impl_sizes[i] = NuImplSize(impl_ptrs[i]);
                    break;
                }
            }
        }
        do {
            c = *ptr++;
        } while (c != '\n' && c != 0);
    }
}

NuIrLabel *NuCreateLabel() {
    NuIrLabel *r;
    static int labelnum = 0;
    r = calloc(sizeof(*r), 1);
    r->num = labelnum++;
    snprintf(r->name, sizeof(r->name), "__Label_%05u", r->num);
    return r;
}

static NuIr *NuCreateIr() {
    NuIr *r;
    r = calloc(sizeof(*r), 1);
    return r;
}

NuIr *NuEmitOp(NuIrList *irl, NuIrOpcode op) {
    NuIr *r;
    NuIr *last;
    
    r = NuCreateIr();
    r->op = op;
    last = irl->tail;
    irl->tail = r;
    r->prev = last;
    if (last) {
        last->next = r;
    }
    if (!irl->head) {
        irl->head = r;
    }
    return r;
}

NuIr *NuEmitCommentedOp(NuIrList *irl, NuIrOpcode op, const char *comment) {
    NuIr *r = NuEmitOp(irl, op);
    if (r) {
        r->comment = comment;
    }
    return r;
}

NuIr *NuEmitConst(NuIrList *irl, int32_t val) {
    NuIr *r;

    r = NuEmitOp(irl, NU_OP_PUSHI);
    r->val = val;
    return r;
}

NuIr *NuEmitAddress(NuIrList *irl, NuIrLabel *label) {
    NuIr *r = NuEmitOp(irl, NU_OP_PUSHA);
    r->label = label;
    return r;
}

NuIr *NuEmitBranch(NuIrList *irl, NuIrOpcode op, NuIrLabel *label) {
    NuIr *r = NuEmitOp(irl, op);
    r->label = label;
    return r;
}

NuIr *NuEmitLabel(NuIrList *irl, NuIrLabel *label) {
    NuIr *r = NuEmitOp(irl, NU_OP_LABEL);
    r->label = label;
    return r;
}

NuIr *NuEmitNamedOpcode(NuIrList *irl, const char *name) {
    NuIrOpcode op;
    for (op = NU_OP_ILLEGAL; op < NU_OP_DUMMY; op++) {
        if (!strcasecmp(NuOpName[op], name)) {
            break;
        }
    }
    if (op == NU_OP_DUMMY) {
        ERROR(NULL, "Unknown opcode %s", name);
        return NULL;
    }
    return NuEmitOp(irl, op);
}

#define MAX_NUM_BYTECODE 0x8000
static int num_bytecodes = 0;
static NuBytecode *globalBytecodes[MAX_NUM_BYTECODE];
static NuBytecode *staticOps[NU_OP_DUMMY];
#define MAX_CONST_OPS 0xffff
static NuBytecode *constOps[MAX_CONST_OPS+1];

static NuBytecode *
AllocBytecode()
{
    NuBytecode *b;
    if (num_bytecodes == MAX_NUM_BYTECODE) {
        return NULL;
    }
    b = calloc(sizeof(*b), 1);
    b->usage = 1;
    globalBytecodes[num_bytecodes] = b;
    num_bytecodes++;
    return b;
}

static NuBytecode *
GetBytecodeForConst(intptr_t val, int is_label)
{
    int hash;
    NuBytecode *b;
    
    hash = val & MAX_CONST_OPS;
    for (b = constOps[hash]; b; b = b->link) {
        if (b->value == val) {
            b->usage++;
            return b;
        }
    }
    b = AllocBytecode();
    if (!b) {
        ERROR(NULL, "ran out of bytecode space");
        return NULL;
    }
    b->value = val;
    b->link = constOps[hash];
    b->is_const = 1;
    b->is_label = is_label;
    constOps[hash] = b;
    return b;
}

static NuBytecode *
GetBytecodeFor(NuIr *ir)
{
    NuBytecode *b;
    if (ir->op >= NU_OP_DUMMY) {
        return NULL;
    }
    if (ir->op == NU_OP_PUSHI) {
        return GetBytecodeForConst(ir->val, 0);
    }
    if (ir->op == NU_OP_PUSHA) {
        return GetBytecodeForConst( (intptr_t)(ir->label), 1);
    }
    b = staticOps[ir->op];
    if (b) {
        b->usage++;
        return b;
    }
    b = AllocBytecode();
    if (b) {
        b->name = NuOpName[ir->op];
        b->impl_ptr = impl_ptrs[ir->op];
        if (!b->impl_ptr[0]) {
            // builtin: access it via jumping to it
            b->impl_ptr = auto_printf(64, "\tjmp\t#\\impl_%s\n\n", b->name);
            b->impl_size = 1;
        } else {
            b->impl_size = impl_sizes[ir->op];
        }
        staticOps[ir->op] = b;
        if (ir->op >= NU_OP_JMP && ir->op < NU_OP_DUMMY) {
            b->is_any_branch = 1;
            if (ir->op >= NU_OP_BRA) {
                b->is_rel_branch = 1;
            }
        }
        if (ir->op == NU_OP_INLINEASM) {
            b->is_inline_asm = 1;
        }
    } else {
        ERROR(NULL, "Internal error, too many bytecodes\n");
        return NULL;
    }
    return b;
}
//
// scan list of instructions for pairs that may be combined into a macro
// only opcodes that have been assigned single bytecodes may be merged
//
typedef struct NuMacro {
    NuBytecode *firstCode;
    NuBytecode *secondCode;
    int count;
    int depth;
} NuMacro;

static NuMacro macros[256][256];

#define MAX_MACRO_DEPTH 4

// scan for potential macro pairs
static NuMacro *NuScanForMacros(NuIrList *lists, int *savings) {
    NuIrList *irl = lists;
    NuIr *ir;
    int maxCount = 0;
    NuBytecode *prevCode, *curCode;
    NuMacro *where = 0;
    int savedBytes;
    static int found_macro = 0;

//    if (found_macro > 48) {
//        return NULL;
//    }
    memset(macros, 0, sizeof(macros));
    while (irl) {
        prevCode = NULL;
        for (ir = irl->head; ir; ir = ir->next) {
            curCode = ir->bytecode;
            if (curCode && (curCode->is_inline_asm || curCode->is_rel_branch)) {
                // no macros involving inline asm or relative branches
                curCode = NULL;
            }
            if (curCode && prevCode && curCode->macro_depth < MAX_MACRO_DEPTH && prevCode->macro_depth < MAX_MACRO_DEPTH) {
                int bc1, bc2;
                bc1 = prevCode->code;
                bc2 = curCode->code;
                if (bc1 >= FIRST_BYTECODE && bc2 >= FIRST_BYTECODE) {
                    macros[bc1][bc2].count++;
                    if (macros[bc1][bc2].count > maxCount) {
                        maxCount = macros[bc1][bc2].count;
                        where = &macros[bc1][bc2];
                        where->firstCode = prevCode;
                        where->secondCode = curCode;
                    }
                }
            }
            if (curCode && !curCode->is_any_branch) {
                prevCode = curCode;
            } else {
                prevCode = NULL;
            }
        }
        irl = irl->nextList;
    }
    // figure out the benefit of doing this replacement
    // the new macro requires at least 10 bytes implementation
    // it will save 1 byte per invocation
    savedBytes = maxCount - 10;
    if (savedBytes < 0) {
        return NULL;
    }
    *savings = savedBytes;
    found_macro++;
    where->depth = where->firstCode->macro_depth;
    if (where->secondCode->macro_depth > where->depth) {
        where->depth = where->secondCode->macro_depth;
    }
    where->depth += 1;
    return where;
}

// find bytecode to compress
// this may be either a single PUSHI/PUSHA (which is compressed by creating a single opcode immediate version)
// or a macro pair (which is compressed by creating a new bytecode which does both macros)

static NuBytecode *NuFindCompressBytecode(NuIrList *irl, int *savings) {
    int i;
    NuBytecode *bc;
    int savedBytes;

    // globalBytecodes are assumed sorted by usage...
    for (i = 0; i < num_bytecodes; i++) {
        bc = globalBytecodes[i];
        if (bc->is_const && bc->usage > 1) {
            // cost of implementation is 8 bytes for small, 12 for large
            int impl_cost = 12;
            // cost of each invocation is 5 bytes normally (PUSHI + data)
            // so we save 4 bytes by replacing with a singleton
            int invoke_cost = 4;
            if (bc->value >= -511 && bc->value <= 511) {
                impl_cost = 8;
            }
            savedBytes = (invoke_cost * bc->usage) - impl_cost;
            if (savedBytes < 1) {
                return NULL;
            }
            *savings = savedBytes;
            return bc;
        }
    }
    return NULL;
}

static void
NuCopyImpl(Flexbuf *fb, const char *lineptr, int skipRet) {
    int c;
    if (!strncmp(lineptr, "impl_", 5)) {
        // skip label
        while (*lineptr && *lineptr != '\n') lineptr++;
        if (*lineptr) lineptr++;
    }
    for(;;) {
        c = *lineptr++;
        if (!c) break;
        flexbuf_addchar(fb, c);
        if (c == '\n' && (lineptr[0] == 0 || lineptr[0] == '\n')) {
            break;
        }
        if (c == ' ' || c == '\t') {
            if (skipRet) {
                if (!strncmp(lineptr, "_ret_\t", 6) || !strncmp(lineptr, "_ret_ ", 6)) {
                    flexbuf_addstr(fb, "     ");
                    lineptr += 5;
                } else if (!strncmp(lineptr, "jmp\t", 4) || !strncmp(lineptr, "jmp ", 4)) {
                    flexbuf_addstr(fb, "call");
                    lineptr += 3;
                }
            }
        }
    }
}

static
const char *NuMergeBytecodes(const char *bcname, NuBytecode *first, NuBytecode *second) {
    Flexbuf *fb;
    Flexbuf fb_s;

    fb = &fb_s;
    flexbuf_init(fb, 256);
    flexbuf_printf(fb, "impl_%s\n", bcname);
    if (first->impl_size < 3) {
        NuCopyImpl(fb, first->impl_ptr, 1);
    } else {
        flexbuf_printf(fb, "\tcall\t#\\impl_%s\n", first->name);
    }
    if (second->impl_size < 2) {
        NuCopyImpl(fb, second->impl_ptr, 0);
    } else {
        flexbuf_printf(fb, "\tjmp\t#\\impl_%s\n", second->name);
    }
    flexbuf_addchar(fb, '\n');
    flexbuf_addchar(fb, 0);
    return flexbuf_get(fb);
}

static NuBytecode *NuReplaceMacro(NuIrList *lists, NuMacro *macro) {
    NuIrList *irl = lists;
    NuIr *ir;
    NuBytecode *bc, *first, *second;

    bc = AllocBytecode();
    if (!bc) {
        return NULL;
    }
    bc->usage = 0;
    bc->macro_depth = macro->depth;
    first = macro->firstCode;
    second = macro->secondCode;
    bc->is_any_branch = first->is_any_branch | second->is_any_branch;
    bc->name = auto_printf(128, "%s_%s", first->name, second->name);
    bc->impl_ptr = NuMergeBytecodes(bc->name, first, second);
    bc->impl_size = NuImplSize(bc->impl_ptr);
    while (irl) {
        for (ir = irl->head; ir; ir = ir->next) {
            NuIr *delir = ir->next;
            if (ir->bytecode == first && delir && delir->bytecode == second) {
                ir->bytecode = bc;
                bc->usage++;
                ir->next = delir->next;
                if (ir->next) {
                    ir->next->prev = ir;
                } else {
                    irl->tail = ir;
                }
            }
        }
        
        irl = irl->nextList;
    }
    return bc;
}

#define NuBcIsRelBranch(bc) ((bc)->is_rel_branch)

void NuCreateBytecodes(NuIrList *lists)
{
    NuIr *ir;
    NuIrList *irl;
    int i;
    int code;
    NuBytecode *bc;
    
    // create an initial set of bytecodes
    irl = lists;
    while (irl) {
        for (ir = irl->head; ir; ir = ir->next) {
            ir->bytecode = GetBytecodeFor(ir);
        }
        irl = irl->nextList;
    }

    // sort the bytecodes in order of usage
    size_t elemsize = sizeof(NuBytecode *);
    qsort(&globalBytecodes, num_bytecodes, elemsize, usage_sortfunc);

    // assign bytecodes based on order of usage
    code = FIRST_BYTECODE;
    for (i = 0; i < num_bytecodes; i++) {
        bc = globalBytecodes[i];
        if (bc->is_const) {
            if (bc->is_label) {
                globalBytecodes[i]->code = PUSHA_BYTECODE;
            } else {
                globalBytecodes[i]->code = PUSHI_BYTECODE;
            }
        } else if (NuBcIsRelBranch(globalBytecodes[i])) {
            globalBytecodes[i]->code = code++;
        } else if (code >= MAX_BYTECODE || globalBytecodes[i]->usage <= 1) {
            // out of bytecode space, or we don't care about compressing
            globalBytecodes[i]->code = DIRECT_BYTECODE;
        } else {
            globalBytecodes[i]->code = code++;
        }
    }

    // while there's room for more bytecodes, find ways to compress the code
    while (code < (MAX_BYTECODE-1)) {
        int32_t val;
        int compressValue, macroValue;
        const char *instr = "mov";
        const char *opname;
        NuMacro *macro;
        
        bc = NuFindCompressBytecode(lists, &compressValue);
        macro = NuScanForMacros(lists, &macroValue);
        if (bc && macro) {
            // pick which is better
            if (compressValue >= macroValue) {
                macro = NULL;
            } else {
                bc = NULL;
            }
        }
        if (bc) {
            const char *immflag = "#";
            const char *valstr;
            const char *namestr;
            NuIrLabel *label = NULL;

            opname = "PUSH_";
            if (bc->is_label) {
                label = (NuIrLabel *)bc->value;
                if (label->offset) {
                    valstr = auto_printf(128, "(%s+%u)", label->name, label->offset);
                    namestr = auto_printf(128, "%s_%u", label->name, label->offset);
                } else {
                    valstr = namestr = label->name;
                }
            } else {
                val = (int32_t)bc->value;
                if (val < 0) {
                    val = -val;
                    instr = "neg";
                    opname = "PUSH_M";
                }
                if (val >= 0 && val < 512) {
                    immflag = "";
                }
                valstr = namestr = auto_printf(32, "%u", val);
            }
            bc->name = auto_printf(32, "%s%s", opname, namestr);
            // impl_PUSH_0
            //       call #\impl_DUP
            // _ret_ mov  tos, #0
            bc->impl_ptr = auto_printf(128, "impl_%s\n\tcall\t#\\impl_DUP\n _ret_\t%s\ttos, #%s%s\n\n", bc->name, instr, immflag, valstr);
            bc->is_const = 0; // don't need to emit PUSHI for this one
        } else if (macro) {
            bc = NuReplaceMacro(lists, macro);
        } else {
            break;
        }
        bc->code = code++;
    }
    // finally, sort byte bytecode
    qsort(&globalBytecodes, num_bytecodes, elemsize, codenum_sortfunc);
}

void NuOutputLabel(Flexbuf *fb, NuIrLabel *label) {
    if (!label) {
        flexbuf_printf(fb, "0");
        return;
    }
    if (label->offset) {
        flexbuf_printf(fb, "(%s + %d)", label->name, label->offset);
    } else {
        flexbuf_printf(fb, "%s", label->name);
    }
}
void NuOutputLabelNL(Flexbuf *fb, NuIrLabel *label) {
    NuOutputLabel(fb, label);
    flexbuf_addchar(fb, '\n');
}

static uint32_t nu_heap_size;

static void
OutputEscapedChar(Flexbuf *fb, int c, NuContext *ctxt)
{
    switch(c) {
    case '0':
        flexbuf_printf(fb, "%u", ctxt->clockFreq);
        break;
    case '1':
        flexbuf_printf(fb, "$%x", ctxt->clockMode);
        break;
    case '2':
        NuOutputLabel(fb, ctxt->entryPt);
        break;
    case '3':
        NuOutputLabel(fb, ctxt->initObj);
        break;
    case '4':
        NuOutputLabel(fb, ctxt->initFrame);
        break;
    case '5':
        NuOutputLabel(fb, ctxt->initSp);
        break;
    case '6':
        flexbuf_printf(fb, "%u", nu_heap_size / 4);
        break;
    default:
        ERROR(NULL, "Unknown escape char %c", c);
        break;
    }
}

static long GetHeapSize() {
    Symbol *sym;
    if (! (gl_features_used & FEATURE_NEED_HEAP)) return 0;
    sym = LookupSymbolInTable(&systemModule->objsyms, "__real_heapsize__");
    if (!sym || sym->kind != SYM_CONSTANT) return 0;
    uint32_t heapsize = EvalPasmExpr((AST *)sym->val) * LONG_SIZE;

    heapsize += 4*LONG_SIZE; // reserve a slot at the end
    return heapsize;
}

void NuOutputInterpreter(Flexbuf *fb, NuContext *ctxt)
{
    const char *ptr = (char *)sys_nuinterp_spin;
    int c;
    int i;
    uint32_t heapsize = GetHeapSize() + 4;
    
    heapsize = (heapsize+3)&~3; // long align
    nu_heap_size = heapsize;
    
    // copy until ^L
    for(;;) {
        c = *ptr++;
        if (c == 0 || c == '\014') break;
        if (c == '\001') {
            c = *ptr++;
            OutputEscapedChar(fb, c, ctxt);
        } else {
            flexbuf_addchar(fb, c);
        }
    }

    // add the predefined entries
    flexbuf_printf(fb, "\tword\timpl_DIRECT\n");
    flexbuf_printf(fb, "\tword\timpl_PUSHI\n");
    flexbuf_printf(fb, "\tword\timpl_PUSHA\n");
    
    // now add the jump table
    for (i = 0; i < num_bytecodes; i++) {
        NuBytecode *bc = globalBytecodes[i];
        int code = bc->code;
        if (code >= FIRST_BYTECODE) {
            flexbuf_printf(fb, "\tword\timpl_%s\n", bc->name);
        }
    }
    // end of jump table
    flexbuf_printf(fb, "\talignl\nOPC_TABLE_END\n");

    // emit constants for everything
    flexbuf_printf(fb, "\ncon\n");
    // predefined
    flexbuf_printf(fb, "\tNU_OP_DIRECT = %d\n", DIRECT_BYTECODE);
    flexbuf_printf(fb, "\tNU_OP_PUSHI = %d\n", PUSHI_BYTECODE);
    flexbuf_printf(fb, "\tNU_OP_PUSHA = %d\n", PUSHA_BYTECODE);
    // others
    for (i = 0; i < num_bytecodes; i++) {
        NuBytecode *bc = globalBytecodes[i];
        int code = bc->code;
        if (code >= FIRST_BYTECODE) {
            flexbuf_printf(fb, "\tNU_OP_%s = %d  ' (used %d times)\n", bc->name, code, bc->usage);
        }
    }
    
    // now emit opcode implementations
    flexbuf_printf(fb, "dat\n\torgh ($ < $400) ? $400 : $\n");
    for (i = 0; i < num_bytecodes; i++) {
        NuBytecode *bc = globalBytecodes[i];
        const char *ptr = bc->impl_ptr;
        if (!ptr) {
            if ( ! (bc->is_const) ) {
                WARNING(NULL, "no implementation for %s", bc->name);
            }
            continue;
        }
        if (strncmp(ptr, "impl_", 5) != 0) {
            // does not start with impl_, so not really needed
            continue;
        }
        for(;;) {
            c = *ptr++;
            if (!c) break;
            flexbuf_addchar(fb, c);
            // empty line indicates end of code
            if (c == '\n' && *ptr == '\n') {
                flexbuf_addchar(fb, c);
                break;
            }
        }
    }
}

void NuOutputFinish(Flexbuf *fb, NuContext *ctxt)
{
    int c;
    // find tail of interpreter
    const char *ptr = (char *)sys_nuinterp_spin;
    ptr += sys_nuinterp_spin_len-1;
    // go back to last ^L
    while (ptr && *ptr != '\014') {
        --ptr;
    }
    // output tail
    ptr++;
    for(;;) {
        c = *ptr++;
        if (c == 0) break;
        if (c == '\001') {
            c = *ptr++;
            OutputEscapedChar(fb, c, ctxt);
        } else {
            flexbuf_addchar(fb, c);
        }
    }
}

static const char *
NuBytecodeString(NuBytecode *bc) {
    static char dummy[1024];
    if (bc->code == DIRECT_BYTECODE) {
        sprintf(dummy, "NU_OP_DIRECT, word impl_%s", bc->name);
    } else {
        sprintf(dummy, "NU_OP_%s", bc->name);
    }
    return dummy;
}

void
NuOutputIrList(Flexbuf *fb, NuIrList *irl)
{
    NuIr *ir;
    NuIrOpcode op;
    NuBytecode *bc;
    if (!irl || !irl->head) {
        return;
    }
    for (ir = irl->head; ir; ir = ir->next) {
        op = ir->op;
        bc = ir->bytecode;
        switch(op) {
        case NU_OP_LABEL:
            NuOutputLabel(fb, ir->label);
            break;
        case NU_OP_ALIGN:
            flexbuf_printf(fb, "\talignl");
            break;
        case NU_OP_BRA:
        case NU_OP_CBEQ:
        case NU_OP_CBNE:
        case NU_OP_CBLTS:
        case NU_OP_CBLES:
        case NU_OP_CBLTU:
        case NU_OP_CBLEU:
        case NU_OP_CBGTS:
        case NU_OP_CBGES:
        case NU_OP_CBGTU:
        case NU_OP_CBGEU:
            flexbuf_printf(fb, "\tbyte\t%s, word (", NuBytecodeString(bc));
            NuOutputLabel(fb, ir->label);
            flexbuf_printf(fb, " - ($+2))");
            break;
        default:
            if (bc) {
                if (bc->is_const) {
                    if (bc->is_label) {
                        flexbuf_printf(fb, "\tbyte\t long NU_OP_PUSHA | (");
                        NuOutputLabel(fb, ir->label);
                        flexbuf_printf(fb, " << 8)");
                    } else {
                        flexbuf_printf(fb, "\tbyte\tNU_OP_PUSHI, long %d", ir->val);
                    }
                } else {
                    flexbuf_printf(fb, "\tbyte\t%s", NuBytecodeString(bc));
                }
            }
            break;
        }
        if (ir->comment) {
            flexbuf_printf(fb, "\t' %s", ir->comment);
        }
        flexbuf_addchar(fb, '\n');
    }
}
