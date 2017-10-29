

#include <fts.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <syslog.h>


#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/xattr.h>



#include <CoreFoundation/CoreFoundation.h>

#undef DEBUG

#ifdef DEBUG



#define logerror(...) syslog(LOG_ERR, __VA_ARGS__);

#else

#define logerror(...) (void)fprintf(stdout,__VA_ARGS__)

#endif

enum
{
	ASF_RESOURCEFORK=2,
	ASF_FINDERINFO=9
};

#define ASF_ENTRY_COUNT	2

typedef struct asf_entry_t
{
	uint32_t entryID;
	ssize_t entrySize;
	
	char * extendedAttributeName;	/* XATTR_FINDERINFO_NAME / XATTR_RESOURCEFORK_NAME */
	
} asf_entry_t;

int goldin_splitfork(FTSENT * inFileHierarchyNode,bool inRemoveExtendAttributes);

void log_write_error(int inError,const char * inFileName);

void log_write_error(int inError,const char * inFileName)
{
	switch(inError)
	{
		case EDQUOT:
			
			logerror("Your quota are exceeded\n");
			
		case ENOSPC:
			
			logerror("Disk is full\n");
			
			break;
			
		default:
			
			logerror("An unknown error (%d) occurred while writing the AppleDouble file of %s\n",inError,inFileName);
			
			break;
	}
}

int goldin_splitfork(FTSENT * inFileHierarchyNode,bool inRemoveExtendAttributes)
{
	int tFileDescriptor=open(inFileHierarchyNode->fts_accpath,O_RDONLY,O_NOFOLLOW);
	
	if (tFileDescriptor==-1)
	{
		// A COMPLETER
		
		return -1;
	}
	
	/* Retrieve the list of extended attributes and their sizes */
	
	ssize_t tBufferSize=flistxattr(tFileDescriptor, NULL, 0, 0);	/* XATTR_NODEFAULT ???? */
	
	if (tBufferSize==-1)
	{
		close(tFileDescriptor);
		
		switch(errno)
		{
			case ENOTSUP:
			case EPERM:
				
				return 0;
				
			default:
				
				logerror("An error occurred when retrieving the list of extended attributes (%d)\n",errno);
				
				close(tFileDescriptor);
				
				return -1;
		}
	}
	
	if (tBufferSize==0)
	{
		close(tFileDescriptor);
		return 0;
	}
	
	char * tNamesBuffer=malloc(tBufferSize*sizeof(char));
	
	if (tNamesBuffer==NULL)
	{
		close(tFileDescriptor);
		
		logerror("An error occurred because available memory is too low\n");
		
		return -1;
	}
	
	asf_entry_t tFinderInfoEntry={
		.entryID=ASF_FINDERINFO,
		.entrySize=0,
		.extendedAttributeName=XATTR_FINDERINFO_NAME
	};
	
	asf_entry_t tResourceForkEntry={
		.entryID=ASF_RESOURCEFORK,
		.entrySize=0,
		.extendedAttributeName=XATTR_RESOURCEFORK_NAME
	};
	
	asf_entry_t * tEntriesPtrArray[ASF_ENTRY_COUNT]={&tFinderInfoEntry,&tResourceForkEntry};
	uint16_t tEntriesCount=0;
	
	ssize_t tBufferReadSize=flistxattr(tFileDescriptor, tNamesBuffer, tBufferSize, 0);
	ssize_t tOffset=0;
	
	while (tOffset<tBufferReadSize)
	{
		char * tAttributeName=tNamesBuffer+tOffset;
		
		if (strncmp(XATTR_FINDERINFO_NAME, tAttributeName, sizeof(XATTR_FINDERINFO_NAME)+1)==0)
		{
			tFinderInfoEntry.entrySize=fgetxattr(tFileDescriptor, tAttributeName, NULL, 0, 0, 0);
			
			tEntriesCount++;
		}
		else if (strncmp(XATTR_RESOURCEFORK_NAME, tAttributeName, sizeof(XATTR_RESOURCEFORK_NAME)+1)==0)
		{
			tResourceForkEntry.entrySize=fgetxattr(tFileDescriptor, tAttributeName, NULL, 0, 0, 0);
			
			tEntriesCount++;
		}
		
		tOffset+=strlen(tAttributeName)+1;
	}
	
	free(tNamesBuffer);
	
	if (tEntriesCount==0)
	{
		/* No extended attributes, no need to split */
		
		close(tFileDescriptor);
		return 0;
	}
	
	tEntriesCount=ASF_ENTRY_COUNT;	// SplitForks create 2 entries even if the Resource Fork is empty
	
	// We need to create the AppleDouble Format file
	
	char * tParentDirectoryPath=dirname(inFileHierarchyNode->fts_accpath);
	
	size_t tAccessPathLength=strlen(tParentDirectoryPath)+1+2+inFileHierarchyNode->fts_namelen+1;
	
	if (tAccessPathLength>FILENAME_MAX)
	{
		logerror("File name is too long. The maximum length allowed is %ld characters\n",(long)FILENAME_MAX);
		
		close(tFileDescriptor);
		
		return -1;
	}
	
	char tAppleDoubleFormatFileAccessPath[FILENAME_MAX];
	
	size_t tValue=(size_t)snprintf(tAppleDoubleFormatFileAccessPath,tAccessPathLength, "%s/._%s",tParentDirectoryPath,inFileHierarchyNode->fts_name);
	
	if (tValue!=(tAccessPathLength-1))
	{
		logerror("File name could not be created\n");
		
		close(tFileDescriptor);
		
		return -1;
	}
	
	FILE * fp=fopen(tAppleDoubleFormatFileAccessPath,"w+");
	
	if (fp==NULL)
	{
		switch(errno)
		{
			case EROFS:
				
				logerror("The AppleDouble file \"._%s\" could not be created because the file system is read-only",inFileHierarchyNode->fts_name);
				
				break;
			
			case EACCES:
				
				logerror("The AppleDouble file \"._%s\" could not be created because you do not have the appropriate permissions",inFileHierarchyNode->fts_name);
				
				break;
				
			case ENOSPC:
				
				logerror("Disk is full\n");
				
				break;
			
			case EDQUOT:
				
			default:
				
				logerror("error: %d",errno);
				
				break;
		}
		
		fclose(fp);
		
		close(tFileDescriptor);
		
		return -1;
	}
	
	// Write the Magic Number
	
	unsigned char tAppleDoubleMagicNumber[4]={0x00,0x05,0x16,0x07};
	
	if (fwrite(tAppleDoubleMagicNumber, sizeof(tAppleDoubleMagicNumber),1, fp)!=1)
	{
		int tError=ferror(fp);
		
		if (tError!=0)
			log_write_error(errno,inFileHierarchyNode->fts_path);
		
		fclose(fp);
		
		close(tFileDescriptor);
		
		return -1;
	}
	
	/* Write the Version Number */
	
	unsigned char tAppleDoubleVersionNumber[4]={0x00,0x02,0x00,0x00};
	
	if (fwrite(tAppleDoubleVersionNumber, sizeof(tAppleDoubleVersionNumber),1, fp)!=1)
	{
		int tError=ferror(fp);
		
		if (tError!=0)
			log_write_error(errno,inFileHierarchyNode->fts_path);
		
		fclose(fp);
		
		close(tFileDescriptor);
		
		return -1;
	}
	
	/* Write the Filler */
	
	unsigned char tAppleDoubleFiller[16]={0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
	
	if (fwrite(tAppleDoubleFiller, sizeof(tAppleDoubleFiller),1, fp)!=1)
	{
		int tError=ferror(fp);
		
		if (tError!=0)
			log_write_error(errno,inFileHierarchyNode->fts_path);
		
		fclose(fp);
		
		close(tFileDescriptor);
		
		return -1;
	}
	
	/* Write the number of Entries */
	
	uint16_t tTransormedNumberOfEntries=tEntriesCount;
	
#ifdef __LITTLE_ENDIAN__
	
	tTransormedNumberOfEntries=CFSwapInt16(tTransormedNumberOfEntries);
	
#endif
	
	if (fwrite(&tTransormedNumberOfEntries, sizeof(uint16_t),1, fp)!=1)
	{
		int tError=ferror(fp);
		
		if (tError!=0)
			log_write_error(errno,inFileHierarchyNode->fts_path);
		
		fclose(fp);
		
		close(tFileDescriptor);
		
		return -1;
	}
	
	/* Write the Entry Descriptors */
	
	uint32_t tEntryDataOffset=0x0000001A+tEntriesCount*12;
	
	for(uint16_t tIndex=0;tIndex<tEntriesCount;tIndex++)
	{
		asf_entry_t * tEntryPtr=tEntriesPtrArray[tIndex];
		
		uint32_t tEntryID=tEntryPtr->entryID;
		uint32_t tEntryOffset=tEntryDataOffset;
		uint32_t tEntryLength=(uint32_t)tEntryPtr->entrySize;
		
		tEntryDataOffset+=tEntryLength;
		
#ifdef __LITTLE_ENDIAN__
		
		tEntryID=CFSwapInt32(tEntryID);
		tEntryOffset=CFSwapInt32(tEntryOffset);
		tEntryLength=CFSwapInt32(tEntryLength);
		
#endif
		
		fwrite(&tEntryID, sizeof(uint32_t),1, fp);
		fwrite(&tEntryOffset, sizeof(uint32_t),1, fp);
		fwrite(&tEntryLength, sizeof(uint32_t),1, fp);
	}
	
	// Write the Entries
	
	for(uint16_t tIndex=0;tIndex<tEntriesCount;tIndex++)
	{
		asf_entry_t * tEntryPtr=tEntriesPtrArray[tIndex];
		
		if (tEntryPtr->entrySize==0)
			continue;
		
		switch (tEntryPtr->entryID)
		{
			case ASF_FINDERINFO:
			{
				char * tAttributeBuffer=malloc(tEntryPtr->entrySize);
				
				if (tAttributeBuffer==NULL)
				{
					logerror("An error occurred because available memory is too low\n");
					
					fclose(fp);
					
					close(tFileDescriptor);
					
					return -1;
				}
				
				ssize_t tAttributeBufferReadSize=fgetxattr(tFileDescriptor, tEntryPtr->extendedAttributeName, tAttributeBuffer, tEntryPtr->entrySize, 0, 0);
				
				if (tAttributeBufferReadSize==-1)
				{
					logerror("An error occurred while reading the \"%s\" attribute (%d)\n",tEntryPtr->extendedAttributeName,errno);
					
					free(tAttributeBuffer);
					
					fclose(fp);
					
					close(tFileDescriptor);
					
					return -1;
				}
				
				if (fwrite(tAttributeBuffer,tAttributeBufferReadSize,1, fp)!=1)
				{
					int tError=ferror(fp);
					
					if (tError!=0)
						log_write_error(errno,inFileHierarchyNode->fts_path);
					
					free(tAttributeBuffer);
					
					fclose(fp);
					
					close(tFileDescriptor);
					
					return -1;
				}
				
				free(tAttributeBuffer);
				
				break;
			}
			case ASF_RESOURCEFORK:
			{
#define GOLDIN_BUFFER_QUARTER_MEGABYTE_SIZE		262144
				
				size_t tAttributeBufferSize=GOLDIN_BUFFER_QUARTER_MEGABYTE_SIZE;
				
				char * tAttributeBuffer=malloc(GOLDIN_BUFFER_QUARTER_MEGABYTE_SIZE);
				
				while (tAttributeBuffer==NULL && tAttributeBufferSize>1024)		// If we can't even allocate 1KB of RAM, it's time to capitulate
				{
					tAttributeBufferSize=tAttributeBufferSize>>1;
					
					tAttributeBuffer=malloc(tAttributeBufferSize);
				}
				
				if (tAttributeBuffer==NULL)
				{
					logerror("An error occurred because available memory is too low\n");
					
					fclose(fp);
					
					close(tFileDescriptor);
					
					return -1;
				}
				
				u_int32_t tPosition=0;
				
				do
				{
					ssize_t tAttributeBufferReadSize=fgetxattr(tFileDescriptor, tEntryPtr->extendedAttributeName, tAttributeBuffer, tAttributeBufferSize, tPosition, 0);
					
					if (tAttributeBufferReadSize==-1)
					{
						logerror("An error occurred while reading the \"%s\" attribute (%d)\n",tEntryPtr->extendedAttributeName,errno);
						
						free(tAttributeBuffer);
						
						fclose(fp);
						
						close(tFileDescriptor);
						
						return -1;
					}
					
					if (fwrite(tAttributeBuffer,tAttributeBufferReadSize,1, fp)!=1)
					{
						int tError=ferror(fp);
						
						if (tError!=0)
							log_write_error(errno,inFileHierarchyNode->fts_path);
						
						free(tAttributeBuffer);
						
						fclose(fp);
						
						close(tFileDescriptor);
						
						return -1;
					}
					
					tPosition+=tAttributeBufferReadSize;
				}
				while (tPosition<tEntryPtr->entrySize);
				
				free(tAttributeBuffer);
				
				break;
			}
				
			default:
				
				break;
		}
	}
	
	if (fclose(fp)!=0)
	{
		close(tFileDescriptor);
		
		switch(errno)
		{
			default:
				
				break;
		}
		
		return -1;
	}
	
	if (inRemoveExtendAttributes==true)
	{
		for(uint16_t tIndex=0;tIndex<tEntriesCount;tIndex++)
		{
			asf_entry_t * tEntryPtr=tEntriesPtrArray[tIndex];
		
			if (fremovexattr(tFileDescriptor,tEntryPtr->extendedAttributeName,0)==-1)
			{
				switch(errno)
				{
					case ENOATTR:
						
						continue;
					
					case EACCES:
						
						logerror("The extended attribute \"%s\" could not be removed from \"%s\" because you do not have the appropriate permissions\n",tEntryPtr->extendedAttributeName,inFileHierarchyNode->fts_name);
						
						break;
						
					default:
						
						logerror("The extended attribute \"%s\" could not be removed from \"%s\" (%d)\n",tEntryPtr->extendedAttributeName,inFileHierarchyNode->fts_name,errno);
						
						break;
				}
				
				close(tFileDescriptor);
				
				return -1;
			}
		}
	}
	
	close(tFileDescriptor);
	
	return 0;
}

#pragma mark - usage

static void usage(const char * inProcessName)
{
	printf("usage: %s [-s][-v][-u] <file or directory>\n",inProcessName);
	printf("       -s  --  Strip resource fork from source after splitting\n");
	printf("       -v  --  Verbose mode\n");
	printf("       -u  --  Show usage\n");
	
	exit(1);
}

#pragma mark -

int main (int argc, const char * argv[])
{
	char ch;
	bool tRemovedExtendedAttributes=false;
	
	
	while ((ch = getopt(argc, (char ** const) argv, "svu")) != -1)
	{
		switch (ch)
		{
			case 's':
				/* Strip the resource fork */
				
				tRemovedExtendedAttributes=true;
				break;
				
			case 'v':
				/*Verbose */
				
				//gVerboseMode=TRUE;
				break;
			case 'u':
			case '?':
			default:
				usage(argv[0]);
				break;
		}
	}
	
	argv+=optind;
	argc-=optind;
	
	if (argc != 1)
	{
		if (argc==0)
			logerror("No file or directory was specified\n");
		else
			logerror("Only one file or directory may be specified\n");
		
		return -1;
	}

	//char * const tPath="/Users/stephane/Documents/goldin_apfs/test_files_&_folders/file_custom_icon";//"/Volumes/ro_volume/test_files_&_folders";
	
	char tResolvedPath[PATH_MAX];
	
	if (realpath(argv[0],tResolvedPath)==NULL)
	{
		switch(errno)
		{
			case ENOENT:
				/* No such file or directory */
				
				logerror("\"%s\" was not found\n",argv[0]);
				
				break;
			
			case EACCES:
				
				logerror("You do not have the appropriate permissions to access \"%s\"\n",argv[0]);
				
				break;
				
			default:
				/* A COMPLETER */
				break;
		}
		
		return -1;
	}
	
	struct statfs tStatFileSystem;
	
	if (statfs(tResolvedPath,&tStatFileSystem)!=0)
	{
		switch(errno)
		{
			case ENOTDIR:
				/* A COMPLETER */
				break;
			case ENAMETOOLONG:
				/* A COMPLETER */
				break;
			case ENOENT:
				/* No such file or directory */
				
				logerror("\"%s\" was not found\n",tResolvedPath);
				break;
			case EACCES:
				/* A COMPLETER */
				break;
			case EIO:
				/* A COMPLETER */
				break;
			default:
				/* A COMPLETER */
				break;
		}
		
		return -1;
	}
	
	if (strcmp(tStatFileSystem.f_fstypename,"hfs")!=0 && strcmp(tStatFileSystem.f_fstypename,"apfs")!=0)
	{
		/* Return (-2) if this is not a HFS or Extended HFS or APFS File System */
		
		logerror("\"%s\" is neither on an hfs nor an apfs disk\n",argv[0]);
		
		return -2;
	}
	
	char * const tPathsArray[2]={tResolvedPath,NULL};
	
	FTS* tFileHierarchyPtr = fts_open(tPathsArray,FTS_PHYSICAL,NULL);
	
	
	FTSENT* tFileHierarchyNode = NULL;
	
	while( (tFileHierarchyNode = fts_read(tFileHierarchyPtr)) != NULL)
	{
		switch (tFileHierarchyNode->fts_info)
		{
			case FTS_DC:
			case FTS_DNR:
			case FTS_ERR:
			case FTS_NS:
				
				logerror("An error occurred while traversing the file hierarchy\n");
				
			case FTS_SLNONE:
				
				fts_close(tFileHierarchyPtr);
				
				return -1;
				
			case FTS_D :
			case FTS_F :
			case FTS_NSOK:
				
				if (goldin_splitfork(tFileHierarchyNode,tRemovedExtendedAttributes)!=0)
				{
					fts_close(tFileHierarchyPtr);
					
					return -1;
				}
				
				break;
				
			case FTS_SL:
				
				// Symbolic Link
				
				break;
				
			default:
				break;
		}
		
		
	}
	
	fts_close(tFileHierarchyPtr);
	
	return 0;
}