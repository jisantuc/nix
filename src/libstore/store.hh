#ifndef __STORE_H
#define __STORE_H

#include <string>

#include "hash.hh"
#include "db.hh"

using namespace std;


/* A substitute is a program invocation that constructs some store
   path (typically by fetching it from somewhere, e.g., from the
   network). */
struct Substitute
{
    /* Program to be executed to create the store path.  Must be in
       the output path of `storeExpr'. */
    Path program;

    /* Extra arguments to be passed to the program (the first argument
       is the store path to be substituted). */
    Strings args;

    bool operator == (const Substitute & sub);
};

typedef list<Substitute> Substitutes;


/* Open the database environment. */
void openDB();

/* Create the required database tables. */
void initDB();

/* Get a transaction object. */
void createStoreTransaction(Transaction & txn);

/* Copy a path recursively. */
void copyPath(const Path & src, const Path & dst);

/* Register a substitute. */
typedef list<pair<Path, Substitute> > SubstitutePairs;
void registerSubstitutes(const Transaction & txn,
    const SubstitutePairs & subPairs);

/* Return the substitutes expression for the given path. */
Substitutes querySubstitutes(const Path & srcPath);

/* Deregister all substitutes. */
void clearSubstitutes();

/* Register the validity of a path, i.e., that `path' exists, that the
   paths referenced by it exists, and in the case of an output path of
   a derivation, that it has been produced by a succesful execution of
   the derivation (or something equivalent).  Also register the hash
   of the file system contents of the path.  The hash must be a
   SHA-256 hash. */
void registerValidPath(const Transaction & txn,
    const Path & path, const Hash & hash);

/* Throw an exception if `path' is not directly in the Nix store. */
void assertStorePath(const Path & path);

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
bool isValidPath(const Path & path);

/* Sets the set of outgoing FS references for a store path. */
void setReferences(const Transaction & txn, const Path & storePath,
    const PathSet & references);

/* Queries the set of outgoing FS references for a store path.  The
   result is not cleared. */
void queryReferences(const Path & storePath, PathSet & references);

/* Constructs a unique store path name. */
Path makeStorePath(const string & type,
    const Hash & hash, const string & suffix);
    
/* Copy the contents of a path to the store and register the validity
   the resulting path.  The resulting path is returned. */
Path addToStore(const Path & srcPath);

/* Like addToStore, but the contents written to the output path is a
   regular file containing the given string. */
Path addTextToStore(const string & suffix, const string & s);

/* Delete a value from the nixStore directory. */
void deleteFromStore(const Path & path);

void verifyStore();


#endif /* !__STORE_H */
