#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h> 
#include <fcntl.h>

#define SECUREERASE 1
// if 0 - while erasing only change bitmaps, if 1 - erasing also inodes and data blocks
#define BLOCKSIZE 1024
#define MAXNAME 20
#define INODESIZE 32
#define SYSBLOCKS 3
// 1 superblock, 1 inodes bitmap, 1 data blocks bitmap
#define MINSIZE (5 * 1024)
// 3 sys blocks, 1 inodes block, 1 data block
#define MAXSIZE (3072 + 32768 + 1048576)
// 3 sys blocks, 32 inodes block 32*INODESIZE = 1024 inodes, 1024 data blocks
#define INFOSIZE 468

/*script:
#!/bin/sh
cc vfs.c -o vfs
echo "Create 102400 bytes vfs - mvfs\n" > results
./vfs -c 102400 mvfs
./vfs -p mvfs >> results
echo "\n" >> results
./vfs -m mvfs >> results
echo "\n\nAdd 1.4kb, 5.5kb and 7.5kb files\n" >> results
./vfs -d 1,4kb mvfs
./vfs -d 5,5kb mvfs
./vfs -d 7,5kb mvfs
./vfs -p mvfs >> results
echo "\n" >> results
./vfs -m mvfs >> results
echo "\n\nRemove 5.5kb file and add 7.8kb file\n" >> results
./vfs -r 5,5kb mvfs
./vfs -d 7,8kb mvfs
./vfs -p mvfs >> results
echo "\n" >> results
./vfs -m mvfs >> results
echo "\n\nErase mvfs\n" >> results
./vfs -e mvfs
./vfs -p mvfs >> results
echo "\n" >> results
./vfs -m mvfs >> results
*/

char vfsInfo[INFOSIZE] =
"Virtual File System Structure:\n\
[S][i][d][I][I][I]..[D][D][D]..\n\
[S] - superblock\n\
[i] - inodes alloc bitmap\n\
[d] - data alloc bitmap\n\
[I] - inodes block\n\
[D] - data block";

struct inode {
	unsigned size;
	unsigned blocks;
	unsigned begin;
	char name[MAXNAME];
}; // sizeof = INODESIZE

struct vfs {
	char name[MAXNAME];
	char info[INFOSIZE];
	unsigned size;			// real size
	unsigned howManyBlocks;
	unsigned inodesBegin;	// where inodes blocks begin
	unsigned dataBegin;		// where data blocks begin
	unsigned inodesBlocksNum;
	unsigned dataBlocksNum;
}; // sizeof = 512

int same_filenames(char *filename1, char *filename2) {
	unsigned i;
	for (i = 0; i < MAXNAME; ++i) {
		if (filename1[i] != filename2[i])
			return 0;
		if (filename1[i] == '\0')
			return 1;
	}
	return 1;
}

int get_indexes(FILE *fp, unsigned *indexesBuffer, unsigned whereStart) {
	char fchar;
	unsigned howMany = 0, n;
	fseek(fp, whereStart, 0);
	for (n = 0; n < BLOCKSIZE; ++n) { // get indexes of inodes or blocks in use, depends whereStart
		fchar = fgetc(fp);
		if (fchar == '1')
			indexesBuffer[howMany++] = n;
		else if (fchar == '0')
			continue;
		else
			break;
	}
	return howMany;
}

int file_exists(char *filename, FILE *fp, struct vfs *currVFS) { // return index of existing file or -1 if not exists
	unsigned n, howManyFiles;
	struct inode tempInode;
	unsigned inodesIndexes[BLOCKSIZE];
	howManyFiles = get_indexes(fp, inodesIndexes, BLOCKSIZE);	 // get inodes indexes
	for (n = 0; n < howManyFiles; ++n) {
		fseek(fp, currVFS->inodesBegin + inodesIndexes[n] * INODESIZE, 0);
		fread(&tempInode, INODESIZE, 1, fp);
		if (same_filenames(tempInode.name, filename))
			break; // file w/ same filename exists
	}
	if (n == howManyFiles)
		return -1; // didnt find file w/ same filename
	return n;
}

int copy_files(FILE *from, FILE *to, unsigned size) {
	char BUFFER[BLOCKSIZE];
	unsigned blocks = size / BLOCKSIZE;
	unsigned i;
	if (size % BLOCKSIZE > 0)
		++blocks;
	for (i = 0; i < blocks; ++i) {
		if (blocks - i == 1 && size % BLOCKSIZE != 0) { // coping last data block if incompleted
			fread(BUFFER, sizeof(char), size % BLOCKSIZE, from);
			fwrite(BUFFER, sizeof(char), size % BLOCKSIZE, to);
		}
		else {
			fread(BUFFER, sizeof(char), BLOCKSIZE, from);
			fwrite(BUFFER, sizeof(char), BLOCKSIZE, to);
		}
	}
	return 0;
}

int enough_space(FILE *fp, unsigned blocks) { // return index of first block where is enough space
	unsigned index = 0, freeblocks = 0, begin;
	char fchar;
	while (index < BLOCKSIZE) {
		fchar = fgetc(fp);
		if (fchar == '1')
			++index;
		else if (fchar == '0') {
			begin = index;
			++index;
			++freeblocks; // first free block
			while (freeblocks < blocks && index < BLOCKSIZE) {
				fchar = fgetc(fp);
				++index;
				if (fchar == '0')
					++freeblocks;
				else if (fchar == '1') {
					freeblocks = 0;
					break;
				}
				else
					return -1;
			}
		}
		else
			return -1; // reach end of bitmap
		if (freeblocks == blocks)
			return begin;
	}
	return -1; // index == blocksize
}

int download(char *fileToDownload, char *virtualDisk) {
	FILE *fp, *dfp;
	unsigned i, n = 0, m = 0;
	int begin = 0;
	char fchar;
	while (n < MAXNAME)
		if (fileToDownload[n++] == '\0')
			break;
	if (n == MAXNAME)
		return 1; // too long filename

	dfp = fopen(fileToDownload, "rb");
	fp = fopen(virtualDisk, "r+b");
	if (fp == NULL || dfp == NULL)
		return 1;
	struct vfs currVFS;
	struct inode newInode;
	fread(&currVFS, sizeof(struct vfs), 1, fp);
	if (file_exists(fileToDownload, fp, &currVFS) >= 0)
		return 1; // file w/ same filename exists in vfs

	fseek(dfp, 0, SEEK_END);
	newInode.size = (unsigned)ftell(dfp);

	for (i = 0; i < MAXNAME; ++i) {
		if (fileToDownload[i] == '\0') {
			while (i < MAXNAME)
				newInode.name[i++] = '\0';
			break;
		}
		newInode.name[i] = fileToDownload[i];
	}
	fseek(fp, BLOCKSIZE, 0);
	
	for (n = 0, fchar = fgetc(fp);			// search free space for inode
		n < BLOCKSIZE && fchar == '1';		// do it as long as it's '1' = inode space in use
		++n, fchar = fgetc(fp)) {}			// get next char after each loop
	if (fchar != '0')						// fchar may be '0' - found inode space, '\0' - reach end and not found
		return 1;

	newInode.blocks = newInode.size / BLOCKSIZE;
	if (newInode.size % BLOCKSIZE > 0)
		++newInode.blocks;

	fseek(fp, 2 * BLOCKSIZE, 0);
	begin = enough_space(fp, newInode.blocks);	// need to use (int)begin because enough_space may return -1
	if (begin < 0)
		return 1;								// not enough space
	newInode.begin = begin;
	fseek(fp, BLOCKSIZE + n, 0);				// n - inode index
	fputc('1', fp);
	fseek(fp, 2 * BLOCKSIZE + newInode.begin, 0);
	for (i = 0; i < newInode.blocks; ++i)
		fputc('1', fp);

	fseek(fp, currVFS.inodesBegin + n * INODESIZE, 0);
	fwrite(&newInode, INODESIZE, 1, fp);		// writing new inode

	fseek(fp, currVFS.dataBegin + newInode.begin * BLOCKSIZE, 0);	// go to beginning of file data blocks
	fseek(dfp, 0, 0);												// go to beginning of download file
	copy_files(dfp, fp, newInode.size);								// from dfp to fp
	fclose(fp);
	fclose(dfp);
	return 0;
}

int upload(char *fileToUpload, char *virtualDisk) {
	FILE *fp, *ufp;
	int fileInodeIndex;

	ufp = fopen(fileToUpload, "wb");
	fp = fopen(virtualDisk, "rb");
	if (fp == NULL || ufp == NULL)
		return 1;
	struct vfs currVFS;
	fread(&currVFS, sizeof(struct vfs), 1, fp);
	fileInodeIndex = file_exists(fileToUpload, fp, &currVFS);
	if (fileInodeIndex < 0)
		return 1; // didnt find file w/ this filename
	struct inode tempInode;
	fseek(fp, currVFS.inodesBegin + fileInodeIndex * INODESIZE, 0);
	fread(&tempInode, INODESIZE, 1, fp);
	fseek(fp, currVFS.dataBegin + tempInode.begin * BLOCKSIZE, 0);
	copy_files(fp, ufp, tempInode.size);
	fclose(fp);
	fclose(ufp);
	return 0;
}

int print_files(char *virtualDisk) {
	FILE *fp;
	fp = fopen(virtualDisk, "rb");
	if (fp == NULL)
		return 1;
	struct vfs currVFS;
	struct inode currInode;
	fread(&currVFS, sizeof(struct vfs), 1, fp);
	unsigned inodesIndexes[BLOCKSIZE];
	unsigned i;
	unsigned howManyFiles = get_indexes(fp, inodesIndexes, BLOCKSIZE);
	if (howManyFiles == 0) {
		printf("No files in %s\n", currVFS.name);
		return 0;
	}
	printf("Files in %s:\n\n", currVFS.name);
	for (i = 0; i < howManyFiles; ++i) {
		fseek(fp, currVFS.inodesBegin + inodesIndexes[i] * INODESIZE, 0);
		fread(&currInode, INODESIZE, 1, fp);
		printf("Filename: %s\n\tSize: %d\n\tSize on disk: %d\n", 
			currInode.name, currInode.size, currInode.blocks * BLOCKSIZE);
	}
	fclose(fp);
	return 0;
}

int remove_file(char *fileToRmv, char *virtualDisk) {
	FILE *fp;
	int fileInodeIndex;
	unsigned i;
	fp = fopen(virtualDisk, "r+b");
	if (fp == NULL)
		return 1;
	struct vfs currVFS;
	fread(&currVFS, sizeof(struct vfs), 1, fp);
	fileInodeIndex = file_exists(fileToRmv, fp, &currVFS);
	if (fileInodeIndex < 0)
		return 1; // didnt find file w/ this filename
	struct inode tempInode;
	char BUFFER[BLOCKSIZE] = "";
	fseek(fp, currVFS.inodesBegin + fileInodeIndex * INODESIZE, 0);
	fread(&tempInode, INODESIZE, 1, fp);
	if (SECUREERASE) {
		fseek(fp, currVFS.inodesBegin + fileInodeIndex * INODESIZE, 0);
		fwrite(BUFFER, sizeof(char), INODESIZE, fp); // erasing file inode
		fseek(fp, currVFS.dataBegin + tempInode.begin * BLOCKSIZE, 0);
		for (i = 0; i < tempInode.blocks; ++i) { // erasing file data blocks
			if (tempInode.blocks - i == 1 && tempInode.size % BLOCKSIZE != 0)
				fwrite(BUFFER, sizeof(char), tempInode.size % BLOCKSIZE, fp);
			else
				fwrite(BUFFER, sizeof(char), BLOCKSIZE, fp);
		}
	}
	for (i = 0; i < BLOCKSIZE; ++i)
		BUFFER[i] = '0';
	fseek(fp, BLOCKSIZE + fileInodeIndex, 0);
	fwrite(BUFFER, sizeof(char), 1, fp); // change inode bitmap
	fseek(fp, 2 * BLOCKSIZE + tempInode.begin, 0);
	fwrite(BUFFER, sizeof(char), tempInode.blocks, fp); // change data block bitmap
	fclose(fp);
	return 0;
}

int rmv_virt_disk(char *virtualDisk) {
	FILE *fp;
	fp = fopen(virtualDisk, "r+b");
	if (fp == NULL)
		return 1;
	struct vfs currVFS;
	fread(&currVFS, sizeof(struct vfs), 1, fp);

	unsigned inodesIndxes[BLOCKSIZE];
	unsigned dataIndexes[BLOCKSIZE];
	unsigned i;
	char BUFFER[BLOCKSIZE] = "";
	unsigned j = 0, k = 0, howManyFiles = 0, howManyBlocksInUse = 0;

	if (SECUREERASE) {
		howManyFiles = get_indexes(fp, inodesIndxes, BLOCKSIZE);
		howManyBlocksInUse = get_indexes(fp, dataIndexes, 2 * BLOCKSIZE);
		for (i = 0; i < howManyFiles; ++i) {
			fseek(fp, currVFS.inodesBegin + inodesIndxes[i] * INODESIZE, 0);
			fwrite(BUFFER, sizeof(char), INODESIZE, fp);
		}
		for (i = 0; i < howManyBlocksInUse; ++i) {
			fseek(fp, currVFS.dataBegin + dataIndexes[i] * BLOCKSIZE, 0);
			fwrite(BUFFER, sizeof(char), BLOCKSIZE, fp);
		}
	}
	for (i = 0; i < BLOCKSIZE; ++i)
		BUFFER[i] = '0';
	fseek(fp, BLOCKSIZE, 0);
	fwrite(BUFFER, sizeof(char), currVFS.inodesBlocksNum, fp);
	fseek(fp, 2 * BLOCKSIZE, 0);
	fwrite(BUFFER, sizeof(char), currVFS.dataBlocksNum, fp);
	fclose(fp);
	return 0;
}

int print_map(char *virtualDisk) {
	FILE *fp;
	char fchar;
	unsigned i;
	fp = fopen(virtualDisk, "rb");
	if (fp == NULL)
		return 1;
	struct vfs currVFS;
	fread(&currVFS, sizeof(struct vfs), 1, fp);
	printf("%s\n\nVFS name: \t\t%s\nVFS size: \t\t%d\nVFS blocks: \t\t%d\n", currVFS.info, currVFS.name, currVFS.size, currVFS.howManyBlocks);
	printf("VFS inodes blocks: \t%d\nVFS data blocks: \t%d\n", currVFS.inodesBlocksNum, currVFS.dataBlocksNum);
	printf("This VFS sturcture: \t[S][i][d]");
	for (i = 0; i < currVFS.inodesBlocksNum; ++i)
		printf("[I]");
	for (i = 0; i < currVFS.dataBlocksNum; ++i)
		printf("[D]");
	printf("\nInodes bitmap: \t\t");
	fseek(fp, BLOCKSIZE, 0);
	for (i = 0; i < BLOCKSIZE; ++i) {
		fchar = fgetc(fp);
		if (fchar == '1')
			printf("[1]");
		else if (fchar == '0')
			printf("[0]");
		else
			break;
	}
	printf("\nData bitmap: \t\t");
	fseek(fp, 2 * BLOCKSIZE, 0);
	for (i = 0; i < BLOCKSIZE; ++i) {
		fchar = fgetc(fp);
		if (fchar == '1')
			printf("[1]");
		else if (fchar == '0')
			printf("[0]");
		else
			break;
	}
	fclose(fp);
	return 0;
}

int create_vfs(unsigned size, char *name) {
	if (size < MINSIZE || size > MAXSIZE)
		return 1;
	struct vfs newVFS;
	unsigned i;
	unsigned numOfAvailableBlocks = size / BLOCKSIZE - SYSBLOCKS;
	unsigned blocksForInodes = numOfAvailableBlocks / (BLOCKSIZE / INODESIZE);
	int temp = numOfAvailableBlocks - (blocksForInodes - 1) * (BLOCKSIZE / INODESIZE);
	if (numOfAvailableBlocks % (BLOCKSIZE / INODESIZE) > 0) // calculate how many inodes blocks needed
		++blocksForInodes;
	if (temp % (BLOCKSIZE / INODESIZE) <= (int)blocksForInodes)
		--blocksForInodes;
	unsigned blocksForData = numOfAvailableBlocks - blocksForInodes;

	for (i = 0; i < MAXNAME; ++i) { // copy name
		if (name[i] == '\0') {
			while (i < MAXNAME)
				newVFS.name[i++] = '\0';
			break;
		}
		newVFS.name[i] = name[i];
	}
	for (i = 0; i < INFOSIZE; ++i) { // copy info
		if (vfsInfo[i] == '\0') {
			while (i < INFOSIZE)
				newVFS.info[i++] = '\0';
			break;
		}
		newVFS.info[i] = vfsInfo[i];
	}
	newVFS.size = size;
	newVFS.howManyBlocks = numOfAvailableBlocks + SYSBLOCKS;		// num of all blocks
	newVFS.inodesBegin = SYSBLOCKS * BLOCKSIZE;						// addr where inodes blocks begin
	newVFS.dataBegin = (SYSBLOCKS + blocksForInodes) * BLOCKSIZE;	// addr where data blocks begin
	newVFS.inodesBlocksNum = blocksForInodes;
	newVFS.dataBlocksNum = numOfAvailableBlocks - blocksForInodes;
	unsigned suIndx = sizeof(struct vfs);
	unsigned inodeBitmapIndx = blocksForInodes * (BLOCKSIZE / INODESIZE);
	unsigned dataBitmapIndx = numOfAvailableBlocks - blocksForInodes;
	if (inodeBitmapIndx > dataBitmapIndx)
		inodeBitmapIndx = dataBitmapIndx; // no need to have more inodes than data blocks
	char BUFFERNULL[BLOCKSIZE] = "";
	char BUFFER0[BLOCKSIZE];
	for (i = 0; i < BLOCKSIZE; ++i)
		BUFFER0[i] = '0';
	FILE *fp;
	fp = fopen(name, "wb");
	if (fp == NULL)
		return 1;

	fwrite(&newVFS, sizeof(struct vfs), 1, fp);
	fwrite(BUFFERNULL, sizeof(char), BLOCKSIZE - suIndx, fp);			// fill SUblock

	fwrite(BUFFER0, sizeof(char), inodeBitmapIndx, fp);
	fwrite(BUFFERNULL, sizeof(char), BLOCKSIZE - inodeBitmapIndx, fp);	// filling inodes bitmap block
	
	fwrite(BUFFER0, sizeof(char), dataBitmapIndx, fp);
	fwrite(BUFFERNULL, sizeof(char), BLOCKSIZE - dataBitmapIndx, fp);	// filling data bitmap block

	for (i = 0; i < blocksForInodes; ++i)
		fwrite(BUFFERNULL, sizeof(char), BLOCKSIZE, fp);
	for (i = 0; i < numOfAvailableBlocks - blocksForInodes; ++i) 
		fwrite(BUFFERNULL, sizeof(char), BLOCKSIZE, fp);
	if (size - (numOfAvailableBlocks + SYSBLOCKS) * BLOCKSIZE > 0)
		fwrite(BUFFERNULL, sizeof(char), size - (numOfAvailableBlocks + SYSBLOCKS) * BLOCKSIZE, fp);
	fclose(fp);
	return 0;
}

int main(int argc, char *argv[]) {
	if (argc < 3)
		return 1;
	if (sizeof(struct inode) != INODESIZE)
		fprintf(stderr, "Fatal error occured, check inode structure\n");
	unsigned vfsSize;
	char param = *(argv[1] + 1);
	char ret;
	switch (param) {
	case 'c':
		if (argc != 4)
			return 1;
		vfsSize = (unsigned)strtoul(argv[2], NULL, 0);
		ret = create_vfs(vfsSize, argv[3]); // create
		break;
	case 'd':
		if (argc != 4)
			return 1;
		ret = download(argv[2], argv[3]); // download from linux disk
		break;
	case 'u':
		if (argc != 4)
			return 1;
		ret = upload(argv[2], argv[3]); // upload
		break;
	case 'p':
		if (argc != 3)
			return 1;
		ret = print_files(argv[2]); // print files
		break;
	case 'r':
		if (argc != 4)
			return 1;
		ret = remove_file(argv[2], argv[3]); // remove file
		break;
	case 'e':
		if (argc != 3)
			return 1;
		ret = rmv_virt_disk(argv[2]); // remove virtual disk
		break;
	case 'm':
		if (argc != 3)
			return 1;
		ret = print_map(argv[2]); // map of used blocks
		break;
	default:
		ret = 1;
	}
	if (ret == 1)
		fprintf(stderr, "An error occured, check arguments\n");
	return 0;
}