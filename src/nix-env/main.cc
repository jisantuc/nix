#include "profiles.hh"
#include "names.hh"
#include "globals.hh"
#include "build.hh"
#include "gc.hh"
#include "shared.hh"
#include "parser.hh"
#include "eval.hh"
#include "help.txt.hh"
#include "nixexpr-ast.hh"
#include "get-drvs.hh"

#include <cerrno>
#include <ctime>
#include <algorithm>

#include <unistd.h>


typedef enum {
    srcNixExprDrvs,
    srcNixExprs,
    srcStorePaths,
    srcProfile,
    srcUnknown
} InstallSourceType;


struct InstallSourceInfo
{
    InstallSourceType type;
    Path nixExprPath; /* for srcNixExprDrvs, srcNixExprs */
    Path profile; /* for srcProfile */
    string systemFilter; /* for srcNixExprDrvs */
};


struct Globals
{
    InstallSourceInfo instSource;
    Path profile;
    EvalState state;
    bool dryRun;
    bool preserveInstalled;
    bool keepDerivations;
};


typedef void (* Operation) (Globals & globals,
    Strings opFlags, Strings opArgs);


void printHelp()
{
    cout << string((char *) helpText, sizeof helpText);
}


static void loadDerivations(EvalState & state, Path nixExprPath,
    string systemFilter, DrvInfos & elems)
{
    getDerivations(state,
        parseExprFromFile(state, absPath(nixExprPath)), elems);

    /* Filter out all derivations not applicable to the current
       system. */
    for (DrvInfos::iterator i = elems.begin(), j; i != elems.end(); i = j) {
        j = i; j++;
        if (systemFilter != "*" && i->system != systemFilter)
            elems.erase(i);
    }
}


static Path getHomeDir()
{
    Path homeDir(getEnv("HOME", ""));
    if (homeDir == "") throw Error("HOME environment variable not set");
    return homeDir;
}


static Path getDefNixExprPath()
{
    return getHomeDir() + "/.nix-defexpr";
}


struct AddPos : TermFun
{
    ATerm operator () (ATerm e)
    {
        ATerm x, y, z;
        if (matchBind(e, x, y, z)) return e;
        if (matchBind2(e, x, y))
            return makeBind(x, y, makeNoPos());
        return e;
    }
};


static DrvInfos queryInstalled(EvalState & state, const Path & userEnv)
{
    Path path = userEnv + "/manifest";

    if (!pathExists(path))
        return DrvInfos(); /* not an error, assume nothing installed */

    Expr e = ATreadFromNamedFile(path.c_str());
    if (!e) throw Error(format("cannot read Nix expression from `%1%'") % path);

    /* Compatibility: Bind(x, y) -> Bind(x, y, NoPos). */
    AddPos addPos;
    e = bottomupRewrite(addPos, e);

    DrvInfos elems;
    getDerivations(state, e, elems);
    return elems;
}


static void createUserEnv(EvalState & state, const DrvInfos & elems,
    const Path & profile, bool keepDerivations)
{
    /* Build the components in the user environment, if they don't
       exist already. */
    PathSet drvsToBuild;
    for (DrvInfos::const_iterator i = elems.begin(); 
         i != elems.end(); ++i)
        /* Call to `isDerivation' is for compatibility with Nix <= 0.7
           user environments. */
        if (i->queryDrvPath(state) != "" &&
            isDerivation(i->queryDrvPath(state)))
            drvsToBuild.insert(i->queryDrvPath(state));

    debug(format("building user environment dependencies"));
    buildDerivations(drvsToBuild);

    /* Get the environment builder expression. */
    Expr envBuilder = parseExprFromFile(state,
        nixDataDir + "/nix/corepkgs/buildenv"); /* !!! */

    /* Construct the whole top level derivation. */
    PathSet references;
    ATermList manifest = ATempty;
    ATermList inputs = ATempty;
    for (DrvInfos::const_iterator i = elems.begin(); 
         i != elems.end(); ++i)
    {
        Path drvPath = keepDerivations ? i->queryDrvPath(state) : "";
        ATerm t = makeAttrs(ATmakeList5(
            makeBind(toATerm("type"),
                makeStr(toATerm("derivation")), makeNoPos()),
            makeBind(toATerm("name"),
                makeStr(toATerm(i->name)), makeNoPos()),
            makeBind(toATerm("system"),
                makeStr(toATerm(i->system)), makeNoPos()),
            makeBind(toATerm("drvPath"),
                makePath(toATerm(drvPath)), makeNoPos()),
            makeBind(toATerm("outPath"),
                makePath(toATerm(i->queryOutPath(state))), makeNoPos())
            ));
        manifest = ATinsert(manifest, t);
        inputs = ATinsert(inputs, makeStr(toATerm(i->queryOutPath(state))));

        /* This is only necessary when installing store paths, e.g.,
           `nix-env -i /nix/store/abcd...-foo'. */
        addTempRoot(i->queryOutPath(state));
        ensurePath(i->queryOutPath(state));
        
        references.insert(i->queryOutPath(state));
        if (drvPath != "") references.insert(drvPath);
    }

    /* Also write a copy of the list of inputs to the store; we need
       it for future modifications of the environment. */
    Path manifestFile = addTextToStore("env-manifest",
        atPrint(makeList(ATreverse(manifest))), references);

    Expr topLevel = makeCall(envBuilder, makeAttrs(ATmakeList3(
        makeBind(toATerm("system"),
            makeStr(toATerm(thisSystem)), makeNoPos()),
        makeBind(toATerm("derivations"),
            makeList(ATreverse(inputs)), makeNoPos()),
        makeBind(toATerm("manifest"),
            makeAttrs(ATmakeList2(
                makeBind(toATerm("type"),
                    makeStr(toATerm("storePath")), makeNoPos()),
                makeBind(toATerm("outPath"),
                    makePath(toATerm(manifestFile)), makeNoPos())
                )), makeNoPos())
        )));

    /* Instantiate it. */
    debug(format("evaluating builder expression `%1%'") % topLevel);
    DrvInfo topLevelDrv;
    if (!getDerivation(state, topLevel, topLevelDrv))
        abort();
    
    /* Realise the resulting store expression. */
    debug(format("building user environment"));
    buildDerivations(singleton<PathSet>(topLevelDrv.queryDrvPath(state)));

    /* Switch the current user environment to the output path. */
    debug(format("switching to new user environment"));
    Path generation = createGeneration(profile, topLevelDrv.queryOutPath(state));
    switchLink(profile, generation);
}


static DrvInfos filterBySelector(EvalState & state,
    const DrvInfos & allElems,
    const Strings & args, bool newestOnly)
{
    DrvNames selectors = drvNamesFromArgs(args);

    DrvInfos elems;
    PathSet done;

#if 0    
    /* Filter out the ones we're not interested in. */
    for (DrvInfos::const_iterator i = allElems.begin();
         i != allElems.end(); ++i)
    {
        DrvName drvName(i->name);
        for (DrvNames::iterator j = selectors.begin();
             j != selectors.end(); ++j)
        {
            if (j->matches(drvName)) {
                j->hits++;
                elems.push_back(*i);
            }
        }
    }
#endif

    for (DrvNames::iterator i = selectors.begin();
         i != selectors.end(); ++i)
    {
        DrvInfos matches;
        for (DrvInfos::const_iterator j = allElems.begin();
             j != allElems.end(); ++j)
        {
            DrvName drvName(j->name);
            if (i->matches(drvName)) {
                i->hits++;
                matches.push_back(*j);
            }
        }

        if (newestOnly) {

            /* Map from package names to derivations. */
            map<string, DrvInfo> newest;

            for (DrvInfos::const_iterator j = matches.begin();
                 j != matches.end(); ++j)
            {
                DrvName drvName(j->name);
                map<string, DrvInfo>::iterator k = newest.find(drvName.name);
                if (k != newest.end())
                    if (compareVersions(drvName.version, DrvName(k->second.name).version) > 0)
                        newest[drvName.name] = *j;
                    else
                        ;
                else
                    newest[drvName.name] = *j;
            }

            matches.clear();
            for (map<string, DrvInfo>::iterator j = newest.begin();
                 j != newest.end(); ++j)
                matches.push_back(j->second);
        }

        /* Insert only those elements in the final list that we
           haven't inserted before. */
        for (DrvInfos::const_iterator j = matches.begin();
             j != matches.end(); ++j)
            if (done.find(j->queryOutPath(state)) == done.end()) {
                done.insert(j->queryOutPath(state));
                elems.push_back(*j);
            }
    }
            
    /* Check that all selectors have been used. */
    for (DrvNames::iterator i = selectors.begin();
         i != selectors.end(); ++i)
        if (i->hits == 0)
            throw Error(format("selector `%1%' matches no derivations")
                % i->fullName);

    return elems;
}


static void queryInstSources(EvalState & state,
    const InstallSourceInfo & instSource, const Strings & args,
    DrvInfos & elems, bool newestOnly)
{
    InstallSourceType type = instSource.type;
    if (type == srcUnknown && args.size() > 0 && args.front()[0] == '/')
        type = srcStorePaths;
    
    switch (type) {

        /* Get the available user environment elements from the
           derivations specified in a Nix expression, including only
           those with names matching any of the names in `args'. */
        case srcUnknown:
        case srcNixExprDrvs: {

            /* Load the derivations from the (default or specified)
               Nix expression. */
            DrvInfos allElems;
            loadDerivations(state, instSource.nixExprPath,
                instSource.systemFilter, allElems);

            elems = filterBySelector(state, allElems, args, newestOnly);
    
            break;
        }

        /* Get the available user environment elements from the Nix
           expressions specified on the command line; these should be
           functions that take the default Nix expression file as
           argument, e.g., if the file is `./foo.nix', then the
           argument `x: x.bar' is equivalent to `(x: x.bar)
           (import ./foo.nix)' = `(import ./foo.nix).bar'. */
        case srcNixExprs: {
                

            Expr e1 = parseExprFromFile(state,
                absPath(instSource.nixExprPath));

            for (Strings::const_iterator i = args.begin();
                 i != args.end(); ++i)
            {
                Expr e2 = parseExprFromString(state, *i, absPath("."));
                Expr call = makeCall(e2, e1);
                getDerivations(state, call, elems);
            }
            
            break;
        }
            
        /* The available user environment elements are specified as a
           list of store paths (which may or may not be
           derivations). */
        case srcStorePaths: {

            for (Strings::const_iterator i = args.begin();
                 i != args.end(); ++i)
            {
                assertStorePath(*i);

                DrvInfo elem;
                string name = baseNameOf(*i);
                unsigned int dash = name.find('-');
                if (dash != string::npos)
                    name = string(name, dash + 1);

                if (isDerivation(*i)) {
                    elem.setDrvPath(*i);
                    elem.setOutPath(findOutput(derivationFromPath(*i), "out"));
                    if (name.size() >= drvExtension.size() &&
                        string(name, name.size() - drvExtension.size()) == drvExtension)
                        name = string(name, 0, name.size() - drvExtension.size());
                }
                else elem.setOutPath(*i);

                elem.name = name;

                elems.push_back(elem);
            }
            
            break;
        }
            
        /* Get the available user environment elements from another
           user environment.  These are then filtered as in the
           `srcNixExprDrvs' case. */
        case srcProfile: {
            elems = filterBySelector(state,
                queryInstalled(state, instSource.profile),
                args, newestOnly);
            break;
        }
    }
}


static void installDerivations(Globals & globals,
    const Strings & args, const Path & profile)
{
    debug(format("installing derivations"));

    /* Get the set of user environment elements to be installed. */
    DrvInfos newElems;
    queryInstSources(globals.state, globals.instSource, args, newElems, true);

    StringSet newNames;
    for (DrvInfos::iterator i = newElems.begin(); i != newElems.end(); ++i)
        newNames.insert(DrvName(i->name).name);

    /* Add in the already installed derivations, unless they have the
       same name as a to-be-installed element. */
    DrvInfos installedElems = queryInstalled(globals.state, profile);

    DrvInfos allElems(newElems);
    for (DrvInfos::iterator i = installedElems.begin();
         i != installedElems.end(); ++i)
    {
        DrvName drvName(i->name);
        if (!globals.preserveInstalled &&
            newNames.find(drvName.name) != newNames.end())
            printMsg(lvlInfo,
                format("replacing old `%1%'") % i->name);
        else
            allElems.push_back(*i);
    }

    for (DrvInfos::iterator i = newElems.begin(); i != newElems.end(); ++i)
        printMsg(lvlInfo,
            format("installing `%1%'") % i->name);
        
    if (globals.dryRun) return;

    createUserEnv(globals.state, allElems,
        profile, globals.keepDerivations);
}


static void opInstall(Globals & globals,
    Strings opFlags, Strings opArgs)
{
    if (opFlags.size() > 0)
        throw UsageError(format("unknown flags `%1%'") % opFlags.front());

    installDerivations(globals, opArgs, globals.profile);
}


typedef enum { utLt, utLeq, utAlways } UpgradeType;


static void upgradeDerivations(Globals & globals,
    const Strings & args, const Path & profile,
    UpgradeType upgradeType)
{
    debug(format("upgrading derivations"));

    /* Upgrade works as follows: we take all currently installed
       derivations, and for any derivation matching any selector, look
       for a derivation in the input Nix expression that has the same
       name and a higher version number. */

    /* Load the currently installed derivations. */
    DrvInfos installedElems = queryInstalled(globals.state, profile);

    /* Fetch all derivations from the input file. */
    DrvInfos availElems;
    queryInstSources(globals.state, globals.instSource, args, availElems, false);

    /* Go through all installed derivations. */
    DrvInfos newElems;
    for (DrvInfos::iterator i = installedElems.begin();
         i != installedElems.end(); ++i)
    {
        DrvName drvName(i->name);

        /* Find the derivation in the input Nix expression with the
           same name and satisfying the version constraints specified
           by upgradeType.  If there are multiple matches, take the
           one with highest version. */
        DrvInfos::iterator bestElem = availElems.end();
        DrvName bestName;
        for (DrvInfos::iterator j = availElems.begin();
             j != availElems.end(); ++j)
        {
            DrvName newName(j->name);
            if (newName.name == drvName.name) {
                int d = compareVersions(drvName.version, newName.version);
                if (upgradeType == utLt && d < 0 ||
                    upgradeType == utLeq && d <= 0 ||
                    upgradeType == utAlways)
                {
                    if ((bestElem == availElems.end() ||
                         compareVersions(
                             bestName.version, newName.version) < 0))
                    {
                        bestElem = j;
                        bestName = newName;
                    }
                }
            }
        }

        if (bestElem != availElems.end() &&
            i->queryOutPath(globals.state) !=
                bestElem->queryOutPath(globals.state))
        {
            printMsg(lvlInfo,
                format("upgrading `%1%' to `%2%'")
                % i->name % bestElem->name);
            newElems.push_back(*bestElem);
        } else newElems.push_back(*i);
    }
    
    if (globals.dryRun) return;

    createUserEnv(globals.state, newElems,
        profile, globals.keepDerivations);
}


static void opUpgrade(Globals & globals,
    Strings opFlags, Strings opArgs)
{
    UpgradeType upgradeType = utLt;
    for (Strings::iterator i = opFlags.begin();
         i != opFlags.end(); ++i)
        if (*i == "--lt") upgradeType = utLt;
        else if (*i == "--leq") upgradeType = utLeq;
        else if (*i == "--always") upgradeType = utAlways;
        else throw UsageError(format("unknown flag `%1%'") % *i);

    upgradeDerivations(globals, opArgs, globals.profile, upgradeType);
}


static void uninstallDerivations(Globals & globals, DrvNames & selectors,
    Path & profile)
{
    DrvInfos installedElems = queryInstalled(globals.state, profile);
    DrvInfos newElems;

    for (DrvInfos::iterator i = installedElems.begin();
         i != installedElems.end(); ++i)
    {
        DrvName drvName(i->name);
        bool found = false;
        for (DrvNames::iterator j = selectors.begin();
             j != selectors.end(); ++j)
            if (j->matches(drvName)) {
                printMsg(lvlInfo,
                    format("uninstalling `%1%'") % i->name);
                found = true;
                break;
            }
        if (!found) newElems.push_back(*i);
    }

    if (globals.dryRun) return;

    createUserEnv(globals.state, newElems,
        profile, globals.keepDerivations);
}


static void opUninstall(Globals & globals,
    Strings opFlags, Strings opArgs)
{
    if (opFlags.size() > 0)
        throw UsageError(format("unknown flags `%1%'") % opFlags.front());

    DrvNames drvNames = drvNamesFromArgs(opArgs);

    uninstallDerivations(globals, drvNames,
        globals.profile);
}


static bool cmpChars(char a, char b)
{
    return toupper(a) < toupper(b);
}


static bool cmpElemByName(const DrvInfo & a, const DrvInfo & b)
{
    return lexicographical_compare(
        a.name.begin(), a.name.end(),
        b.name.begin(), b.name.end(), cmpChars);
}


typedef list<Strings> Table;


void printTable(Table & table)
{
    unsigned int nrColumns = table.size() > 0 ? table.front().size() : 0;
    
    vector<unsigned int> widths;
    widths.resize(nrColumns);
    
    for (Table::iterator i = table.begin(); i != table.end(); ++i) {
        assert(i->size() == nrColumns);
        Strings::iterator j;
        unsigned int column;
        for (j = i->begin(), column = 0; j != i->end(); ++j, ++column)
            if (j->size() > widths[column]) widths[column] = j->size();
    }

    for (Table::iterator i = table.begin(); i != table.end(); ++i) { 
        Strings::iterator j;
        unsigned int column;
        for (j = i->begin(), column = 0; j != i->end(); ++j, ++column)
        {
            cout << *j;
            if (column < nrColumns - 1)
                cout << string(widths[column] - j->size() + 2, ' ');
        }
        cout << endl;
    }
}


/* This function compares the version of a element against the
   versions in the given set of elements.  `cvLess' means that only
   lower versions are in the set, `cvEqual' means that at most an
   equal version is in the set, and `cvGreater' means that there is at
   least one element with a higher version in the set.  `cvUnavail'
   means that there are no elements with the same name in the set. */

typedef enum { cvLess, cvEqual, cvGreater, cvUnavail } VersionDiff;

static VersionDiff compareVersionAgainstSet(
    const DrvInfo & elem, const DrvInfos & elems, string & version)
{
    DrvName name(elem.name);
    
    VersionDiff diff = cvUnavail;
    version = "?";
    
    for (DrvInfos::const_iterator i = elems.begin(); i != elems.end(); ++i) {
        DrvName name2(i->name);
        if (name.name == name2.name) {
            int d = compareVersions(name.version, name2.version);
            if (d < 0) {
                diff = cvGreater;
                version = name2.version;
            }
            else if (diff != cvGreater && d == 0) {
                diff = cvEqual;
                version = name2.version;
            }
            else if (diff != cvGreater && diff != cvEqual && d > 0) {
                diff = cvLess;
                if (version == "" || compareVersions(version, name2.version) < 0)
                    version = name2.version;
            }
        }
    }

    return diff;
}


static string colorString(const string & s)
{
    if (!isatty(STDOUT_FILENO)) return s;
    return "\e[1;31m" + s + "\e[0m";
}


static void opQuery(Globals & globals,
    Strings opFlags, Strings opArgs)
{
    bool printStatus = false;
    bool printName = true;
    bool printSystem = false;
    bool printDrvPath = false;
    bool printOutPath = false;
    bool compareVersions = false;

    enum { sInstalled, sAvailable } source = sInstalled;

    readOnlyMode = true; /* makes evaluation a bit faster */

    for (Strings::iterator i = opFlags.begin();
         i != opFlags.end(); ++i)
        if (*i == "--status" || *i == "-s") printStatus = true;
        else if (*i == "--no-name") printName = false;
        else if (*i == "--system") printSystem = true;
        else if (*i == "--compare-versions" || *i == "-c") compareVersions = true;
        else if (*i == "--drv-path") printDrvPath = true;
        else if (*i == "--out-path") printOutPath = true;
        else if (*i == "--installed") source = sInstalled;
        else if (*i == "--available" || *i == "-a") source = sAvailable;
        else throw UsageError(format("unknown flag `%1%'") % *i);

    if (opArgs.size() != 0) throw UsageError("no arguments expected");

    
    /* Obtain derivation information from the specified source. */
    DrvInfos availElems, installedElems;

    if (source == sInstalled || compareVersions || printStatus) {
        installedElems = queryInstalled(globals.state, globals.profile);
    }

    if (source == sAvailable || compareVersions) {
        loadDerivations(globals.state, globals.instSource.nixExprPath,
            globals.instSource.systemFilter, availElems);
    }

    DrvInfos & elems(source == sInstalled ? installedElems : availElems);
    DrvInfos & otherElems(source == sInstalled ? availElems : installedElems);

    
    /* Sort them by name. */
    /* !!! */
    vector<DrvInfo> elems2;
    for (DrvInfos::iterator i = elems.begin(); i != elems.end(); ++i)
        elems2.push_back(*i);
    sort(elems2.begin(), elems2.end(), cmpElemByName);

    
    /* We only need to know the installed paths when we are querying
       the status of the derivation. */
    PathSet installed; /* installed paths */
    
    if (printStatus) {
        for (DrvInfos::iterator i = installedElems.begin();
             i != installedElems.end(); ++i)
            installed.insert(i->queryOutPath(globals.state));
    }

    
    /* Print the desired columns. */
    Table table;
    
    for (vector<DrvInfo>::iterator i = elems2.begin();
         i != elems2.end(); ++i)
    {
        Strings columns;
        
        if (printStatus) {
            Substitutes subs = querySubstitutes(noTxn, i->queryOutPath(globals.state));
            columns.push_back(
                (string) (installed.find(i->queryOutPath(globals.state))
                    != installed.end() ? "I" : "-")
                + (isValidPath(i->queryOutPath(globals.state)) ? "P" : "-")
                + (subs.size() > 0 ? "S" : "-"));
        }

        if (printName) columns.push_back(i->name);

        if (compareVersions) {
            /* Compare this element against the versions of the same
               named packages in either the set of available elements,
               or the set of installed elements.  !!! This is O(N *
               M), should be O(N * lg M). */
            string version;
            VersionDiff diff = compareVersionAgainstSet(*i, otherElems, version);
            char ch;
            switch (diff) {
                case cvLess: ch = '>'; break;
                case cvEqual: ch = '='; break;
                case cvGreater: ch = '<'; break;
                case cvUnavail: ch = '-'; break;
                default: abort();
            }
            string column = (string) "" + ch + " " + version;
            if (diff == cvGreater) column = colorString(column);
            columns.push_back(column);
        }

        if (printSystem) columns.push_back(i->system);

        if (printDrvPath) columns.push_back(
            i->queryDrvPath(globals.state) == ""
            ? "-" : i->queryDrvPath(globals.state));
        
        if (printOutPath) columns.push_back(i->queryOutPath(globals.state));
        table.push_back(columns);
    }

    printTable(table);
}


static void opSwitchProfile(Globals & globals,
    Strings opFlags, Strings opArgs)
{
    if (opFlags.size() > 0)
        throw UsageError(format("unknown flag `%1%'") % opFlags.front());
    if (opArgs.size() != 1)
        throw UsageError(format("exactly one argument expected"));

    Path profile = opArgs.front();
    Path profileLink = getHomeDir() + "/.nix-profile";

    SwitchToOriginalUser sw;
    switchLink(profileLink, profile);
}


static const int prevGen = -2;


static void switchGeneration(Globals & globals, int dstGen)
{
    int curGen;
    Generations gens = findGenerations(globals.profile, curGen);

    Generation dst;
    for (Generations::iterator i = gens.begin(); i != gens.end(); ++i)
        if ((dstGen == prevGen && i->number < curGen) ||
            (dstGen >= 0 && i->number == dstGen))
            dst = *i;

    if (!dst)
        if (dstGen == prevGen)
            throw Error(format("no generation older than the current (%1%) exists")
                % curGen);
        else
            throw Error(format("generation %1% does not exist") % dstGen);

    printMsg(lvlInfo, format("switching from generation %1% to %2%")
        % curGen % dst.number);
    
    if (globals.dryRun) return;
    
    switchLink(globals.profile, dst.path);
}


static void opSwitchGeneration(Globals & globals,
    Strings opFlags, Strings opArgs)
{
    if (opFlags.size() > 0)
        throw UsageError(format("unknown flag `%1%'") % opFlags.front());
    if (opArgs.size() != 1)
        throw UsageError(format("exactly one argument expected"));

    int dstGen;
    if (!string2Int(opArgs.front(), dstGen))
        throw UsageError(format("expected a generation number"));

    switchGeneration(globals, dstGen);
}


static void opRollback(Globals & globals,
    Strings opFlags, Strings opArgs)
{
    if (opFlags.size() > 0)
        throw UsageError(format("unknown flag `%1%'") % opFlags.front());
    if (opArgs.size() != 0)
        throw UsageError(format("no arguments expected"));

    switchGeneration(globals, prevGen);
}


static void opListGenerations(Globals & globals,
    Strings opFlags, Strings opArgs)
{
    if (opFlags.size() > 0)
        throw UsageError(format("unknown flag `%1%'") % opFlags.front());
    if (opArgs.size() != 0)
        throw UsageError(format("no arguments expected"));

    int curGen;
    Generations gens = findGenerations(globals.profile, curGen);

    for (Generations::iterator i = gens.begin(); i != gens.end(); ++i) {
        tm t;
        if (!localtime_r(&i->creationTime, &t)) throw Error("cannot convert time");
        cout << format("%|4|   %|4|-%|02|-%|02| %|02|:%|02|:%|02|   %||\n")
            % i->number
            % (t.tm_year + 1900) % (t.tm_mon + 1) % t.tm_mday
            % t.tm_hour % t.tm_min % t.tm_sec
            % (i->number == curGen ? "(current)" : "");
    }
}


static void deleteGeneration2(const Path & profile, unsigned int gen)
{
    printMsg(lvlInfo, format("removing generation %1%") % gen);
    deleteGeneration(profile, gen);
}


static void opDeleteGenerations(Globals & globals,
    Strings opFlags, Strings opArgs)
{
    if (opFlags.size() > 0)
        throw UsageError(format("unknown flag `%1%'") % opFlags.front());

    int curGen;
    Generations gens = findGenerations(globals.profile, curGen);

    for (Strings::iterator i = opArgs.begin(); i != opArgs.end(); ++i) {

        if (*i == "old") {
            for (Generations::iterator j = gens.begin(); j != gens.end(); ++j)
                if (j->number != curGen)
                    deleteGeneration2(globals.profile, j->number);
        }

        else {
            int n;
            if (!string2Int(*i, n) || n < 0)
                throw UsageError(format("invalid generation specifier `%1%'")  % *i);
            bool found = false;
            for (Generations::iterator j = gens.begin(); j != gens.end(); ++j) {
                if (j->number == n) {
                    deleteGeneration2(globals.profile, j->number);
                    found = true;
                    break;
                }
            }
            if (!found)
                printMsg(lvlError, format("generation %1% does not exist") % n);
        }
    }
}


static void opDefaultExpr(Globals & globals,
    Strings opFlags, Strings opArgs)
{
    if (opFlags.size() > 0)
        throw UsageError(format("unknown flags `%1%'") % opFlags.front());
    if (opArgs.size() != 1)
        throw UsageError(format("exactly one argument expected"));

    Path defNixExpr = absPath(opArgs.front());
    Path defNixExprLink = getDefNixExprPath();
    
    SwitchToOriginalUser sw;
    switchLink(defNixExprLink, defNixExpr);
}


static string needArg(Strings::iterator & i,
    Strings & args, const string & arg)
{
    ++i;
    if (i == args.end()) throw UsageError(
        format("`%1%' requires an argument") % arg);
    return *i;
}


void run(Strings args)
{
    Strings opFlags, opArgs;
    Operation op = 0;
    
    Globals globals;
    
    globals.instSource.type = srcUnknown;
    globals.instSource.nixExprPath = getDefNixExprPath();
    globals.instSource.systemFilter = thisSystem;
    
    globals.dryRun = false;
    globals.preserveInstalled = false;

    globals.keepDerivations =
        queryBoolSetting("env-keep-derivations", false);
    
    for (Strings::iterator i = args.begin(); i != args.end(); ++i) {
        string arg = *i;

        Operation oldOp = op;

        if (arg == "--install" || arg == "-i")
            op = opInstall;
        else if (arg == "--from-expression" || arg == "-E")
            globals.instSource.type = srcNixExprs;
        else if (arg == "--from-profile") {
            globals.instSource.type = srcProfile;
            globals.instSource.profile = needArg(i, args, arg);
        }
        else if (arg == "--uninstall" || arg == "-e")
            op = opUninstall;
        else if (arg == "--upgrade" || arg == "-u")
            op = opUpgrade;
        else if (arg == "--query" || arg == "-q")
            op = opQuery;
        else if (arg == "--import" || arg == "-I") /* !!! bad name */
            op = opDefaultExpr;
        else if (arg == "--profile" || arg == "-p") {
            globals.profile = absPath(needArg(i, args, arg));
        }
        else if (arg == "--file" || arg == "-f") {
            globals.instSource.nixExprPath = absPath(needArg(i, args, arg));
        }
        else if (arg == "--switch-profile" || arg == "-S")
            op = opSwitchProfile;
        else if (arg == "--switch-generation" || arg == "-G")
            op = opSwitchGeneration;
        else if (arg == "--rollback")
            op = opRollback;
        else if (arg == "--list-generations")
            op = opListGenerations;
        else if (arg == "--delete-generations")
            op = opDeleteGenerations;
        else if (arg == "--dry-run") {
            printMsg(lvlInfo, "(dry run; not doing anything)");
            globals.dryRun = true;
        }
        else if (arg == "--preserve-installed" || arg == "-P")
            globals.preserveInstalled = true;
        else if (arg == "--system-filter") {
            globals.instSource.systemFilter = needArg(i, args, arg);
        }
        else if (arg[0] == '-')
            opFlags.push_back(arg);
        else
            opArgs.push_back(arg);

        if (oldOp && oldOp != op)
            throw UsageError("only one operation may be specified");
    }

    if (!op) throw UsageError("no operation specified");

    if (globals.profile == "") {
        SwitchToOriginalUser sw;
        Path profileLink = getHomeDir() + "/.nix-profile";
        globals.profile = pathExists(profileLink)
            ? absPath(readLink(profileLink), dirOf(profileLink))
            : canonPath(nixStateDir + "/profiles/default");
    }
    
    openDB();

    op(globals, opFlags, opArgs);

    printEvalStats(globals.state);
}


string programId = "nix-env";
