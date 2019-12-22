/* See https://github.com/apple/darwin-xnu/blob/master/bsd/vfs/vfs_xattr.c for documentation on AppleDoubleFormat + Extended Attributes */

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
	ASF_EXTENDEDATTRIBUTE=0,	// 0 is invalid according to the format specs, so we can use it for extended attributes.
	ASF_RESOURCEFORK=2,
	ASF_FINDERINFO=9
};

#define ASF_ENTRY_COUNT	2

#define ASF_DEFAULT_FINDERINFO_SIZE	32

typedef struct asf_entry_t
{
	uint32_t entryID;
	ssize_t entrySize;
	
	char * extendedAttributeName;	/* XATTR_FINDERINFO_NAME / XATTR_RESOURCEFORK_NAME */
	
	u_int8_t nameLength;
	uint32_t offset;
	
	u_int8_t entryLength;
	
} asf_entry_t;

typedef struct asf_entry_node_t
{
	asf_entry_t entry;
	
	struct asf_entry_node_t * next;
	
} asf_entry_node_t;

typedef asf_entry_node_t asf_entry_head_t;


void releaseEntryNodes(asf_entry_node_t * inHead);




typedef enum
{
	SPLITOPTION_STRIP_AFTER_SPLIT=1<<0,
	SPLITOPTION_PRESERVE_EXTENDED_ATTRIBUTES=1<<1
} split_options_t;


int goldin_splitfork(FTSENT * inFileHierarchyNode,split_options_t inSplitOptions);

void log_write_error(int inError,const char * inFileName);


void releaseEntryNodes(asf_entry_node_t * inHead)
{
	while(inHead!=NULL)
	{
		free(inHead->entry.extendedAttributeName);
		
		asf_entry_node_t * tNext=inHead->next;
		
		free(inHead);
		
		inHead=tNext;
	}
}


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

int goldin_splitfork(FTSENT * inFileHierarchyNode,split_options_t inSplitOptions)
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
	
	asf_entry_head_t * tExtendedAttributesListHead=NULL;
	
	asf_entry_t * tEntriesPtrArray[ASF_ENTRY_COUNT]={&tFinderInfoEntry,&tResourceForkEntry};	// Must start with FinderInfo (for extended attributes hack)
	uint16_t tEntriesCount=0;
	
	ssize_t tBufferReadSize=flistxattr(tFileDescriptor, tNamesBuffer, tBufferSize, 0);
	ssize_t tOffset=0;
	
	asf_entry_node_t * tCurrentNode=NULL;
	
	while (tOffset<tBufferReadSize)
	{
		char * tAttributeName=tNamesBuffer+tOffset;
		
		if (strncmp(XATTR_RESOURCEFORK_NAME, tAttributeName, sizeof(XATTR_RESOURCEFORK_NAME)+1)==0)
		{
			tResourceForkEntry.entrySize=fgetxattr(tFileDescriptor, tAttributeName, NULL, 0, 0, 0);
			
			tEntriesCount++;
		}
		else
		{
			if (strncmp(XATTR_FINDERINFO_NAME, tAttributeName, sizeof(XATTR_FINDERINFO_NAME)+1)==0)
			{
				tFinderInfoEntry.entrySize=fgetxattr(tFileDescriptor, tAttributeName, NULL, 0, 0, 0);
			
				tEntriesCount++;
			}
			else if ((inSplitOptions&SPLITOPTION_PRESERVE_EXTENDED_ATTRIBUTES)==SPLITOPTION_PRESERVE_EXTENDED_ATTRIBUTES ||
					 (inSplitOptions&SPLITOPTION_STRIP_AFTER_SPLIT)==SPLITOPTION_STRIP_AFTER_SPLIT)
			{
				asf_entry_node_t * tNode=malloc(sizeof(asf_entry_node_t));
				tNode->next=NULL;
				
				if (tNode==NULL)
				{
					close(tFileDescriptor);
					
					logerror("An error occurred because available memory is too low\n");
					
					return -1;
				}
				
				tNode->entry.entryID=ASF_EXTENDEDATTRIBUTE;
				tNode->entry.entrySize=fgetxattr(tFileDescriptor, tAttributeName, NULL, 0, 0, 0);
				tNode->entry.extendedAttributeName=strdup(tAttributeName);
				tNode->entry.nameLength=strlen(tAttributeName)+1;
				
				tNode->entry.entryLength=11+tNode->entry.nameLength;
				
				uint32_t tModulo=tNode->entry.entryLength%4;
				
				if (tModulo!=0)
				{
					tNode->entry.entryLength+=(4-tModulo);
				}
				
				if (tNode->entry.extendedAttributeName==NULL)
				{
					close(tFileDescriptor);
					
					logerror("An error occurred because available memory is too low\n");
					
					return -1;
				}
				
				if (tExtendedAttributesListHead==NULL)
				{
					tExtendedAttributesListHead=tNode;
					
					tCurrentNode=tExtendedAttributesListHead;
				}
				else
				{
					tCurrentNode->next=tNode;
					
					tCurrentNode=tNode;
				}
				
				tEntriesCount++;
			}
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
	
	int tReturnCode=-1;
	
	tEntriesCount=ASF_ENTRY_COUNT;	// SplitForks create 2 entries even if the Resource Fork is empty
	
	// We need to create the AppleDouble Format file
	
	char * tParentDirectoryPath=dirname(inFileHierarchyNode->fts_accpath);
	
	size_t tAccessPathLength=strlen(tParentDirectoryPath)+1+2+inFileHierarchyNode->fts_namelen+1;
	
	if (tAccessPathLength>FILENAME_MAX)
	{
		logerror("File name is too long. The maximum length allowed is %ld characters\n",(long)FILENAME_MAX);
		
		close(tFileDescriptor);
		
		releaseEntryNodes(tExtendedAttributesListHead);
		
		return -1;
	}
	
	char tAppleDoubleFormatFileAccessPath[FILENAME_MAX];
	
	size_t tValue=(size_t)snprintf(tAppleDoubleFormatFileAccessPath,tAccessPathLength, "%s/._%s",tParentDirectoryPath,inFileHierarchyNode->fts_name);
	
	if (tValue!=(tAccessPathLength-1))
	{
		logerror("File name could not be created\n");
		
		goto bail;
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
		
		goto bail;
	}
	
	// Write the Magic Number
	
	unsigned char tAppleDoubleMagicNumber[4]={0x00,0x05,0x16,0x07};
	
	if (fwrite(tAppleDoubleMagicNumber, sizeof(tAppleDoubleMagicNumber),1, fp)!=1)
	{
		int tError=ferror(fp);
		
		if (tError!=0)
			log_write_error(errno,inFileHierarchyNode->fts_path);
		
		fclose(fp);
		
		goto bail;
	}
	
	/* Write the Version Number */
	
	unsigned char tAppleDoubleVersionNumber[4]={0x00,0x02,0x00,0x00};
	
	if (fwrite(tAppleDoubleVersionNumber, sizeof(tAppleDoubleVersionNumber),1, fp)!=1)
	{
		int tError=ferror(fp);
		
		if (tError!=0)
			log_write_error(errno,inFileHierarchyNode->fts_path);
		
		fclose(fp);
		
		goto bail;
	}
	
	/* Write the Filler */
	
	unsigned char tAppleDoubleFiller[16]={0x4d,0x61,0x63,0x20,0x4f,0x53,0x20,0x58,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20};	// "Mac OS X        "
	
	if (fwrite(tAppleDoubleFiller, sizeof(tAppleDoubleFiller),1, fp)!=1)
	{
		int tError=ferror(fp);
		
		if (tError!=0)
			log_write_error(errno,inFileHierarchyNode->fts_path);
		
		fclose(fp);
		
		goto bail;
	}
	
	/* Write the number of Entries */
	
	uint16_t tTransormedNumberOfEntries=tEntriesCount; // i.e. ASF_ENTRY_COUNT
	
#ifdef __LITTLE_ENDIAN__
	
	tTransormedNumberOfEntries=CFSwapInt16(tTransormedNumberOfEntries);
	
#endif
	
	if (fwrite(&tTransormedNumberOfEntries, sizeof(uint16_t),1, fp)!=1)
	{
		int tError=ferror(fp);
		
		if (tError!=0)
			log_write_error(errno,inFileHierarchyNode->fts_path);
		
		fclose(fp);
		
		goto bail;
	}
	
	/* Write the Entry Descriptors */
	
	u_int32_t tTotalSize=0;
	u_int32_t tDataStart=0;
	u_int32_t tDataLength=0;
	u_int16_t tFlags=0;
	u_int16_t tNumAttrs=0;
	
	uint32_t tEntryDataOffset=0x0000001A+tEntriesCount*12;
	
	for(uint16_t tIndex=0;tIndex<tEntriesCount;tIndex++)
	{
		asf_entry_t * tEntryPtr=tEntriesPtrArray[tIndex];
		
		uint32_t tEntryID=tEntryPtr->entryID;
		uint32_t tEntryOffset=tEntryDataOffset;
		uint32_t tEntryLength=(uint32_t)tEntryPtr->entrySize;
		
		
		
		tEntryDataOffset+=tEntryLength;
		
		if (strncmp(XATTR_FINDERINFO_NAME, tEntryPtr->extendedAttributeName, sizeof(XATTR_FINDERINFO_NAME)+1)==0 && (inSplitOptions&SPLITOPTION_PRESERVE_EXTENDED_ATTRIBUTES)==SPLITOPTION_PRESERVE_EXTENDED_ATTRIBUTES)
		{
			if (tExtendedAttributesListHead!=NULL)
				tEntryDataOffset+=30;
			
			tDataStart=tEntryDataOffset;
			
			asf_entry_node_t * tNode=tExtendedAttributesListHead;
			
			while(tNode!=NULL)
			{
				tNumAttrs+=1;
				
				tEntryDataOffset+=tNode->entry.entryLength;
				tDataStart+=tNode->entry.entryLength;
				
				tEntryDataOffset+=tNode->entry.entrySize;
				
				tDataLength+=tNode->entry.entrySize;
				
				tNode=tNode->next;
			}
			
			tTotalSize=tEntryDataOffset;
			
			uint32_t tAttributesDataOffset=tDataStart;
			
			tNode=tExtendedAttributesListHead;
			
			while(tNode!=NULL)
			{
				tNode->entry.offset=tAttributesDataOffset;
				
				tAttributesDataOffset+=tNode->entry.entrySize;
				
				tNode=tNode->next;
			}
			
			tEntryLength=tEntryDataOffset-tEntryOffset;
		}
		
#ifdef __LITTLE_ENDIAN__
		
		tEntryID=CFSwapInt32(tEntryID);
		tEntryOffset=CFSwapInt32(tEntryOffset);
		tEntryLength=CFSwapInt32(tEntryLength);
		
#endif
		
		if (fwrite(&tEntryID, sizeof(uint32_t),1, fp)!=1)
		{
			int tError=ferror(fp);
			
			if (tError!=0)
				log_write_error(errno,inFileHierarchyNode->fts_path);
			
			fclose(fp);
			
			goto bail;
		}
		
		if (fwrite(&tEntryOffset, sizeof(uint32_t),1, fp)!=1)
		{
			int tError=ferror(fp);
			
			if (tError!=0)
				log_write_error(errno,inFileHierarchyNode->fts_path);
			
			fclose(fp);
			
			goto bail;
		}
		
		if (fwrite(&tEntryLength, sizeof(uint32_t),1, fp)!=1)
		{
			int tError=ferror(fp);
			
			if (tError!=0)
				log_write_error(errno,inFileHierarchyNode->fts_path);
			
			fclose(fp);
			
			goto bail;
		}
	}
	
	// Write the Entries
	
	for(uint16_t tIndex=0;tIndex<tEntriesCount;tIndex++)
	{
		asf_entry_t * tEntryPtr=tEntriesPtrArray[tIndex];
		
		switch (tEntryPtr->entryID)
		{
			case ASF_FINDERINFO:
			{
				if (tEntryPtr->entrySize==0)
				{
					uint8_t tEmptyBuffer[ASF_DEFAULT_FINDERINFO_SIZE];
					
					memset(tEmptyBuffer, 0, ASF_DEFAULT_FINDERINFO_SIZE*sizeof(uint8_t));
					
					if (fwrite(tEmptyBuffer,ASF_DEFAULT_FINDERINFO_SIZE,1, fp)!=1)
					{
						int tError=ferror(fp);
						
						if (tError!=0)
							log_write_error(errno,inFileHierarchyNode->fts_path);
						
						fclose(fp);
						
						goto bail;
					}
					
					tEntryPtr->entrySize=ASF_DEFAULT_FINDERINFO_SIZE;
				}
				else
				{
					char * tAttributeBuffer=malloc(tEntryPtr->entrySize);
					
					if (tAttributeBuffer==NULL)
					{
						logerror("An error occurred because available memory is too low\n");
						
						fclose(fp);
						
						goto bail;
					}
					
					ssize_t tAttributeBufferReadSize=fgetxattr(tFileDescriptor, tEntryPtr->extendedAttributeName, tAttributeBuffer, tEntryPtr->entrySize, 0, 0);
					
					if (tAttributeBufferReadSize==-1)
					{
						logerror("An error occurred while reading the \"%s\" attribute (%d)\n",tEntryPtr->extendedAttributeName,errno);
						
						free(tAttributeBuffer);
						
						fclose(fp);
						
						goto bail;
					}
					
					if (fwrite(tAttributeBuffer,tAttributeBufferReadSize,1, fp)!=1)
					{
						int tError=ferror(fp);
						
						if (tError!=0)
							log_write_error(errno,inFileHierarchyNode->fts_path);
						
						free(tAttributeBuffer);
						
						fclose(fp);
						
						goto bail;
					}
					
					free(tAttributeBuffer);
				}
				
				if ((inSplitOptions&SPLITOPTION_PRESERVE_EXTENDED_ATTRIBUTES)==SPLITOPTION_PRESERVE_EXTENDED_ATTRIBUTES && tExtendedAttributesListHead!=NULL)
				{
					// Write the extended attributes embedded header
					
						// Write padding
					
					uint32_t tZero=0;
					
					if (fwrite(&tZero,sizeof(uint16_t),1, fp)!=1)
					{
						int tError=ferror(fp);
						
						if (tError!=0)
							log_write_error(errno,inFileHierarchyNode->fts_path);
						
						fclose(fp);
						
						goto bail;
					}
					
					// Write 'ATTR' magic number
					
					unsigned char tAttributeMagicNumber[4]={0x41,0x54,0x54,0x52};
					
					if (fwrite(tAttributeMagicNumber, sizeof(tAttributeMagicNumber),1, fp)!=1)
					{
						int tError=ferror(fp);
						
						if (tError!=0)
							log_write_error(errno,inFileHierarchyNode->fts_path);
						
						fclose(fp);
						
						goto bail;
					}

					u_int32_t tDebugTag=0;
					
#ifdef __LITTLE_ENDIAN__
					
					tDebugTag=CFSwapInt32(tDebugTag);
					tTotalSize=CFSwapInt32(tTotalSize);
					tDataStart=CFSwapInt32(tDataStart);
					tDataLength=CFSwapInt32(tDataLength);
					
					tFlags=CFSwapInt16(tFlags);
					tNumAttrs=CFSwapInt16(tNumAttrs);
#endif
					
					// Write debug tag
					
					if (fwrite(&tDebugTag, sizeof(uint32_t),1, fp)!=1)
					{
						int tError=ferror(fp);
						
						if (tError!=0)
							log_write_error(errno,inFileHierarchyNode->fts_path);
						
						fclose(fp);
						
						goto bail;
					}
					
					// Write Total Size
					
					if (fwrite(&tTotalSize, sizeof(uint32_t),1, fp)!=1)
					{
						int tError=ferror(fp);
						
						if (tError!=0)
							log_write_error(errno,inFileHierarchyNode->fts_path);
						
						fclose(fp);
						
						goto bail;
					}
					
					// Write Data Start
					
					if (fwrite(&tDataStart, sizeof(uint32_t),1, fp)!=1)
					{
						int tError=ferror(fp);
						
						if (tError!=0)
							log_write_error(errno,inFileHierarchyNode->fts_path);
						
						fclose(fp);
						
						goto bail;
					}
					
					// Write Data Length
					
					if (fwrite(&tDataLength, sizeof(uint32_t),1, fp)!=1)
					{
						int tError=ferror(fp);
						
						if (tError!=0)
							log_write_error(errno,inFileHierarchyNode->fts_path);
						
						fclose(fp);
						
						goto bail;
					}
					
					// Write Reserved
					
					uint32_t tReserved[3]={0x00,0x00,0x00};
					
					if (fwrite(&tReserved,sizeof(uint32_t)*3,1, fp)!=1)
					{
						int tError=ferror(fp);
						
						if (tError!=0)
							log_write_error(errno,inFileHierarchyNode->fts_path);
						
						fclose(fp);
						
						goto bail;
					}
					
					// Write Flags
					
					if (fwrite(&tFlags, sizeof(uint16_t),1, fp)!=1)
					{
						int tError=ferror(fp);
						
						if (tError!=0)
							log_write_error(errno,inFileHierarchyNode->fts_path);
						
						fclose(fp);
						
						goto bail;
					}
					
					// Write Number of Attributes
					
					if (fwrite(&tNumAttrs, sizeof(uint16_t),1, fp)!=1)
					{
						int tError=ferror(fp);
						
						if (tError!=0)
							log_write_error(errno,inFileHierarchyNode->fts_path);
						
						fclose(fp);
						
						goto bail;
					}

					// Write Attributes Entries
					
					asf_entry_node_t * tNode=tExtendedAttributesListHead;
					
					while (tNode!=NULL)
					{
						u_int32_t tOffset=tNode->entry.offset;
						u_int32_t tLength=(u_int32_t)tNode->entry.entrySize;
						u_int16_t tFlags=0;
						u_int8_t tNameLength=tNode->entry.nameLength;
						
#ifdef __LITTLE_ENDIAN__
						
						tOffset=CFSwapInt32(tOffset);
						tLength=CFSwapInt32(tLength);
						tFlags=CFSwapInt16(tFlags);
#endif
						
						if (fwrite(&tOffset, sizeof(uint32_t),1, fp)!=1)
						{
							int tError=ferror(fp);
							
							if (tError!=0)
								log_write_error(errno,inFileHierarchyNode->fts_path);
							
							fclose(fp);
							
							goto bail;
						}
						
						if (fwrite(&tLength, sizeof(uint32_t),1, fp)!=1)
						{
							int tError=ferror(fp);
							
							if (tError!=0)
								log_write_error(errno,inFileHierarchyNode->fts_path);
							
							fclose(fp);
							
							goto bail;
						}
						
						if (fwrite(&tFlags, sizeof(uint16_t),1, fp)!=1)
						{
							int tError=ferror(fp);
							
							if (tError!=0)
								log_write_error(errno,inFileHierarchyNode->fts_path);
							
							fclose(fp);
							
							goto bail;
						}
						
						if (fwrite(&tNameLength, sizeof(uint8_t),1, fp)!=1)
						{
							int tError=ferror(fp);
							
							if (tError!=0)
								log_write_error(errno,inFileHierarchyNode->fts_path);
							
							fclose(fp);
							
							goto bail;
						}
						
						if (fwrite(tNode->entry.extendedAttributeName, sizeof(uint8_t)*tNameLength,1, fp)!=1)
						{
							int tError=ferror(fp);
							
							if (tError!=0)
								log_write_error(errno,inFileHierarchyNode->fts_path);
							
							fclose(fp);
							
							goto bail;
						}
						
						// Fill the gap for 4-byte alignment
						
						if ((11+tNameLength)!=tNode->entry.entryLength)
						{
							if (fwrite(&tZero, sizeof(uint8_t)*(tNode->entry.entryLength-11-tNameLength),1, fp)!=1)
							{
								int tError=ferror(fp);
								
								if (tError!=0)
									log_write_error(errno,inFileHierarchyNode->fts_path);
								
								fclose(fp);
								
								goto bail;
							}
						}
						
						tNode=tNode->next;
					}
					
					// Write Attributes Data
					
					// We can not stream the reading of the extended attribute as the fgetxattr API only supports the position parameter for the resource fork attribute
					
					tNode=tExtendedAttributesListHead;
					
					while (tNode!=NULL)
					{
						char * tAttributeName=tNode->entry.extendedAttributeName;
						
						size_t tAttributeBufferSize=(size_t)tNode->entry.entrySize;
							
						char * tAttributeBuffer=malloc(tAttributeBufferSize);
						
						if (tAttributeBuffer==NULL)
						{
							logerror("An error occurred because the available memory is too low (%.2f MB buffer needed)\n",tAttributeBufferSize/(1024.0*1024));
							
							fclose(fp);
							
							goto bail;
						}
						
						ssize_t tAttributeBufferReadSize=fgetxattr(tFileDescriptor,tAttributeName , tAttributeBuffer, tAttributeBufferSize, 0, 0);
							
						if (tAttributeBufferReadSize==-1)
						{
							logerror("An error occurred while reading the \"%s\" attribute (%d)\n",tAttributeName,errno);
							
							free(tAttributeBuffer);
							
							fclose(fp);
							
							goto bail;
						}
							
						if (fwrite(tAttributeBuffer,tAttributeBufferReadSize,1, fp)!=1)
						{
							int tError=ferror(fp);
							
							if (tError!=0)
								log_write_error(errno,inFileHierarchyNode->fts_path);
							
							free(tAttributeBuffer);
							
							fclose(fp);
							
							goto bail;
						}
							
						free(tAttributeBuffer);
						
						tNode=tNode->next;
					}
				}
				
				break;
			}
				
			case ASF_RESOURCEFORK:
			{
				if (tEntryPtr->entrySize==0)
					continue;
				
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
					
					goto bail;
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
						
						goto bail;
					}
					
					if (fwrite(tAttributeBuffer,tAttributeBufferReadSize,1, fp)!=1)
					{
						int tError=ferror(fp);
						
						if (tError!=0)
							log_write_error(errno,inFileHierarchyNode->fts_path);
						
						free(tAttributeBuffer);
						
						fclose(fp);
						
						goto bail;
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
		switch(errno)
		{
			default:
				
				break;
		}
		
		goto bail;
	}
	
	if ((inSplitOptions & SPLITOPTION_STRIP_AFTER_SPLIT)==SPLITOPTION_STRIP_AFTER_SPLIT)
	{
		for(uint16_t tIndex=0;tIndex<tEntriesCount;tIndex++)
		{
			asf_entry_t * tEntryPtr=tEntriesPtrArray[tIndex];
		
			if (tEntryPtr->entryID==ASF_RESOURCEFORK && inFileHierarchyNode->fts_info==FTS_D)	// Folders do not have resource forks
				continue;
			
			if (fremovexattr(tFileDescriptor,tEntryPtr->extendedAttributeName,0)==-1)
			{
				switch(errno)
				{
					case EACCES:
						
						logerror("The extended attribute \"%s\" could not be removed from \"%s\" because you do not have the appropriate permissions\n",tEntryPtr->extendedAttributeName,inFileHierarchyNode->fts_name);
						
						break;
						
					case ENOATTR:
						
						continue;
						
					default:
						
						logerror("The extended attribute \"%s\" could not be removed from \"%s\" (%d)\n",tEntryPtr->extendedAttributeName,inFileHierarchyNode->fts_name,errno);
						
						break;
				}
				
				goto bail;
			}
		}
		
		asf_entry_node_t * tNode=tExtendedAttributesListHead;
		
		while (tNode!=NULL)
		{
			if (fremovexattr(tFileDescriptor,tNode->entry.extendedAttributeName,0)==-1)
			{
				switch(errno)
				{
					case EACCES:
						
						logerror("The extended attribute \"%s\" could not be removed from \"%s\" because you do not have the appropriate permissions\n",tNode->entry.extendedAttributeName,inFileHierarchyNode->fts_name);
						
						break;
						
					case ENOATTR:
						
						continue;
						
					default:
						
						logerror("The extended attribute \"%s\" could not be removed from \"%s\" (%d)\n",tNode->entry.extendedAttributeName,inFileHierarchyNode->fts_name,errno);
						
						break;
				}
				
				goto bail;
			}
			
			tNode=tNode->next;
		}
	}
	
	// Success!
	
	tReturnCode=0;
	
bail:
	
	close(tFileDescriptor);
	
	releaseEntryNodes(tExtendedAttributesListHead);
	
	return tReturnCode;
}

#pragma mark - usage

static void usage(const char * inProcessName)
{
	printf("usage: %s [-s][-v][-u] <file or directory>\n",inProcessName);
	printf("       -s  --  Strip resource fork from source after splitting\n");
	printf("       -e  --  Preserve extended attributes\n");
	printf("       -v  --  Verbose mode\n");
	printf("       -u  --  Show usage\n");
	
	exit(1);
}

#pragma mark -

int main (int argc, const char * argv[])
{
	char ch;
	split_options_t tSplitOptions=0;
	
	
	while ((ch = getopt(argc, (char ** const) argv, "svue")) != -1)
	{
		switch (ch)
		{
			case 'e':
				/* Preserve extended attributes */
				
				tSplitOptions|=SPLITOPTION_PRESERVE_EXTENDED_ATTRIBUTES;
				break;
				
			case 's':
				/* Strip the resource fork */
				
				tSplitOptions|=SPLITOPTION_STRIP_AFTER_SPLIT;
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
				
				if (goldin_splitfork(tFileHierarchyNode,tSplitOptions)!=0)
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
