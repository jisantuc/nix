#include "build.hh"


Derivation derivationFromPath(const Path & drvPath)
{
    assertStorePath(drvPath);
    ensurePath(drvPath);
    ATerm t = ATreadFromNamedFile(drvPath.c_str());
    if (!t) throw Error(format("cannot read aterm from `%1%'") % drvPath);
    return parseDerivation(t);
}


void computeFSClosure(const Path & storePath,
    PathSet & paths, bool flipDirection)
{
    if (paths.find(storePath) != paths.end()) return;
    paths.insert(storePath);

    PathSet references;
    if (flipDirection)
        queryReferers(storePath, references);
    else
        queryReferences(storePath, references);

    for (PathSet::iterator i = references.begin();
         i != references.end(); ++i)
        computeFSClosure(*i, paths, flipDirection);
}


void storePathRequisites(const Path & storePath,
    bool includeOutputs, PathSet & paths)
{
    checkInterrupt();
    
    if (paths.find(storePath) != paths.end()) return;

    if (isDerivation(storePath)) {

        paths.insert(storePath);
        
        Derivation drv = derivationFromPath(storePath);

        for (DerivationInputs::iterator i = drv.inputDrvs.begin();
             i != drv.inputDrvs.end(); ++i)
            /* !!! Maybe this is too strict, since it will include
               *all* output paths of the input derivation, not just
               the ones needed by this derivation. */
            storePathRequisites(i->first, includeOutputs, paths);

        for (PathSet::iterator i = drv.inputSrcs.begin();
             i != drv.inputSrcs.end(); ++i)
            storePathRequisites(*i, includeOutputs, paths);

        if (includeOutputs) {

            for (DerivationOutputs::iterator i = drv.outputs.begin();
                 i != drv.outputs.end(); ++i)
                if (isValidPath(i->second.path))
                    storePathRequisites(i->second.path, includeOutputs, paths);

        }
        
    }

    else {
        computeFSClosure(storePath, paths);
    }
}
