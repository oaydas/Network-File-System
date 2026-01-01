/***************************************************************************************************
 *                                             Network                                             *
 ***************************************************************************************************/
#pragma once

#include <string>
#include <set> 
#include <netinet/in.h>
#include <optional>
#include <deque>
#include <utility>
#include <memory>
#include <unordered_map>

#include <boost/thread.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <boost/thread/locks.hpp>

#include "fs_server.h"
#include "request.hpp"


static constexpr unsigned short BACKLOG = 30; 
static constexpr unsigned int BUFFER    = 1024;

/*
 * Raii class wrapper to help us with the hand over hand locking
*/

// do this to avoid deep nested namespace scope declarations
using shared_mutex = boost::shared_mutex;
using shared_lock  = boost::shared_lock<shared_mutex>;
using unique_lock  = boost::unique_lock<shared_mutex>;
using upgrade_lock = boost::upgrade_lock<shared_mutex>;

// the hand-over-hand raii wrapperg
template <typename Lock, typename Mutex>
class HandOverLock {
public: 
    explicit HandOverLock(Mutex& m) : lock_(m) {}

    HandOverLock(HandOverLock&&) = default;
    HandOverLock& operator=(HandOverLock&&) = default;

    // never actually want to copy and have to locks to the same mutex
    HandOverLock(const HandOverLock&) = delete;
    HandOverLock& operator=(const HandOverLock&) = delete;

    ~HandOverLock() = default;

    // We enforce that we always aquire the parents lock before the childs, preventes deadlocks
    void hand_over(Mutex & next_m) {
        Lock next_lock(next_m); // lock the next
        
        lock_.swap(next_lock);  // now this instance owns next
    } // old lock destructs and releases the lock

    Lock& get() {return lock_; }

    Lock steal() {
        return std::move(lock_);
    }

private:
    Lock lock_;
};

using inode_read_block    = HandOverLock<shared_lock, shared_mutex>;
using inode_upgrade_block = HandOverLock<upgrade_lock, shared_mutex>;

/*
 * The network class for our file system, one instance for a file system
*/
class Network {
public:
    Network(int port_in);

    /*
     * start_server
     * 
     * Creates the server for the file network
     *
     * start_server does not return 
     */
    void start_server(); 
private:

    template<typename LockT>
    struct path_find_info {
        std::shared_ptr<shared_mutex> mtx_sp;       
        LockT lock;
    };

    struct create_scan_info {
        bool exists                = false;         // if this target already exists
    
        bool has_open_entry        = false;         // if theres an open direntry
        int open_parent_blocks_idx = -1;            // index into parents.blocks[]
        int open_dir_offset        = -1;            // index in fs_direntry[]
        fs_direntry   open_dir_page[FS_DIRENTRIES]; // the open page if there is one
    };

    struct delete_scan_info {
        bool found            = false;          // if this target is found
        int inode_block       = -1;             // if found this is that block
    
        int parent_blocks_idx = -1;            // index into parents.blocks[]
        int dir_block         = -1;            // # block this page of direntries is
        int dir_offset        = -1;            // index in fs_direntry[]     
        fs_direntry   dir_page[FS_DIRENTRIES]; // the dir page this dientry is in 
        bool only_entry       = false;         // if its the only entry in the block
    };

    int sockfd = 0; 
    int portnum = 0;
    sockaddr_in addr{};
    std::set<uint32_t> free_disk_blocks;

    boost::mutex free_disk_mutex;
    // per inode reader/write blocks
    // the 6 credit version needs to be space efficient with the use of smart pointers - weak pointers allow them to deallocate when not being used
    boost::mutex lock_table_mutex;
    std::unordered_map<uint32_t, std::weak_ptr<shared_mutex>> inode_lock_table;

    // this helper will return the sp for a given inode_block
    std::shared_ptr<shared_mutex> get_inode_mutex_sp(uint32_t block);

    /*
     * sys_init
     *
     * MODIFIES:
     *              free_disk_blocks
     *
     * Initialize the list of free disk blocks by reading the relevant data from the existing file system.
     * The file server should be able to start with any valid file system (an empty file system as well as file systems
     * containing directories and/or files)
     *
     */
    void sys_init();

    /*
     * get_port_number
     *
     * MODIFIES:
     *              Network::portnum
     *
     * If the user does not provide a portnum, portnum is 0 and the OS will choose
     * a valid portnum for the server 
     *
     * Throws an exception if syscall to getsockname() fails
     */
    void get_port_number(int sockfd);

    /*
     * handle_request
     *     
     * This is the wrapper function called with every new thread created to 
     * completely handle the request.
     */
    void handle_request(int connection_sock);

    /*
     * receive_data
     *
     * RETURNS:
     *          A string to data received from the client 
     *
     * Performs a syscall to recv() in which a byte of data is read 
     * until a null terminator is read. 
     *
     * Throws an exception if an error occurs on recv()
     */
    std::string receive_data(int connection_sock);

    /*
     * read_inode_block
     *
     *  Disk read the block and fill the inode
     */
    void read_inode_block(const int &block, fs_inode &inode);

    /*
     * Handles FS_READBLOCK request
     * - Uses path_find() to locate the target inode and holds a shared_lock
     *     on it while validating and reading.
     * - Verifies: target is a file, owned by username, and block index is balid
     * - On success: disk_readblock() + send header then data that data read.
     * - On error: sends no response; caller closes the socket
     * 
     */
    void read_block(request &request, int socket);


    /*
     * Handles FS_WRITEBLOCK request
     * - Uses path_find_upgrade() to locate the file and hold an upgrade_lock.
     * - Verifies ownership, type=file, block index in [0, size] and within 
     *   FS_MAXFILEBLOCKS, and space available if extending.
     * - Overwrite: upgrade to unique_lock and write new data to existing block.
     * - Extend: allocate new block, write data, then update inode (data first
     *   then metadta for crash safety).
     * - On success: sends back only the request header.s
     * 
     */
    void write_block(request &request, int socket);

    /*
     * Handle an FS_CREATE request (new file or directory).
     * - Splits pathname into parent path + final name.
     * - Uses path_find_upgrade() on the parent and holds an upgrade lock.
     * - Verifies parent is a directory, owned by username or root, and that
     *   the name does not already exist; finds lowest free direntry (or 
     *    allocates a new dir block if needed)
     * - Allocates and initializes a new inode block, writes inode first, then 
     *   updates the directory entry (and parent inode if adding a new block).
     *  - On succes: sends back orginal request header.
     */
    void sys_create(request &request, int socket);

    /*
     * Handle on FS_DELETE reqest (file or empty directory).
     * - Splits pathname, uses path_find_upgrade() on parent, then finds
     *   the target direntry via scan_directory_for_delete().
     * - Verifies parent is a directory with proper ownership; verifes target
     *   is owned by username, not "/", and (if a directory) is empty.
     * - Under a unique_lock on the parent, either clears just the entry or
     *   shrinks the directory by removing an all-empty dir block and compacting
     *   parent_inodes.blocks[].
     * - Under a unique lock on the target plus free_disk_mutex, 
     *   returns all target data blocks and its inode block to free_disk_blocks.
     * - On succes: sends back the orginal request header.
     */
    void sys_delete(request &request, int socket);

    /*
     * path_find
     *
     *  Starts from the root and traveres looking for our given path with hand over hand locking of a shared lock
     *  Given a path, explore the path and return the block of the destination, if it does not exist, return -1
     *  (could be the root 0 )
     * 
     * Will return out_info with the type of lock requested
     * 
    */
    template <typename LockT>

    //
    int path_find_impl(std::deque<std::string> &path, std::string &user, path_find_info<LockT>* out_info);
    
    int path_find(std::deque<std::string> &path, std::string &user, path_find_info<shared_lock>* out_info);

    int path_find_upgrade(std::deque<std::string> &path, std::string &user, path_find_info<upgrade_lock>* out_info);

    
    /*
     * find_child
     *
     *  This function will be used within path_find to only explore for the target name.
     *  
     *  input: 
     *          dir inode
     *          target name
     *  output: 
     *          on success, int inode_block 
     *          on failure -1 
     *  
    */
    int find_child(const fs_inode &dir_inode, const std::string &name);

    /*
     * scan_directory_for_create
     *
     *  This function will be used within create to accomplish more within a regular walk in path_find
     *  It will check if the target already exists, and if theres an open direntry.
     *  
     *  input: 
     *          dir inode
     *          target name
     *  output: 
     *          create_scan struct object       
     *  
    */
    create_scan_info scan_directory_for_create(const fs_inode &parent_inode, const std::string &name);
    
    /*
     * scan_directory_for_delete
     *
     *  This function will be used within delete to accomplish more within a regular walk in path_find
     *  It will check if the target even exists, and if its the only entry in the block
     *  
     *  input: 
     *          dir inode
     *          target name
     *  output: 
     *          a delete_scan struct object
     *  
    */
    delete_scan_info scan_directory_for_delete(const fs_inode &parent_inode, const std::string &name);
    
    /*
     * send_all
     *
     *  send() does not guarantee to send all the bytes, so we need to loop until it does
     *  we will do this in each function so well make a helper
    */
    void send_all(int sockfd, const void* buf, size_t len);

    /*
     * get_new_block
     *      gets the next free block 
     *      failure returns -1
     */
    int get_new_block();

};
