#include <iostream>

#include <sys/types.h>
#include <sys/wait.h>

#include "store.hh"
#include "globals.hh"
#include "db.hh"
#include "archive.hh"
#include "fstate.hh"


struct CopySink : DumpSink
{
    int fd;
    virtual void operator () (const unsigned char * data, unsigned int len)
    {
        if (write(fd, (char *) data, len) != (ssize_t) len)
            throw SysError("writing to child");
    }
};


struct CopySource : RestoreSource
{
    int fd;
    virtual void operator () (const unsigned char * data, unsigned int len)
    {
        ssize_t res = read(fd, (char *) data, len);
        if (res == -1)
            throw SysError("reading from parent");
        if (res != (ssize_t) len)
            throw Error("not enough data available on parent");
    }
};


void copyPath(string src, string dst)
{
    /* Unfortunately C++ doesn't support coprocedures, so we have no
       nice way to chain CopySink and CopySource together.  Instead we
       fork off a child to run the sink.  (Fork-less platforms should
       use a thread). */

    /* Create a pipe. */
    int fds[2];
    if (pipe(fds) == -1) throw SysError("creating pipe");

    /* Fork. */
    pid_t pid;
    switch (pid = fork()) {

    case -1:
        throw SysError("unable to fork");

    case 0: /* child */
        try {
            close(fds[1]);
            CopySource source;
            source.fd = fds[0];
            restorePath(dst, source);
            _exit(0);
        }  catch (exception & e) {
            cerr << "error: " << e.what() << endl;
        }
        _exit(1);        
    }

    close(fds[0]);
    
    /* Parent. */

    CopySink sink;
    sink.fd = fds[1];
    dumpPath(src, sink);

    /* Wait for the child to finish. */
    int status;
    if (waitpid(pid, &status, 0) != pid)
        throw SysError("waiting for child");

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw Error("cannot copy file: child died");
}


void registerSubstitute(const FSId & srcId, const FSId & subId)
{
    Strings subs;
    queryListDB(nixDB, dbSubstitutes, srcId, subs); /* non-existence = ok */

    for (Strings::iterator it = subs.begin(); it != subs.end(); it++)
        if (parseHash(*it) == subId) return;
    
    subs.push_back(subId);
    
    setListDB(nixDB, dbSubstitutes, srcId, subs);
}


void registerPath(const string & _path, const FSId & id)
{
    string path(canonPath(_path));

    setDB(nixDB, dbPath2Id, path, id);

    Strings paths;
    queryListDB(nixDB, dbId2Paths, id, paths); /* non-existence = ok */

    for (Strings::iterator it = paths.begin();
         it != paths.end(); it++)
        if (*it == path) return;
    
    paths.push_back(path);
    
    setListDB(nixDB, dbId2Paths, id, paths);
}


void unregisterPath(const string & _path)
{
    string path(canonPath(_path));

    string _id;
    if (!queryDB(nixDB, dbPath2Id, path, _id))
        return;
    FSId id(parseHash(_id));

    /* begin transaction */
    
    Strings paths, paths2;
    queryListDB(nixDB, dbId2Paths, id, paths); /* non-existence = ok */

    bool changed = false;
    for (Strings::iterator it = paths.begin();
         it != paths.end(); it++)
        if (*it != path) paths2.push_back(*it); else changed = true;

    if (changed)
        setListDB(nixDB, dbId2Paths, id, paths2);

    /* end transaction */
}


bool isInPrefix(const string & path, const string & _prefix)
{
    string prefix = canonPath(_prefix + "/");
    return string(path, 0, prefix.size()) == prefix;
}


string expandId(const FSId & id, const string & target,
    const string & prefix)
{
    Strings paths;

    if (!target.empty() && !isInPrefix(target, prefix))
        abort();

    queryListDB(nixDB, dbId2Paths, id, paths);

    /* Pick one equal to `target'. */
    if (!target.empty()) {

        for (Strings::iterator i = paths.begin();
             i != paths.end(); i++)
        {
            string path = *i;
            if (path == target && pathExists(path))
                return path;
        }
        
    }

    /* Arbitrarily pick the first one that exists and isn't stale. */
    for (Strings::iterator it = paths.begin();
         it != paths.end(); it++)
    {
        string path = *it;
        if (isInPrefix(path, prefix) && pathExists(path)) {
            if (target.empty())
                return path;
            else {
                copyPath(path, target);
                registerPath(target, id);
                return target;
            }
        }
    }

#if 0
    /* Try to realise the substitutes. */

    Strings subs;
    queryListDB(nixDB, dbSubstitutes, id, subs); /* non-existence = ok */

    for (Strings::iterator it = subs.begin(); it != subs.end(); it++) {
        realiseSlice(normaliseFState(*it));
        
        FState nf = realiseFState(hash2fstate(parseHash(*it)), dummy);
        string path = fstatePath(nf);

        if (hashPath(path) != hash)
            throw Error(format("bad substitute in `%1%'") % (string) path);

        if (target.empty())
            return path; /* !!! prefix */
        else {
            if (path != target) {
                copyPath(path, target);
                registerPath(target, hash);
            }
            return target;
        }
    }
#endif
    
    throw Error(format("cannot expand id `%1%'") % (string) id);
}

    
void addToStore(string srcPath, string & dstPath, FSId & id,
    bool deterministicName)
{
    srcPath = absPath(srcPath);
    id = hashPath(srcPath);

    string baseName = baseNameOf(srcPath);
    dstPath = canonPath(nixStore + "/" + (string) id + "-" + baseName);

    try {
        /* !!! should not use the substitutes! */
        dstPath = expandId(id, deterministicName ? dstPath : "", nixStore);
        return;
    } catch (...) {
    }
    
    copyPath(srcPath, dstPath);
    registerPath(dstPath, id);
}


void deleteFromStore(const string & path)
{
    string prefix =  + "/";
    if (!isInPrefix(path, nixStore))
        throw Error(format("path %1% is not in the store") % path);

    unregisterPath(path);

    deletePath(path);
}
