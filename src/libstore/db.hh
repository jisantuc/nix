#ifndef __DB_H
#define __DB_H

#include <string>
#include <list>
#include <map>

#include <db_cxx.h>

#include "util.hh"

using namespace std;


class Database;


class Transaction
{
    friend class Database;

private:
    DbTxn * txn;
    
public:
    Transaction();
    Transaction(Database & _db);
    ~Transaction();

    void begin(Database & db);
    void abort();
    void commit();

    void moveTo(Transaction & t);
};


#define noTxn Transaction()


typedef unsigned int TableId; /* table handles */


class Database
{
    friend class Transaction;

private:
    DbEnv * env;

    int fdLock;
    int fdAccessors;

    TableId nextId;
    map<TableId, Db *> tables;

    void requireEnv();

    Db * getDb(TableId table);

public:
    Database();
    ~Database();
    
    void open(const string & path);
    void close();

    TableId openTable(const string & table);

    bool queryString(const Transaction & txn, TableId table, 
        const string & key, string & data);

    bool queryStrings(const Transaction & txn, TableId table, 
        const string & key, Strings & data);

    void setString(const Transaction & txn, TableId table,
        const string & key, const string & data);

    void setStrings(const Transaction & txn, TableId table,
        const string & key, const Strings & data,
        bool deleteEmpty = true);

    void delPair(const Transaction & txn, TableId table,
        const string & key);

    void enumTable(const Transaction & txn, TableId table,
        Strings & keys);
};


class DbNoPermission : public Error
{
public:
    DbNoPermission(const format & f) : Error(f) { };
};


#endif /* !__DB_H */
