#ifndef __SHARED_H
#define __SHARED_H

#include "types.hh"

#include <signal.h>


/* These are not implemented here, but must be implemented by a
   program linking against libmain. */

/* Main program.  Called by main() after the ATerm library has been
   initialised and some default arguments have been processed (and
   removed from `args').  main() will catch all exceptions. */
void run(nix::Strings args);

/* Should print a help message to stdout and return. */
void printHelp();

extern std::string programId;


namespace nix {

/* Ugh.  No better place to put this. */
Path makeRootName(const Path & gcRoot, int & counter);
void printGCWarning();

unsigned int getIntArg(const string & opt,
    Strings::iterator & i, const Strings::iterator & end);

/* Whether we're running setuid. */
extern bool setuidMode;

extern volatile ::sig_atomic_t blockInt;

MakeError(UsageError, nix::Error);

struct RemoveTempRoots 
{
    ~RemoveTempRoots();    
};

}


#endif /* !__SHARED_H */
