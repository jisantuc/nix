#include <iostream>

#include "globals.hh"
#include "normalise.hh"
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


/* Build the given derivations. */
static void opBuild(Strings opFlags, Strings opArgs)
{
    if (!opFlags.empty()) throw UsageError("unknown flag");

    for (Strings::iterator i = opArgs.begin();
         i != opArgs.end(); i++)
        buildDerivation(*i);
}


/* Add files to the Nix values directory and print the resulting
   paths. */
static void opAdd(Strings opFlags, Strings opArgs)
{
    if (!opFlags.empty()) throw UsageError("unknown flag");

    for (Strings::iterator i = opArgs.begin(); i != opArgs.end(); i++)
        cout << format("%1%\n") % addToStore(*i);
}


#if 0
Path maybeNormalise(const Path & ne, bool normalise, bool realise)
{
    if (realise) {
        Path ne2 = realiseStoreExpr(ne);
        return normalise ? ne2 : ne;
    } else
        return normalise ? normaliseStoreExpr(ne) : ne;
}


/* Perform various sorts of queries. */
static void opQuery(Strings opFlags, Strings opArgs)
{
    enum { qList, qRequisites, qPredecessors, qGraph 
    } query = qList;
    bool normalise = false;
    bool realise = false;
    bool includeExprs = true;
    bool includeSuccessors = false;

    for (Strings::iterator i = opFlags.begin();
         i != opFlags.end(); i++)
        if (*i == "--list" || *i == "-l") query = qList;
        else if (*i == "--requisites" || *i == "-R") query = qRequisites;
        else if (*i == "--predecessors") query = qPredecessors;
        else if (*i == "--graph") query = qGraph;
        else if (*i == "--normalise" || *i == "-n") normalise = true;
        else if (*i == "--force-realise" || *i == "-f") realise = true;
        else if (*i == "--exclude-exprs") includeExprs = false;
        else if (*i == "--include-successors") includeSuccessors = true;
        else throw UsageError(format("unknown flag `%1%'") % *i);

    switch (query) {
        
        case qList: {
            for (Strings::iterator i = opArgs.begin();
                 i != opArgs.end(); i++)
            {
                StringSet paths = storeExprRoots(
                    maybeNormalise(*i, normalise, realise));
                for (StringSet::iterator j = paths.begin(); 
                     j != paths.end(); j++)
                    cout << format("%s\n") % *j;
            }
            break;
        }

        case qRequisites: {
            StringSet paths;
            for (Strings::iterator i = opArgs.begin();
                 i != opArgs.end(); i++)
            {
                StringSet paths2 = storeExprRequisites(
                    maybeNormalise(*i, normalise, realise),
                    includeExprs, includeSuccessors);
                paths.insert(paths2.begin(), paths2.end());
            }
            for (StringSet::iterator i = paths.begin(); 
                 i != paths.end(); i++)
                cout << format("%s\n") % *i;
            break;
        }

        case qPredecessors: {
            for (Strings::iterator i = opArgs.begin();
                 i != opArgs.end(); i++)
            {
                Paths preds = queryPredecessors(*i);
                for (Paths::iterator j = preds.begin();
                     j != preds.end(); j++)
                    cout << format("%s\n") % *j;
            }
            break;
        }

        case qGraph: {
            PathSet roots;
            for (Strings::iterator i = opArgs.begin();
                 i != opArgs.end(); i++)
                roots.insert(maybeNormalise(*i, normalise, realise));
	    printDotGraph(roots);
            break;
        }

        default:
            abort();
    }
}
#endif


static void opSubstitute(Strings opFlags, Strings opArgs)
{
    if (!opFlags.empty()) throw UsageError("unknown flag");
    if (!opArgs.empty())
        throw UsageError("no arguments expected");

    SubstitutePairs subPairs;
    Transaction txn;
    createStoreTransaction(txn);

    while (1) {
        Path srcPath;
        Substitute sub;
        getline(cin, srcPath);
        if (cin.eof()) break;
        getline(cin, sub.program);
        string s;
        getline(cin, s);
        int n;
        if (!string2Int(s, n)) throw Error("number expected");
        while (n--) {
            getline(cin, s);
            sub.args.push_back(s);
        }
        if (!cin || cin.eof()) throw Error("missing input");
        subPairs.push_back(pair<Path, Substitute>(srcPath, sub));
    }

    registerSubstitutes(txn, subPairs);
    
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
        registerValidPath(txn, *i);
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
#if 0
    /* Do what? */
    enum { soPrintLive, soPrintDead, soDelete } subOp;
    time_t minAge = 0;
    for (Strings::iterator i = opFlags.begin();
         i != opFlags.end(); ++i)
        if (*i == "--print-live") subOp = soPrintLive;
        else if (*i == "--print-dead") subOp = soPrintDead;
        else if (*i == "--delete") subOp = soDelete;
        else if (*i == "--min-age") {
            int n;
            if (opArgs.size() == 0 || !string2Int(opArgs.front(), n))
                throw UsageError("`--min-age' requires an integer argument");
            minAge = n;
        }
        else throw UsageError(format("bad sub-operation `%1%' in GC") % *i);
        
    Paths roots;
    while (1) {
        Path root;
        getline(cin, root);
        if (cin.eof()) break;
        roots.push_back(root);
    }

    PathSet live = findLivePaths(roots);

    if (subOp == soPrintLive) {
        for (PathSet::iterator i = live.begin(); i != live.end(); ++i)
            cout << *i << endl;
        return;
    }

    PathSet dead = findDeadPaths(live, minAge * 3600);

    if (subOp == soPrintDead) {
        for (PathSet::iterator i = dead.begin(); i != dead.end(); ++i)
            cout << *i << endl;
        return;
    }

    if (subOp == soDelete) {

        /* !!! What happens if the garbage collector run is aborted
           halfway through?  In particular, dead paths can always
           become live again (through re-instantiation), and might
           then refer to deleted paths. => check instantiation
           invariants */

        for (PathSet::iterator i = dead.begin(); i != dead.end(); ++i) {
            printMsg(lvlInfo, format("deleting `%1%'") % *i);
            deleteFromStore(*i);
        }
    }
#endif
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

    for (Strings::iterator i = args.begin(); i != args.end(); ++i) {
        string arg = *i;

        Operation oldOp = op;

        if (arg == "--build" || arg == "-b")
            op = opBuild;
        else if (arg == "--add" || arg == "-A")
            op = opAdd;
        //        else if (arg == "--query" || arg == "-q")
        //            op = opQuery;
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
