#ifndef __GC_H
#define __GC_H

#include "storeexpr.hh"


/* Determine the set of "live" store paths, given a set of root store
   expressions.  The live store paths are those that are reachable
   from the roots.  The roots are reachable by definition.  Any path
   mentioned in a reachable store expression is also reachable.  If a
   derivation store expression is reachable, then its successor (if it
   exists) if also reachable.  It is not an error for store
   expressions not to exist (since this can happen on derivation store
   expressions, for instance, due to the substitute mechanism), but
   successor links are followed even for non-existant derivations. */
PathSet findLivePaths(const Paths & roots);

/* Given a set of "live" store paths, determine the set of "dead"
   store paths (which are simply all store paths that are not in the
   live set). */
PathSet findDeadPaths(const PathSet & live);


#endif /* !__GC_H */
