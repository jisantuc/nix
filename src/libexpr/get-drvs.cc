#include "get-drvs.hh"
#include "nixexpr-ast.hh"


string DrvInfo::queryDrvPath(EvalState & state) const
{
    if (drvPath == "") {
        Expr a = attrs->get(toATerm("drvPath"));
        (string &) drvPath = a ? evalPath(state, a) : "";
    }
    return drvPath;
}


string DrvInfo::queryOutPath(EvalState & state) const
{
    if (outPath == "") {
        Expr a = attrs->get(toATerm("outPath"));
        if (!a) throw TypeError("output path missing");
        (string &) outPath = evalPath(state, a);
    }
    return outPath;
}


MetaInfo DrvInfo::queryMetaInfo(EvalState & state) const
{
    MetaInfo meta;
    
    Expr a = attrs->get(toATerm("meta"));
    if (!a) return meta; /* fine, empty meta information */

    ATermMap attrs2(16); /* !!! */
    queryAllAttrs(evalExpr(state, a), attrs2);

    for (ATermMap::const_iterator i = attrs2.begin(); i != attrs2.end(); ++i) {
        ATerm s = coerceToString(evalExpr(state, i->value));
        if (s)
            meta[aterm2String(i->key)] = aterm2String(s);
        /* For future compatibility, ignore attribute values that are
           not strings. */
    }

    return meta;
}


/* Cache for already evaluated derivations.  Usually putting ATerms in
   a STL container is unsafe (they're not scanning for GC roots), but
   here it doesn't matter; everything in this set is reachable from
   the stack as well. */
typedef set<Expr> Exprs;


/* Evaluate expression `e'.  If it evaluates to an attribute set of
   type `derivation', then put information about it in `drvs' (unless
   it's already in `doneExprs').  The result boolean indicates whether
   it makes sense for the caller to recursively search for derivations
   in `e'. */
static bool getDerivation(EvalState & state, Expr e,
    const string & attrPath, DrvInfos & drvs, Exprs & doneExprs)
{
    try {
        
        ATermList es;
        e = evalExpr(state, e);
        if (!matchAttrs(e, es)) return true;

        shared_ptr<ATermMap> attrs(new ATermMap(32)); /* !!! */
        queryAllAttrs(e, *attrs, false);
    
        Expr a = attrs->get(toATerm("type"));
        if (!a || evalString(state, a) != "derivation") return true;

        /* Remove spurious duplicates (e.g., an attribute set like
           `rec { x = derivation {...}; y = x;}'. */
        if (doneExprs.find(e) != doneExprs.end()) return false;
        doneExprs.insert(e);

        DrvInfo drv;
    
        a = attrs->get(toATerm("name"));
        /* !!! We really would like to have a decent back trace here. */
        if (!a) throw TypeError("derivation name missing");
        drv.name = evalString(state, a);

        a = attrs->get(toATerm("system"));
        if (!a)
            drv.system = "unknown";
        else
            drv.system = evalString(state, a);

        drv.attrs = attrs;

        drv.attrPath = attrPath;

        drvs.push_back(drv);
        return false;
    
    } catch (AssertionError & e) {
        return false;
    }
}


bool getDerivation(EvalState & state, Expr e, DrvInfo & drv)
{
    Exprs doneExprs;
    DrvInfos drvs;
    getDerivation(state, e, "", drvs, doneExprs);
    if (drvs.size() != 1) return false;
    drv = drvs.front();
    return true;
}


static string addToPath(const string & s1, const string & s2)
{
    return s1.empty() ? s2 : s1 + "." + s2;
}


static void getDerivations(EvalState & state, Expr e,
    const string & pathPrefix, const ATermMap & autoArgs,
    DrvInfos & drvs, Exprs & doneExprs)
{
    e = evalExpr(state, autoCallFunction(evalExpr(state, e), autoArgs));

    /* Process the expression. */
    ATermList es;
    DrvInfo drv;

    if (!getDerivation(state, e, pathPrefix, drvs, doneExprs))
        return;

    if (matchAttrs(e, es)) {
        ATermMap drvMap(ATgetLength(es));
        queryAllAttrs(e, drvMap);
        
        for (ATermMap::const_iterator i = drvMap.begin(); i != drvMap.end(); ++i) {
            startNest(nest, lvlDebug,
                format("evaluating attribute `%1%'") % aterm2String(i->key));
            string pathPrefix2 = addToPath(pathPrefix, aterm2String(i->key));
            if (getDerivation(state, i->value, pathPrefix2, drvs, doneExprs)) {
                /* If the value of this attribute is itself an
                   attribute set, should we recurse into it?  => Only
                   if it has a `recurseForDerivations = true'
                   attribute. */
                ATermList es;
                Expr e = evalExpr(state, i->value);
                if (matchAttrs(e, es)) {
                    ATermMap attrs(ATgetLength(es));
                    queryAllAttrs(e, attrs, false);
                    Expr e2 = attrs.get(toATerm("recurseForDerivations"));
                    if (e2 && evalBool(state, e2))
                        getDerivations(state, e, pathPrefix2, autoArgs, drvs, doneExprs);
                }
            }
        }
        
        return;
    }

    if (matchList(e, es)) {
        int n = 0;
        for (ATermIterator i(es); i; ++i, ++n) {
            startNest(nest, lvlDebug,
                format("evaluating list element"));
            string pathPrefix2 = addToPath(pathPrefix, (format("%1%") % n).str());
            if (getDerivation(state, *i, pathPrefix2, drvs, doneExprs))
                getDerivations(state, *i, pathPrefix2, autoArgs, drvs, doneExprs);
        }
        return;
    }

    throw TypeError("expression does not evaluate to a derivation (or a set or list of those)");
}


void getDerivations(EvalState & state, Expr e, const string & pathPrefix,
    const ATermMap & autoArgs, DrvInfos & drvs)
{
    Exprs doneExprs;
    getDerivations(state, e, pathPrefix, autoArgs, drvs, doneExprs);
}
