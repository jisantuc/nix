#include <iostream>

#include <sys/types.h>
#include <sys/wait.h>

#include "values.hh"
#include "globals.hh"
#include "db.hh"
#include "archive.hh"


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


void copyFile(string src, string dst)
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


void addToStore(string srcPath, string & dstPath, Hash & hash)
{
    srcPath = absPath(srcPath);

    hash = hashPath(srcPath);

    string path;
    if (queryDB(nixDB, dbRefs, hash, path)) {
        debug((string) hash + " already known");
        dstPath = path;
        return;
    }

    string baseName = baseNameOf(srcPath);
    dstPath = nixStore + "/" + (string) hash + "-" + baseName;

    copyFile(srcPath, dstPath);

    setDB(nixDB, dbRefs, hash, dstPath);
}


#if 0
/* Download object referenced by the given URL into the sources
   directory.  Return the file name it was downloaded to. */
string fetchURL(string url)
{
    string filename = baseNameOf(url);
    string fullname = nixSourcesDir + "/" + filename;
    struct stat st;
    if (stat(fullname.c_str(), &st)) {
        cerr << "fetching " << url << endl;
        /* !!! quoting */
        string shellCmd =
            "cd " + nixSourcesDir + " && wget --quiet -N \"" + url + "\"";
        int res = system(shellCmd.c_str());
        if (WEXITSTATUS(res) != 0)
            throw Error("cannot fetch " + url);
    }
    return fullname;
}
#endif


void deleteFromStore(const string & path)
{
    string prefix = nixStore + "/";
    if (string(path, 0, prefix.size()) != prefix)
        throw Error(format("path %1% is not in the store") % path);
    deletePath(path);
//     delDB(nixDB, dbRefs, hash);
}


string queryFromStore(Hash hash)
{
    bool checkedNet = false;

    while (1) {

        string fn, url;

        if (queryDB(nixDB, dbRefs, hash, fn)) {

            /* Verify that the file hasn't changed. !!! race !!! slow */
            if (hashPath(fn) != hash)
                throw Error("file " + fn + " is stale");

            return fn;
        }

        throw Error("a file with hash " + (string) hash + " is required, "
            "but it is not known to exist locally or on the network");
#if 0
        if (checkedNet)
            throw Error("consistency problem: file fetched from " + url + 
                " should have hash " + (string) hash + ", but it doesn't");

        if (!queryDB(nixDB, dbNetSources, hash, url))
            throw Error("a file with hash " + (string) hash + " is required, "
                "but it is not known to exist locally or on the network");

        checkedNet = true;
        
        fn = fetchURL(url);
        
        setDB(nixDB, dbRefs, hash, fn);
#endif
    }
}
