#ifndef __GET_DRVS_H
#define __GET_DRVS_H

#include <string>
#include <map>

#include <boost/shared_ptr.hpp>

#include "eval.hh"


namespace nix {


struct MetaValue
{
    enum { tpNone, tpString, tpStrings, tpInt } type;
    string stringValue;
    Strings stringValues;
    int intValue;
};


typedef std::map<string, MetaValue> MetaInfo;


struct DrvInfo
{
private:
    string drvPath;
    string outPath;
    
public:
    string name;
    string attrPath; /* path towards the derivation */
    string system;

    /* !!! these should really be hidden, and setMetaInfo() should
       make a copy since the ATermMap can be shared between multiple
       DrvInfos. */
    boost::shared_ptr<ATermMap> attrs;

    string queryDrvPath(EvalState & state) const;
    string queryOutPath(EvalState & state) const;
    MetaInfo queryMetaInfo(EvalState & state) const;
    MetaValue queryMetaInfo(EvalState & state, const string & name) const;

    void setDrvPath(const string & s)
    {
        drvPath = s;
    }
    
    void setOutPath(const string & s)
    {
        outPath = s;
    }

    void setMetaInfo(const MetaInfo & meta);
};


typedef list<DrvInfo> DrvInfos;


/* Evaluate expression `e'.  If it evaluates to a derivation, store
   information about the derivation in `drv' and return true.
   Otherwise, return false. */
bool getDerivation(EvalState & state, Expr e, DrvInfo & drv);

void getDerivations(EvalState & state, Expr e, const string & pathPrefix,
    const ATermMap & autoArgs, DrvInfos & drvs);

 
}


#endif /* !__GET_DRVS_H */
