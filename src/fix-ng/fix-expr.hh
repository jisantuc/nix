#ifndef __FIXEXPR_H
#define __FIXEXPR_H

#include <map>

#include <aterm2.h>

#include "util.hh"


/* Fix expressions are represented as ATerms.  The maximal sharing
   property of the ATerm library allows us to implement caching of
   normals forms efficiently. */
typedef ATerm Expr;


/* Mappings from ATerms to ATerms.  This is just a wrapper around
   ATerm tables. */
class ATermMap
{
private:
    unsigned int maxLoadPct;
    ATermTable table;
    
public:
    ATermMap(unsigned int initialSize = 16, unsigned int maxLoadPct = 75);
    ATermMap(const ATermMap & map);
    ~ATermMap();

    void set(ATerm key, ATerm value);
    void set(const string & key, ATerm value);

    ATerm get(ATerm key) const;
    ATerm get(const string & key) const;

    void remove(ATerm key);
    void remove(const string & key);

    ATermList keys() const;
};


/* Convert a string to an ATerm (i.e., a quoted nullary function
   applicaton). */
ATerm string2ATerm(const string & s);
string aterm2String(ATerm t);

/* Generic bottomup traversal over ATerms.  The traversal first
   recursively descends into subterms, and then applies the given term
   function to the resulting term. */
struct TermFun
{
    virtual ATerm operator () (ATerm e) = 0;
};
ATerm bottomupRewrite(TermFun & f, ATerm e);

/* Query all attributes in an attribute set expression.  The
   expression must be in normal form. */
void queryAllAttrs(Expr e, ATermMap & attrs);

/* Query a specific attribute from an attribute set expression.  The
   expression must be in normal form. */
Expr queryAttr(Expr e, const string & name);

/* Create an attribute set expression from an Attrs value. */
Expr makeAttrs(const ATermMap & attrs);

/* Perform a set of substitutions on an expression. */
Expr substitute(const ATermMap & subs, Expr e);

/* Create an expression representing a boolean. */
Expr makeBool(bool b);


#endif /* !__FIXEXPR_H */
