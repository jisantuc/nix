#include "serialise.hh"
#include "util.hh"
#include "remote-store.hh"
#include "worker-protocol.hh"
#include "archive.hh"
#include "globals.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <iostream>
#include <unistd.h>


namespace nix {


RemoteStore::RemoteStore()
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == -1)
        throw SysError("cannot create sockets");

    fdSelf = sockets[0];
    AutoCloseFD fdChild = sockets[1];

    
    /* Start the worker. */
    Path worker = getEnv("NIX_WORKER");
    if (worker == "")
        worker = nixBinDir + "/nix-worker";

    string verbosityArg = "-";
    for (int i = 1; i < verbosity; ++i)
        verbosityArg += "v";

    child = fork();
    
    switch (child) {
        
    case -1:
        throw SysError("unable to fork");

    case 0:
        try { /* child */
            
            if (dup2(fdChild, STDOUT_FILENO) == -1)
                throw SysError("dupping write side");

            if (dup2(fdChild, STDIN_FILENO) == -1)
                throw SysError("dupping read side");

            close(fdSelf);
            close(fdChild);

            int fdDebug = open("/tmp/worker-log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            assert(fdDebug != -1);
            if (dup2(fdDebug, STDERR_FILENO) == -1)
                throw SysError("dupping stderr");
            close(fdDebug);
            
            execlp(worker.c_str(), worker.c_str(), "--slave",
                /* hacky - must be at the end */
                verbosityArg == "-" ? NULL : verbosityArg.c_str(),
                NULL);

            throw SysError(format("executing `%1%'") % worker);
            
        } catch (std::exception & e) {
            std::cerr << format("child error: %1%\n") % e.what();
        }
        quickExit(1);
    }

    fdChild.close();

    from.fd = fdSelf;
    to.fd = fdSelf;

    
    /* Send the magic greeting, check for the reply. */
    try {
        processStderr();
        writeInt(WORKER_MAGIC_1, to);
        unsigned int magic = readInt(from);
        if (magic != WORKER_MAGIC_2) throw Error("protocol mismatch");
    } catch (Error & e) {
        throw Error(format("cannot start worker process `%1%' (%2%)")
            % worker % e.msg());
    }
}


RemoteStore::~RemoteStore()
{
    try {
        fdSelf.close();
        child.wait(true);
    } catch (Error & e) {
        printMsg(lvlError, format("error (ignored): %1%") % e.msg());
    }
}


bool RemoteStore::isValidPath(const Path & path)
{
    writeInt(wopIsValidPath, to);
    writeString(path, to);
    unsigned int reply = readInt(from);
    return reply != 0;
}


Substitutes RemoteStore::querySubstitutes(const Path & path)
{
    throw Error("not implemented 2");
}


bool RemoteStore::hasSubstitutes(const Path & path)
{
    writeInt(wopHasSubstitutes, to);
    writeString(path, to);
    unsigned int reply = readInt(from);
    return reply != 0;
}


Hash RemoteStore::queryPathHash(const Path & path)
{
    writeInt(wopQueryPathHash, to);
    writeString(path, to);
    string hash = readString(from);
    return parseHash(htSHA256, hash);
}


void RemoteStore::queryReferences(const Path & path,
    PathSet & references)
{
    writeInt(wopQueryReferences, to);
    writeString(path, to);
    PathSet references2 = readStringSet(from);
    references.insert(references2.begin(), references2.end());
}


void RemoteStore::queryReferrers(const Path & path,
    PathSet & referrers)
{
    writeInt(wopQueryReferrers, to);
    writeString(path, to);
    PathSet referrers2 = readStringSet(from);
    referrers.insert(referrers2.begin(), referrers2.end());
}


Path RemoteStore::addToStore(const Path & _srcPath, bool fixed,
    bool recursive, string hashAlgo)
{
    Path srcPath(absPath(_srcPath));
    
    writeInt(wopAddToStore, to);
    writeString(baseNameOf(srcPath), to);
    writeInt(fixed ? 1 : 0, to);
    writeInt(recursive ? 1 : 0, to);
    writeString(hashAlgo, to);
    dumpPath(srcPath, to);
    Path path = readString(from);
    return path;
}


Path RemoteStore::addTextToStore(const string & suffix, const string & s,
    const PathSet & references)
{
    writeInt(wopAddTextToStore, to);
    writeString(suffix, to);
    writeString(s, to);
    writeStringSet(references, to);
    
    Path path = readString(from);
    return path;
}


void RemoteStore::buildDerivations(const PathSet & drvPaths)
{
    writeInt(wopBuildDerivations, to);
    writeStringSet(drvPaths, to);
    processStderr();
    readInt(from);
}


void RemoteStore::ensurePath(const Path & path)
{
    writeInt(wopEnsurePath, to);
    writeString(path, to);
    readInt(from);
}


void RemoteStore::addTempRoot(const Path & path)
{
    writeInt(wopAddTempRoot, to);
    writeString(path, to);
    readInt(from);
}


void RemoteStore::syncWithGC()
{
    writeInt(wopSyncWithGC, to);
    readInt(from);
}


void RemoteStore::processStderr()
{
    unsigned int msg;
    while ((msg = readInt(from)) == STDERR_NEXT) {
        string s = readString(from);
        writeToStderr((unsigned char *) s.c_str(), s.size());
    }
    if (msg == STDERR_ERROR)
        throw Error(readString(from));
    else if (msg != STDERR_LAST)
        throw Error("protocol error processing standard error");
}


}
