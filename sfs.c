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
#define NUM_BLOCKS (MAX_DISK_SIZE / BLOCK_SIZE)
#define NUM_INODES 128
#define INODES_SIZE (sizeof(struct stat) * NUM_INODES)
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

///////////////////////////////////////////////////////////
//
// Prototypes for all these functions, and the C-style comments,
// come indirectly from /usr/include/fuse.h
//

// Save all Inodes into buf,
// return 0 if successful, -1 otherwise
int getInodes(struct stat * buf) {
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
int setInodes(struct stat * buf) {
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
    char buffer[BLOCK_SIZE];
    if (block_read(FS_BLOCK, buffer) != BLOCK_SIZE) {
        return -1;
    }
    memcpy(buf, buffer, sizeof(struct filesystem));
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
int allocateBlock() {

    // Getting filesystem metadata
    struct filesystem fs;
    if (getFilesystem(&fs)) {
        return -1;
    }

    // Getting the bitmap
    char bitmap[BLOCK_SIZE * fs.numBitmapBlocks];
    if (getBitmap(&fs, bitmap)) {
        return -1;
    }

    // Finding the next available block and updating the bitmap
    int i;
    for (i = 0; i < (BLOCK_SIZE * fs.numBitmapBlocks); i++) {
        int j;
        for (j = 0; j < 8; j++) {
            int index = (i * 8) + j;
            if (index >= fs.numDataBlocks) {
                return 0;
            }
            char bit = (bitmap[i] >> (7 - j)) & 1;
            if (!bit) {
                bitmap[i] = bitmap[i] | (1 << (7 - j));
                int offset = i / BLOCK_SIZE;
                block_write(offset + fs.bitmapBlocks, bitmap + offset);
                return index + fs.dataBlocks;
            }
        }
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

    // Zero out all blocks except for 1st
    disk_open(DISKFILE);
    char buf[BLOCK_SIZE] = { 0 };
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

    for (i = 0; i < 5000; i++) {
        int test = allocateBlock();
        log_msg("test %d: %d\n", i, test);
    }

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
