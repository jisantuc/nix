#ifndef __EVAL_H
#define __EVAL_H

#include <map>

#include "aterm.hh"
#include "hash.hh"
#include "nixexpr.hh"


typedef map<Path, PathSet> DrvRoots;
typedef map<Path, Hash> DrvHashes;

struct EvalState;

/* Note: using a ATermVector is safe here, since when we call a primop
   we also have an ATermList on the stack. */
typedef Expr (* PrimOp) (EvalState &, const ATermVector & args);


struct EvalState 
{
    ATermMap normalForms;
    ATermMap primOps;
    DrvRoots drvRoots;
    DrvHashes drvHashes; /* normalised derivation hashes */
    Expr blackHole;

    unsigned int nrEvaluated;
    unsigned int nrCached;

    EvalState();

    void addPrimOps();
    void addPrimOp(const string & name,
        unsigned int arity, PrimOp primOp);
};


/* Evaluate an expression to normal form. */
Expr evalExpr(EvalState & state, Expr e);

/* Evaluate an expression read from the given file to normal form. */
Expr evalFile(EvalState & state, const Path & path);

/* Specific results. */
string evalString(EvalState & state, Expr e);
Path evalPath(EvalState & state, Expr e);

/* Print statistics. */
void printEvalStats(EvalState & state);


#endif /* !__EVAL_H */
