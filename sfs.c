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
#define INODE_SIZE sizeof(struct inode)
#define INODES_SIZE (INODE_SIZE * NUM_INODES)
#define MAX_FILE_SIZE (MAX_FILE_BLOCKS * BLOCK_SIZE)
#define INT_SIZE sizeof(int)
#define DIR_ROW_SIZE sizeof(struct dirRow)

#define NUM_BLOCKS (MAX_DISK_SIZE / BLOCK_SIZE)
#define NUM_INODES 128
#define INTS_IN_BLOCK (BLOCK_SIZE / INT_SIZE)
#define MAX_FILE_BLOCKS (13 + INTS_IN_BLOCK + (INTS_IN_BLOCK * INTS_IN_BLOCK))

#define INDOES_BLOCKS 1
#define FS_BLOCK 0

#define FS_CAST(ptr) ((struct filesystem *) (ptr))

struct filesystem {
    int rootInodeIndex;
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
    char name[255];
    int inodeIndex;
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
    int firstByte = index * INODE_SIZE;
    int blockIndex = firstByte / BLOCK_SIZE;
    int blockNumber = blockIndex + INDOES_BLOCKS;
    int byteInBlock = firstByte % BLOCK_SIZE;
    int numBlocks = ((byteInBlock + INODE_SIZE) / BLOCK_SIZE) + 1;

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
    memcpy(buf, block + byteInBlock, INODE_SIZE);
    free(block);
    return 0;
}

// Updates the inode at index with buf.
// Returns 0 on success, else returns -1.
int setInode(int index, struct inode * buf) {

    // Calculating numbers
    int firstByte = index * INODE_SIZE;
    int blockIndex = firstByte / BLOCK_SIZE;
    int blockNumber = blockIndex + INDOES_BLOCKS;
    int byteInBlock = firstByte % BLOCK_SIZE;
    int numBlocks = ((byteInBlock + INODE_SIZE) / BLOCK_SIZE) + 1;

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
    memcpy(block + byteInBlock, buf, INODE_SIZE);

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
int getInodes(struct inode * buf, struct filesystem * fs) {
    int i;
    for (i = 0; i < fs->numIndoesBlocks; i++) {
        if (block_read(i + INDOES_BLOCKS, ((char *) buf) + (i * BLOCK_SIZE)) != BLOCK_SIZE) {
            return -1;
        }
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

// Removes filename from dir. Returns 0 on success -1 otherwise.
int removeDirRow(struct inode * dir, char * filename, struct filesystem * fs) {
    int numRows;
    if (readInodeData(dir, INT_SIZE, 0, &numRows, fs)) {
        return -1;
    }
    struct dirRow * rows = malloc(numRows * DIR_ROW_SIZE);
    if (readInodeData(dir, numRows * DIR_ROW_SIZE, INT_SIZE, rows, fs)) {
        return -1;
    }
    int i;
    int j = -1;
    for (i = 0; i < numRows; i++) {
        if (!strcmp(rows[i].name, filename)) {
            j = i;
        }
    }

    if (j > -1) {
        int index = j + 1;
        if (index < numRows) {
            struct dirRow * temp = malloc(numRows * DIR_ROW_SIZE);
            size_t size = (numRows - index) * DIR_ROW_SIZE;
            memcpy(temp, rows + index, size);
            memcpy(rows + j, temp, size);
            if (writeInodeData(dir, DIR_ROW_SIZE * (numRows - 1), INT_SIZE, rows, fs)) {
                return -1;
            }
        }
        numRows--;
        if (writeInodeData(dir, INT_SIZE, 0, &numRows, fs)) {
            return -1;
        }
    } else {
        return -1;
    }
}

// Removes the file at path. Returns 0 on success and 
// -1 on failure.
int removeFile(const char * path, struct filesystem * fs) {

    char fpath[PATH_MAX];
    strcpy(fpath, path);
    int inodeIndex = 0;
    char * filename = strrchr(fpath, '/');
    *filename = 0;
    filename++;
    if (*fpath) {
        inodeIndex = getInodeFromPath(fpath);
        if (inodeIndex == -1) {
            return -1;
        }
    }

    struct inode parent;
    if (getInode(inodeIndex, &parent)) {
        return -1;
    }

    struct inode inode;
    inodeIndex = getInodeFromPath(path);
    if (inodeIndex == -1) {
        return -1;
    }

    char emptyBlock[BLOCK_SIZE] = { 0 };

    int i;
    for (i = 0; i < 13; i++) {
        if (inode.block[i]) {
            if (freeBlock(fs, inode.block[i])) {
                return -1;
            }
            if (block_write(inode.block[i], emptyBlock) != BLOCK_SIZE) {
                return -1;
            }
        }
    }
    if (inode.block1) {
        int * temp = malloc(BLOCK_SIZE);
        if (block_read(inode.block1, temp) != BLOCK_SIZE) {
            free(temp);
            return -1;
        }
        for (i = 0; i < INTS_IN_BLOCK; i++) {
            if (temp[i]) {
                if (freeBlock(fs, temp[i])) {
                    free(temp);
                    return -1;
                }
                if (block_write(temp[i], emptyBlock) != BLOCK_SIZE) {
                    free(temp);
                    return -1;
                }
            }
        }
        if (freeBlock(fs, inode.block1)) {
            free(temp);
            return -1;
        }
        if (block_write(inode.block1, emptyBlock) != BLOCK_SIZE) {
            free(temp);
            return -1;
        }
    }
    if (inode.block2) {
        int * temp = malloc(BLOCK_SIZE);
        if (block_read(inode.block2, temp) != BLOCK_SIZE) {
            free(temp);
            return -1;
        }
        for (i = 0; i < INTS_IN_BLOCK; i++) {
            int * temp2 = malloc(BLOCK_SIZE);
            if (block_read(temp[i], temp) != BLOCK_SIZE) {
                free(temp);
                free(temp2);
                return -1;
            }
            int j;
            for (j = 0; i < INTS_IN_BLOCK; i++) {
                if (temp2[j]) {
                    if (freeBlock(fs, temp2[j])) {
                        free(temp);
                        free(temp2);
                        return -1;
                    }
                    if (block_write(temp2[j], emptyBlock) != BLOCK_SIZE) {
                        free(temp);
                        free(temp2);
                        return -1;
                    }
                }
            }
            if (freeBlock(fs, temp[i])) {
                free(temp);
                free(temp2);
                return -1;
            }
            if (block_write(temp[i], emptyBlock) != BLOCK_SIZE) {
                free(temp);
                free(temp2);
                return -1;
            }
        }
        if (freeBlock(fs, inode.block2)) {
            free(temp);
            return -1;
        }
        if (block_write(inode.block2, emptyBlock) != BLOCK_SIZE) {
            free(temp);
            return -1;
        }
    }
    if (removeDirRow(&parent, filename, fs)) {
        return -1;
    }
    memset(&inode, 0, INODE_SIZE);
    if (setInode(inodeIndex, &inode)) {
        return -1;
    }
    return 0;
}

// Returns the inode index for the next free inode.
// Returns -1 on error.
int allocateInode(struct filesystem * fs) {

    // Get all inodes
    struct inode * inodes = malloc(BLOCK_SIZE * fs->numIndoesBlocks);
    if (getInodes(inodes, fs)) {
        free(inodes);
        return -1;
    }

    // Find the first free one and return it
    int i;
    for (i = 0; i < NUM_INODES; i++) {
        if(!inodes[i].info.st_nlink) {
            free(inodes);
            return i;
        }
    }

    // No free inodes were found
    free(inodes);
    return -1;
}

// Add filename to dir with mode. Return 0 on success and -1 otherwise.
int addFileToInode(struct inode * dir, char * filename, mode_t mode, struct filesystem * fs) {

    // Add the directory entry
    int numRows;
    if (readInodeData(dir, INT_SIZE, 0, &numRows, fs)) {
        return -1;
    }
    struct dirRow row;
    row.inodeIndex = allocateInode(fs);
    strcpy(row.name, filename);
    if (writeInodeData(dir, DIR_ROW_SIZE, INT_SIZE + (DIR_ROW_SIZE * numRows), &row, fs)) {
        return -1;
    }
    numRows++;
    if (writeInodeData(dir, INT_SIZE, 0, &numRows, fs)) {
        return -1;
    }

    // Add the inode
    struct inode inode;
    inode.info.st_nlink = 1;
    inode.info.st_mode = mode;
    inode.info.st_blksize = BLOCK_SIZE;
    inode.info.st_ino = row.inodeIndex;
    inode.info.st_atime = time(NULL);
    inode.info.st_mtime = time(NULL);
    inode.info.st_ctime = time(NULL);
    if (mode & S_IFDIR) {
        numRows = 0;
        if (writeInodeData(&inode, INT_SIZE, 0, &numRows, fs)) {
            return -1;
        }
    }
    if (setInode(row.inodeIndex, &inode)) {
        return -1;
    }
    return 0;
}

// Add the path as mode. Return 0 on success and -1 otherwise.
int addFilePath(const char * path, mode_t mode, struct filesystem * fs) {

    // Find the parent inode index
    char fpath[PATH_MAX];
    strcpy(fpath, path);
    int inodeIndex = 0;
    char * filename = strrchr(fpath, '/');
    *filename = 0;
    filename++;
    if (*fpath) {
        inodeIndex = getInodeFromPath(fpath);
        if (inodeIndex == -1) {
            return -1;
        }
    }

    // Add the file to parent
    struct inode inode;
    if (getInode(inodeIndex, &inode)) {
        return -1;
    }
    if (addFileToInode(&inode, filename, mode, fs)) {
        return -1;
    }
    if (setInode(inodeIndex, &inode)) {
        return -1;
    }

    return 0;
}

// Frees the inodes at index.
// Returns 0 on success and -1
// on failure.
// int freeInode(int index) {

//     struct inode inode;
//     if (getInode(index, &inode)) {
//         return -1;
//     }


// }

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
            (inode->info.st_blocks)++;
        }
        return inode->block[blockIndex];

    // If blockIndex is in the singly indirect blocks then handle that
    } else if (blockIndex >= startIndex1 && blockIndex <= endIndex1) {

        // Allocate a block for block1 if not already
        if (!(inode->block1)) {
            int temp = allocateBlock(fs);
            if (!(temp > 0)) {
                return -1;
            }
            inode->block1 = temp;
        }

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
            (inode->info.st_blocks)++;
        }
        free(buf);
        return buf[localIndex];

    // If blockIndex is in the doubly indirect blocks then handle that
    } else if (blockIndex >= startIndex2 && blockIndex <= endIndex2) {

        // Allocate a block for block2 if not already
        if (!(inode->block2)) {
            int temp = allocateBlock(fs);
            if (!(temp > 0)) {
                return -1;
            }
            inode->block2 = temp;
        }
        
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
            (inode->info.st_blocks)++;
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
// Return 0 on success and -1 on error.
int writeInodeData(struct inode * inode, size_t size, off_t offset, const void * buf, struct filesystem * fs) {

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
        if (block_write(blocks[i], dataBlock + (i * BLOCK_SIZE)) != BLOCK_SIZE) {
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

// Test function
void writeBytesToInode(struct inode * inode, size_t num, void * buf, size_t bufSize, struct filesystem * fs) {

    int rounds = (num / bufSize);
    if (num % bufSize) {
        rounds++;
    }

    int i;
    for (i = 0; i < rounds; i++) {
        int write = writeInodeData(inode, bufSize, i * bufSize, buf, fs);
    }
}

// Initializes a directory return 0 on succes and -1 on failure
int initializeDirectory(struct inode * inode, int index, int parent, struct filesystem * fs) {

    inode->info.st_mode = S_IFDIR;
    inode->info.st_ino = index;
    inode->info.st_blksize = BLOCK_SIZE;
    inode->info.st_nlink = 1;
    inode->info.st_atime = time(NULL);
    inode->info.st_mtime = time(NULL);
    inode->info.st_ctime = time(NULL);

    int numRows = 0;

    if (writeInodeData(inode, INT_SIZE, 0, &numRows, fs) != BLOCK_SIZE) {
        return -1;
    }

    return 0;
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
    fs->rootInodeIndex = 0;
    
    // Write calculated numbers to fisrt block
    block_write(FS_BLOCK, buf);

    // Log the numbers
    log_msg("\ndisk size: %lu\n", MAX_DISK_SIZE);
    log_msg("block size: %lu\n", BLOCK_SIZE);
    log_msg("inodes size: %lu\n", INODES_SIZE);
    log_msg("inode size: %lu\n", INODE_SIZE);
    log_msg("bitmap size: %lu\n", fs->dataBlocks * BLOCK_SIZE);
    log_msg("max file size: %lu\n\n", MAX_FILE_SIZE);
    log_msg("number of disk blocks: %d\n", NUM_BLOCKS);
    log_msg("number of inodes blocks: %d\n", fs->numIndoesBlocks);
    log_msg("number of bitmap blocks: %d\n", fs->numBitmapBlocks);
    log_msg("number of data blocks: %d\n", fs->numDataBlocks);
    log_msg("max blocks per file: %d\n\n", MAX_FILE_BLOCKS);
    log_msg("metadata block number: %d\n", FS_BLOCK);
    log_msg("inodes start at block: %d\n", INDOES_BLOCKS);
    log_msg("bitmap starts at block: %d\n", fs->bitmapBlocks);
    log_msg("data blocks start at block: %d\n\n", fs->dataBlocks);

    // Initialize root inode
    struct inode inode;
    memset(&inode, 0, sizeof(struct inode));
    initializeDirectory(&inode, 0, 0, fs);
    setInode(0, &inode);

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

// Return the index of the inode refrenced by the child of dir with
// the name childName. Return -1 on error
int getInodeIndexFromDir(struct inode * dir, char * childName) {

    // Check if directory
    if (!(dir->info.st_mode & S_IFDIR)) {
        return -1;
    }

    // Get data from disk
    struct filesystem fs;
    if (getFilesystem(&fs)) {
        return -1;
    }
    int numRows;
    if (readInodeData(dir, INT_SIZE, 0, &numRows, &fs)) {
        return -1;
    }
    struct dirRow * rows = malloc(DIR_ROW_SIZE * numRows);
    if (readInodeData(dir, DIR_ROW_SIZE * numRows, INT_SIZE, rows, &fs)) {
        return -1;
    }

    // Search for childName
    int i;
    for (i = 0; i < numRows; i++) {
        if (!strcmp(rows[i].name, childName)) {
            free(rows);
            return rows[i].inodeIndex;
        }
    }

    free(rows);
    return -1;
}

// Return the inode index refrenced by path. Return -1 on error.
int getInodeFromPath(const char * path) {

    // Return root inode if root
    if (!strcmp(path, "/")) {
        return 0;

    // If not root
    } else { 

        // Get root inode
        struct inode inode;
        if (getInode(0, &inode)) {
            return -1;
        }

        // Loop through path tokens
        char fpath[PATH_MAX];
        strcpy(fpath, path);
        char * next = strtok(fpath, "/");
        while (next) {

            char * current = next;
            next = strtok(NULL, "/");
            
            // Find child
            int childInode = getInodeIndexFromDir(&inode, current);
            if (childInode == -1) {
                return -1;
            }
            if (!next) {
                return childInode;
            }

            // Store the inode of the child to continue the
            // search.
            if (getInode(childInode, &inode)) {
                return -1;
            }
        }

    }
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

    int inodeIndex = getInodeFromPath(path);
    if (inodeIndex == -1) {
        return -ENOENT;
    }
    struct inode inode;
    getInode(inodeIndex, &inode);
    memcpy(statbuf, &(inode.info), sizeof(struct stat));

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
    
    struct filesystem fs;
    getFilesystem(&fs);

    addFilePath(path, mode, &fs);
    
    return retstat;
}

/** Remove a file */
int sfs_unlink(const char *path)
{
    int retstat = 0;
    log_msg("sfs_unlink(path=\"%s\")\n", path);

    struct filesystem fs;
    if (getFilesystem(&fs)) {
        return -ENOENT;
    }

    if (removeFile(path, &fs)) {
        return -ENOENT;
    }

    
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

    // fi->flags = O_CREAT|O_EXCL|O_TRUNC;
    
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

    int inodeIndex = getInodeFromPath(path);
    if (inodeIndex == -1) {
        return -ENOENT;
    }

    struct inode inode;
    if (getInode(inodeIndex, &inode)) {
        return -ENOENT;
    }

    struct filesystem fs;
    if (getFilesystem(&fs)) {
        return -ENOENT;
    }

    size_t totalSize = size + offset;
    if (totalSize > inode.info.st_size) {
        size_t fileSize = inode.info.st_size - offset;
        if (readInodeData(&inode, fileSize, offset, buf, &fs)) {
            return -ENOENT;
        }
        memset(buf + fileSize, 0, size - fileSize);
    } else {
        if (readInodeData(&inode, size, offset, buf, &fs)) {
            return -ENOENT;
        }
    }

    return size;
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
    
    int inodeIndex = getInodeFromPath(path);
    if (inodeIndex == -1) {
        return -ENOENT;
    }

    struct inode inode;
    if (getInode(inodeIndex, &inode)) {
        return -ENOENT;
    }

    struct filesystem fs;
    if (getFilesystem(&fs)) {
        return -ENOENT;
    }

    if (writeInodeData(&inode, size, offset, buf, &fs)) {
        return -ENOENT;
    }

    if (setInode(inodeIndex, &inode)) {
        return -ENOENT;
    }
    
    return size;
}


/** Create a directory */
int sfs_mkdir(const char *path, mode_t mode)
{
    int retstat = 0;
    log_msg("\nsfs_mkdir(path=\"%s\", mode=0%3o)\n",
	    path, mode);

    struct filesystem  fs;
    if (getFilesystem(&fs)) {
        return -ENOENT;
    }

    if (addFilePath(path, mode | S_IFDIR, &fs)) {
        return -ENOENT;
    }
   
    
    return retstat;
}


/** Remove a directory */
int sfs_rmdir(const char *path)
{
    int retstat = 0;
    log_msg("sfs_rmdir(path=\"%s\")\n",
	    path);


    struct filesystem fs;
    if (getFilesystem(&fs)) {
        return -ENOENT;
    }

    if (removeFile(path, &fs)) {
        return -ENOENT;
    }
    
    
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

    int inodeIndex = getInodeFromPath(path);
    if (inodeIndex == -1) {
        return ENOENT;
    }

    struct inode inode;
    if (getInode(inodeIndex, &inode)) {
        return ENOMEM;
    }

    if (!(inode.info.st_mode & S_IFDIR)) {
        return ENOTDIR;
    }

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

    struct filesystem fs;
    getFilesystem(&fs);
    struct inode inode;
    int inodeIndex = getInodeFromPath(path);
    if (inodeIndex == -1) {
        return -ENOENT;
    }
    if (getInode(inodeIndex, &inode)) {
        return -ENOENT;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    int numFiles;
    if (readInodeData(&inode, INT_SIZE, 0, &numFiles, &fs)) {
        return -ENOENT;
    }
    struct dirRow * rows = malloc(DIR_ROW_SIZE * numFiles);
    if (readInodeData(&inode, DIR_ROW_SIZE * numFiles, INT_SIZE, rows, &fs)) {
        free(rows);
        return -ENOENT;
    }
    int i;
    for (i = 0; i < numFiles; i++) {
        struct inode temp;
        getInode(rows[i].inodeIndex, &temp);
        filler(buf, rows[i].name, &(temp.info), 0);
    }
    free(rows);
    
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
