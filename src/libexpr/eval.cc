#include "eval.hh"
#include "parser.hh"
#include "primops.hh"


EvalState::EvalState()
    : normalForms(32768, 75)
{
    blackHole = ATmake("BlackHole()");
    if (!blackHole) throw Error("cannot build black hole");
    nrEvaluated = nrCached = 0;
}


/* Substitute an argument set into the body of a function. */
static Expr substArgs(Expr body, ATermList formals, Expr arg)
{
    ATMatcher m;
    ATermMap subs;
    Expr undefined = ATmake("Undefined");

    /* Get the formal arguments. */
    for (ATermIterator i(formals); i; ++i) {
        Expr name, def;
        if (atMatch(m, *i) >> "NoDefFormal" >> name)
            subs.set(name, undefined);
        else if (atMatch(m, *i) >> "DefFormal" >> name >> def)
            subs.set(name, def);
        else abort(); /* can't happen */
    }

    /* Get the actual arguments, and check that they match with the
       formals. */
    ATermMap args;
    queryAllAttrs(arg, args);
    for (ATermIterator i(args.keys()); i; ++i) {
        Expr key = *i;
        Expr cur = subs.get(key);
        if (!cur)
            throw badTerm(format("function has no formal argument `%1%'")
                % aterm2String(key), arg);
        subs.set(key, args.get(key));
    }

    /* Check that all arguments are defined. */
    for (ATermIterator i(subs.keys()); i; ++i)
        if (subs.get(*i) == undefined)
            throw badTerm(format("formal argument `%1%' missing")
                % aterm2String(*i), arg);
    
    return substitute(subs, body);
}


/* Transform a mutually recursive set into a non-recursive set.  Each
   attribute is transformed into an expression that has all references
   to attributes substituted with selection expressions on the
   original set.  E.g., e = `rec {x = f x y, y = x}' becomes `{x = f
   (e.x) (e.y), y = e.x}'. */
ATerm expandRec(ATerm e, ATermList bnds)
{
    ATMatcher m;

    /* Create the substitution list. */
    ATermMap subs;
    for (ATermIterator i(bnds); i; ++i) {
        string s;
        Expr e2;
        if (!(atMatch(m, *i) >> "Bind" >> s >> e2))
            abort(); /* can't happen */
        subs.set(s, ATmake("Select(<term>, <str>)", e, s.c_str()));
    }

    /* Create the non-recursive set. */
    ATermMap as;
    for (ATermIterator i(bnds); i; ++i) {
        string s;
        Expr e2;
        if (!(atMatch(m, *i) >> "Bind" >> s >> e2))
            abort(); /* can't happen */
        as.set(s, substitute(subs, e2));
    }

    return makeAttrs(as);
}


string evalString(EvalState & state, Expr e)
{
    e = evalExpr(state, e);
    ATMatcher m;
    string s;
    if (!(atMatch(m, e) >> "Str" >> s))
        throw badTerm("string expected", e);
    return s;
}


Path evalPath(EvalState & state, Expr e)
{
    e = evalExpr(state, e);
    ATMatcher m;
    string s;
    if (!(atMatch(m, e) >> "Path" >> s))
        throw badTerm("path expected", e);
    return s;
}


bool evalBool(EvalState & state, Expr e)
{
    e = evalExpr(state, e);
    ATMatcher m;
    if (atMatch(m, e) >> "Bool" >> "True") return true;
    else if (atMatch(m, e) >> "Bool" >> "False") return false;
    else throw badTerm("expecting a boolean", e);
}


Expr evalExpr2(EvalState & state, Expr e)
{
    ATMatcher m;
    Expr e1, e2, e3, e4;
    string s1;

    /* Normal forms. */
    if (atMatch(m, e) >> "Str" ||
        atMatch(m, e) >> "Path" ||
        atMatch(m, e) >> "Uri" ||
        atMatch(m, e) >> "Int" ||
        atMatch(m, e) >> "Bool" ||
        atMatch(m, e) >> "Function" ||
        atMatch(m, e) >> "Attrs" ||
        atMatch(m, e) >> "List")
        return e;

    /* Any encountered variables must be undeclared or primops. */
    if (atMatch(m, e) >> "Var" >> s1) {
        if (s1 == "null") return primNull(state);
        return e;
    }

    /* Function application. */
    if (atMatch(m, e) >> "Call" >> e1 >> e2) {

        ATermList formals;
        
        /* Evaluate the left-hand side. */
        e1 = evalExpr(state, e1);

        /* Is it a primop or a function? */
        if (atMatch(m, e1) >> "Var" >> s1) {
            if (s1 == "import") return primImport(state, e2);
            if (s1 == "derivation") return primDerivation(state, e2);
            if (s1 == "toString") return primToString(state, e2);
            if (s1 == "baseNameOf") return primBaseNameOf(state, e2);
            if (s1 == "isNull") return primIsNull(state, e2);
            else throw badTerm("undefined variable/primop", e1);
        }

        else if (atMatch(m, e1) >> "Function" >> formals >> e4)
            return evalExpr(state, 
                substArgs(e4, formals, evalExpr(state, e2)));
        
        else throw badTerm("expecting a function or primop", e1);
    }

    /* Attribute selection. */
    if (atMatch(m, e) >> "Select" >> e1 >> s1) {
        Expr a = queryAttr(evalExpr(state, e1), s1);
        if (!a) throw badTerm(format("missing attribute `%1%'") % s1, e);
        return evalExpr(state, a);
    }

    /* Mutually recursive sets. */
    ATermList bnds;
    if (atMatch(m, e) >> "Rec" >> bnds)
        return expandRec(e, bnds);

    /* Let expressions `let {..., body = ...}' are just desugared
       into `(rec {..., body = ...}).body'. */
    if (atMatch(m, e) >> "LetRec" >> bnds)
        return evalExpr(state, ATmake("Select(Rec(<term>), \"body\")", bnds));

    /* Conditionals. */
    if (atMatch(m, e) >> "If" >> e1 >> e2 >> e3) {
        if (evalBool(state, e1))
            return evalExpr(state, e2);
        else
            return evalExpr(state, e3);
    }

    /* Assertions. */
    if (atMatch(m, e) >> "Assert" >> e1 >> e2) {
        if (!evalBool(state, e1)) throw badTerm("guard failed", e);
        return evalExpr(state, e2);
    }

    /* Generic equality. */
    if (atMatch(m, e) >> "OpEq" >> e1 >> e2)
        return makeBool(evalExpr(state, e1) == evalExpr(state, e2));

    /* Generic inequality. */
    if (atMatch(m, e) >> "OpNEq" >> e1 >> e2)
        return makeBool(evalExpr(state, e1) != evalExpr(state, e2));

    /* Negation. */
    if (atMatch(m, e) >> "OpNot" >> e1)
        return makeBool(!evalBool(state, e1));

    /* Implication. */
    if (atMatch(m, e) >> "OpImpl" >> e1 >> e2)
        return makeBool(!evalBool(state, e1) || evalBool(state, e2));

    /* Conjunction (logical AND). */
    if (atMatch(m, e) >> "OpAnd" >> e1 >> e2)
        return makeBool(evalBool(state, e1) && evalBool(state, e2));

    /* Disjunction (logical OR). */
    if (atMatch(m, e) >> "OpOr" >> e1 >> e2)
        return makeBool(evalBool(state, e1) || evalBool(state, e2));

    /* Barf. */
    throw badTerm("invalid expression", e);
}


Expr evalExpr(EvalState & state, Expr e)
{
    startNest(nest, lvlVomit,
        format("evaluating expression: %1%") % e);

    state.nrEvaluated++;

    /* Consult the memo table to quickly get the normal form of
       previously evaluated expressions. */
    Expr nf = state.normalForms.get(e);
    if (nf) {
        if (nf == state.blackHole)
            throw badTerm("infinite recursion", e);
        state.nrCached++;
        return nf;
    }

    /* Otherwise, evaluate and memoize. */
    state.normalForms.set(e, state.blackHole);
    nf = evalExpr2(state, e);
    state.normalForms.set(e, nf);
    return nf;
}


Expr evalFile(EvalState & state, const Path & path)
{
    startNest(nest, lvlTalkative, format("evaluating file `%1%'") % path);
    Expr e = parseExprFromFile(path);
    return evalExpr(state, e);
}


void printEvalStats(EvalState & state)
{
    debug(format("evaluated %1% expressions, %2% cache hits, %3%%% efficiency")
        % state.nrEvaluated % state.nrCached
        % ((float) state.nrCached / (float) state.nrEvaluated * 100));
}
