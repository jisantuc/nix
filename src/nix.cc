#include <iostream>
#include <list>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <cstdio>

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "util.hh"
#include "hash.hh"
#include "db.hh"
#include "nix.hh"
#include "eval.hh"

using namespace std;


void readPkgDescr(Hash hash,
    Params & pkgImports, Params & fileImports, Params & arguments)
{
    string pkgfile;

    pkgfile = getFile(hash);

    ATerm term = ATreadFromNamedFile(pkgfile.c_str());
    if (!term) throw Error("cannot read aterm " + pkgfile);

    ATerm bindings;
    if (!ATmatch(term, "Descr(<term>)", &bindings))
        throw Error("invalid term in " + pkgfile);

    char * cname;
    ATerm value;
    while (ATmatch(bindings, "[Bind(<str>, <term>), <list>]", 
               &cname, &value, &bindings))
    {
        string name(cname);
        char * arg;
        if (ATmatch(value, "Pkg(<str>)", &arg)) {
            parseHash(arg);
            pkgImports[name] = arg;
        } else if (ATmatch(value, "File(<str>)", &arg)) {
            parseHash(arg);
            fileImports[name] = arg;
        } else if (ATmatch(value, "Str(<str>)", &arg))
            arguments[name] = arg;
        else if (ATmatch(value, "Bool(True)"))
            arguments[name] = "1";
        else if (ATmatch(value, "Bool(False)"))
            arguments[name] = "";
        else {
            ATprintf("%t\n", value);
            throw Error("invalid binding in " + pkgfile);
        }
    }
}


string getPkg(Hash hash);


void fetchDeps(Hash hash, Environment & env)
{
    /* Read the package description file. */
    Params pkgImports, fileImports, arguments;
    readPkgDescr(hash, pkgImports, fileImports, arguments);

    /* Recursively fetch all the dependencies, filling in the
       environment as we go along. */
    for (Params::iterator it = pkgImports.begin();
         it != pkgImports.end(); it++)
    {
        cerr << "fetching package dependency "
             << it->first << " <- " << it->second
             << endl;
        env[it->first] = getPkg(parseHash(it->second));
    }

    for (Params::iterator it = fileImports.begin();
         it != fileImports.end(); it++)
    {
        cerr << "fetching file dependency "
             << it->first << " = " << it->second
             << endl;

        string file;

        file = getFile(parseHash(it->second));

        env[it->first] = file;
    }

    string buildSystem;

    for (Params::iterator it = arguments.begin();
         it != arguments.end(); it++)
    {
        env[it->first] = it->second;
        if (it->first == "system")
            buildSystem = it->second;
    }

    if (buildSystem != thisSystem)
        throw Error("descriptor requires a `" + buildSystem +
            "' but I am a `" + thisSystem + "'");
}


string getFromEnv(const Environment & env, const string & key)
{
    Environment::const_iterator it = env.find(key);
    if (it == env.end())
        throw Error("key " + key + " not found in the environment");
    return it->second;
}


string queryPkgId(Hash hash)
{
    Params pkgImports, fileImports, arguments;
    readPkgDescr(hash, pkgImports, fileImports, arguments);
    return getFromEnv(arguments, "id");
}


void installPkg(Hash hash)
{
    string pkgfile;
    string src;
    string path;
    string cmd;
    string builder;
    Environment env;

    /* Fetch dependencies. */
    fetchDeps(hash, env);

    builder = getFromEnv(env, "build");

    string id = getFromEnv(env, "id");

    /* Construct a path for the installed package. */
    path = nixHomeDir + "/pkg/" + id + "-" + (string) hash;

    /* Create the path. */
    if (mkdir(path.c_str(), 0777))
        throw Error("unable to create directory " + path);

    /* Create a log file. */
    string logFileName = 
        nixLogDir + "/" + id + "-" + (string) hash + ".log";
    /* !!! auto-pclose on exit */
    FILE * logFile = popen(("tee " + logFileName + " >&2").c_str(), "w"); /* !!! escaping */
    if (!logFile)
        throw Error("unable to create log file " + logFileName);

    try {

        /* Fork a child to build the package. */
        pid_t pid;
        switch (pid = fork()) {
            
        case -1:
            throw Error("unable to fork");

        case 0: 

            try { /* child */

                /* Go to the build directory. */
                if (chdir(path.c_str())) {
                    cerr << "unable to chdir to package directory\n";
                    _exit(1);
                }

                /* Try to use a prebuilt. */
                string prebuiltHashS, prebuiltFile;
                if (queryDB(nixDB, dbPrebuilts, hash, prebuiltHashS)) {

                    try {
                        prebuiltFile = getFile(parseHash(prebuiltHashS));
                    } catch (Error e) {
                        cerr << "cannot obtain prebuilt (ignoring): " << e.what() << endl;
                        goto build;
                    }
                
                    cerr << "substituting prebuilt " << prebuiltFile << endl;

                    int res = system(("tar xfj " + prebuiltFile + " 1>&2").c_str()); // !!! escaping
                    if (WEXITSTATUS(res) != 0)
                        /* This is a fatal error, because path may now
                           have clobbered. */
                        throw Error("cannot unpack " + prebuiltFile);

                    _exit(0);
                }

            build:

                /* Fill in the environment.  We don't bother freeing
                   the strings, since we'll exec or die soon
                   anyway. */
                const char * env2[env.size() + 1];
                int i = 0;
                for (Environment::iterator it = env.begin();
                     it != env.end(); it++, i++)
                    env2[i] = (new string(it->first + "=" + it->second))->c_str();
                env2[i] = 0;

                /* Dup the log handle into stderr. */
                if (dup2(fileno(logFile), STDERR_FILENO) == -1)
                    throw Error("cannot pipe standard error into log file: " + string(strerror(errno)));
            
                /* Dup stderr to stdin. */
                if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1)
                    throw Error("cannot dup stderr into stdout");

                /* Execute the builder.  This should not return. */
                execle(builder.c_str(), builder.c_str(), 0, env2);

                throw Error("unable to execute builder: " +
                    string(strerror(errno)));
            
            } catch (exception & e) {
                cerr << "build error: " << e.what() << endl;
                _exit(1);
            }

        }

        /* parent */

        /* Close the logging pipe.  Note that this should not cause
           the logger to exit until builder exits (because the latter
           has an open file handle to the former). */
        pclose(logFile);
    
        /* Wait for the child to finish. */
        int status;
        if (waitpid(pid, &status, 0) != pid)
            throw Error("unable to wait for child");
    
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            throw Error("unable to build package");

        /* Remove write permission from the build directory. */
        int res = system(("chmod -R -w " + path).c_str()); // !!! escaping
        if (WEXITSTATUS(res) != 0)
            throw Error("cannot remove write permission from " + path);

    } catch (exception &) {
//         system(("rm -rf " + path).c_str());
        throw;
    }

    setDB(nixDB, dbInstPkgs, hash, path);
}


string getPkg(Hash hash)
{
    string path;
    while (!queryDB(nixDB, dbInstPkgs, hash, path))
        installPkg(hash);
    return path;
}


void runPkg(Hash hash, 
    Strings::iterator firstArg, 
    Strings::iterator lastArg)
{
    string src;
    string path;
    string cmd;
    string runner;
    Environment env;

    /* Fetch dependencies. */
    fetchDeps(hash, env);

    runner = getFromEnv(env, "run");
    
    /* Fill in the environment.  We don't bother freeing the
       strings, since we'll exec or die soon anyway. */
    for (Environment::iterator it = env.begin();
         it != env.end(); it++)
    {
        string * s = new string(it->first + "=" + it->second);
        putenv((char *) s->c_str());
    }

    /* Create the list of arguments. */
    const char * args2[env.size() + 2];
    int i = 0;
    args2[i++] = runner.c_str();
    for (Strings::const_iterator it = firstArg; it != lastArg; it++, i++)
        args2[i] = it->c_str();
    args2[i] = 0;

    /* Execute the runner.  This should not return. */
    execv(runner.c_str(), (char * *) args2);

    cerr << strerror(errno) << endl;
    throw Error("unable to execute runner");
}


void ensurePkg(Hash hash)
{
    Params pkgImports, fileImports, arguments;
    readPkgDescr(hash, pkgImports, fileImports, arguments);

    if (fileImports.find("build") != fileImports.end())
        getPkg(hash);
    else if (fileImports.find("run") != fileImports.end()) {
        Environment env;
        fetchDeps(hash, env);
    } else throw Error("invalid descriptor");
}


void delPkg(Hash hash)
{
    string path;
    if (queryDB(nixDB, dbInstPkgs, hash, path)) {
        int res = system(("chmod -R +w " + path + " && rm -rf " + path).c_str()); // !!! escaping
        delDB(nixDB, dbInstPkgs, hash); // not a bug ??? 
        if (WEXITSTATUS(res) != 0)
            cerr << "errors deleting " + path + ", ignoring" << endl;
    }
}


void exportPkgs(string outDir, 
    Strings::iterator firstHash, 
    Strings::iterator lastHash)
{
    outDir = absPath(outDir);

    for (Strings::iterator it = firstHash; it != lastHash; it++) {
        Hash hash = parseHash(*it);
        string pkgDir = getPkg(hash);
        string tmpFile = outDir + "/export_tmp";

        string cmd = "cd " + pkgDir + " && tar cfj " + tmpFile + " .";
        int res = system(cmd.c_str()); // !!! escaping
        if (!WIFEXITED(res) || WEXITSTATUS(res) != 0)
            throw Error("cannot tar " + pkgDir);

        string prebuiltHash = hashFile(tmpFile);
        string pkgId = queryPkgId(hash);
        string prebuiltFile = outDir + "/" +
            pkgId + "-" + (string) hash + "-" + prebuiltHash + ".tar.bz2";
        
        rename(tmpFile.c_str(), prebuiltFile.c_str());
    }
}


void registerPrebuilt(Hash pkgHash, Hash prebuiltHash)
{
    setDB(nixDB, dbPrebuilts, pkgHash, prebuiltHash);
}


Hash registerFile(string filename)
{
    filename = absPath(filename);
    Hash hash = hashFile(filename);
    setDB(nixDB, dbRefs, hash, filename);
    return hash;
}


void registerURL(Hash hash, string url)
{
    setDB(nixDB, dbNetSources, hash, url);
    /* !!! currently we allow only one network source per hash */
}


/* This is primarily used for bootstrapping. */
void registerInstalledPkg(Hash hash, string path)
{
    if (path == "")
        delDB(nixDB, dbInstPkgs, hash);
    else
        setDB(nixDB, dbInstPkgs, hash, path);
}


void verifyDB()
{
    /* Check that all file references are still valid. */
    DBPairs fileRefs;
    
    enumDB(nixDB, dbRefs, fileRefs);

    for (DBPairs::iterator it = fileRefs.begin();
         it != fileRefs.end(); it++)
    {
        try {
            Hash hash = parseHash(it->first);
            if (hashFile(it->second) != hash) {
                cerr << "file " << it->second << " has changed\n";
                delDB(nixDB, dbRefs, it->first);
            }
        } catch (Error e) { /* !!! better error check */ 
            cerr << "error: " << e.what() << endl;
            delDB(nixDB, dbRefs, it->first);
        }
    }

    /* Check that all installed packages are still there. */
    DBPairs instPkgs;

    enumDB(nixDB, dbInstPkgs, instPkgs);

    for (DBPairs::iterator it = instPkgs.begin();
         it != instPkgs.end(); it++)
    {
        struct stat st;
        if (stat(it->second.c_str(), &st) == -1) {
            cerr << "package " << it->first << " has disappeared\n";
            delDB(nixDB, dbInstPkgs, it->first);
        }
    }

    /* TODO: check that all directories in pkgHome are installed
       packages. */
}


void listInstalledPkgs()
{
    DBPairs instPkgs;

    enumDB(nixDB, dbInstPkgs, instPkgs);

    for (DBPairs::iterator it = instPkgs.begin();
         it != instPkgs.end(); it++)
        cout << it->first << endl;
}


void printInfo(Strings::iterator first, Strings::iterator last)
{
    for (Strings::iterator it = first; it != last; it++) {
        try {
            cout << *it << " " << queryPkgId(parseHash(*it)) << endl;
        } catch (Error & e) { // !!! more specific
            cout << *it << " (descriptor missing)\n";
        }
    }
}


void computeClosure(Strings::iterator first, Strings::iterator last, 
    set<string> & result)
{
    list<string> workList(first, last);
    set<string> doneSet;

    while (!workList.empty()) {
        Hash hash = parseHash(workList.front());
        workList.pop_front();
        
        if (doneSet.find(hash) == doneSet.end()) {
            doneSet.insert(hash);
    
            Params pkgImports, fileImports, arguments;
            readPkgDescr(hash, pkgImports, fileImports, arguments);

            for (Params::iterator it = pkgImports.begin();
                 it != pkgImports.end(); it++)
                workList.push_back(it->second);
        }
    }

    result = doneSet;
}


void printClosure(Strings::iterator first, Strings::iterator last)
{
    set<string> allHashes;
    computeClosure(first, last, allHashes);
    for (set<string>::iterator it = allHashes.begin();
         it != allHashes.end(); it++)
        cout << *it << endl;
}


string dotQuote(const string & s)
{
    return "\"" + s + "\"";
}


void printGraph(Strings::iterator first, Strings::iterator last)
{
    set<string> allHashes;
    computeClosure(first, last, allHashes);

    cout << "digraph G {\n";

    for (set<string>::iterator it = allHashes.begin();
         it != allHashes.end(); it++)
    {
        Params pkgImports, fileImports, arguments;
        readPkgDescr(parseHash(*it), pkgImports, fileImports, arguments);

        cout << dotQuote(*it) << "[label = \"" 
             << getFromEnv(arguments, "id")
             << "\"];\n";

        for (Params::iterator it2 = pkgImports.begin();
             it2 != pkgImports.end(); it2++)
            cout << dotQuote(it2->second) << " -> " 
                 << dotQuote(*it) << ";\n";
    }

    cout << "}\n";
}


void fetch(string id)
{
    string fn;

    /* Fetch the object referenced by id. */
    if (isHash(id)) {
        throw Error("not implemented");
    } else {
        fn = fetchURL(id);
    }

    /* Register it by hash. */
    Hash hash = registerFile(fn);
    cout << (string) hash << endl;
}


void fetch(Strings::iterator first, Strings::iterator last)
{
    for (Strings::iterator it = first; it != last; it++)
        fetch(*it);
}


void printUsage()
{
    cerr <<
"Usage: nix SUBCOMMAND OPTIONS...\n\
\n\
Subcommands:\n\
\n\
  init\n\
    Initialize the database.\n\
\n\
  verify\n\
    Remove stale entries from the database.\n\
\n\
  regfile FILENAME...\n\
    Register each FILENAME keyed by its hash.\n\
\n\
  reginst HASH PATH\n\
    Register an installed package.\n\
\n\
  getpkg HASH...\n\
    For each HASH, ensure that the package referenced by HASH is\n\
    installed. Print out the path of the installation on stdout.\n\
\n\
  delpkg HASH...\n\
    Uninstall the package referenced by each HASH, disregarding any\n\
    dependencies that other packages may have on HASH.\n\
\n\
  listinst\n\
    Prints a list of installed packages.\n\
\n\
  run HASH ARGS...\n\
    Run the descriptor referenced by HASH with the given arguments.\n\
\n\
  ensure HASH...\n\
    Like getpkg, but if HASH refers to a run descriptor, fetch only\n\
    the dependencies.\n\
\n\
  export DIR HASH...\n\
    Export installed packages to DIR.\n\
\n\
  regprebuilt HASH1 HASH2\n\
    Inform Nix that an export HASH2 can be used to fast-build HASH1.\n\
\n\
  info HASH...\n\
    Print information about the specified descriptors.\n\
\n\
  closure HASH...\n\
    Determine the closure of the set of descriptors under the import\n\
    relation, starting at the given roots.\n\
\n\
  graph HASH...\n\
    Like closure, but print a dot graph specification.\n\
\n\
  fetch ID...\n\
    Fetch the objects identified by ID and place them in the Nix\n\
    sources directory.  ID can be a hash or URL.  Print out the hash\n\
    of the object.\n\
";
}


void run(Strings::iterator argCur, Strings::iterator argEnd)
{
    umask(0022);

    char * homeDir = getenv(nixHomeDirEnvVar.c_str());
    if (homeDir) nixHomeDir = homeDir;

    nixSourcesDir = nixHomeDir + "/var/nix/sources";
    nixLogDir = nixHomeDir + "/var/log/nix";
    nixDB = nixHomeDir + "/var/nix/pkginfo.db";

    /* Parse the global flags. */
    for ( ; argCur != argEnd; argCur++) {
        string arg(*argCur);
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return;
        } else if (arg[0] == '-') {
            throw UsageError("invalid option `" + arg + "'");
        } else break;
    }

    UsageError argcError("wrong number of arguments");

    /* Parse the command. */
    if (argCur == argEnd) throw UsageError("no command specified");
    string cmd = *argCur++;
    int argc = argEnd - argCur;

    if (cmd == "init") {
        if (argc != 0) throw argcError;
        initDB();
    } else if (cmd == "verify") {
        if (argc != 0) throw argcError;
        verifyDB();
    } else if (cmd == "getpkg") {
        for (Strings::iterator it = argCur; it != argEnd; it++) {
            string path = getPkg(parseHash(*it));
            cout << path << endl;
        }
    } else if (cmd == "delpkg") {
        for (Strings::iterator it = argCur; it != argEnd; it++)
            delPkg(parseHash(*it));
    } else if (cmd == "run") {
        if (argc < 1) throw argcError;
        runPkg(parseHash(*argCur), argCur + 1, argEnd);
    } else if (cmd == "ensure") {
        for (Strings::iterator it = argCur; it != argEnd; it++)
            ensurePkg(parseHash(*it));
    } else if (cmd == "export") {
        if (argc < 1) throw argcError;
        exportPkgs(*argCur, argCur + 1, argEnd);
    } else if (cmd == "regprebuilt") {
        if (argc != 2) throw argcError;
        registerPrebuilt(parseHash(argCur[0]), parseHash(argCur[1]));
    } else if (cmd == "regfile") {
        for_each(argCur, argEnd, registerFile);
    } else if (cmd == "regurl") {
        registerURL(parseHash(argCur[0]), argCur[1]);
    } else if (cmd == "reginst") {
        if (argc != 2) throw argcError;
        registerInstalledPkg(parseHash(argCur[0]), argCur[1]);
    } else if (cmd == "listinst") {
        if (argc != 0) throw argcError;
        listInstalledPkgs();
    } else if (cmd == "info") {
        printInfo(argCur, argEnd);
    } else if (cmd == "closure") {
        printClosure(argCur, argEnd);
    } else if (cmd == "graph") {
        printGraph(argCur, argEnd);
    } else if (cmd == "fetch") {
        fetch(argCur, argEnd);
    } else
        throw UsageError("unknown command: " + string(cmd));
}


int main(int argc, char * * argv)
{
    ATerm bottomOfStack;
    ATinit(argc, argv, &bottomOfStack);

    /* Put the arguments in a vector. */
    Strings args;
    while (argc--) args.push_back(*argv++);
    Strings::iterator argCur = args.begin(), argEnd = args.end();

    argCur++;

    try {
        run(argCur, argEnd);
    } catch (UsageError & e) {
        cerr << "error: " << e.what() << endl
             << "Try `nix -h' for more information.\n";
        return 1;
    } catch (exception & e) {
        cerr << "error: " << e.what() << endl;
        return 1;
    }

    return 0;
}
