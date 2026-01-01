// Code for file server
#include <iostream>
#include <cstring>
#include <string>
#include <netdb.h>
#include <sstream> 
#include <cctype>
#include <boost/regex.hpp>

#include "request.hpp"
#include "fs_server.h"


/*
 * regex statements for each request, ensuring that the input must be exactly what we expect.
 * Here is read_re explained:
 *      ^ to ensure FS_READBLOCK is the first sequence in the string
 *      ' ' exactly one space
 *      ([^ ]+) 1 or more non space characters
 *      [1-9][0-9]*|0 ensures the block is in the correct format
 *          -- [1-9] ensures no leading zeros
 *          -- [0-9]* zero or more integers
 *          -- |0 or just zero
 */

static const boost::regex read_re{
    R"(^(FS_READBLOCK) ([^ ]+) (/[^ ]+) ([1-9][0-9]*|0)$)"
};

static const boost::regex write_re{
    R"(^(FS_WRITEBLOCK) ([^ ]+) (/[^ ]+) ([1-9][0-9]*|0)$)"
};

static const boost::regex create_re{
    R"(^(FS_CREATE) ([^ ]+) (/[^ ]+) ([fd])$)"
};

static const boost::regex delete_re{
    R"(^(FS_DELETE) ([^ ]+) (/[^ ]+)$)"
};


bool parse_request(std::string &header, request &out){
    // the object that will hold the contents if there is a match
    // m[0] is the entire capture 
    // 1, n is the other caputes
    boost::smatch m;

    if(boost::regex_match(header, m, read_re)){
        out.type     = FS_READBLOCK;
        if(!fill_user_and_path(m, out)) return false;
        if(!fill_block(m, out))         return false;
    } else if(boost::regex_match(header, m, write_re)){
        out.type     = FS_WRITEBLOCK;
        if(!fill_user_and_path(m, out)) return false;
        if(!fill_block(m, out))         return false;
    } else if(boost::regex_match(header, m, create_re)){
        out.type        = FS_CREATE;
        if(!fill_user_and_path(m, out)) return false;
        out.create_type = m[4].str()[0];
    } else if(boost::regex_match(header, m, delete_re)){
        out.type        = FS_DELETE;
        if(!fill_user_and_path(m, out)) return false;
    } else {
        // else its invalid input
        return false;
    }
    out.header = header;
    return true;
} // parse_request()

bool fill_user_and_path(const boost::smatch &m, request &out) { 
    out.username    = m[2];
    if (out.username.empty() || 
        out.username.size() > FS_MAXUSERNAME ||
        has_space(out.username)) {
        return false;
    }

    out.pathname = m[3];
    if (has_space(out.pathname)) {
        return false;
    }

    out.path = split_path_ss(out.pathname);
    if (out.path.empty()) {
        return false;
    }
    return true;
}

bool fill_block(const boost::smatch &m, request &out) {
    out.block = std::stoll(m[4]);
    if (out.block < 0 || 
        static_cast<unsigned int>(out.block) >= FS_MAXFILEBLOCKS) {
            return false;
        }
    return true;
}

std::deque<std::string> split_path_ss(const std::string &path) {
    std::deque<std::string> d;
    // skip initial /
    std::stringstream ss(path.substr(1)); 
    std::string step;

    // should always begin with / 
    if(path.empty() || path[0] != '/') {
        return {};
    }

    // should not end with /
    if(path.size() > 1 && path.back() == '/') {
        return {};
    }
    // max path limit 
    if(path.size() > FS_MAXPATHNAME) {
        return {};
    }

    while(std::getline(ss, step, '/')){
        // if its a // or its too big return error
        if (step.empty() || step.size() > FS_MAXFILENAME) {
            return {};
        }
        d.push_back(step);
    }
    return d;
} // split_path_ss

bool has_space(std::string& s) {
    for (unsigned char c: s) {
        if (std::isspace(c)) {
            return true;
        }
    }
    return false;
}
