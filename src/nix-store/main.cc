#include <iostream>

#include "globals.hh"
#include "build.hh"
#include "gc.hh"
#include "archive.hh"
#include "shared.hh"
#include "dotgraph.hh"
#include "help.txt.hh"


typedef void (* Operation) (Strings opFlags, Strings opArgs);


void printHelp()
{
    cout << string((char *) helpText, sizeof helpText);
}


static Path gcRoot;
static int rootNr = 0;
static bool indirectRoot = false;


static Path findOutput(const Derivation & drv, string id)
{
    for (DerivationOutputs::const_iterator i = drv.outputs.begin();
         i != drv.outputs.end(); ++i)
        if (i->first == id) return i->second.path;
    throw Error(format("derivation has no output `%1%'") % id);
}


static Path followSymlinks(Path & path)
{
    path = absPath(path);
    while (!isStorePath(path)) {
        if (!isLink(path)) return path;
        string target = readLink(path);
        path = absPath(target, dirOf(path));
    }
    return path;
}


/* Realisation the given path.  For a derivation that means build it;
   for other paths it means ensure their validity. */
static Path realisePath(const Path & path)
{
    if (isDerivation(path)) {
        PathSet paths;
        paths.insert(path);
        buildDerivations(paths);
        Path outPath = findOutput(derivationFromPath(path), "out");
        
        if (gcRoot == "")
            printGCWarning();
        else
            outPath = addPermRoot(outPath,
                makeRootName(gcRoot, rootNr),
                indirectRoot);
        
        return outPath;
    } else {
        ensurePath(path);
        return path;
    }
}


/* Realise the given paths. */
static void opRealise(Strings opFlags, Strings opArgs)
{
    if (!opFlags.empty()) throw UsageError("unknown flag");

    for (Strings::iterator i = opArgs.begin();
         i != opArgs.end(); i++)
        *i = followSymlinks(*i);
            
    if (opArgs.size() > 1) {
        PathSet drvPaths;
        for (Strings::iterator i = opArgs.begin();
             i != opArgs.end(); i++)
            if (isDerivation(*i))
                drvPaths.insert(*i);
        buildDerivations(drvPaths);
    }

    for (Strings::iterator i = opArgs.begin();
         i != opArgs.end(); i++)
        cout << format("%1%\n") % realisePath(*i);
}


/* Add files to the Nix values directory and print the resulting
   paths. */
static void opAdd(Strings opFlags, Strings opArgs)
{
    if (!opFlags.empty()) throw UsageError("unknown flag");

    for (Strings::iterator i = opArgs.begin(); i != opArgs.end(); i++)
        cout << format("%1%\n") % addToStore(*i);
}


/* Place in `paths' the set of paths that are required to `realise'
   the given store path, i.e., all paths necessary for valid
   deployment of the path.  For a derivation, this is the union of
   requisites of the inputs, plus the derivation; for other store
   paths, it is the set of paths in the FS closure of the path.  If
   `includeOutputs' is true, include the requisites of the output
   paths of derivations as well.

   Note that this function can be used to implement three different
   deployment policies:

   - Source deployment (when called on a derivation).
   - Binary deployment (when called on an output path).
   - Source/binary deployment (when called on a derivation with
     `includeOutputs' set to true).
*/
static void storePathRequisites(const Path & storePath,
    bool includeOutputs, PathSet & paths)
{
    computeFSClosure(storePath, paths);

    if (includeOutputs) {
        for (PathSet::iterator i = paths.begin();
             i != paths.end(); ++i)
            if (isDerivation(*i)) {
                Derivation drv = derivationFromPath(*i);
                for (DerivationOutputs::iterator j = drv.outputs.begin();
                     j != drv.outputs.end(); ++j)
                    if (isValidPath(j->second.path))
                        computeFSClosure(j->second.path, paths);
            }
    }
}


static Path maybeUseOutput(const Path & storePath, bool useOutput, bool forceRealise)
{
    if (forceRealise) realisePath(storePath);
    if (useOutput && isDerivation(storePath)) {
        Derivation drv = derivationFromPath(storePath);
        return findOutput(drv, "out");
    }
    else return storePath;
}


static void printPathSet(const PathSet & paths)
{
    for (PathSet::iterator i = paths.begin(); 
         i != paths.end(); i++)
        cout << format("%s\n") % *i;
}


/* Perform various sorts of queries. */
static void opQuery(Strings opFlags, Strings opArgs)
{
    enum { qOutputs, qRequisites, qReferences, qReferers,
           qReferersClosure, qGraph } query = qOutputs;
    bool useOutput = false;
    bool includeOutputs = false;
    bool forceRealise = false;

    for (Strings::iterator i = opFlags.begin();
         i != opFlags.end(); i++)
        if (*i == "--outputs") query = qOutputs;
        else if (*i == "--requisites" || *i == "-R") query = qRequisites;
        else if (*i == "--references") query = qReferences;
        else if (*i == "--referers") query = qReferers;
        else if (*i == "--referers-closure") query = qReferersClosure;
        else if (*i == "--graph") query = qGraph;
        else if (*i == "--use-output" || *i == "-u") useOutput = true;
        else if (*i == "--force-realise" || *i == "-f") forceRealise = true;
        else if (*i == "--include-outputs") includeOutputs = true;
        else throw UsageError(format("unknown flag `%1%'") % *i);

    switch (query) {
        
        case qOutputs: {
            for (Strings::iterator i = opArgs.begin();
                 i != opArgs.end(); i++)
            {
                *i = followSymlinks(*i);
                if (forceRealise) realisePath(*i);
                Derivation drv = derivationFromPath(*i);
                cout << format("%1%\n") % findOutput(drv, "out");
            }
            break;
        }

        case qRequisites:
        case qReferences:
        case qReferers:
        case qReferersClosure: {
            PathSet paths;
            for (Strings::iterator i = opArgs.begin();
                 i != opArgs.end(); i++)
            {
                *i = followSymlinks(*i);
                Path path = maybeUseOutput(*i, useOutput, forceRealise);
                if (query == qRequisites)
                    storePathRequisites(path, includeOutputs, paths);
                else if (query == qReferences) queryReferences(path, paths);
                else if (query == qReferers) queryReferers(path,  paths);
                else if (query == qReferersClosure) computeFSClosure(path, paths, true);
            }
            printPathSet(paths);
            break;
        }

#if 0            
        case qGraph: {
            PathSet roots;
            for (Strings::iterator i = opArgs.begin();
                 i != opArgs.end(); i++)
                roots.insert(maybeNormalise(*i, normalise, realise));
	    printDotGraph(roots);
            break;
        }
#endif

        default:
            abort();
    }
}


static void opSubstitute(Strings opFlags, Strings opArgs)
{
    if (!opFlags.empty()) throw UsageError("unknown flag");
    if (!opArgs.empty())
        throw UsageError("no arguments expected");

    Transaction txn;
    createStoreTransaction(txn);

    while (1) {
        Path srcPath;
        Substitute sub;
        PathSet references;
        getline(cin, srcPath);
        if (cin.eof()) break;
        getline(cin, sub.program);
        string s;
        int n;
        getline(cin, s);
        if (!string2Int(s, n)) throw Error("number expected");
        while (n--) {
            getline(cin, s);
            sub.args.push_back(s);
        }
        getline(cin, s);
        if (!string2Int(s, n)) throw Error("number expected");
        while (n--) {
            getline(cin, s);
            references.insert(s);
        }
        if (!cin || cin.eof()) throw Error("missing input");
        registerSubstitute(txn, srcPath, sub);
        setReferences(txn, srcPath, references);
    }

    txn.commit();
}


static void opClearSubstitutes(Strings opFlags, Strings opArgs)
{
    if (!opFlags.empty()) throw UsageError("unknown flag");
    if (!opArgs.empty())
        throw UsageError("no arguments expected");

    clearSubstitutes();
}


static void opValidPath(Strings opFlags, Strings opArgs)
{
    if (!opFlags.empty()) throw UsageError("unknown flag");
    
    Transaction txn;
    createStoreTransaction(txn);
    for (Strings::iterator i = opArgs.begin();
         i != opArgs.end(); ++i)
        registerValidPath(txn, *i, hashPath(htSHA256, *i), PathSet());
    txn.commit();
}


static void opIsValid(Strings opFlags, Strings opArgs)
{
    if (!opFlags.empty()) throw UsageError("unknown flag");

    for (Strings::iterator i = opArgs.begin();
         i != opArgs.end(); ++i)
        if (!isValidPath(*i))
            throw Error(format("path `%1%' is not valid") % *i);
}


static void opGC(Strings opFlags, Strings opArgs)
{
    GCAction action = gcDeleteDead;
    
    /* Do what? */
    for (Strings::iterator i = opFlags.begin();
         i != opFlags.end(); ++i)
        if (*i == "--print-roots") action = gcReturnRoots;
        else if (*i == "--print-live") action = gcReturnLive;
        else if (*i == "--print-dead") action = gcReturnDead;
        else if (*i == "--delete") action = gcDeleteDead;
        else throw UsageError(format("bad sub-operation `%1%' in GC") % *i);

    PathSet result;
    collectGarbage(action, result);

    if (action != gcDeleteDead) {
        for (PathSet::iterator i = result.begin(); i != result.end(); ++i)
            cout << *i << endl;
    }
}


/* A sink that writes dump output to stdout. */
struct StdoutSink : DumpSink
{
    virtual void operator ()
        (const unsigned char * data, unsigned int len)
    {
        writeFull(STDOUT_FILENO, data, len);
    }
};


/* Dump a path as a Nix archive.  The archive is written to standard
   output. */
static void opDump(Strings opFlags, Strings opArgs)
{
    if (!opFlags.empty()) throw UsageError("unknown flag");
    if (opArgs.size() != 1) throw UsageError("only one argument allowed");

    StdoutSink sink;
    string path = *opArgs.begin();
    dumpPath(path, sink);
}


/* A source that read restore intput to stdin. */
struct StdinSource : RestoreSource
{
    virtual void operator () (unsigned char * data, unsigned int len)
    {
        readFull(STDIN_FILENO, data, len);
    }
};


/* Restore a value from a Nix archive.  The archive is written to
   standard input. */
static void opRestore(Strings opFlags, Strings opArgs)
{
    if (!opFlags.empty()) throw UsageError("unknown flag");
    if (opArgs.size() != 1) throw UsageError("only one argument allowed");

    StdinSource source;
    restorePath(*opArgs.begin(), source);
}


/* Initialise the Nix databases. */
static void opInit(Strings opFlags, Strings opArgs)
{
    if (!opFlags.empty()) throw UsageError("unknown flag");
    if (!opArgs.empty())
        throw UsageError("no arguments expected");
    initDB();
}


/* Verify the consistency of the Nix environment. */
static void opVerify(Strings opFlags, Strings opArgs)
{
    verifyStore();
}


/* Scan the arguments; find the operation, set global flags, put all
   other flags in a list, and put all other arguments in another
   list. */
void run(Strings args)
{
    Strings opFlags, opArgs;
    Operation op = 0;

    for (Strings::iterator i = args.begin(); i != args.end(); ) {
        string arg = *i++;

        Operation oldOp = op;

        if (arg == "--realise" || arg == "-r")
            op = opRealise;
        else if (arg == "--add" || arg == "-A")
            op = opAdd;
        else if (arg == "--query" || arg == "-q")
            op = opQuery;
        else if (arg == "--substitute")
            op = opSubstitute;
        else if (arg == "--clear-substitutes")
            op = opClearSubstitutes;
        else if (arg == "--validpath")
            op = opValidPath;
        else if (arg == "--isvalid")
            op = opIsValid;
        else if (arg == "--gc")
            op = opGC;
        else if (arg == "--dump")
            op = opDump;
        else if (arg == "--restore")
            op = opRestore;
        else if (arg == "--init")
            op = opInit;
        else if (arg == "--verify")
            op = opVerify;
        else if (arg == "--add-root") {
            if (i == args.end())
                throw UsageError("`--add-root requires an argument");
            gcRoot = absPath(*i++);
        }
        else if (arg == "--indirect")
            indirectRoot = true;
        else if (arg[0] == '-')
            opFlags.push_back(arg);
        else
            opArgs.push_back(arg);

        if (oldOp && oldOp != op)
            throw UsageError("only one operation may be specified");
    }

    if (!op) throw UsageError("no operation specified");

    if (op != opDump && op != opRestore) /* !!! hack */
        openDB();

    op(opFlags, opArgs);
}


string programId = "nix-store";
