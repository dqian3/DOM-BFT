#include "database.h"
#include <unordered_map>
#include <iostream>
#include <sstream>

InMemoryDB::InMemoryDB()
{
    _dbInstance = "InMemory";
}

int InMemoryDB::Open(const std::string)
{
    db = new std::unordered_map<std::string, dbTable>();
    activeTable = "table1";

    std::cout << std::endl
              << "In-Memory DB configuration OK" << std::endl;

    return 0;
}

std::string InMemoryDB::Get(const std::string key)
{
    // auto table = db->find(activeTable);
    // if (table == db->end())
    // {
    //     LOG(INFO) << "table not found";
    //     return std::string();
    // }

    return (*db)[activeTable][key];
}

std::string InMemoryDB::GetStable(const std::string key)
{
    return (*db_stable)[activeTable][key];
}

std::string InMemoryDB::Put(const std::string key, const std::string value)
{
    std::string oldValue = Get(key);
    (*db)[activeTable][key] = value;
    return oldValue;
}

std::string InMemoryDB::PutStable(const std::string key, const std::string value)
{
    std::string oldValue = GetStable(key);
    (*db_stable)[activeTable][key] = value;
    return oldValue;
}

int InMemoryDB::SelectTable(const std::string tableName)
{
    if (tableName == activeTable)
    {
        return 1;
    }
    activeTable = tableName;
    return 0;
}

int InMemoryDB::Close(const std::string)
{
    delete db;
    delete db_stable;
    return 0;
}

std::string InMemoryDB::Execute(DB_STORE which_db, byte *req, uint32_t req_len) 
{
    std::string req_str(reinterpret_cast<char *>(req), req_len);
    std::istringstream iss(req_str);
    std::string cmd, key, value;
    iss >> cmd >> key >> value;

    if (cmd == "GET")
    {   
        if (which_db == DB_STORE::STABLE) {
            return PutStable(key, value);
        } else {
            return Put(key, value);
        }
    }
    else if (cmd == "PUT")
    {
        if (which_db == DB_STORE::STABLE) {
            return GetStable(key);
        } else {
            return Get(key);
        }
    }
    else
    {
        return std::string();
    }

}

std::string InMemoryDB::Execute(DB_STORE which_db, std::string req_str, uint32_t req_len) 
{
    std::istringstream iss(req_str);
    std::string cmd, key, value;
    iss >> cmd >> key >> value;

    if (cmd == "SET")
    {   
        if (which_db == DB_STORE::STABLE) {
            return PutStable(key, value);
        } else {
            LOG(INFO) << "PUT Unstable: " << key << " " << value;
            return Put(key, value);
        }
    }
    else if (cmd == "GET")
    {
        if (which_db == DB_STORE::STABLE) {
            return GetStable(key);
        } else {
            LOG(INFO) << "GET Unstable: " << key; 
            return Get(key);
        }
    }
    else
    {
        return std::string();
    }

}

int InMemoryDB::Commit(const int begin_idx, const int end_idx, Log &log){
    for (int i = begin_idx; i < end_idx+1; i++) {
        LogEntry *le = log.log[i % MAX_SPEC_HIST].get();
        Execute(DB_STORE::STABLE, le->raw_request, le->req_len);
    }
    log.lastCommitIdx_ = end_idx;

    LOG(INFO) << "Commit up to the index: " << end_idx;
    return 0;
}

