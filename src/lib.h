#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>


#define _FILE_OFFSET_BITS 64
#define MAX_FILE_NAME_SIZE PATH_MAX + NAME_MAX + 1

struct configStruct {
    size_t dataBufSize;
};

struct deviceNames {
    char *sourceDeviceName;
    char *destinationDeviceName;
};

struct optionsStruct {
    bool sourceDeviceGiven;
    bool destinationDeviceGiven;
    bool dataBufSizeGiven;
    bool verifyWrites;
    bool verifyIntegrity;
};

struct miscStruct {
    uint64_t returnVal;
};

struct dataStruct {
    struct configStruct cryptSt;
    struct deviceNames deviceNameSt;
    struct optionsStruct optSt;
    struct miscStruct miscSt;
};

#define PRINT_SYS_ERROR(errCode) \
    { \
        fprintf(stderr, "%s:%s:%d: %s\n", __FILE__, __func__, __LINE__, strerror(errCode)); \
    }

#define PRINT_DEVICE_ERROR(deviceName, errCode) \
    { \
        fprintf(stderr, "%s: %s (Line: %i)\n", deviceName, strerror(errCode), __LINE__); \
    }

#define PRINT_ERROR(errMsg) \
    { \
        fprintf(stderr, "%s:%s:%d: %s\n", __FILE__, __func__, __LINE__, errMsg); \
    }

// Double-Free/Dangling-Pointer-free cleanup function wrapper for free()
#define DDFREE(freeFunc, ptr) \
    do { \
        if ((ptr) != NULL) { \
            freeFunc(ptr); \
            (ptr) = NULL; \
        } \
    } while (0)

#define PRINT_BUFFER(buffer, size, message) \
    do { \
        printf("%s:%d %s:\n", __func__, __LINE__, message); \
        for (int i = 0; i < size; i++) { \
            printf("%02x", buffer[i] & 0xff); \
        } \
        printf("\n"); \
    } while (0)

uint64_t preadFull(int device,
                   void *buffer,
                   uint64_t bytesToRead,
                   off_t deviceOffset,
                   struct dataStruct *st);
                   
uint64_t pwriteFull(int device,
                    void *buffer,
                    uint64_t bytesToWrite,
                    off_t deviceOffset,
                    struct dataStruct *st);
                    
                    
uint64_t getDeviceSize(const char *device);
uint64_t greatestCommonDenominator(uint64_t a, uint64_t b);
uint64_t leastCommonDenominator(uint64_t a, uint64_t b);
uint8_t printSyntax(char *arg);
void makeMultipleOf(size_t *numberToChange, size_t multiple);
uint8_t printSyntax(char *arg);
void parseOptions(int argc, char *argv[], struct dataStruct *st);
void bytesPrefixed(char *prefixedString, unsigned long long bytes);
size_t getBufSizeMultiple(char *value);

void diffDup(int sourceDevice,
             int destinationDevice,
             uint64_t sourceDeviceSize,
             struct dataStruct *st);

void installSignalHandlers(void);

int  sigusr1Pending(void);
int  sigintCount(void);
void clearSigusr1(void);


void printStats(uint64_t totalBytesRead,
                uint64_t totalBytesWritten,
                struct timespec startTime);
