#ifndef __STORE_H
#define __STORE_H

#include <string>

#include "hash.hh"
#include "db.hh"

using namespace std;


/* Nix store and database schema version.  Version 1 (or 0) was Nix <=
   0.7.  Version 2 was Nix 0.8 and 0.8.  Version 3 is Nix 0.10 and
   up. */
const int nixSchemaVersion = 3;


/* A substitute is a program invocation that constructs some store
   path (typically by fetching it from somewhere, e.g., from the
   network). */
struct Substitute
{       
    /* The derivation that built this store path (empty if none). */
    Path deriver;
    
    /* Program to be executed to create the store path.  Must be in
       the output path of `storeExpr'. */
    Path program;

    /* Extra arguments to be passed to the program (the first argument
       is the store path to be substituted). */
    Strings args;

    bool operator == (const Substitute & sub) const;
};

typedef list<Substitute> Substitutes;


/* Open the database environment.  If `reserveSpace' is true, make
   sure that a big empty file exists in /nix/var/nix/db/reserved.  If
   `reserveSpace' is false, delete this file if it exists.  The idea
   is that on normal operation, the file exists; but when we run the
   garbage collector, it is deleted.  This is to ensure that the
   garbage collector has a small amount of disk space available, which
   is required to open the Berkeley DB environment. */
void openDB(bool reserveSpace = true);

/* Create the required database tables. */
void initDB();

/* Get a transaction object. */
void createStoreTransaction(Transaction & txn);

/* Copy a path recursively. */
void copyPath(const Path & src, const Path & dst);

/* Register a substitute. */
void registerSubstitute(const Transaction & txn,
    const Path & srcPath, const Substitute & sub);

/* Return the substitutes for the given path. */
Substitutes querySubstitutes(const Transaction & txn, const Path & srcPath);

/* Deregister all substitutes. */
void clearSubstitutes();

/* Register the validity of a path, i.e., that `path' exists, that the
   paths referenced by it exists, and in the case of an output path of
   a derivation, that it has been produced by a succesful execution of
   the derivation (or something equivalent).  Also register the hash
   of the file system contents of the path.  The hash must be a
   SHA-256 hash. */
void registerValidPath(const Transaction & txn,
    const Path & path, const Hash & hash, const PathSet & references,
    const Path & deriver);

struct ValidPathInfo 
{
    Path path;
    Path deriver;
    Hash hash;
    PathSet references;
};

typedef list<ValidPathInfo> ValidPathInfos;

void registerValidPaths(const Transaction & txn,
    const ValidPathInfos & infos);

/* Throw an exception if `path' is not directly in the Nix store. */
void assertStorePath(const Path & path);

bool isInStore(const Path & path);
bool isStorePath(const Path & path);

void checkStoreName(const string & name);

/* Chop off the parts after the top-level store name, e.g.,
   /nix/store/abcd-foo/bar => /nix/store/abcd-foo. */
Path toStorePath(const Path & path);

/* "Fix", or canonicalise, the meta-data of the files in a store path
   after it has been built.  In particular:
   - the last modification date on each file is set to 0 (i.e.,
     00:00:00 1/1/1970 UTC)
   - the permissions are set of 444 or 555 (i.e., read-only with or
     without execute permission; setuid bits etc. are cleared)
   - the owner and group are set to the Nix user and group, if we're
     in a setuid Nix installation. */
void canonicalisePathMetaData(const Path & path);

/* Checks whether a path is valid. */ 
bool isValidPathTxn(const Transaction & txn, const Path & path);
bool isValidPath(const Path & path);

/* Queries the hash of a valid path. */ 
Hash queryPathHash(const Path & path);

/* Sets the set of outgoing FS references for a store path.  Use with
   care! */
void setReferences(const Transaction & txn, const Path & storePath,
    const PathSet & references);

/* Queries the set of outgoing FS references for a store path.  The
   result is not cleared. */
void queryReferences(const Transaction & txn,
    const Path & storePath, PathSet & references);

/* Queries the set of incoming FS references for a store path.  The
   result is not cleared. */
void queryReferrers(const Transaction & txn,
    const Path & storePath, PathSet & referrers);

/* Sets the deriver of a store path.  Use with care! */
void setDeriver(const Transaction & txn, const Path & storePath,
    const Path & deriver);

/* Query the deriver of a store path.  Return the empty string if no
   deriver has been set. */
Path queryDeriver(const Transaction & txn, const Path & storePath);

/* Constructs a unique store path name. */
Path makeStorePath(const string & type,
    const Hash & hash, const string & suffix);
    
/* Copy the contents of a path to the store and register the validity
   the resulting path.  The resulting path is returned. */
Path addToStore(const Path & srcPath);

/* Like addToStore(), but for pre-adding the outputs of fixed-output
   derivations. */
Path addToStoreFixed(bool recursive, string hashAlgo, const Path & srcPath);

Path makeFixedOutputPath(bool recursive,
    string hashAlgo, Hash hash, string name);

/* Like addToStore, but the contents written to the output path is a
   regular file containing the given string. */
Path addTextToStore(const string & suffix, const string & s,
    const PathSet & references);

/* Delete a value from the nixStore directory. */
void deleteFromStore(const Path & path, unsigned long long & bytesFreed);

void verifyStore(bool checkContents);


#endif /* !__STORE_H */
