#include <iostream>
#include <cctype>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <aterm2.h>
}

#include "globals.hh"
#include "shared.hh"

#include "config.h"


void sigintHandler(int signo)
{
    _isInterrupted = 1;
}


void setLogType(string lt)
{
    if (lt == "pretty") logType = ltPretty;
    else if (lt == "escapes") logType = ltEscapes;
    else if (lt == "flat") logType = ltFlat;
    else throw UsageError("unknown log type");
}


void checkStoreNotSymlink(Path path)
{
    struct stat st;
    while (path.size()) {
        if (lstat(path.c_str(), &st))
            throw SysError(format("getting status of `%1%'") % path);
        if (S_ISLNK(st.st_mode))
            throw Error(format(
                "the path `%1%' is a symlink; "
                "this is not allowed for the Nix store and its parent directories")
                % path);
        path = dirOf(path);
    }
}


/* Initialize and reorder arguments, then call the actual argument
   processor. */
static void initAndRun(int argc, char * * argv)
{
    char * root = getenv("NIX_ROOT");

    if (root) {
        if (chroot(root) != 0)
            throw SysError(format("changing root to `%1%'") % root);
    }
    
    /* Setup Nix paths. */
    nixStore = canonPath(NIX_STORE_DIR);
    nixDataDir = canonPath(NIX_DATA_DIR);
    nixLogDir = canonPath(NIX_LOG_DIR);
    nixStateDir = canonPath(NIX_STATE_DIR);
    nixDBPath = canonPath(NIX_STATE_DIR) + "/db";

    /* Check that the store directory and its parent are not
       symlinks. */
    checkStoreNotSymlink(nixStore);

    /* Catch SIGINT. */
    struct sigaction act, oact;
    act.sa_handler = sigintHandler;
    sigfillset(&act.sa_mask);
    act.sa_flags = 0;
    if (sigaction(SIGINT, &act, &oact))
        throw SysError("installing handler for SIGINT");

    /* Process the NIX_LOG_TYPE environment variable. */
    char * lt = getenv("NIX_LOG_TYPE");
    if (lt) setLogType(lt);

    /* Put the arguments in a vector. */
    Strings args, remaining;
    while (argc--) args.push_back(*argv++);
    args.erase(args.begin());
    
    /* Expand compound dash options (i.e., `-qlf' -> `-q -l -f'), and
       ignore options for the ATerm library. */
    for (Strings::iterator i = args.begin(); i != args.end(); ++i) {
        string arg = *i;
        if (string(arg, 0, 4) == "-at-") ;
        else if (arg.length() > 2 && arg[0] == '-' && arg[1] != '-') {
            for (unsigned int j = 1; j < arg.length(); j++)
                if (isalpha(arg[j]))
                    remaining.push_back((string) "-" + arg[j]);
                else {
                    remaining.push_back(string(arg, j));
                    break;
                }
        } else remaining.push_back(arg);
    }
    args = remaining;
    remaining.clear();

    /* Process default options. */
    for (Strings::iterator i = args.begin(); i != args.end(); ++i) {
        string arg = *i;
        if (arg == "--verbose" || arg == "-v")
            verbosity = (Verbosity) ((int) verbosity + 1);
        else if (arg == "--log-type") {
            ++i;
            if (i == args.end()) throw UsageError("`--log-type' requires an argument");
            setLogType(*i);
        } else if (arg == "--build-output" || arg == "-B")
            buildVerbosity = lvlError; /* lowest */
        else if (arg == "--help") {
            printHelp();
            return;
        } else if (arg == "--version") {
            cout << format("%1% (Nix) %2%") % programId % NIX_VERSION << endl;
            return;
        } else if (arg == "--keep-failed" || arg == "-K")
            keepFailed = true;
        else remaining.push_back(arg);
    }

    run(remaining);
}


static char buf[1024];

int main(int argc, char * * argv)
{
    /* ATerm setup. */
    ATerm bottomOfStack;
    ATinit(argc, argv, &bottomOfStack);

    /* Turn on buffering for cerr. */
#if HAVE_PUBSETBUF
    cerr.rdbuf()->pubsetbuf(buf, sizeof(buf));
#endif

    try {
        initAndRun(argc, argv);
    } catch (UsageError & e) {
        printMsg(lvlError, 
            format(
                "error: %1%\n"
                "Try `%2% --help' for more information.")
            % e.what() % programId);
        return 1;
    } catch (Error & e) {
        printMsg(lvlError, format("error: %1%") % e.msg());
        return 1;
    } catch (exception & e) {
        printMsg(lvlError, format("error: %1%") % e.what());
        return 1;
    }

    return 0;
}
