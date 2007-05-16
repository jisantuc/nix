#include "misc.hh"
#include "eval.hh"
#include "globals.hh"
#include "store-api.hh"
#include "util.hh"
#include "archive.hh"
#include "expr-to-xml.hh"
#include "nixexpr-ast.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>


namespace nix {


/*************************************************************
 * Constants
 *************************************************************/


static Expr prim_builtins(EvalState & state, const ATermVector & args)
{
    /* Return an attribute set containing all primops.  This allows
       Nix expressions to test for new primops and take appropriate
       action if they're not available.  For instance, rather than
       calling a primop `foo' directly, they could say `if builtins ?
       foo then builtins.foo ... else ...'. */

    ATermMap builtins(state.primOps.size());

    for (ATermMap::const_iterator i = state.primOps.begin();
         i != state.primOps.end(); ++i)
    {
        string name = aterm2String(i->key);
        if (string(name, 0, 2) == "__")
            name = string(name, 2);
        /* !!! should use makePrimOp here, I guess. */
        builtins.set(toATerm(name), makeAttrRHS(makeVar(i->key), makeNoPos()));
    }

    return makeAttrs(builtins);
}


/* Boolean constructors. */
static Expr prim_true(EvalState & state, const ATermVector & args)
{
    return eTrue;
}


static Expr prim_false(EvalState & state, const ATermVector & args)
{
    return eFalse;
}


/* Return the null value. */
static Expr prim_null(EvalState & state, const ATermVector & args)
{
    return makeNull();
}


/* Return a string constant representing the current platform.  Note!
   that differs between platforms, so Nix expressions using
   `__currentSystem' can evaluate to different values on different
   platforms. */
static Expr prim_currentSystem(EvalState & state, const ATermVector & args)
{
    return makeStr(thisSystem);
}


static Expr prim_currentTime(EvalState & state, const ATermVector & args)
{
    return ATmake("Int(<int>)", time(0));
}


/*************************************************************
 * Miscellaneous
 *************************************************************/


/* Load and evaluate an expression from path specified by the
   argument. */ 
static Expr prim_import(EvalState & state, const ATermVector & args)
{
    PathSet context;
    Path path = coerceToPath(state, args[0], context);

    for (PathSet::iterator i = context.begin(); i != context.end(); ++i) {
        assert(isStorePath(*i));
        if (!store->isValidPath(*i))
            throw EvalError(format("cannot import `%1%', since path `%2%' is not valid")
                % path % *i);
        if (isDerivation(*i))
            store->buildDerivations(singleton<PathSet>(*i));
    }

    return evalFile(state, path);
}


/* Determine whether the argument is the null value. */
static Expr prim_isNull(EvalState & state, const ATermVector & args)
{
    return makeBool(matchNull(evalExpr(state, args[0])));
}


/* Determine whether the argument is a function. */
static Expr prim_isFunction(EvalState & state, const ATermVector & args)
{
    Expr e = evalExpr(state, args[0]);
    ATermList formals;
    ATerm name, body, pos;
    return makeBool(
        matchFunction(e, formals, body, pos) ||
        matchFunction1(e, name, body, pos));
}


static Path findDependency(Path dir, string dep)
{
    if (dep[0] == '/') throw EvalError(
        format("illegal absolute dependency `%1%'") % dep);

    Path p = canonPath(dir + "/" + dep);

    if (pathExists(p))
        return p;
    else
        return "";
}


/* Make path `p' relative to directory `pivot'.  E.g.,
   relativise("/a/b/c", "a/b/x/y") => "../x/y".  Both input paths
   should be in absolute canonical form. */
static string relativise(Path pivot, Path p)
{
    assert(pivot.size() > 0 && pivot[0] == '/');
    assert(p.size() > 0 && p[0] == '/');
        
    if (pivot == p) return ".";

    /* `p' is in `pivot'? */
    Path pivot2 = pivot + "/";
    if (p.substr(0, pivot2.size()) == pivot2) {
        return p.substr(pivot2.size());
    }

    /* Otherwise, `p' is in a parent of `pivot'.  Find up till which
       path component `p' and `pivot' match, and add an appropriate
       number of `..' components. */
    string::size_type i = 1;
    while (1) {
        string::size_type j = pivot.find('/', i);
        if (j == string::npos) break;
        j++;
        if (pivot.substr(0, j) != p.substr(0, j)) break;
        i = j;
    }

    string prefix;
    unsigned int slashes = count(pivot.begin() + i, pivot.end(), '/') + 1;
    while (slashes--) {
        prefix += "../";
    }

    return prefix + p.substr(i);
}


static Expr prim_dependencyClosure(EvalState & state, const ATermVector & args)
{
    startNest(nest, lvlDebug, "finding dependencies");

    Expr attrs = evalExpr(state, args[0]);

    /* Get the start set. */
    Expr startSet = queryAttr(attrs, "startSet");
    if (!startSet) throw EvalError("attribute `startSet' required");
    ATermList startSet2 = evalList(state, startSet);

    Path pivot;
    PathSet workSet;
    for (ATermIterator i(startSet2); i; ++i) {
        PathSet context; /* !!! what to do? */
        Path p = coerceToPath(state, *i, context);
        workSet.insert(p);
        pivot = dirOf(p);
    }

    /* Get the search path. */
    PathSet searchPath;
    Expr e = queryAttr(attrs, "searchPath");
    if (e) {
        ATermList list = evalList(state, e);
        for (ATermIterator i(list); i; ++i) {
            PathSet context; /* !!! what to do? */
            Path p = coerceToPath(state, *i, context);
            searchPath.insert(p);
        }
    }

    Expr scanner = queryAttr(attrs, "scanner");
    if (!scanner) throw EvalError("attribute `scanner' required");
    
    /* Construct the dependency closure by querying the dependency of
       each path in `workSet', adding the dependencies to
       `workSet'. */
    PathSet doneSet;
    while (!workSet.empty()) {
	Path path = *(workSet.begin());
	workSet.erase(path);

	if (doneSet.find(path) != doneSet.end()) continue;
        doneSet.insert(path);

        try {
            
            /* Call the `scanner' function with `path' as argument. */
            debug(format("finding dependencies in `%1%'") % path);
            ATermList deps = evalList(state, makeCall(scanner, makeStr(path)));

            /* Try to find the dependencies relative to the `path'. */
            for (ATermIterator i(deps); i; ++i) {
                string s = evalStringNoCtx(state, *i);
                
                Path dep = findDependency(dirOf(path), s);

                if (dep == "") {
                    for (PathSet::iterator j = searchPath.begin();
                         j != searchPath.end(); ++j)
                    {
                        dep = findDependency(*j, s);
                        if (dep != "") break;
                    }
                }
                
                if (dep == "")
                    debug(format("did NOT find dependency `%1%'") % s);
                else {
                    debug(format("found dependency `%1%'") % dep);
                    workSet.insert(dep);
                }
            }

        } catch (Error & e) {
            e.addPrefix(format("while finding dependencies in `%1%':\n")
                % path);
            throw;
        }
    }

    /* Return a list of the dependencies we've just found. */
    ATermList deps = ATempty;
    for (PathSet::iterator i = doneSet.begin(); i != doneSet.end(); ++i) {
        deps = ATinsert(deps, makeStr(relativise(pivot, *i)));
        deps = ATinsert(deps, makeStr(*i));
    }

    debug(format("dependency list is `%1%'") % makeList(deps));
    
    return makeList(deps);
}


static Expr prim_abort(EvalState & state, const ATermVector & args)
{
    PathSet context;
    throw Abort(format("evaluation aborted with the following error message: `%1%'") %
        evalString(state, args[0], context));
}


static Expr prim_throw(EvalState & state, const ATermVector & args)
{
    PathSet context;
    throw ThrownError(format("user-thrown exception: `%1%'") %
        evalString(state, args[0], context));
}


/* Return an environment variable.  Use with care. */
static Expr prim_getEnv(EvalState & state, const ATermVector & args)
{
    string name = evalStringNoCtx(state, args[0]);
    return makeStr(getEnv(name));
}


static Expr prim_relativise(EvalState & state, const ATermVector & args)
{
    PathSet context; /* !!! what to do? */
    Path pivot = coerceToPath(state, args[0], context);
    Path path = coerceToPath(state, args[1], context);
    return makeStr(relativise(pivot, path));
}


/*************************************************************
 * Derivations
 *************************************************************/


/* Returns the hash of a derivation modulo fixed-output
   subderivations.  A fixed-output derivation is a derivation with one
   output (`out') for which an expected hash and hash algorithm are
   specified (using the `outputHash' and `outputHashAlgo'
   attributes).  We don't want changes to such derivations to
   propagate upwards through the dependency graph, changing output
   paths everywhere.

   For instance, if we change the url in a call to the `fetchurl'
   function, we do not want to rebuild everything depending on it
   (after all, (the hash of) the file being downloaded is unchanged).
   So the *output paths* should not change.  On the other hand, the
   *derivation store expression paths* should change to reflect the
   new dependency graph.

   That's what this function does: it returns a hash which is just the
   of the derivation ATerm, except that any input store expression
   paths have been replaced by the result of a recursive call to this
   function, and that for fixed-output derivations we return
   (basically) its outputHash. */
static Hash hashDerivationModulo(EvalState & state, Derivation drv)
{
    /* Return a fixed hash for fixed-output derivations. */
    if (drv.outputs.size() == 1) {
        DerivationOutputs::const_iterator i = drv.outputs.begin();
        if (i->first == "out" &&
            i->second.hash != "")
        {
            return hashString(htSHA256, "fixed:out:"
                + i->second.hashAlgo + ":"
                + i->second.hash + ":"
                + i->second.path);
        }
    }

    /* For other derivations, replace the inputs paths with recursive
       calls to this function.*/
    DerivationInputs inputs2;
    for (DerivationInputs::iterator i = drv.inputDrvs.begin();
         i != drv.inputDrvs.end(); ++i)
    {
        Hash h = state.drvHashes[i->first];
        if (h.type == htUnknown) {
            Derivation drv2 = derivationFromPath(i->first);
            h = hashDerivationModulo(state, drv2);
            state.drvHashes[i->first] = h;
        }
        inputs2[printHash(h)] = i->second;
    }
    drv.inputDrvs = inputs2;
    
    return hashTerm(unparseDerivation(drv));
}


/* Construct (as a unobservable side effect) a Nix derivation
   expression that performs the derivation described by the argument
   set.  Returns the original set extended with the following
   attributes: `outPath' containing the primary output path of the
   derivation; `drvPath' containing the path of the Nix expression;
   and `type' set to `derivation' to indicate that this is a
   derivation. */
static Expr prim_derivationStrict(EvalState & state, const ATermVector & args)
{
    startNest(nest, lvlVomit, "evaluating derivation");

    ATermMap attrs;
    queryAllAttrs(evalExpr(state, args[0]), attrs, true);

    /* Figure out the name already (for stack backtraces). */
    ATerm posDrvName;
    Expr eDrvName = attrs.get(toATerm("name"));
    if (!eDrvName)
        throw EvalError("required attribute `name' missing");
    if (!matchAttrRHS(eDrvName, eDrvName, posDrvName)) abort();
    string drvName;
    try {        
        drvName = evalStringNoCtx(state, eDrvName);
    } catch (Error & e) {
        e.addPrefix(format("while evaluating the derivation attribute `name' at %1%:\n")
            % showPos(posDrvName));
        throw;
    }

    /* Build the derivation expression by processing the attributes. */
    Derivation drv;
    
    PathSet context;

    string outputHash;
    string outputHashAlgo;
    bool outputHashRecursive = false;

    for (ATermMap::const_iterator i = attrs.begin(); i != attrs.end(); ++i) {
        string key = aterm2String(i->key);
        ATerm value;
        Expr pos;
        ATerm rhs = i->value;
        if (!matchAttrRHS(rhs, value, pos)) abort();
        startNest(nest, lvlVomit, format("processing attribute `%1%'") % key);

        try {

            /* The `args' attribute is special: it supplies the
               command-line arguments to the builder. */
            if (key == "args") {
                ATermList es;
                value = evalExpr(state, value);
                if (!matchList(value, es)) {
                    static bool haveWarned = false;
                    warnOnce(haveWarned, "the `args' attribute should evaluate to a list");
                    es = flattenList(state, value);
                }
                for (ATermIterator i(es); i; ++i) {
                    string s = coerceToString(state, *i, context, true);
                    drv.args.push_back(s);
                }
            }

            /* All other attributes are passed to the builder through
               the environment. */
            else {
                string s = coerceToString(state, value, context, true);
                drv.env[key] = s;
                if (key == "builder") drv.builder = s;
                else if (key == "system") drv.platform = s;
                else if (key == "name") drvName = s;
                else if (key == "outputHash") outputHash = s;
                else if (key == "outputHashAlgo") outputHashAlgo = s;
                else if (key == "outputHashMode") {
                    if (s == "recursive") outputHashRecursive = true; 
                    else if (s == "flat") outputHashRecursive = false;
                    else throw EvalError(format("invalid value `%1%' for `outputHashMode' attribute") % s);
                }
            }

        } catch (Error & e) {
            e.addPrefix(format("while evaluating the derivation attribute `%1%' at %2%:\n")
                % key % showPos(pos));
            e.addPrefix(format("while instantiating the derivation named `%1%' at %2%:\n")
                % drvName % showPos(posDrvName));
            throw;
        }

    }
    
    /* Everything in the context of the strings in the derivation
       attributes should be added as dependencies of the resulting
       derivation. */
    for (PathSet::iterator i = context.begin(); i != context.end(); ++i) {
        debug(format("derivation uses `%1%'") % *i);
        assert(isStorePath(*i));
        if (isDerivation(*i))
            drv.inputDrvs[*i] = singleton<StringSet>("out");
        else
            drv.inputSrcs.insert(*i);
    }
            
    /* Do we have all required attributes? */
    if (drv.builder == "")
        throw EvalError("required attribute `builder' missing");
    if (drv.platform == "")
        throw EvalError("required attribute `system' missing");

    /* If an output hash was given, check it. */
    if (outputHash == "")
        outputHashAlgo = "";
    else {
        HashType ht = parseHashType(outputHashAlgo);
        if (ht == htUnknown)
            throw EvalError(format("unknown hash algorithm `%1%'") % outputHashAlgo);
        Hash h(ht);
        if (outputHash.size() == h.hashSize * 2)
            /* hexadecimal representation */
            h = parseHash(ht, outputHash);
        else if (outputHash.size() == hashLength32(h))
            /* base-32 representation */
            h = parseHash32(ht, outputHash);
        else
            throw Error(format("hash `%1%' has wrong length for hash type `%2%'")
                % outputHash % outputHashAlgo);
        string s = outputHash;
        outputHash = printHash(h);
        if (outputHashRecursive) outputHashAlgo = "r:" + outputHashAlgo;
    }

    /* Check whether the derivation name is valid. */
    checkStoreName(drvName);
    if (isDerivation(drvName))
        throw EvalError(format("derivation names are not allowed to end in `%1%'")
            % drvExtension);

    /* Construct the "masked" derivation store expression, which is
       the final one except that in the list of outputs, the output
       paths are empty, and the corresponding environment variables
       have an empty value.  This ensures that changes in the set of
       output names do get reflected in the hash. */
    drv.env["out"] = "";
    drv.outputs["out"] =
        DerivationOutput("", outputHashAlgo, outputHash);
        
    /* Use the masked derivation expression to compute the output
       path. */
    Path outPath = makeStorePath("output:out",
        hashDerivationModulo(state, drv), drvName);

    /* Construct the final derivation store expression. */
    drv.env["out"] = outPath;
    drv.outputs["out"] =
        DerivationOutput(outPath, outputHashAlgo, outputHash);

    /* Write the resulting term into the Nix store directory. */
    Path drvPath = writeDerivation(drv, drvName);

    printMsg(lvlChatty, format("instantiated `%1%' -> `%2%'")
        % drvName % drvPath);

    /* Optimisation, but required in read-only mode! because in that
       case we don't actually write store expressions, so we can't
       read them later. */
    state.drvHashes[drvPath] = hashDerivationModulo(state, drv);

    /* !!! assumes a single output */
    ATermMap outAttrs(2);
    outAttrs.set(toATerm("outPath"),
        makeAttrRHS(makeStr(outPath, singleton<PathSet>(drvPath)), makeNoPos()));
    outAttrs.set(toATerm("drvPath"),
        makeAttrRHS(makeStr(drvPath, singleton<PathSet>(drvPath)), makeNoPos()));

    return makeAttrs(outAttrs);
}


static Expr prim_derivationLazy(EvalState & state, const ATermVector & args)
{
    Expr eAttrs = evalExpr(state, args[0]);
    ATermMap attrs;    
    queryAllAttrs(eAttrs, attrs, true);

    attrs.set(toATerm("type"),
        makeAttrRHS(makeStr("derivation"), makeNoPos()));

    Expr drvStrict = makeCall(makeVar(toATerm("derivation!")), eAttrs);

    attrs.set(toATerm("outPath"),
        makeAttrRHS(makeSelect(drvStrict, toATerm("outPath")), makeNoPos()));
    attrs.set(toATerm("drvPath"),
        makeAttrRHS(makeSelect(drvStrict, toATerm("drvPath")), makeNoPos()));
    
    return makeAttrs(attrs);
}


/*************************************************************
 * Paths
 *************************************************************/


/* Convert the argument to a path.  !!! obsolete? */
static Expr prim_toPath(EvalState & state, const ATermVector & args)
{
    PathSet context;
    string path = coerceToPath(state, args[0], context);
    return makeStr(canonPath(path), context);
}


static Expr prim_pathExists(EvalState & state, const ATermVector & args)
{
    PathSet context;
    Path path = coerceToPath(state, args[0], context);
    if (!context.empty())
        throw EvalError(format("string `%1%' cannot refer to other paths") % path);
    return makeBool(pathExists(path));
}


/* Return the base name of the given string, i.e., everything
   following the last slash. */
static Expr prim_baseNameOf(EvalState & state, const ATermVector & args)
{
    PathSet context;
    return makeStr(baseNameOf(coerceToString(state, args[0], context)), context);
}


/* Return the directory of the given path, i.e., everything before the
   last slash.  Return either a path or a string depending on the type
   of the argument. */
static Expr prim_dirOf(EvalState & state, const ATermVector & args)
{
    PathSet context;
    Expr e = evalExpr(state, args[0]); ATerm dummy;
    bool isPath = matchPath(e, dummy);
    Path dir = dirOf(coerceToPath(state, e, context));
    return isPath ? makePath(toATerm(dir)) : makeStr(dir, context);
}


/*************************************************************
 * Creating files
 *************************************************************/


/* Convert the argument (which can be any Nix expression) to an XML
   representation returned in a string.  Not all Nix expressions can
   be sensibly or completely represented (e.g., functions). */
static Expr prim_toXML(EvalState & state, const ATermVector & args)
{
    std::ostringstream out;
    PathSet context;
    printTermAsXML(strictEvalExpr(state, args[0]), out, context);
    return makeStr(out.str(), context);
}


/* Store a string in the Nix store as a source file that can be used
   as an input by derivations. */
static Expr prim_toFile(EvalState & state, const ATermVector & args)
{
    PathSet context;
    string name = evalStringNoCtx(state, args[0]);
    string contents = evalString(state, args[1], context);

    PathSet refs;

    for (PathSet::iterator i = context.begin(); i != context.end(); ++i) {
        if (isDerivation(*i))
            throw EvalError(format("in `toFile': the file `%1%' cannot refer to derivation outputs") % name);
        refs.insert(*i);
    }
    
    Path storePath = readOnlyMode
        ? computeStorePathForText(name, contents, refs)
        : store->addTextToStore(name, contents, refs);

    /* Note: we don't need to add `context' to the context of the
       result, since `storePath' itself has references to the paths
       used in args[1]. */
    
    return makeStr(storePath, singleton<PathSet>(storePath));
}


struct FilterFromExpr : PathFilter
{
    EvalState & state;
    Expr filter;
    
    FilterFromExpr(EvalState & state, Expr filter)
        : state(state), filter(filter)
    {
    }

    bool operator () (const Path & path)
    {
        struct stat st;
        if (lstat(path.c_str(), &st))
            throw SysError(format("getting attributes of path `%1%'") % path);

        Expr call =
            makeCall(
                makeCall(filter, makeStr(path)),
                makeStr(
                    S_ISREG(st.st_mode) ? "regular" :
                    S_ISDIR(st.st_mode) ? "directory" :
                    S_ISLNK(st.st_mode) ? "symlink" :
                    "unknown" /* not supported, will fail! */
                    ));
                
        return evalBool(state, call);
    }
};


static Expr prim_filterSource(EvalState & state, const ATermVector & args)
{
    PathSet context;
    Path path = coerceToPath(state, args[1], context);
    if (!context.empty())
        throw EvalError(format("string `%1%' cannot refer to other paths") % path);

    FilterFromExpr filter(state, args[0]);

    Path dstPath = readOnlyMode
        ? computeStorePathForPath(path, false, false, "", filter).first
        : store->addToStore(path, false, false, "", filter);

    return makeStr(dstPath, singleton<PathSet>(dstPath));
}


/*************************************************************
 * Attribute sets
 *************************************************************/


/* Return the names of the attributes in an attribute set as a sorted
   list of strings. */
static Expr prim_attrNames(EvalState & state, const ATermVector & args)
{
    ATermMap attrs;
    queryAllAttrs(evalExpr(state, args[0]), attrs);

    StringSet names;
    for (ATermMap::const_iterator i = attrs.begin(); i != attrs.end(); ++i)
        names.insert(aterm2String(i->key));

    ATermList list = ATempty;
    for (StringSet::const_reverse_iterator i = names.rbegin();
         i != names.rend(); ++i)
        list = ATinsert(list, makeStr(*i, PathSet()));

    return makeList(list);
}


/* Dynamic version of the `.' operator. */
static Expr prim_getAttr(EvalState & state, const ATermVector & args)
{
    string attr = evalStringNoCtx(state, args[0]);
    return evalExpr(state, makeSelect(args[1], toATerm(attr)));
}


/* Dynamic version of the `?' operator. */
static Expr prim_hasAttr(EvalState & state, const ATermVector & args)
{
    string attr = evalStringNoCtx(state, args[0]);
    return evalExpr(state, makeOpHasAttr(args[1], toATerm(attr)));
}


static Expr prim_removeAttrs(EvalState & state, const ATermVector & args)
{
    ATermMap attrs;
    queryAllAttrs(evalExpr(state, args[0]), attrs, true);
    
    ATermList list = evalList(state, args[1]);

    for (ATermIterator i(list); i; ++i)
        /* It's not an error for *i not to exist. */
        attrs.remove(toATerm(evalStringNoCtx(state, *i)));

    return makeAttrs(attrs);
}


/*************************************************************
 * Lists
 *************************************************************/


/* Determine whether the argument is a list. */
static Expr prim_isList(EvalState & state, const ATermVector & args)
{
    ATermList list;
    return makeBool(matchList(evalExpr(state, args[0]), list));
}


/* Return the first element of a list. */
static Expr prim_head(EvalState & state, const ATermVector & args)
{
    ATermList list = evalList(state, args[0]);
    if (ATisEmpty(list))
        throw Error("`head' called on an empty list");
    return evalExpr(state, ATgetFirst(list));
}


/* Return a list consisting of everything but the the first element of
   a list. */
static Expr prim_tail(EvalState & state, const ATermVector & args)
{
    ATermList list = evalList(state, args[0]);
    if (ATisEmpty(list))
        throw Error("`tail' called on an empty list");
    return makeList(ATgetNext(list));
}


/* Apply a function to every element of a list. */
static Expr prim_map(EvalState & state, const ATermVector & args)
{
    Expr fun = evalExpr(state, args[0]);
    ATermList list = evalList(state, args[1]);

    ATermList res = ATempty;
    for (ATermIterator i(list); i; ++i)
        res = ATinsert(res, makeCall(fun, *i));

    return makeList(ATreverse(res));
}


/*************************************************************
 * Integer arithmetic
 *************************************************************/


static Expr prim_add(EvalState & state, const ATermVector & args)
{
    int i1 = evalInt(state, args[0]);
    int i2 = evalInt(state, args[1]);
    return makeInt(i1 + i2);
}


static Expr prim_sub(EvalState & state, const ATermVector & args)
{
    int i1 = evalInt(state, args[0]);
    int i2 = evalInt(state, args[1]);
    return makeInt(i1 - i2);
}


static Expr prim_lessThan(EvalState & state, const ATermVector & args)
{
    int i1 = evalInt(state, args[0]);
    int i2 = evalInt(state, args[1]);
    return makeBool(i1 < i2);
}


/*************************************************************
 * String manipulation
 *************************************************************/


/* Convert the argument to a string.  Paths are *not* copied to the
   store, so `toString /foo/bar' yields `"/foo/bar"', not
   `"/nix/store/whatever..."'. */
static Expr prim_toString(EvalState & state, const ATermVector & args)
{
    PathSet context;
    string s = coerceToString(state, args[0], context, true, false);
    return makeStr(s, context);
}


/* `substr start len str' returns the substring of `str' starting at
   character position `min(start, stringLength str)' inclusive and
   ending at `min(start + len, stringLength str)'.  `start' must be
   non-negative. */
static Expr prim_substring(EvalState & state, const ATermVector & args)
{
    int start = evalInt(state, args[0]);
    int len = evalInt(state, args[1]);
    PathSet context;
    string s = coerceToString(state, args[2], context);

    if (start < 0) throw EvalError("negative start position in `substring'");

    return makeStr(string(s, start, len), context);
}


static Expr prim_stringLength(EvalState & state, const ATermVector & args)
{
    PathSet context;
    string s = coerceToString(state, args[0], context);
    return makeInt(s.size());
}


/*************************************************************
 * Primop registration
 *************************************************************/


void EvalState::addPrimOps()
{
    addPrimOp("builtins", 0, prim_builtins);
        
    // Constants
    addPrimOp("true", 0, prim_true);
    addPrimOp("false", 0, prim_false);
    addPrimOp("null", 0, prim_null);
    addPrimOp("__currentSystem", 0, prim_currentSystem);
    addPrimOp("__currentTime", 0, prim_currentTime);

    // Miscellaneous
    addPrimOp("import", 1, prim_import);
    addPrimOp("isNull", 1, prim_isNull);
    addPrimOp("__isFunction", 1, prim_isFunction);
    addPrimOp("dependencyClosure", 1, prim_dependencyClosure);
    addPrimOp("abort", 1, prim_abort);
    addPrimOp("throw", 1, prim_throw);
    addPrimOp("__getEnv", 1, prim_getEnv);

    addPrimOp("relativise", 2, prim_relativise);

    // Derivations
    addPrimOp("derivation!", 1, prim_derivationStrict);
    addPrimOp("derivation", 1, prim_derivationLazy);

    // Paths
    addPrimOp("__toPath", 1, prim_toPath);
    addPrimOp("__pathExists", 1, prim_pathExists);
    addPrimOp("baseNameOf", 1, prim_baseNameOf);
    addPrimOp("dirOf", 1, prim_dirOf);

    // Creating files
    addPrimOp("__toXML", 1, prim_toXML);
    addPrimOp("__toFile", 2, prim_toFile);
    addPrimOp("__filterSource", 2, prim_filterSource);

    // Attribute sets
    addPrimOp("__attrNames", 1, prim_attrNames);
    addPrimOp("__getAttr", 2, prim_getAttr);
    addPrimOp("__hasAttr", 2, prim_hasAttr);
    addPrimOp("removeAttrs", 2, prim_removeAttrs);

    // Lists
    addPrimOp("__isList", 1, prim_isList);
    addPrimOp("__head", 1, prim_head);
    addPrimOp("__tail", 1, prim_tail);
    addPrimOp("map", 2, prim_map);

    // Integer arithmetic
    addPrimOp("__add", 2, prim_add);
    addPrimOp("__sub", 2, prim_sub);
    addPrimOp("__lessThan", 2, prim_lessThan);

    // String manipulation
    addPrimOp("toString", 1, prim_toString);
    addPrimOp("__substring", 3, prim_substring);
    addPrimOp("__stringLength", 1, prim_stringLength);
}


}
