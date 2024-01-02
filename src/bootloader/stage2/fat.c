#include "fat.h"
#include "disk.h"
#include "memdefs.h"
#include "memory.h"
#include "string.h"
#include "utility.h"
#include "stdio.h"
#define SECTOR_SIZE 512
#define MAX_PATH_SIZE 256
#define MAX_FILE_HANDLES 10
#define ROOT_DIRECTORY_HANDLE -1

#pragma pack(push, 1)

typedef struct
{
    uint8_t BootJumpInstruction[3];
    uint8_t OemIdentifier[8];
    uint16_t BytesPerSector;
    uint8_t SectorsPerCluster;
    uint16_t ReservedSectors;
    uint8_t FatCount;
    uint16_t DirEntryCount;
    uint16_t TotalSectors;
    uint8_t MediaDescriptorType;
    uint16_t SectorsPerFat;
    uint16_t SectorsPerTrack;
    uint16_t Heads;
    uint32_t HiddenSectors;
    uint32_t LargeSectorCount;

    // extended boot record
    uint8_t DriveNumber;
    uint8_t _Reserved;
    uint8_t Signature;
    uint32_t VolumeId;       // serial number, value doesn't matter
    uint8_t VolumeLabel[11]; // 11 bytes, padded with spaces
    uint8_t SystemId[8];

    // ... we don't care about code ...

} FAT_BootSector;
#pragma pack(pop)

typedef struct
{
    uint8_t Buffer[SECTOR_SIZE];
    FAT_File Public;
    bool Opened;
    uint32_t FirstCluster;
    uint32_t CurrentCluster;
    uint32_t CurrentSectorInCluter;
} FAT_FileData;

typedef struct
{
    union fat
    {
        FAT_BootSector BootSector;
        uint8_t BootSectorBytes[SECTOR_SIZE];
    } BS;
    FAT_FileData RootDirectory;
    FAT_FileData OpenedFiles[MAX_FILE_HANDLES];

} FAT_Data;

static FAT_Data far* g_Data;
static uint8_t far* g_Fat = NULL;
static uint32_t g_DataSectionLba;

bool FAT_ReadBootSector(DISK *disk)
{
    return DISK_ReadSectors(disk ,0, 1, g_Data->BS.BootSectorBytes);
}
bool FAT_ReadFat(DISK* disk)
{
    return DISK_ReadSectors(disk, g_Data->BS.BootSector.ReservedSectors, g_Data->BS.BootSector.SectorsPerFat, g_Fat);
}
bool FAT_Initialize(DISK* disk)
{
    g_Data = (FAT_Data far*)MEMORY_FAT_ADDR;
    if(!FAT_ReadBootSector(disk))
    {
        printf("FAT: read boot sector falied\r\n");
        return false;
    }

    //Read FAT
    g_Fat = (uint8_t far*)g_Data + sizeof(FAT_Data);
    uint32_t fatSize = g_Data->BS.BootSector.BytesPerSector * g_Data->BS.BootSector.SectorsPerFat;
    if(sizeof(FAT_Data) + fatSize >= MEMORY_FAT_SIZE)
    {
        printf("fat: not enough memory to read fat! required %lu, only have%u\r\n", sizeof(FAT_Data)+fatSize, MEMORY_FAT_SIZE);
        return false;
    }

    if(!FAT_ReadFat(disk))
    {
        printf("fat: read fat failed\r\n");
        return false;
    }
    //open root dir file
    uint32_t rootDirLba = g_Data->BS.BootSector.ReservedSectors + g_Data->BS.BootSector.SectorsPerFat * g_Data->BS.BootSector.FatCount;
    uint32_t rootDirSize = sizeof(FAT_DirectoryEntry) * g_Data->BS.BootSector.DirEntryCount;

    g_Data->RootDirectory.Public.Handle = ROOT_DIRECTORY_HANDLE;
    g_Data->RootDirectory.Public.isDirectory = true;
    g_Data->RootDirectory.Public.Position = 0;
    g_Data->RootDirectory.Public.Size = sizeof(FAT_DirectoryEntry) * g_Data->BS.BootSector.DirEntryCount;
    g_Data->RootDirectory.Opened = true;
    g_Data->RootDirectory.FirstCluster = rootDirLba;
    g_Data->RootDirectory.CurrentCluster = rootDirLba;
    g_Data->RootDirectory.CurrentSectorInCluter = 0;

    if(!DISK_ReadSectors(disk, rootDirLba, 1, g_Data->RootDirectory.Buffer))
    {
        printf("fat: read root dir failed\r\n");
        return false;
    }

    //calculate data section

    uint32_t rootDirSectors = (rootDirSize + g_Data->BS.BootSector.BytesPerSector - 1) / g_Data->BS.BootSector.BytesPerSector;
    g_DataSectionLba = rootDirLba + rootDirSectors;

    for(int i = 0; i < MAX_FILE_HANDLES; i++)
    {
        g_Data->OpenedFiles[i].Opened = false;
    }
    return true;

} 

uint32_t FAT_Cluster2Lba(uint32_t cluster)
{
    return g_DataSectionLba + (cluster - 2) * g_Data->BS.BootSector.SectorsPerCluster;
}

FAT_File far* FAT_OpenEntry(DISK* disk, FAT_DirectoryEntry* entry)
{
    //find empty handle
    int handle = -1;
    for(int i = 0; i < MAX_FILE_HANDLES && handle < 0; i++)
    {   
        if(!g_Data->OpenedFiles[i].Opened)
            handle = i;
    }
    //out of handles
    if(handle < 0)
    {
        printf("fat: out of file handles\r\n");
        return false;
    }
    FAT_FileData far* fd = &g_Data->OpenedFiles[handle];
    fd->Public.Handle = handle;
    fd->Public.isDirectory = (entry->Attributes & FAT_ATTRIBUTE_DIRECTORY) != 0;
    fd->Public.Position = 0;
    fd->Public.Size = entry->Size;
    fd->FirstCluster = entry->FirstClusterLow + ((uint32_t)entry->FirstClusterHigh<<16);
    fd->CurrentCluster = fd->FirstCluster;
    fd->CurrentSectorInCluter = 0;

    if(!DISK_ReadSectors(disk, FAT_Cluster2Lba(fd->CurrentCluster), 1, fd->Buffer))
    {
        printf("fat: read error1\r\n");
        return false;
    }

    fd->Opened = true;
    return &fd->Public;
}

uint32_t FAT_NextCluster(uint32_t currentCluster)
{
    uint32_t fatIndex = currentCluster * 3 / 2;
    if(currentCluster % 2 == 0)
        return (*(uint16_t far*)(g_Fat + fatIndex)) & 0x0FFF;
    else    
        return (*(uint16_t far*)(g_Fat + fatIndex)) >> 4;
}

uint32_t FAT_Read(DISK* disk, FAT_File far* file, uint32_t byteCount, void* dataOut)
{
    //get file data
    static int count = 1;
    //printf("\r\nthe %dth read\r\n", count);
    count++;
    FAT_FileData far* fd = (file->Handle == ROOT_DIRECTORY_HANDLE) ?
                             &g_Data->RootDirectory
                           : &g_Data->OpenedFiles[file->Handle];
    uint8_t* u8DataOut = (uint8_t*)dataOut;
    //printf("byte count args=%d\r\n", byteCount);
    if(!fd->Public.isDirectory)
        byteCount = min(byteCount, fd->Public.Size - fd->Public.Position);
    //printf("size=%d,pos=%d\r\n", fd->Public.Size,fd->Public.Position);
    //printf("byte count ret=%d\r\n", byteCount);

    while(byteCount > 0)
    {
        
        uint32_t leftInBuffer = SECTOR_SIZE - (fd->Public.Position % SECTOR_SIZE);
        uint32_t take = min(byteCount, leftInBuffer);
        //printf("take1=%ld",(long long)leftInBuffer);
        memcpy(u8DataOut, fd->Buffer + fd->Public.Position % SECTOR_SIZE, take);
        u8DataOut += take;
        fd->Public.Position += take;
        byteCount -= take;

        if(leftInBuffer == take)
        {
            if(fd->Public.Handle == ROOT_DIRECTORY_HANDLE)
            {
                ++fd->CurrentCluster;

                //read next sector
                if(!DISK_ReadSectors(disk, fd->CurrentCluster, 1, fd->Buffer))
                {
                    printf("fat: read error2\r\n");
                    
                    // while(1)
                    // {

                    // }
                    break;
                }
                
            }
            else 
            {
                //calculate next cluster & sector to read
                if(++(fd->CurrentSectorInCluter) >= g_Data->BS.BootSector.SectorsPerCluster)
                {
                    fd->CurrentSectorInCluter = 0;
                    fd->CurrentCluster = FAT_NextCluster(fd->CurrentCluster);
                }
                if(fd->CurrentCluster >= 0xFF8)
                {
                    //end of file
                    fd->Public.Size = fd->Public.Position;
                    break;
                }
                //read next sector
                if(!DISK_ReadSectors(disk, FAT_Cluster2Lba(fd->CurrentCluster) + fd->CurrentSectorInCluter, 1, fd->Buffer))
                {
                    printf("fat: read error3\r\n");
                    break;
                }
            }
        }

    }
    return u8DataOut - (uint8_t*)dataOut;
}

bool FAT_ReadEntry(DISK* disk, FAT_File far* file, FAT_DirectoryEntry* dirEntry)
{
    return FAT_Read(disk, file, sizeof(FAT_DirectoryEntry), dirEntry) == sizeof(FAT_DirectoryEntry);
}

void FAT_Close(FAT_File far* file)
{
    if(file->Handle == ROOT_DIRECTORY_HANDLE)
    {
        file->Position = 0;
        g_Data->RootDirectory.CurrentCluster = g_Data->RootDirectory.FirstCluster;
    }
    else
    {
        g_Data->OpenedFiles[file->Handle].Opened = false;
    }
}


bool FAT_FindFile(DISK* disk, FAT_File far* file, const char* name, FAT_DirectoryEntry* entryOut)
{
    char fatName[32];
    FAT_DirectoryEntry entry;

    memset(fatName, ' ', sizeof(fatName));
    fatName[11] = '\0';

    const char* ext = strchr(name, '.');
    if(ext == NULL)
        ext = name + 11;
    
    for(int i = 0; i < 8 && name[i] && name + i <ext; i++)
    {
        fatName[i] = toupper(name[i]);
    }
    if(ext != NULL)
    {
        for(int i = 0 ; i < 3 && ext[i + 1]; i++)
            fatName[i+8] = toupper(ext[i+1]);
    }

    while(FAT_ReadEntry(disk, file, &entry))
    {
        if(memcmp(fatName, entry.Name, 11) == 0)
        {
            *entryOut = entry;
            return true;
        }
    }
    return false;
}

FAT_File far* FAT_Open(DISK* disk, const char* path)
{
    char name[MAX_PATH_SIZE];

    if(path[0] == '/')
        path++;
    FAT_File far* current = &g_Data->RootDirectory.Public;

    while(*path)
    {
        bool isLast = false;
        const char* delim = strchr(path, '/');
        if(delim != NULL)
        {
            memcpy(name, path, delim - path);
            name[delim - path + 1] = '\0';
            path = delim + 1;
        }
        else 
        {
            unsigned len  = strlen(path);
            memcpy(name, path, len);
            name[len + 1]  = '\0';
            path += len;
            isLast = true;
        }

        FAT_DirectoryEntry entry;
        if(FAT_FindFile(disk, current, name, &entry))
        {
            FAT_Close(current);

            if(!isLast && (entry.Attributes & FAT_ATTRIBUTE_DIRECTORY == 0))
            {
                printf("fat:%s not a dir\r\n", name);
                return NULL;
            }

            current =FAT_OpenEntry(disk, &entry);
        }
        else
        {
            FAT_Close(current);
            printf("fat: %s not found\r\n", name);
            return NULL;
        }
    }
    return current;
    
}
