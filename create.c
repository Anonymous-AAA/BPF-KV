#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "create.h"
#include "parse.h"
#include "db_types.h"
#include "simplekv.h"


int do_create_cmd(int argc, char *argv[], struct ArgState *as) {
    parse_create_opts(argc, argv);
    return load(as->layers, as->filename);
}

/* Create a new database on disk at [db_path] with [layer_num] layers */
int load(size_t layer_num, char *db_path) {
    printf("Load the database of %lu layers\n", layer_num);
    //Gets the fd of the database file.
    int db = initialize(layer_num, LOAD_MODE, db_path);

    //1 MB = 2^20 bytes
    int const MB = (1<<20);

    // 1. Load the index
    /*
        typedef struct _Node {
            meta__t next;
            meta__t type;
            key__t key[NODE_CAPACITY];
            ptr__t ptr[NODE_CAPACITY];
        } Node;
    */
    
    //!did not understand why does this work uninitialized even though it is a const pointer to a node
    Node * const node_begin;
    //! did not understand node_entries why is there 10 MB?
    //! node_entries= 2^20*10/512= 20480 (I guess it is kind of an upper limit to number of nodes)
    int node_entries = (MB * 10) / sizeof(Node);
    /*
       int posix_memalign(void **memptr, size_t alignment, size_t size);
    
       The function posix_memalign() allocates size bytes and places the
       address of the allocated memory in *memptr.  The address of the
       allocated memory will be a multiple of alignment, which must be a
       power of two and a multiple of sizeof(void *).  This address can
       later be successfully passed to free(3).  If size is 0, then the
       value placed in *memptr is either NULL or a unique pointer value

       Which means the address allocated will be a multiple of the alignment (Particularly useful for SIMD instructions)
    */
    //Allocates node_begin
    if (posix_memalign((void **)&node_begin, 512, node_entries * sizeof(Node))) {
        perror("posix_memalign failed");
        close(db);
        free_globals();
        exit(1);
    }
    /*
    Disk layout:
    B+ tree nodes, each with 31 keys and 31 associated block offsets to other nodes
    Nodes are written by level in order, so, the root is first, followed by all nodes on the second level.
    Since each node has pointers to 31 other nodes, fanout is 31
    | 0  - 1  2  3  4 ... 31 - .... | ### LOG DATA ### |

    Leaf nodes have pointers into the log data, which is appended as a "heap" in the same file
    at the end of the B+tree. Once we reach a leaf node, we scan through its keys and if one matches
    the key we need, we read the offset into the heap and can retrieve the value.
    */
    ptr__t next_pos = 1;
    long next_node_offset = 1;
    Node *node = node_begin;
    //!did not understand node_buf_end
    Node * const node_buf_end = node_begin + node_entries;

    //looping over each layer of the b+ tree
    for (size_t i = 0; i < layer_num; i++) {

        //! did not understand extent
        //* Possible explanation: extent is used for uniformly dividing keys among nodes
        size_t extent = max_key / layer_cap[i], start_key = 0;
        printf("layer %lu extent %lu\n", i, extent);

        //looping over each node in a layer
        for (size_t j = 0; j < layer_cap[i]; ++j, ++next_node_offset) {

            //marking node as leaf node if it is the last layer else internal node
            node->type = (i == layer_num - 1) ? LEAF : INTERNAL;
            //sub_extent are used to uniformly divide keys among key fields in a node
            //sub_extent is the value difference between two adjacent keys in a node
            size_t sub_extent = extent / NODE_CAPACITY;
            if (j == layer_cap[i] - 1) {
                /* Last node in this level(ie:level i) */
                node->next = 0;
            } else {
                /* Pointer to the next node in this level; used for efficient scans */
                node->next = next_node_offset * sizeof(Node);
            }
            
            //Iterating through each key in a node
            for (size_t k = 0; k < NODE_CAPACITY; k++) {
                //uniform filling of keys in a node
                node->key[k] = start_key + k * sub_extent;
                node->ptr[k] = node->type == INTERNAL ?
                               encode(next_pos   * BLK_SIZE) :
                               //
                               encode(total_node * BLK_SIZE + (next_pos - total_node) * VAL_SIZE);
                next_pos++;
            }

            node += 1;
            if (node == node_buf_end) {
                ssize_t write_size = node_entries * sizeof(Node);
                ssize_t bytes_written = write(db, node_begin, write_size);
                if (bytes_written != write_size) {
                    fprintf(stderr, "failure: partial write of index node\n");
                    exit(1);
                }
                node = node_begin;
            }
            start_key += extent;
        }
    }
    /* Write any remaining node buffer */
    if (node > node_begin) {
        ssize_t write_size = (node - node_begin) * sizeof(Node);
        ssize_t bytes_written = write(db, node_begin, write_size);
        if (bytes_written != write_size) {
            fprintf(stderr, "failure: partial write of index node\n");
            exit(1);
        }
    }
    free(node_begin);

    // 2. Load the value log
    Log * const log_begin;
    int const log_entries = (MB * 10) / sizeof(Log);
    if (posix_memalign((void **)&log_begin, 512, log_entries * sizeof(Log))) {
        perror("posix_memalign failed");
        close(db);
        free_globals();
        exit(1);
    }
    printf("Writing value heap\n");
    Log *log = log_begin;
    Log * const log_end = log_begin + log_entries;
    for (size_t i = 0; i < max_key; i += LOG_CAPACITY) {
        for (size_t j = 0; j < LOG_CAPACITY; j++) {
            sprintf((char *) log->val[j], "%63lu", i + j);
        }
        ++log;
        if (log == log_end) {
            ssize_t write_size = log_entries * sizeof(Log);
            ssize_t bytes_written = write(db, log_begin, write_size);
            if (bytes_written != write_size) {
                fprintf(stderr, "failure: partial write of log data\n");
                exit(1);
            }
            log = log_begin;
        }
    }
    /* Write any remaining entries */
    if (log != log_begin) {
        ssize_t write_size = (log - log_begin) * sizeof(Log);
        ssize_t bytes_written = write(db, log_begin, write_size);
        if (bytes_written != write_size) {
            fprintf(stderr, "failure: partial write of log data\n");
            exit(2);
        }
    }

    free(log_begin);
    close(db);
    return terminate();
}
