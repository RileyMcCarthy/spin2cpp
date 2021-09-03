#ifndef BACKEND_COMMON_H
#define BACKEND_COMMON_H

#include "ast.h"
#include "symbol.h"
#include "expr.h"

AST *BuildMethodPointer(AST *ast);
void OutputAlignLong(Flexbuf *fb);
void OutputDataBlob(Flexbuf *fb, Flexbuf *databuf, Flexbuf *relocbuf, const char *startLabel);

void NormalizeVarOffsets(Function *f);

void CompileAsmToBinary(const char *binname, const char *asmname); // in cmdline.c

// evaluate any constant expressions inside a string
AST *EvalStringConst(AST *expr);

// turn an AST stringptr into a flexbuf buffer
void StringBuildBuffer(Flexbuf *fb, AST *expr);

// print into a freshly allocated string
char *auto_printf(size_t max,const char *format,...) __attribute__((format(printf,2,3)));

// output for DAT blocks
/* functions for printing data into a flexbuf */
typedef struct DataBlockOutFuncs {
    void (*startAst)(Flexbuf *f, AST *ast);
    void (*putByte)(Flexbuf *f, int c);
    void (*endAst)(Flexbuf *f, AST *ast);
} DataBlockOutFuncs;

/*
 * information about relocations:
 * data blocks are normally just binary blobs; but if an absolute address
 * (like @@@foo) is requested, we need a way to specify that address
 * Note that a normal @foo in a dat section is a relative address;
 * @@@foo requires that we add the base of the dat to @foo.
 * For now, the relocation system works only on longs, and only in some
 * modes. For each long that needs the base of dat added to it we emit
 * a relocation r, which contains (a) the offset of the relocatable long
 * in bytes from the start of the dat, and (b) the symbol to add to the base
 * of the dat section at that long.
 * The relocs should be sorted in order of increasing offset, so we can
 * easily process them in order along with the output.
 *
 * We also re-use the Reloc struct to hold debug information as well, so
 * that we can provide source listings for the DAT section contents.
 * for that purpose we emit DebugEntry
 */
typedef struct Reloc {
    int32_t  kind;    // reloc or debug
    int32_t  addr;    // the address the entry affects (offset from dat base)
    Symbol   *sym;    // the symbol this relocation is relative to (NULL for dat base)
    int32_t  symoff;  // offset relative to sym
} Reloc;

#define RELOC_KIND_NONE  0  // no relocation, should not produce an entry
#define RELOC_KIND_DEBUG 1  // not a real relocation, a debug entry
#define RELOC_KIND_I32   2  // add a symbol to a 32 bit value
#define RELOC_KIND_AUGS  3  // relocation for AUGS
#define RELOC_KIND_AUGD  4  // relocation for AUGD

/* for PASM debug */
#define MAX_BRK 256
extern unsigned brkAssigned; // Currently assigned BRK codes
int AsmDebug_CodeGen(AST *ast);
Flexbuf CompileBrkDebugger(size_t appsize);

void PrintDataBlock(Flexbuf *f, AST *list, DataBlockOutFuncs *funcs, Flexbuf *relocs);
void PrintDataBlockForGas(Flexbuf *f, Module *P, int inlineAsm);

/* flags for PrintExpr and friends */
#define PRINTEXPR_DEFAULT    0x0000
#define PRINTEXPR_GAS        0x0001  /* printing in a GAS context */
#define PRINTEXPR_ASSIGNMENT 0x0002  /* printing from an assignment operator */
#define PRINTEXPR_ISREF      0x0004  /* expression used as a reference */
#define PRINTEXPR_GASIMM     0x0008  /* GAS expression is an immediate value (so divide labels by 4) */
#define PRINTEXPR_GASOP      0x0010  /* GAS expression used in an operand */
#define PRINTEXPR_GASABS     0x0020  /* absolute address, not relative */
#define PRINTEXPR_USECONST   0x0040  /* print constant names, not values */
#define PRINTEXPR_TOPLEVEL   0x0080  /* leave out parens around operators */
#define PRINTEXPR_USEFLOATS  0x0100  /* print  expression as floats if appropriate */
#define PRINTEXPR_INLINESYM  0x0200  /* printing symbols in inline assembly */
#define PRINTEXPR_FORCE_UNS  0x0400  /* force arguments to be unsigned */
#define PRINTEXPR_DEBUG      0x0800  /* print symbols for DEBUG */

/* printing functions */
void PrintTypedExpr(Flexbuf *f, AST *casttype, AST *expr, int flags);
void PrintExpr(Flexbuf *f, AST *expr, int flags);
void PrintLHS(Flexbuf *f, AST *expr, int flags);
void PrintBoolExpr(Flexbuf *f, AST *expr, int flags);
void PrintAsAddr(Flexbuf *f, AST *expr, int flags);
void PrintExprList(Flexbuf *f, AST *list, int flags, Function *func);
void PrintType(Flexbuf *f, AST *type, int flags);
void PrintCastType(Flexbuf *f, AST *type);
void PrintPostfix(Flexbuf *f, AST *val, int toplevel, int flags);
void PrintInteger(Flexbuf *f, int32_t v, int flags);
void PrintFloat(Flexbuf *f, int32_t v, int flags);
int  PrintLookupArray(Flexbuf *f, AST *arr, int flags);
void PrintGasExpr(Flexbuf *f, AST *expr, bool useFloat);
void PrintSymbol(Flexbuf *f, Symbol *sym, int flags);
void PrintObjConstName(Flexbuf *f, Module *P, const char* name);
void PrintStatementList(Flexbuf *f, AST *ast, int indent);

#endif /* BACKEND_COMMON_H */
