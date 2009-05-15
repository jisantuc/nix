#include "nixexpr.hh"
#include "derivations.hh"
#include "util.hh"
#include "aterm.hh"

#include "nixexpr-ast.hh"
#include "nixexpr-ast.cc"

#include <cstdlib>


namespace nix {
    

string showPos(ATerm pos)
{
    ATerm path;
    int line, column;
    if (matchNoPos(pos)) return "undefined position";
    if (!matchPos(pos, path, line, column))
        throw badTerm("position expected", pos);
    return (format("`%1%:%2%:%3%'") % aterm2String(path) % line % column).str();
}
    

ATerm bottomupRewrite(TermFun & f, ATerm e)
{
    checkInterrupt();

    if (ATgetType(e) == AT_APPL) {
        AFun fun = ATgetAFun(e);
        int arity = ATgetArity(fun);
        ATerm args[arity];

        for (int i = 0; i < arity; ++i)
            args[i] = bottomupRewrite(f, ATgetArgument(e, i));
        
        e = (ATerm) ATmakeApplArray(fun, args);
    }

    else if (ATgetType(e) == AT_LIST) {
        ATermList in = (ATermList) e;
        ATermList out = ATempty;

        for (ATermIterator i(in); i; ++i)
            out = ATinsert(out, bottomupRewrite(f, *i));

        e = (ATerm) ATreverse(out);
    }

    return f(e);
}


void queryAllAttrs(Expr e, ATermMap & attrs, bool withPos)
{
    ATermList bnds;
    if (!matchAttrs(e, bnds))
        throw TypeError(format("value is %1% while an attribute set was expected") % showType(e));

    for (ATermIterator i(bnds); i; ++i) {
        ATerm name;
        Expr e;
        ATerm pos;
        if (!matchBind(*i, name, e, pos)) abort(); /* can't happen */
        attrs.set(name, withPos ? makeAttrRHS(e, pos) : e);
    }
}


Expr queryAttr(Expr e, const string & name)
{
    ATerm dummy;
    return queryAttr(e, name, dummy);
}


Expr queryAttr(Expr e, const string & name, ATerm & pos)
{
    ATermList bnds;
    if (!matchAttrs(e, bnds))
        throw TypeError(format("value is %1% while an attribute set was expected") % showType(e));

    for (ATermIterator i(bnds); i; ++i) {
        ATerm name2, pos2;
        Expr e;
        if (!matchBind(*i, name2, e, pos2))
            abort(); /* can't happen */
        if (aterm2String(name2) == name) {
            pos = pos2;
            return e;
        }
    }

    return 0;
}


Expr makeAttrs(const ATermMap & attrs)
{
    ATermList bnds = ATempty;
    for (ATermMap::const_iterator i = attrs.begin(); i != attrs.end(); ++i) {
        Expr e;
        ATerm pos;
        if (!matchAttrRHS(i->value, e, pos))
            abort(); /* can't happen */
        bnds = ATinsert(bnds, makeBind(i->key, e, pos));
    }
    return makeAttrs(bnds);
}


static void varsBoundByPattern(ATermMap & map, Pattern pat)
{
    ATerm name;
    ATermList formals;
    Pattern pat1, pat2;
    ATermBool ellipsis;
    /* Use makeRemoved() so that it can be used directly in
       substitute(). */
    if (matchVarPat(pat, name))
        map.set(name, makeRemoved());
    else if (matchAttrsPat(pat, formals, ellipsis)) { 
        for (ATermIterator i(formals); i; ++i) {
            ATerm d1;
            if (!matchFormal(*i, name, d1)) abort();
            map.set(name, makeRemoved());
        }
    }
    else if (matchAtPat(pat, pat1, pat2)) {
        varsBoundByPattern(map, pat1);
        varsBoundByPattern(map, pat2);
    }
    else abort();
}


Expr substitute(const Substitution & subs, Expr e)
{
    checkInterrupt();

    //if (subs.size() == 0) return e;

    ATerm name, pos, e2;

    /* As an optimisation, don't substitute in subterms known to be
       closed. */
    if (matchClosed(e, e2)) return e;

    if (matchVar(e, name)) {
        Expr sub = subs.lookup(name);
        if (sub == makeRemoved()) sub = 0;
        Expr wrapped;
        /* Add a "closed" wrapper around terms that aren't already
           closed.  The check is necessary to prevent repeated
           wrapping, e.g., closed(closed(closed(...))), which kills
           caching. */
        return sub ? (matchClosed(sub, wrapped) ? sub : makeClosed(sub)) : e;
    }

    /* In case of a function, filter out all variables bound by this
       function. */
    Pattern pat;
    ATerm body;
    if (matchFunction(e, pat, body, pos)) {
        ATermMap map(16);
        varsBoundByPattern(map, pat);
        Substitution subs2(&subs, &map);
        return makeFunction(
            (Pattern) substitute(subs2, (Expr) pat),
            substitute(subs2, body), pos);
    }

    /* Idem for a mutually recursive attribute set. */
    ATermList rbnds, nrbnds;
    if (matchRec(e, rbnds, nrbnds)) {
        ATermMap map(ATgetLength(rbnds) + ATgetLength(nrbnds));
        for (ATermIterator i(rbnds); i; ++i)
            if (matchBind(*i, name, e2, pos)) map.set(name, makeRemoved());
            else abort(); /* can't happen */
        for (ATermIterator i(nrbnds); i; ++i)
            if (matchBind(*i, name, e2, pos)) map.set(name, makeRemoved());
            else abort(); /* can't happen */
        return makeRec(
            (ATermList) substitute(Substitution(&subs, &map), (ATerm) rbnds),
            (ATermList) substitute(subs, (ATerm) nrbnds));
    }

    if (ATgetType(e) == AT_APPL) {
        AFun fun = ATgetAFun(e);
        int arity = ATgetArity(fun);
        ATerm args[arity];
        bool changed = false;

        for (int i = 0; i < arity; ++i) {
            ATerm arg = ATgetArgument(e, i);
            args[i] = substitute(subs, arg);
            if (args[i] != arg) changed = true;
        }
        
        return changed ? (ATerm) ATmakeApplArray(fun, args) : e;
    }

    if (ATgetType(e) == AT_LIST) {
        unsigned int len = ATgetLength((ATermList) e);
        ATerm es[len];
        ATermIterator i((ATermList) e);
        for (unsigned int j = 0; i; ++i, ++j)
            es[j] = substitute(subs, *i);
        ATermList out = ATempty;
        for (unsigned int j = len; j; --j)
            out = ATinsert(out, es[j - 1]);
        return (ATerm) out;
    }

    return e;
}


/* We use memoisation to prevent exponential complexity on heavily
   shared ATerms (remember, an ATerm is a graph, not a tree!).  Note
   that using an STL set is fine here wrt to ATerm garbage collection
   since all the ATerms in the set are already reachable from
   somewhere else. */
static void checkVarDefs2(set<Expr> & done, const ATermMap & defs, Expr e)
{
    if (done.find(e) != done.end()) return;
    done.insert(e);
    
    ATerm name, pos, value;
    ATerm with, body;
    ATermList rbnds, nrbnds;
    Pattern pat;

    /* Closed terms don't have free variables, so we don't have to
       check by definition. */
    if (matchClosed(e, value)) return;
    
    else if (matchVar(e, name)) {
        if (!defs.get(name))
            throw EvalError(format("undefined variable `%1%'")
                % aterm2String(name));
    }

    else if (matchFunction(e, pat, body, pos)) {
        ATermMap defs2(defs);
        varsBoundByPattern(defs2, pat);
        set<Expr> done2;
        checkVarDefs2(done2, defs2, pat);
        checkVarDefs2(done2, defs2, body);
    }
        
    else if (matchRec(e, rbnds, nrbnds)) {
        checkVarDefs2(done, defs, (ATerm) nrbnds);
        ATermMap defs2(defs);
        for (ATermIterator i(rbnds); i; ++i) {
            if (!matchBind(*i, name, value, pos)) abort(); /* can't happen */
            defs2.set(name, (ATerm) ATempty);
        }
        for (ATermIterator i(nrbnds); i; ++i) {
            if (!matchBind(*i, name, value, pos)) abort(); /* can't happen */
            defs2.set(name, (ATerm) ATempty);
        }
        set<Expr> done2;
        checkVarDefs2(done2, defs2, (ATerm) rbnds);
    }

    else if (matchWith(e, with, body, pos)) {
        /* We can't check the body without evaluating the definitions
           (which is an arbitrary expression), so we don't do that
           here but only when actually evaluating the `with'. */
        checkVarDefs2(done, defs, with);
    }
    
    else if (ATgetType(e) == AT_APPL) {
        int arity = ATgetArity(ATgetAFun(e));
        for (int i = 0; i < arity; ++i)
            checkVarDefs2(done, defs, ATgetArgument(e, i));
    }

    else if (ATgetType(e) == AT_LIST)
        for (ATermIterator i((ATermList) e); i; ++i)
            checkVarDefs2(done, defs, *i);
}


void checkVarDefs(const ATermMap & defs, Expr e)
{
    set<Expr> done;
    checkVarDefs2(done, defs, e);
}


struct Canonicalise : TermFun
{
    ATerm operator () (ATerm e)
    {
        /* Remove position info. */
        ATerm path;
        int line, column;
        if (matchPos(e, path, line, column))
            return makeNoPos();

        /* Sort attribute sets. */
        ATermList _;
        if (matchAttrs(e, _)) {
            ATermMap attrs;
            queryAllAttrs(e, attrs);
            StringSet names;
            for (ATermMap::const_iterator i = attrs.begin(); i != attrs.end(); ++i)
                names.insert(aterm2String(i->key));

            ATermList attrs2 = ATempty;
            for (StringSet::reverse_iterator i = names.rbegin(); i != names.rend(); ++i)
                attrs2 = ATinsert(attrs2,
                    makeBind(toATerm(*i), attrs.get(toATerm(*i)), makeNoPos()));

            return makeAttrs(attrs2);
        }
        
        return e;
    }
};


Expr canonicaliseExpr(Expr e)
{
    Canonicalise canonicalise;
    return bottomupRewrite(canonicalise, e);
}


Expr makeBool(bool b)
{
    return b ? eTrue : eFalse;
}


bool matchStr(Expr e, string & s, PathSet & context)
{
    ATermList l;
    ATerm s_;

    if (!matchStr(e, s_, l)) return false;

    s = aterm2String(s_);

    for (ATermIterator i(l); i; ++i)
        context.insert(aterm2String(*i));

    return true;
}


Expr makeStr(const string & s, const PathSet & context)
{
    return makeStr(toATerm(s), toATermList(context));
}


string showType(Expr e)
{
    ATerm t1, t2;
    ATermList l1;
    ATermBlob b1;
    int i1;
    Pattern p1;
    if (matchStr(e, t1, l1)) return "a string";
    if (matchPath(e, t1)) return "a path";
    if (matchNull(e)) return "null";
    if (matchInt(e, i1)) return "an integer";
    if (matchBool(e, t1)) return "a boolean";
    if (matchFunction(e, p1, t1, t2)) return "a function";
    if (matchAttrs(e, l1)) return "an attribute set";
    if (matchList(e, l1)) return "a list";
    if (matchPrimOp(e, i1, b1, l1)) return "a partially applied built-in function";
    return "an unknown type";
}


string showValue(Expr e)
{
    PathSet context;
    string s;
    ATerm s2;
    int i;
    if (matchStr(e, s, context)) {
        string u;
        for (string::iterator i = s.begin(); i != s.end(); ++i)
            if (*i == '\"' || *i == '\\') u += "\\" + *i;
            else if (*i == '\n') u += "\\n";
            else if (*i == '\r') u += "\\r";
            else if (*i == '\t') u += "\\t";
            else u += *i;
        return "\"" + u + "\"";
    }
    if (matchPath(e, s2)) return aterm2String(s2);
    if (matchNull(e)) return "null";
    if (matchInt(e, i)) return (format("%1%") % i).str();
    if (e == eTrue) return "true";
    if (e == eFalse) return "false";
    /* !!! incomplete */
    return "<unknown>";
}

 
}
