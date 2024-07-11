#ifndef _DATABASE__
#define _DATABASE__

#include <string>
#include <unordered_map>
#include <vector>

enum class DB_STORE
{
    UNSTABLE = 0,
    STABLE = 1
};


/**
 * class DataBase
 * 
 * The abstract class DataBase provide an unified 
 * interface for managing any king of database 
 * that supports key-value pair
 */
class DataBase
{
protected:
    std::string _dbInstance;

public:
    /**
     * Open a database connection
     * @param id represents the id of connection
     * @return the status code of the operation, 0 if the operation was successful
     */
    virtual int Open(const std::string id = {}) = 0;

    /**
     * Get the value associate to the given key from the database
     * @param key represents a key in the database
     * @return the value associate to the key if the key is present or an empty string
     */
    virtual std::string Get(const std::string key) = 0;

    /**
     * Put a key-value pair in the database
     * @param key represents a key in the database
     * @param value represent the new value that should be associate to the key
     * @return the previous value associate to the key
     */
    virtual std::string Put(const std::string key, const std::string value) = 0;

    // commit all the transactions up to this idx. 
    virtual int Commit(const int idx) = 0;

    /**
     * Select a new active table for the database
     * @param tableName is the name of the table that will be activate
     * @return the status code of the operation, 0 if the operation was successful
     */
    virtual int SelectTable(const std::string tableName) = 0;

    /**
     * Close a database connection
     * @param id represents the id of connection
     * @return the status code of the operation, 0 if the operation was successful
     */
    virtual int Close(const std::string id = {}) = 0;

    /**
     * dbInstance return a string representing the 
     * type of the concrete database instace
     *
     * @return the db type
     */
    std::string dbInstance()
    {
        return _dbInstance;
    }
};


class InMemoryDB : public DataBase
{
private:
    using dbTable = std::unordered_map<std::string, std::string>;

    std::string activeTable;
    std::unordered_map<std::string, dbTable> *db;
    std::unordered_map<std::string, dbTable> *db_stable;

public:
    InMemoryDB();
    int Open(const std::string = "db");
    std::string Get(const std::string key);
    std::string GetStable(const std::string key);
    std::string Put(const std::string key, const std::string value);
    std::string PutStable(const std::string key, const std::string value);
    int SelectTable(const std::string tableName);
    int Close(const std::string = "db");
    // commit all the txns from the beginning to the end_idx in the log to the db_stable. 
    int Commit(const int begin_idx, const int end_idx, Log &log);   
    // execute a particular command
    std::string Execute(DB_STORE which_db, byte *req, uint32_t req_len);
};

#endif