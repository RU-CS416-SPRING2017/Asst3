/*
  Simple File System

  This code is derived from function prototypes found /usr/include/fuse/fuse.h
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  His code is licensed under the LGPLv2.

*/

#include "params.h"
#include "block.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif

#include "log.h"

#define DISKFILE (((struct sfs_state *) fuse_get_context()->private_data)->diskfile)

#define MAX_DISK_SIZE (16 * 1000 * 1000)
#define INDOE_SIZE sizeof(struct inode)
#define INODES_SIZE (INDOE_SIZE * NUM_INODES)
#define MAX_FILE_SIZE (MAX_FILE_BLOCKS * BLOCK_SIZE)

#define NUM_BLOCKS (MAX_DISK_SIZE / BLOCK_SIZE)
#define NUM_INODES 128
#define INTS_IN_BLOCK (BLOCK_SIZE / sizeof(int))
#define MAX_FILE_BLOCKS (13 + INTS_IN_BLOCK + (INTS_IN_BLOCK * INTS_IN_BLOCK))

#define INDOES_BLOCKS 1
#define FS_BLOCK 0

#define FS_CAST(ptr) ((struct filesystem *) (ptr))

struct filesystem {
    int numIndoesBlocks;
    int bitmapBlocks;
    int numBitmapBlocks;
    int dataBlocks;
    int numDataBlocks;
};

struct inode {
    struct stat info;
    int block[13];
    int block1;
    int block2;
};

struct dirRow {
    char name[NAME_MAX];
    int inode;
};

///////////////////////////////////////////////////////////
//
// Prototypes for all these functions, and the C-style comments,
// come indirectly from /usr/include/fuse.h
//

// Test function
void printBits(char byte) {
    int i;
    for (i = 0; i < 8; i++) {
        log_msg("%d", (byte >> (7 - i)) & 1);
    }
    log_msg("\n");
}

// Stores the inode at index into buf.
// Returns 0 on success, else returns -1.
int getInode(int index, struct inode * buf) {

    // Calculating numbers
    int firstByte = index * INDOE_SIZE;
    int blockIndex = firstByte / BLOCK_SIZE;
    int blockNumber = blockIndex + INDOES_BLOCKS;
    int byteInBlock = firstByte % BLOCK_SIZE;
    int numBlocks = ((byteInBlock + INDOE_SIZE) / BLOCK_SIZE) + 1;

    // Get appropriate blocks
    char * block = malloc(BLOCK_SIZE * numBlocks);
    int i;
    for (i = 0; i < numBlocks; i++) {
        if (block_read(blockNumber + i, block + (i * BLOCK_SIZE)) != BLOCK_SIZE) {
            free(block);
            return -1;
        }
    }

    // Store the inode in buf
    memcpy(buf, block + byteInBlock, INDOE_SIZE);
    free(block);
    return 0;
}

// Updates the inode at index with buf.
// Returns 0 on success, else returns -1.
int setInode(int index, struct inode * buf) {

    // Calculating numbers
    int firstByte = index * INDOE_SIZE;
    int blockIndex = firstByte / BLOCK_SIZE;
    int blockNumber = blockIndex + INDOES_BLOCKS;
    int byteInBlock = firstByte % BLOCK_SIZE;
    int numBlocks = ((byteInBlock + INDOE_SIZE) / BLOCK_SIZE) + 1;

    // Get appropriate blocks
    char * block = malloc(BLOCK_SIZE * numBlocks);
    int i;
    for (i = 0; i < numBlocks; i++) {
        if (block_read(blockNumber + i, block + (i * BLOCK_SIZE)) != BLOCK_SIZE) {
            free(block);
            return -1;
        }
    }

    // Store the buf in block
    memcpy(block + byteInBlock, buf, INDOE_SIZE);

    // Write back to disk
    for (i = 0; i < numBlocks; i++) {
        if (block_write(blockNumber + i, block + (i * BLOCK_SIZE)) != BLOCK_SIZE) {
            free(block);
            return -1;
        }
    }
    free(block);
    return 0;
}

// Save all Inodes into buf,
// return 0 if successful, -1 otherwise
int getInodes(struct inode * buf) {
    int i;
    for (i = 0; i < NUM_INODES; i++) {
        if (block_read(i + INDOES_BLOCKS, buf) != BLOCK_SIZE) {
            return -1;
        }
        buf += BLOCK_SIZE;
    }
    return 0;
}

// Write all Inodes from buf,
// return 0 if successful, -1 otherwise
int setInodes(struct inode * buf) {
    int i;
    for (i = 0; i < NUM_INODES; i++) {
        if (block_write(i + INDOES_BLOCKS, buf) != BLOCK_SIZE) {
            return -1;
        }
        buf += BLOCK_SIZE;
    }
    return 0;
}

// Stores the filesystem metadata in buf
// returns 0 on success, else -1
int getFilesystem(struct filesystem * buf) {
    char * buffer = malloc(BLOCK_SIZE);
    if (block_read(FS_BLOCK, buffer) != BLOCK_SIZE) {
        free(buffer);
        return -1;
    }
    memcpy(buf, buffer, sizeof(struct filesystem));
    free(buffer);
    return 0;
}

// Store bitmap in buf return 0 on success,
// else return -1
int getBitmap(struct filesystem * fs, void * buf) {
    int i;
    for (i = 0; i < fs->numBitmapBlocks; i++) {
        if (block_read(i + fs->bitmapBlocks, buf) != BLOCK_SIZE) {
            return -1;
        }
        buf += BLOCK_SIZE;
    }
    return 0;
}

// Allocates a block and returns the block number
// of the allocated block. Returns 0 if no free block found.
// Returns -1 on error.
int allocateBlock(struct filesystem * fs) {

    // Getting the bitmap
    char * bitmap = malloc(BLOCK_SIZE * fs->numBitmapBlocks);
    if (getBitmap(fs, bitmap)) {
        free(bitmap);
        return -1;
    }

    // Finding the next available block and updating the bitmap
    int i;
    for (i = 0; i < (BLOCK_SIZE * fs->numBitmapBlocks); i++) {
        int j;
        for (j = 0; j < 8; j++) {
            int index = (i * 8) + j;
            if (index >= fs->numDataBlocks) {
                free(bitmap);
                return 0;
            }
            char bit = (bitmap[i] >> (7 - j)) & 1;
            if (!bit) {
                bitmap[i] = bitmap[i] | (1 << (7 - j));
                int offset = i / BLOCK_SIZE;
                block_write(offset + fs->bitmapBlocks, bitmap + (offset * BLOCK_SIZE));
                free(bitmap);
                return index + fs->dataBlocks;
            }
        }
    }
    free(bitmap);
    return 0;
}

// Free the allocated block. Return 0 on success, -1
// on error.
int freeBlock(struct filesystem * fs, int dataBlock) {

    // Zeroing out the data
    char * zero = calloc(1, BLOCK_SIZE);
    if (block_write(dataBlock, zero) != BLOCK_SIZE) {
        free(zero);
        return -1;
    }
    free(zero);

    // Calculating numbers
    int bitIndex = dataBlock - fs->dataBlocks;
    int bitsInBlock = 8 * BLOCK_SIZE;
    int bitmapBlockIndex = bitIndex / bitsInBlock;
    int bitInBlockIndex = bitIndex % bitsInBlock;
    int bitmapBlock = bitmapBlockIndex + fs->bitmapBlocks;
    int charBufIndex = bitInBlockIndex / 8;
    int bitInCharBufIndex = bitInBlockIndex % 8;

    // Getting the appropriate bitmap block
    char * bitmapBlockBuf = malloc(BLOCK_SIZE);
    if (block_read(bitmapBlock, bitmapBlockBuf) != BLOCK_SIZE) {
        free(bitmapBlockBuf);
        return -1;
    }

    // Updating bit in buffer
    char mask = 1 << (7 - bitInCharBufIndex);
    mask = mask ^ 0xFF;
    bitmapBlockBuf[charBufIndex] = bitmapBlockBuf[charBufIndex] & mask;

    // Writing buffer to disk
    if (block_write(bitmapBlock, bitmapBlockBuf) != BLOCK_SIZE) {
        free(bitmapBlockBuf);
        return -1;
    }

    free(bitmapBlockBuf);
    return 0;
}

// Returns the data block number of inode at blockIndex.
// Returns 0 if out of index. Returns -1 on error.
// Allocates the block if not allocated.
int getInodeBlock(struct inode * inode, int blockIndex, struct filesystem * fs) {

    // Calculating indexes
    int startIndex = 0;
    int endIndex = 12;
    int startIndex1 = endIndex + 1;
    int endIndex1 = (startIndex1 + INTS_IN_BLOCK) - 1;
    int startIndex2 = endIndex1 + 1;
    int endIndex2 = (startIndex2 + (INTS_IN_BLOCK * INTS_IN_BLOCK)) - 1;

    // If blockIndex is in direct block simpy returns that,
    // allocated if neccesary
    if (blockIndex >= startIndex && blockIndex <= endIndex) {
        if (!(inode->block[blockIndex])) {
            int temp = allocateBlock(fs);
            if (!(temp > 0)) {
                return -1;
            }
            inode->block[blockIndex] = temp;
        }
        return inode->block[blockIndex];

    // If blockIndex is in the singly indirect blocks then handle that
    } else if (blockIndex >= startIndex1 && blockIndex <= endIndex1) {

        // Get the block with the info
        int * buf = malloc(BLOCK_SIZE);
        if (block_read(inode->block1, buf) != BLOCK_SIZE) {
            free(buf);
            return -1;
        }

        // Find the right block, allocate and updated disk
        // if neccesary
        int localIndex = blockIndex - startIndex1;
        if (!buf[localIndex]) {
            int temp = allocateBlock(fs);
            if (!(temp > 0)) {
                free(buf);
                return -1;
            }
            buf[localIndex] = temp;
            if (block_write(inode->block1, buf) != BLOCK_SIZE) {
                free(buf);
                return -1;
            }
        }
        free(buf);
        return buf[localIndex];

    // If blockIndex is in the doubly indirect blocks then handle that
    } else if (blockIndex >= startIndex2 && blockIndex <= endIndex2) {
        
        // Get the first block with the info
        int * buf = malloc(BLOCK_SIZE);
        if (block_read(inode->block2, buf) != BLOCK_SIZE) {
            free(buf);
            return -1;
        }

        // Calculating numbers
        int localIndex = blockIndex - startIndex2;
        int firstBlockIndex = localIndex / INTS_IN_BLOCK;
        int secondBlockIndex = localIndex % INTS_IN_BLOCK;

        // Allocate block for second block if not already
        if (!buf[firstBlockIndex]) {
            int temp = allocateBlock(fs);
            if (!(temp > 0)) {
                free(buf);
                return -1;
            }
            buf[firstBlockIndex] = temp;
            if (block_write(inode->block2, buf) != BLOCK_SIZE) {
                free(buf);
                return -1;
            }
        }

        // Get second block
        int secondBlock = buf[firstBlockIndex];
        if (block_read(secondBlock, buf) != BLOCK_SIZE) {
            free(buf);
            return -1;
        }

        // Find the right block, allocate and updated disk
        // if neccesary
        if (!buf[secondBlockIndex]) {
            int temp = allocateBlock(fs);
            if (!(temp > 0)) {
                free(buf);
                return -1;
            }
            buf[secondBlockIndex] = temp;
            if (block_write(secondBlock, buf) != BLOCK_SIZE) {
                free(buf);
                return -1;
            }
        }
        free(buf);
        return buf[secondBlockIndex];

    } else {
        return 0;
    }
}

// Read size bytes starting at offset from inode's data and store it in buf.
// Return 0 on success and -1 otherwise.
int readInodeData(struct inode * inode, size_t size, off_t offset, void * buf, struct filesystem * fs) {

    // Calculateing numbers
    size_t totalSize = size + offset;
    if (totalSize > inode->info.st_size) {
        return -1;
    }
    int blockIndex = offset / BLOCK_SIZE;
    int byteInBlock = offset % BLOCK_SIZE;
    int numBlocks = ((byteInBlock + size) / BLOCK_SIZE) + 1;

    // Get data
    char * dataBlock = malloc(BLOCK_SIZE * numBlocks);
    int i;
    for (i = 0; i < numBlocks; i++) {
        int block = getInodeBlock(inode, i + blockIndex, fs);
        if (!(block > 0)) {
            free(dataBlock);
            return -1;
        }
        if (block_read(block, dataBlock + (i * BLOCK_SIZE)) != BLOCK_SIZE) {
            free(dataBlock);
            return -1;
        }
    }

    // Return the data
    memcpy(buf, dataBlock + byteInBlock, size);
    free(dataBlock);
    return 0;

}

// Write size bytes from buf into inode's data starting at offset.
// Return 0 on success and 
int writeInodeData(struct inode * inode, size_t size, off_t offset, void * buf, struct filesystem * fs) {

    // Calculate numbers
    size_t totalSize = size + offset;
    if (totalSize > MAX_FILE_SIZE) {
        return -1;
    }
    int blockIndex = offset / BLOCK_SIZE;
    int byteInBlock = offset % BLOCK_SIZE;
    int numBlocks = ((byteInBlock + size) / BLOCK_SIZE) + 1;

    // Store data locally
    char * dataBlock = malloc(BLOCK_SIZE * numBlocks);
    int blocks[numBlocks];
    int i;
    for (i = 0; i < numBlocks; i++) {
        blocks[i] = getInodeBlock(inode, i + blockIndex, fs);
        if (!(blocks[i] > 0)) {
            free(dataBlock);
            return -1;
        }
        if (block_read(blocks[i], dataBlock + (i * BLOCK_SIZE)) != BLOCK_SIZE) {
            free(dataBlock);
            return -1;
        }
    }

    // Update local copy
    memcpy(dataBlock + byteInBlock, buf, size);

    // Update data on disk
    for (i = 0; i < numBlocks; i++) {
        if (block_write(blocks[i], dataBlock + (i + BLOCK_SIZE)) != BLOCK_SIZE) {
            return -1;
        }
    }
    if (totalSize > inode->info.st_size) {
        inode->info.st_size = totalSize;
    }
    return 0;
}

// Test function
void printBitmap() {
    // Getting filesystem metadata
    struct filesystem fs;
    getFilesystem(&fs);

    // Getting the bitmap
    char bitmap[BLOCK_SIZE * fs.numBitmapBlocks];
    getBitmap(&fs, bitmap);

    // Print all bits in bitmap
    int l;
    for (l = 0; l < fs.numBitmapBlocks; l++) {
        int i;
        int accum = 0;
        for (i = 0; i < BLOCK_SIZE; i++) {
            int j;
            for (j = 0; j < 8; j++) {
                int index = (i * 8) + j;
                char bit = (bitmap[(l * BLOCK_SIZE) + i] >> (7 - j)) & 1;
                if (bit) {
                    accum++;
                }
                log_msg("%d", bit);
            }
        }
        log_msg("\nbits found %d\n", accum);
    }
}

/**
 * Initialize filesystem
 *
 * The return value will passed in the private_data field of
 * fuse_context to all file operations and as a parameter to the
 * destroy() method.
 *
 * Introduced in version 2.3
 * Changed in version 2.6
 */
void *sfs_init(struct fuse_conn_info *conn)
{
    fprintf(stderr, "in bb-init\n");
    log_msg("\nsfs_init()\n");
    log_conn(conn);
    log_fuse_context(fuse_get_context());

    // Zero out all blocks
    disk_open(DISKFILE);
    char * buf = calloc(1, BLOCK_SIZE);
    int i;
    for (i = 1; i < NUM_BLOCKS; i++) {
        int ret = block_write(i, buf);
    }

    // Calculate numbers for filesystem
    struct filesystem * fs = FS_CAST(buf);
    if (INODES_SIZE % BLOCK_SIZE) {
        fs->numIndoesBlocks = (INODES_SIZE / BLOCK_SIZE) + 1;
    } else {
        fs->numIndoesBlocks = INODES_SIZE / BLOCK_SIZE;
    }
    int blocksLeft = NUM_BLOCKS - (fs->numIndoesBlocks + 1);
    fs->bitmapBlocks = INDOES_BLOCKS + fs->numIndoesBlocks;
    int bitsInBlock = BLOCK_SIZE * 8;
    fs->numBitmapBlocks = 1;
    fs->numDataBlocks = blocksLeft - 1;
    while ((fs->numBitmapBlocks * bitsInBlock) < fs->numDataBlocks) {
        (fs->numBitmapBlocks)++;
        (fs->numDataBlocks)--;
    }
    fs->dataBlocks = fs->bitmapBlocks + fs->numBitmapBlocks;
    
    // Write calculated numbers to fisrt block
    block_write(FS_BLOCK, buf);

    log_msg("Max Disk Size: %d\nNumber of Blocks: %d\nNumber of Inodes: %d\nSize of all Inodes: %d\nNumber of Inode Blocks: %d\nNumber of Bitmap Blocks: %d\nNumber of Data Blocks: %d\nBitmap Blocks: %d\nData Blocks: %d\n", MAX_DISK_SIZE, NUM_BLOCKS, NUM_INODES, INODES_SIZE, fs->numIndoesBlocks, fs->numBitmapBlocks, fs->numDataBlocks, fs->bitmapBlocks, fs->dataBlocks);

    struct inode inode;
    memset(&inode, 0, sizeof(struct inode));

    int write = writeInodeData(&inode, 10, 0, "hello man", fs);
    char temp[10];
    int read = readInodeData(&inode, 10, 0, temp, fs);

    log_msg("write out: %d, read out: %d\n", write, read);

    for (i = 0; i < 13; i++) {
        log_msg("inode-blk[%d]: %d\n", i, inode.block[i]);
    }


    log_msg("message: %s\n", temp);

    char temp2[BLOCK_SIZE];
    block_write(61, temp2);
    log_msg("message2: %s\n", temp2);

    free(buf);
    return SFS_DATA;
}

/**
 * Clean up filesystem
 *
 * Called on filesystem exit.
 *
 * Introduced in version 2.3
 */
void sfs_destroy(void *userdata)
{
    log_msg("\nsfs_destroy(userdata=0x%08x)\n", userdata);
    disk_close();
}

/** Get file attributes.
 *
 * Similar to stat().  The 'st_dev' and 'st_blksize' fields are
 * ignored.  The 'st_ino' field is ignored except if the 'use_ino'
 * mount option is given.
 */
int sfs_getattr(const char *path, struct stat *statbuf)
{
    int retstat = 0;
    char fpath[PATH_MAX];
    
    log_msg("\nsfs_getattr(path=\"%s\", statbuf=0x%08x)\n",
	  path, statbuf);
    
    return retstat;
}

/**
 * Create and open a file
 *
 * If the file does not exist, first create it with the specified
 * mode, and then open it.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the mknod() and open() methods
 * will be called instead.
 *
 * Introduced in version 2.5
 */
int sfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_create(path=\"%s\", mode=0%03o, fi=0x%08x)\n",
	    path, mode, fi);
    
    
    return retstat;
}

/** Remove a file */
int sfs_unlink(const char *path)
{
    int retstat = 0;
    log_msg("sfs_unlink(path=\"%s\")\n", path);

    
    return retstat;
}

/** File open operation
 *
 * No creation, or truncation flags (O_CREAT, O_EXCL, O_TRUNC)
 * will be passed to open().  Open should check if the operation
 * is permitted for the given flags.  Optionally open may also
 * return an arbitrary filehandle in the fuse_file_info structure,
 * which will be passed to all file operations.
 *
 * Changed in version 2.2
 */
int sfs_open(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_open(path\"%s\", fi=0x%08x)\n",
	    path, fi);

    
    return retstat;
}

/** Release an open file
 *
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open() call there will be exactly one release() call
 * with the same flags and file descriptor.  It is possible to
 * have a file opened more than once, in which case only the last
 * release will mean, that no more reads/writes will happen on the
 * file.  The return value of release is ignored.
 *
 * Changed in version 2.2
 */
int sfs_release(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_release(path=\"%s\", fi=0x%08x)\n",
	  path, fi);
    

    return retstat;
}

/** Read data from an open file
 *
 * Read should return exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the
 * 'direct_io' mount option is specified, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 *
 * Changed in version 2.2
 */
int sfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_read(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",
	    path, buf, size, offset, fi);

   
    return retstat;
}

/** Write data to an open file
 *
 * Write should return exactly the number of bytes requested
 * except on error.  An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 *
 * Changed in version 2.2
 */
int sfs_write(const char *path, const char *buf, size_t size, off_t offset,
	     struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_write(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",
	    path, buf, size, offset, fi);
    
    
    return retstat;
}


/** Create a directory */
int sfs_mkdir(const char *path, mode_t mode)
{
    int retstat = 0;
    log_msg("\nsfs_mkdir(path=\"%s\", mode=0%3o)\n",
	    path, mode);
   
    
    return retstat;
}


/** Remove a directory */
int sfs_rmdir(const char *path)
{
    int retstat = 0;
    log_msg("sfs_rmdir(path=\"%s\")\n",
	    path);
    
    
    return retstat;
}


/** Open directory
 *
 * This method should check if the open operation is permitted for
 * this  directory
 *
 * Introduced in version 2.3
 */
int sfs_opendir(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_opendir(path=\"%s\", fi=0x%08x)\n",
	  path, fi);
    
    
    return retstat;
}

/** Read directory
 *
 * This supersedes the old getdir() interface.  New applications
 * should use this.
 *
 * The filesystem may choose between two modes of operation:
 *
 * 1) The readdir implementation ignores the offset parameter, and
 * passes zero to the filler function's offset.  The filler
 * function will not return '1' (unless an error happens), so the
 * whole directory is read in a single readdir operation.  This
 * works just like the old getdir() method.
 *
 * 2) The readdir implementation keeps track of the offsets of the
 * directory entries.  It uses the offset parameter and always
 * passes non-zero offset to the filler function.  When the buffer
 * is full (or an error happens) the filler function will return
 * '1'.
 *
 * Introduced in version 2.3
 */
int sfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
	       struct fuse_file_info *fi)
{
    int retstat = 0;
    
    
    return retstat;
}

/** Release directory
 *
 * Introduced in version 2.3
 */
int sfs_releasedir(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;

    
    return retstat;
}

struct fuse_operations sfs_oper = {
  .init = sfs_init,
  .destroy = sfs_destroy,

  .getattr = sfs_getattr,
  .create = sfs_create,
  .unlink = sfs_unlink,
  .open = sfs_open,
  .release = sfs_release,
  .read = sfs_read,
  .write = sfs_write,

  .rmdir = sfs_rmdir,
  .mkdir = sfs_mkdir,

  .opendir = sfs_opendir,
  .readdir = sfs_readdir,
  .releasedir = sfs_releasedir
};

void sfs_usage()
{
    fprintf(stderr, "usage:  sfs [FUSE and mount options] diskFile mountPoint\n");
    abort();
}

int main(int argc, char *argv[])
{
    int fuse_stat;
    struct sfs_state *sfs_data;
    
    // sanity checking on the command line
    if ((argc < 3) || (argv[argc-2][0] == '-') || (argv[argc-1][0] == '-'))
	sfs_usage();

    sfs_data = malloc(sizeof(struct sfs_state));
    if (sfs_data == NULL) {
	perror("main calloc");
	abort();
    }

    // Pull the diskfile and save it in internal data
    sfs_data->diskfile = argv[argc-2];
    argv[argc-2] = argv[argc-1];
    argv[argc-1] = NULL;
    argc--;
    
    sfs_data->logfile = log_open();
    
    // turn over control to fuse
    fprintf(stderr, "about to call fuse_main, %s \n", sfs_data->diskfile);
    fuse_stat = fuse_main(argc, argv, &sfs_oper, sfs_data);
    fprintf(stderr, "fuse_main returned %d\n", fuse_stat);
    
    return fuse_stat;
}
