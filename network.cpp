#include <iostream>
#include <cstring>
#include <stdexcept>
#include <string>
#include <set>
#include <deque>
#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <cstdlib> 
#include <optional>
#include <memory>
#include <unordered_map>

#include "network.hpp"
#include "request.hpp"
#include "fs_server.h"

/***************************************************************************************************
 *                                             Network                                             *
 ***************************************************************************************************/

/* function docs are in the header file */

Network::Network(int port_in) : portnum(port_in) {}


void Network::start_server() {
    sys_init();

    sockfd = socket(PF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sockfd < 0) {
        throw std::runtime_error("socket() failed");
    }
    // configure the socket to reuse local addresses
    int yesval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yesval, sizeof(yesval)) < 0) {
        throw std::runtime_error("setsockopt() failed");
    }

    // socket is created and configured, bind the socket
    addr.sin_family         = AF_INET; // Address Family Internet (IPV4)
    addr.sin_addr.s_addr    = htonl(INADDR_ANY);
    addr.sin_port           = htons(portnum);

    if (bind(sockfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error("bind() failed");
    }

    // check to see if OS needs to choose the portnumber 
    get_port_number(sockfd);

    if (listen(sockfd, BACKLOG) < 0) {
        throw std::runtime_error("syscall to listen() failed");
    }

    print_port(portnum);
    // Handle all requests from clients
    while (true) {
        // accept a connection to the server socket
        int connection_sock = accept(sockfd, nullptr, nullptr);
        if (connection_sock < 0) {
            throw std::runtime_error("syscall to accept() failed");
        }

        boost::thread t(&Network::handle_request, this, connection_sock);
        t.detach();
    }

} // Network::start_server()

void Network::handle_request(int connection_sock) {
    try {
        std::string header = receive_data(connection_sock);

        request request;
        if(!parse_request(header, request)){
            // Malformed request
            close(connection_sock);
            return;
        };
        // Handle the data correctly
        switch (request.type) {
            case FS_READBLOCK:
                read_block(request, connection_sock);
                break;
            case FS_WRITEBLOCK: {
                // need to recieve the data to write
                ssize_t n = recv(connection_sock, request.buf, FS_BLOCKSIZE, MSG_WAITALL); 
                // MSG_WAITALL will gauruntee we get enough byte unless the client does not send that much or 
                // closes the connection 
                if (n != FS_BLOCKSIZE) {
                    close(connection_sock);
                    return; // did not recieve the right amount
                }
                write_block(request, connection_sock);
                break;
            }
            case FS_CREATE:
                sys_create(request, connection_sock);
                break;
            case FS_DELETE:
                sys_delete(request, connection_sock);
                break;
        }

        // All went well, close the connection with client
        close(connection_sock);

    } catch (...) {
        close(connection_sock);
    }
} // Network::handle_request

void Network::sys_init() {
    // fill free disk blocks
    for(size_t i = 0; i < FS_DISKSIZE; ++i){
        free_disk_blocks.insert(i);
    }
    // interatively explore the file system and remove them from the set
    std::deque<uint32_t> d;
    // always start at the root
    d.push_back(0);

    // dfs to attempt to reduce worst case space complexity
    while(!d.empty()){
        uint32_t curr_block = d.back();
        d.pop_back();

        fs_inode curr_inode;
        read_inode_block(curr_block, curr_inode);
        free_disk_blocks.erase(curr_block);

        // this inode is a directory
        if (curr_inode.type == 'd') {
            for(size_t i = 0; i < curr_inode.size; ++i) { 
                uint32_t data_block = curr_inode.blocks[i];
                // unused block
                if (data_block == 0){
                    continue;
                }
                free_disk_blocks.erase(data_block);
                fs_direntry entries[FS_DIRENTRIES];
                disk_readblock(data_block , entries);
                for (size_t j = 0; j < FS_DIRENTRIES; ++j) {
                    uint32_t child_block = entries[j].inode_block;
                    // unused block
                    if (child_block == 0) {
                        continue; 
                    }
                    d.push_back(child_block);
                }
            }
        } else if (curr_inode.type == 'f') {
            for(size_t i = 0; i < curr_inode.size; ++i) { 
                // this block is being used
                free_disk_blocks.erase(curr_inode.blocks[i]);
            }
        }
    }
}

void Network::read_block(request &request, int socket) {

    // traverse the path and find if it exists, check if the username checks out, send message w data
    path_find_info<shared_lock> lock_info;
    int target_inode_block = path_find(request.path, request.username, &lock_info);
    // file does not exist
    if (target_inode_block == -1) {
        return;
    } 

    fs_inode target_inode;
    read_inode_block(target_inode_block, target_inode);

    // we cant read a directory block and must be proper owner
    if(target_inode.type != 'f' 
        || std::string(target_inode.owner) != request.username){ 
        return;
    }
    // file does not have that many blocks
    if (request.block >= static_cast<int>(target_inode.size) || target_inode.blocks[request.block] == 0) {
        return;
    }

    // success read the block and send a response
    char data[FS_BLOCKSIZE];

    disk_readblock(target_inode.blocks[request.block], data);

    lock_info.lock.unlock();

    send_all(socket, request.header.data(), request.header.size() + 1);
    send_all(socket, data, FS_BLOCKSIZE);
}

void Network::write_block(request &request, int socket) {

    path_find_info<upgrade_lock> lock_info;
    int target_inode_block = path_find_upgrade(request.path, request.username, &lock_info);
    
    if (target_inode_block == -1) {
        return;
    }

    fs_inode target_inode;
    read_inode_block(target_inode_block, target_inode);

    // not allowed to write more than 1 block past size
    if (request.block > static_cast<int>(target_inode.size)) {
        return;
    }

    // cant write to a file not the owner and not the root
    if (target_inode.type != 'f' || (std::string(target_inode.owner) != request.username)) {
        return;
    }

    bool extends_file = (request.block == static_cast<int>(target_inode.size));
    
    if (!extends_file) {
        unique_lock write_lock(std::move(lock_info.lock));
        disk_writeblock(target_inode.blocks[request.block], request.buf);
    } else {           
        if (target_inode.size >= FS_MAXFILEBLOCKS) {
            return;
        }
        int b = get_new_block();
        if (b == -1){
            return; 
        }
        uint32_t next_block = static_cast<uint32_t>(b);
        // trying to write to the next block
        target_inode.blocks[target_inode.size] = next_block;
        target_inode.size++;
        // data first
        disk_writeblock(next_block, request.buf);

        unique_lock write_lock(std::move(lock_info.lock));
        // Then inode -- We just changed this inode, we have to now write it back
        disk_writeblock(target_inode_block, &target_inode);
    }
    send_all(socket, request.header.data(), request.header.size() + 1);
}

void Network::sys_create(request &request, int socket) {
    // the new file/directory
    std::string new_name = request.path.back();
    request.path.pop_back();

    path_find_info<upgrade_lock> parent_lm;
    int parent_inode_block = path_find_upgrade(request.path, request.username, &parent_lm);
    // path does not exist
    if (parent_inode_block == -1) {
        return;
    }

    fs_inode parent_inode;
    read_inode_block(parent_inode_block, parent_inode);
    // cant make a new file or directory in a file -- not the owner and not the root
    if (parent_inode.type != 'd' || (std::string(parent_inode.owner) != request.username && 
        std::string(parent_inode.owner) != "")) {
        return;
    }
    create_scan_info scan = scan_directory_for_create(parent_inode, new_name);

    // should not exist already exist
    if (scan.exists) {
        return; 
    }

    bool found = scan.has_open_entry;  // if we found an open entry
    int slot_block = 0;                // the index in the array of direntrys this will go in 
    int slot_offset = 0;               // the offset into that bloc this will go in 
    char dbuf[FS_BLOCKSIZE];           // our buffer of writing stuff

    fs_direntry* entries    = nullptr;  // points to the direntry array we are ediiting
    char* write_buf = nullptr;          // raw pointer for disk_write
    uint32_t dir_data_block = 0;        // what block the dir_block is at
    uint32_t new_dir_block = 0;         // new dir block
    
    // if none are found we need to allocate a new direntry block
    if (found) {
        slot_block = scan.open_parent_blocks_idx;
        slot_offset = scan.open_dir_offset;
        dir_data_block = parent_inode.blocks[slot_block];
        entries = scan.open_dir_page;
        write_buf = reinterpret_cast<char*>(scan.open_dir_page);
    } else {
        // already at max size
        if(parent_inode.size >= FS_MAXFILEBLOCKS) {
            return;
        }
        // get new block for new dir page
        int next_block = get_new_block();
        if (next_block == -1){
            return; // failure
        }
        new_dir_block = static_cast<uint32_t>(next_block);
        slot_block = parent_inode.size;
        slot_offset = 0;
        // We have to zero this out becuase we check in other places that if its not being used .inode_block == 0
        std::memset(dbuf, 0, FS_BLOCKSIZE);
        dir_data_block = new_dir_block;
        entries = reinterpret_cast<fs_direntry*>(dbuf); 
        write_buf = dbuf;
    }
    // need a new block 
    int b = get_new_block();
    if (b == -1){
        if (!found) {  
            boost::lock_guard<boost::mutex> g(free_disk_mutex);
            free_disk_blocks.insert(new_dir_block);
        }
        return; // faliure so we must return the block we took 
    } 
    uint32_t new_inode_block = static_cast<uint32_t>(b);

    // Craete the new inode then write it FIRST ensures proper ordering
    fs_inode new_inode{};
    new_inode.type = request.create_type; // f or d
    std::strncpy(new_inode.owner, request.username.c_str(), FS_MAXUSERNAME); // ensure its null terminated
    new_inode.owner[FS_MAXUSERNAME] = '\0';
    new_inode.size = 0;
    disk_writeblock(new_inode_block, &new_inode);

    // fill in the direntry
    std::strncpy(entries[slot_offset].name, new_name.c_str(), FS_MAXFILENAME);
    entries[slot_offset].name[FS_MAXFILENAME] = '\0';
    entries[slot_offset].inode_block = new_inode_block; 

    // if we got a new direntry block, update the parent
    if (!found) {
        // everything went well we can write the pointer
        parent_inode.blocks[slot_block] = new_dir_block;
        parent_inode.size++;
        disk_writeblock(dir_data_block, write_buf);
        unique_lock parent_write_lock(std::move(parent_lm.lock));
        disk_writeblock(parent_inode_block, &parent_inode);
    } else {
        unique_lock parent_write_lock(std::move(parent_lm.lock));
        disk_writeblock(dir_data_block, write_buf);
    }

    send_all(socket, request.header.data(), request.header.size() + 1);
}


 /*
  * 1. Grab upgradeble lock for parent
  * 2. I read the parent
  * 3. Upgrade the parent to a unique lock 
  * 4. Grab updradable lock for child
  * 5. I read the child with 2 upgradable locks 
  * 6. Write to Parents Direntries
  * 7. Drop Parent lock
  * 8. Ugprade the lock for the child 
  * 9. Grab free disk mutex free disk block for child 
  * 10. Drop lock for the child
  * 11. Send all 
  * 
 */
void Network::sys_delete(request &request, int socket) {
    // the file/directory to delete
    std::string target_file = request.path.back();
    request.path.pop_back();

    path_find_info<upgrade_lock> parent_lm;
    int parent_inode_block = path_find_upgrade(request.path, request.username, &parent_lm);

    // path does not exist
    if (parent_inode_block == -1) {
        return;
    }

    fs_inode parent_inode;
    read_inode_block(parent_inode_block, parent_inode);

    // not directory or not proper owner ship
    if (parent_inode.type != 'd' || (std::string(parent_inode.owner) != request.username && 
        std::string(parent_inode.owner) != "")) {
        return;
    }

    delete_scan_info scan = scan_directory_for_delete(parent_inode, target_file);

    // target does not exist
    if(!scan.found) {
        return; 
    }
    
    int target_inode_block = scan.inode_block;
    // aquire write lock for the target
    auto target_mtx_sp = get_inode_mutex_sp(static_cast<uint32_t>(target_inode_block));

    unique_lock parent_write_lock(std::move(parent_lm.lock));
    upgrade_lock target_up_lock(*target_mtx_sp);

    fs_inode target_inode;
    read_inode_block(target_inode_block, target_inode);

    // need proper ownership
    if ((std::string(target_inode.owner) != request.username)) {
        return;
    }

    // ensure that this file exist OR it is an empty directory
    if (target_inode.type == 'd' && target_inode.size > 0) {
        return;
    }

    // If its the last direntry also free that direntry block and send that blocks entry to = 0
    if (!scan.only_entry) {
        scan.dir_page[scan.dir_offset].inode_block = 0;
        scan.dir_page[scan.dir_offset].name[0] = '\0';
        disk_writeblock(scan.dir_block, scan.dir_page);
        parent_write_lock.unlock();
    } else {
        // Delete compression
        for(uint32_t i = static_cast<uint32_t>(scan.parent_blocks_idx); i + 1 < parent_inode.size; ++i) {
            parent_inode.blocks[i] = parent_inode.blocks[i + 1];
        }
        --parent_inode.size;
        disk_writeblock(parent_inode_block, &parent_inode);
        parent_write_lock.unlock();

        {
            boost::lock_guard<boost::mutex> g(free_disk_mutex);
            free_disk_blocks.insert(static_cast<uint32_t>(scan.dir_block));
        }
    }

    // need to free the files blocks 
    {
        unique_lock target_write_lock(std::move(target_up_lock));
        boost::lock_guard<boost::mutex> g(free_disk_mutex);
        for(uint32_t i = 0; i < target_inode.size; ++i) {
            uint32_t b = target_inode.blocks[i];
            if (b != 0) {
                free_disk_blocks.insert(b);
            }
        }
        // mark the target block as free
        free_disk_blocks.insert(target_inode_block);
    }

    
    // only have to send back the request message
    send_all(socket, request.header.data(), request.header.size() + 1);
} 

int Network::find_child(const fs_inode &dir_node, const std::string &name) {
    for (uint32_t i = 0; i < dir_node.size; ++i) {
        uint32_t block = dir_node.blocks[i];
        fs_direntry entries[FS_DIRENTRIES];
        disk_readblock(block, entries);

        for (size_t j = 0; j < FS_DIRENTRIES; ++j) {
            fs_direntry &de = entries[j];
            if (de.inode_block == 0) continue;
            if (std::string(de.name) == name) {
                return de.inode_block;
            }
        }
    }
    return -1;
}

Network::create_scan_info Network::scan_directory_for_create(const fs_inode &parent_inode, const std::string &name) {
    create_scan_info res;
    for (uint32_t i = 0; i < parent_inode.size; ++i) {
        uint32_t block = parent_inode.blocks[i];
        fs_direntry entries[FS_DIRENTRIES];
        disk_readblock(block, entries);

        for (size_t j = 0; j < FS_DIRENTRIES; ++j) {
            fs_direntry &de = entries[j];
            if (de.inode_block == 0) {
                if (!res.has_open_entry) {
                    res.has_open_entry         = true;
                    res.open_parent_blocks_idx = static_cast<int>(i);
                    res.open_dir_offset        = static_cast<int>(j);
                    std::memcpy(res.open_dir_page, entries, FS_BLOCKSIZE);
                }
                continue;
            }
            if (std::string(de.name) == name) {
                res.exists = true;
                return res;
            }
        }
    }
    return res;  
}

Network::delete_scan_info Network::scan_directory_for_delete(const fs_inode &parent_inode, const std::string &name){
    delete_scan_info res;
    for (uint32_t i = 0; i < parent_inode.size; ++i) {
        uint32_t block = parent_inode.blocks[i];
        fs_direntry entries[FS_DIRENTRIES];
        disk_readblock(block, entries);

        int count        = 0;
        int target_block = -1;
        for (size_t j = 0; j < FS_DIRENTRIES; ++j) {
            const fs_direntry &de = entries[j];
            if (de.inode_block == 0) continue;
            ++count;
            if (!res.found && std::string(de.name) == name) {
                res.found = true;
                res.inode_block       = static_cast<int>(de.inode_block);
                res.parent_blocks_idx = static_cast<int>(i);
                res.dir_block         = static_cast<int>(block);
                std::memcpy(res.dir_page, entries, FS_BLOCKSIZE);      
                res.dir_offset        = static_cast<int>(j);
                target_block          = static_cast<int>(j);
            }
        }

        if (res.found && target_block != -1) {
            res.only_entry = (count == 1);
            return res;
        }
    }
    return res;  
}

template <typename LockT>
int Network::path_find_impl(std::deque<std::string> &path, std::string &user, path_find_info<LockT>* out_info) {

    // if path is empty is looking for the root
    if (path.empty()){
        auto root_mtx = get_inode_mutex_sp(0);
        LockT tmp(*root_mtx);
        out_info->mtx_sp = std::move(root_mtx);
        out_info->lock = std::move(tmp);
        return 0;
    }
    uint32_t curr_block = 0;

    // need to first acquire the lock for the root
    auto curr_mtx_sp = get_inode_mutex_sp(curr_block);
    inode_read_block walker(*curr_mtx_sp);
    while(!path.empty()){
        std::string target = path.front();
        path.pop_front();

        fs_inode curr_inode;
        read_inode_block(curr_block, curr_inode);

        // if we are still looking for our target it should be a directory and we shoudl have permmission
        if (curr_inode.type != 'd' || ((std::string(curr_inode.owner) != user) && std::string(curr_inode.owner) != "")) { 
            return -1;
        }
        int child_block = find_child(curr_inode, target);

        if (child_block == -1) {
            return -1;
        }

        auto child_mtx_sp = get_inode_mutex_sp(static_cast<uint32_t>(child_block));
        // hand over hand locking
        if (!path.empty()) {
            walker.hand_over(*child_mtx_sp);
            curr_mtx_sp = std::move(child_mtx_sp);
        } else {
            LockT final_lock(*child_mtx_sp);
            out_info->lock = std::move(final_lock);
            out_info->mtx_sp = std::move(child_mtx_sp); // move lock to caller
        }
        curr_block = static_cast<uint32_t>(child_block);
    }

    return static_cast<int>(curr_block);
}
    
int Network::path_find(std::deque<std::string> &path, std::string &user, path_find_info<shared_lock>* out_info) {
    return path_find_impl<shared_lock>(
        path, user, out_info
    );
}

int Network::path_find_upgrade(std::deque<std::string> &path, std::string &user, path_find_info<upgrade_lock>* out_info) {
    return path_find_impl<upgrade_lock>(
        path, user, out_info
    );
}

void Network::send_all(int sockfd, const void* buf, size_t len) {
    const char *p = static_cast<const char*>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(sockfd, p + total, len - total, MSG_NOSIGNAL); // spec says MSG_NOSIGNAL
        // either the send failed or the user bailed
        if (n <= 0) {
            return; 
        }
        total += static_cast<size_t>(n);
    }
}

void Network::get_port_number(int sockfd) {
    if (portnum == 0) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        
        if (getsockname(sockfd, reinterpret_cast<sockaddr *>(&addr), &len) < 0) {
            throw std::runtime_error("syscall to getsockname() failed");
        }
    
        portnum = ntohs(addr.sin_port);
    }
} // Network::get_port_number()

std::string Network::receive_data(int connection_sock) {
    // Read only a byte of data each time 
    std::string data; 
    char c = 0;
    while (true) {
        ssize_t bytes_recv = recv(connection_sock, &c, 1, 0);
        if (bytes_recv < 0) {
            throw std::runtime_error("syscall to recv() failed");
        }
        // Reached null terminator, only data has not yet been received 
        if(c == '\0') { 
            break;
        }
        data.push_back(c);
        // maximum ish size of a valid request
        if (data.size() > FS_MAXUSERNAME + FS_MAXPATHNAME + 25) {
            break;
        }
    }
    return data;
} // Network::receive_data()

void Network::read_inode_block(const int &block, fs_inode &inode) {
    char buff[FS_BLOCKSIZE];
    disk_readblock(block, buff);
    // the dereference enforces a deep assingment operator avoiding a dangling pointer
    inode = *reinterpret_cast<fs_inode*>(buff);
} // Network::read_inode_block()


// this helper will return the sp for a given inode_block
std::shared_ptr<shared_mutex> Network::get_inode_mutex_sp(uint32_t block) {
    boost::lock_guard<boost::mutex> guard(lock_table_mutex); // acquire lock for the map
    auto &weak = inode_lock_table[block];
    auto sp = weak.lock();
    // might not exist 
    if (!sp) {
        sp = std::make_shared<shared_mutex>();
        weak = sp;
    }
    return sp;
}

int Network::get_new_block() {
    boost::lock_guard<boost::mutex> g(free_disk_mutex);
    if (free_disk_blocks.empty()){
        return -1; 
    }
    uint32_t next_block = *free_disk_blocks.begin();
    free_disk_blocks.erase(next_block);
    return static_cast<int>(next_block);
}