#include <map>
#include <iostream>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include "eval.hh"
#include "globals.hh"
#include "values.hh"
#include "db.hh"


/* A Unix environment is a mapping from strings to strings. */
typedef map<string, string> Environment;


/* Return true iff the given path exists. */
bool pathExists(const string & path)
{
    int res;
    struct stat st;
    res = stat(path.c_str(), &st);
    if (!res) return true;
    if (errno != ENOENT)
        throw SysError("getting status of " + path);
    return false;
}


/* Run a program. */
static void runProgram(const string & program, Environment env)
{
    /* Create a log file. */
    string logFileName = nixLogDir + "/run.log";
    /* !!! auto-pclose on exit */
    FILE * logFile = popen(("tee " + logFileName + " >&2").c_str(), "w"); /* !!! escaping */
    if (!logFile)
        throw SysError(format("unable to create log file %1%") % logFileName);

    /* Fork a child to build the package. */
    pid_t pid;
    switch (pid = fork()) {
            
    case -1:
        throw SysError("unable to fork");

    case 0: 

        try { /* child */

#if 0
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
#endif

            //             build:

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
                throw SysError("cannot pipe standard error into log file");
            
            /* Dup stderr to stdin. */
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1)
                throw SysError("cannot dup stderr into stdout");

            /* Make the program executable.  !!! hack. */
            if (chmod(program.c_str(), 0755))
                throw SysError("cannot make program executable");

            /* Execute the program.  This should not return. */
            execle(program.c_str(), baseNameOf(program).c_str(), 0, env2);

            throw SysError(format("unable to execute %1%") % program);
            
        } catch (exception & e) {
            cerr << "build error: " << e.what() << endl;
        }
        _exit(1);

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
}


/* Throw an exception if the given platform string is not supported by
   the platform we are executing on. */
static void checkPlatform(const string & platform)
{
    if (platform != thisSystem)
        throw Error(format("a `%1%' is required, but I am a `%2%'")
            % platform % thisSystem);
}


string printTerm(ATerm t)
{
    char * s = ATwriteToString(t);
    return s;
}


/* Throw an exception with an error message containing the given
   aterm. */
static Error badTerm(const format & f, ATerm t)
{
    return Error(format("%1%, in `%2%'") % f.str() % printTerm(t));
}


Hash hashTerm(ATerm t)
{
    return hashString(printTerm(t));
}


#if 0
/* Evaluate a list of arguments into normal form. */
void evalArgs(ATermList args, ATermList & argsNF, Environment & env)
{
    argsNF = ATempty;

    while (!ATisEmpty(args)) {
        ATerm eName, eVal, arg = ATgetFirst(args);
        if (!ATmatch(arg, "Tup(<term>, <term>)", &eName, &eVal))
            throw badTerm("invalid argument", arg);

        string name = evalString(eName);
        eVal = evalValue(eVal);

        char * s;
        if (ATmatch(eVal, "Str(<str>)", &s)) {
            env[name] = s;
        } else if (ATmatch(eVal, "Hash(<str>)", &s)) {
            env[name] = queryValuePath(parseHash(s));
        } else throw badTerm("invalid argument value", eVal);

        argsNF = ATinsert(argsNF,
            ATmake("Tup(Str(<str>), <term>)", name.c_str(), eVal));

        args = ATgetNext(args);
    }

    argsNF = ATreverse(argsNF);
}
#endif


struct RStatus
{
    /* !!! the comparator of this hash should match the semantics of
       the file system */
//     map<string, Hash> paths;
};


static FState realise(RStatus & status, FState fs)
{
    char * s1, * s2, * s3;
    Content content;
    ATermList refs, ins, outs, bnds;
    
    if (ATmatch(fs, "File(<str>, <term>, [<list>])", &s1, &content, &refs)) {
        string path(s1);

        if (path[0] != '/') throw Error("absolute path expected: " + path);

        /* Realise referenced paths. */
        ATermList refs2 = ATempty;
        while (!ATisEmpty(refs)) {
            refs2 = ATappend(refs2, realise(status, ATgetFirst(refs)));
            refs = ATgetNext(refs);
        }
        refs2 = ATreverse(refs2);

        if (!ATmatch(content, "Hash(<str>)", &s1))
            throw badTerm("hash expected", content);
        Hash hash = parseHash(s1);

        /* Normal form. */
        ATerm nf = ATmake("File(<str>, <term>, <list>)",
            path.c_str(), content, refs2);

        /* Perhaps the path already exists and has the right hash? */
        if (pathExists(path)) {
            if (hash == hashPath(path)) {
                debug(format("path %1% already has hash %2%")
                    % path % (string) hash);
                return nf;
            }

            throw Error(format("path %1% exists, but does not have hash %2%")
                % path % (string) hash);
        }

        /* Do we know a path with that hash?  If so, copy it. */
        string path2 = queryFromStore(hash);
        copyFile(path2, path);

        return nf;
    }

    else if (ATmatch(fs, "Derive(<str>, <str>, [<list>], <str>, [<list>])",
                 &s1, &s2, &ins, &s3, &bnds)) 
    {
        string platform(s1), builder(s2), outPath(s3);

        checkPlatform(platform);
        
        /* Realise inputs. */
        ATermList ins2 = ATempty;
        while (!ATisEmpty(ins)) {
            ins2 = ATappend(ins2, realise(status, ATgetFirst(ins)));
            ins = ATgetNext(ins);
        }
        ins2 = ATreverse(ins2);

        /* Build the environment. */
        Environment env;
        while (!ATisEmpty(bnds)) {
            ATerm bnd = ATgetFirst(bnds);
            if (!ATmatch(bnd, "(<str>, <str>)", &s1, &s2))
                throw badTerm("string expected", bnd);
            env[s1] = s2;
            bnds = ATgetNext(bnds);
        }

        /* Check whether the target already exists. */
        if (pathExists(outPath))
            deleteFromStore(outPath);
//             throw Error(format("path %1% already exists") % outPath);

        /* Run the builder. */
        runProgram(builder, env);
        
        /* Check whether the result was created. */
        if (!pathExists(outPath))
            throw Error(format("program %1% failed to create a result in %2%")
                % builder % outPath);

#if 0
        /* Remove write permission from the value. */
        int res = system(("chmod -R -w " + targetPath).c_str()); // !!! escaping
        if (WEXITSTATUS(res) != 0)
            throw Error("cannot remove write permission from " + targetPath);
#endif

        /* Hash the result. */
        Hash outHash = hashPath(outPath);

        /* Register targetHash -> targetPath.  !!! this should be in
           values.cc. */
        setDB(nixDB, dbRefs, outHash, outPath);

#if 0
        /* Register that targetHash was produced by evaluating
           sourceHash; i.e., that targetHash is a normal form of
           sourceHash. !!! this shouldn't be here */
        setDB(nixDB, dbNFs, sourceHash, targetHash);
#endif

        return ATmake("File(<str>, Hash(<str>), <list>)",
            outPath.c_str(), ((string) outHash).c_str(), ins2);
    }

    throw badTerm("bad file system state expression", fs);
}


FState realiseFState(FState fs)
{
    RStatus status;
    return realise(status, fs);
}
