# CS OS: Assignment 3 - Simple File System

## Compilation

With the following files in the directory:

* `compile.sh`
* `sfs.c`
* `assignment3_stub.tar.gz`

Run:

```
chmod 700 compile.sh && ./compile.sh
```

## Features

* Create files
* Delete files
* Get the `stat` of files
* Open files
* Close files
* Read files
* Write files
* Read directories

### Extensions

* Single indirection: look at `getInodeBlock()` line `580`
* Double indirection: look at `getInodeBlock()` line `618`
* Directory support
  * Open directories
  * Close directories
  * Make directories
  * Delete directories

## Disk Structure

This file system is organized in order as such:

1. Metadata
1. Inodes
1. Bitmap for data blocks
1. Data blocks

You can check the size of each partition and number of blocks in the log when you mount. This is because everything is dynamically calculated.

## Implementation

`Sfs_Init()`: The call to this method will open up a diskfile, initialize a buffer to the size of `BLOCK_SIZE`. The filesystem struct is the initialized and calculations for the filesystem are performed,  `numIndoesBlocks`, `blocksLeft`, `bitsInBlocks`.

`Sfs_Destroy()`: The call to this function will call the `diskclose()`.

Every file and directory has an inode. The inode contains metadata about the file and has references to its blocks. In order to obtain the inode block, the method `getInodeBlock()` is called. The parameters are inode, block index, and filesystem. The first thirteen index references in the inode block are direct references, the 14th index is a single indirect which obtains the correct location by adding to the start index the value of `INTS _IN_BLOCK`, and the 15th reference is the double indirect references, which starts it’s index 1 + end of the double indirect reference, and obtains the end index by squaring the value `INTS_IN_BLOCKS` added to the start index. The inode data structure, called `struct inode` is composed of struct stat info, an int array of 13 indexes, a int block array for single indirect references, and another int block array for double indirect references.

Directories are similar to files except the data blocks are file information, that contain file names and pointers to files inodes. When you search a file path, it goes to the root inode and then searches through the children of the root path, and so on until a match is discovered.

`WriteInodeData()`, finds the inode through file path, bring the data block into memory, update the data block, then write it back into the disk.

## Data Structures

The file system structure that is stored on the flat file is made up of, rootInodeIndex, the number of Inode Blocks, Bitmap Blocks, the number of bitmap blocks, data blocks and the number of data blocks.

## Test Cases

You can call `printBitmap()` to print the bitmap in the logs.

```
[ami76@grep mountdir]$ mkdir test
[ami76@grep mountdir]$ ls
test
[ami76@grep mountdir]$ cd test
[ami76@grep test]$ ls
[ami76@grep test]$ touch test.txt
[ami76@grep test]$ ls
test.txt
[ami76@grep test]$ echo "THis is a test file. How are you" >> test.txt 
[ami76@grep test]$ ls
test.txt
[ami76@grep test]$ cat test.txt 
THis is a test file. How are you
[ami76@grep test]$ cd ..
[ami76@grep mountdir]$ ls
test
[ami76@grep mountdir]$ rm -r test/
[ami76@grep mountdir]$ ls
```
