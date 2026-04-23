/*******************************************************************************
 * This originates from Pixslam
 * Original Author is Luke Dodd
 ********************************************************************************/

#pragma once
#ifndef PARSER_INCL
#define PARSER_INCL

#include <vector>
#include <string>
#include <cstdint>

namespace gnine
{

    struct Cell
    {
        enum Type
        {
            Symbol,
            Number,
            List
        };
        typedef Cell (*proc_type)(const std::vector<Cell> &);
        typedef std::vector<Cell>::const_iterator iter;
        Type type;
        std::string val;
        std::vector<Cell> list;
        mutable bool cacheKeyValid;
        mutable uint64_t cacheKeyValue;
        Cell(Type type = Symbol) : type(type), cacheKeyValid(false), cacheKeyValue(0) {}
        Cell(Type type, const std::string &val) : type(type), val(val), cacheKeyValid(false), cacheKeyValue(0) {}
    };

    // convert given Cell to a Lisp-readable string
    std::string cellToString(const Cell &exp);

    // return a stable structural cache key for a Cell without allocating the full text form
    std::string cellCacheKey(const Cell &exp);

    // return the Lisp expression represented by the given string
    Cell cellFromString(const std::string &s);

}

#endif // !defined(PARSER_INCL)
