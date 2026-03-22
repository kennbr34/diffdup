/*
 * diffdup - incremental block device duplication
 *
 * Copyright (c) 2026 Kenneth Brown
 * Licensed under the MIT License.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define MAJOR_VER 0
#define MINOR_VER 2
#define PATCH_VER 1

#define _FILE_OFFSET_BITS 64
#define MAX_FILE_NAME_SIZE PATH_MAX + NAME_MAX + 1
#define DEFAULT_BUFFER_SIZE 64 * 1024
#define DEFAULT_VECTOR_NUM 8

struct configStruct
{
    size_t sourceDeviceStart;
    size_t destinationDeviceStart;
    size_t outputAmount;
    size_t dataBufSize;
    int numVectors;
};

struct deviceNames
{
    char *sourceDeviceName;
    char *destinationDeviceName;
};

struct optionsStruct
{
    bool sourceDeviceGiven;
    bool sourceStartGiven;
    bool destinationDeviceGiven;
    bool destinationStartGiven;
    bool dataBufSizeGiven;
    bool numVectorsGiven;
    bool outputAmountGiven;
    bool verifyWrites;
    bool verifyIntegrity;
    bool verifyAfter;
    bool enableManualReadahead;
    bool tuneIO;
    bool directIO;
    bool printProgress;
};

struct readaheadStruct
{
    size_t readaheadChunks;
    double lastLatency;
};

struct miscStruct
{
    uint64_t returnVal;

    struct readaheadStruct sourceReadaheadSt;
    struct readaheadStruct destinationReadaheadSt;
};

struct stateStruct
{
    struct configStruct configSt;
    struct deviceNames deviceNameSt;
    struct optionsStruct optSt;
    struct miscStruct miscSt;
};

#define PRINT_SYS_ERROR(errCode)                                                            \
    {                                                                                       \
        fprintf(stderr, "%s:%s:%d: %s\n", __FILE__, __func__, __LINE__, strerror(errCode)); \
    }

#define PRINT_DEVICE_ERROR(deviceName, errCode)                                          \
    {                                                                                    \
        fprintf(stderr, "%s: %s (Line: %i)\n", deviceName, strerror(errCode), __LINE__); \
    }

#define PRINT_ERROR(errMsg)                                                      \
    {                                                                            \
        fprintf(stderr, "%s:%s:%d: %s\n", __FILE__, __func__, __LINE__, errMsg); \
    }

// Double-Free/Dangling-Pointer-free cleanup function wrapper for free()
#define DDFREE(freeFunc, ptr) \
    do                        \
    {                         \
        if ((ptr) != NULL)    \
        {                     \
            freeFunc(ptr);    \
            (ptr) = NULL;     \
        }                     \
    } while (0)

#define PRINT_BUFFER(buffer, size, message)                 \
    do                                                      \
    {                                                       \
        printf("%s:%d %s:\n", __func__, __LINE__, message); \
        for (int i = 0; i < size; i++)                      \
        {                                                   \
            printf("%02x", buffer[i] & 0xff);               \
        }                                                   \
        printf("\n");                                       \
    } while (0)

static inline double elapsedTime(struct timespec startTime)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0.0;

    return (double)(now.tv_sec - startTime.tv_sec) +
           (double)(now.tv_nsec - startTime.tv_nsec) / 1e9;
}

static inline void humanReadable(double bytes,
                                 double *value,
                                 const char **unit)
{
    static const char *units[] = {
        "B", "KiB", "MiB", "GiB", "TiB", "PiB"};

    int i = 0;

    while (bytes >= 1024.0 && i < 5)
    {
        bytes /= 1024.0;
        i++;
    }

    *value = bytes;
    *unit = units[i];
}

static inline void printStats(uint64_t totalBytesRead,
                              uint64_t totalBytesWritten,
                              struct timespec startTime)
{
    double seconds = elapsedTime(startTime);

    if (seconds <= 0.0)
        seconds = 1e-9;

    double rate = (double)totalBytesRead / seconds;

    double readVal, writeVal, rateVal;
    const char *writeUnit;
    const char *readUnit;
    const char *rateUnit;

    humanReadable((double)totalBytesRead, &readVal, &readUnit);
    humanReadable((double)totalBytesWritten, &writeVal, &writeUnit);
    humanReadable(rate, &rateVal, &rateUnit);

    fprintf(stderr,
            "%" PRIu64 " bytes (%.2f %s) read, "
            "%" PRIu64 " bytes (%.2f %s) written, "
            "%.2f s, "
            "%.2f %s/s\r",
            totalBytesRead,
            readVal,
            readUnit,
            totalBytesWritten,
            writeVal,
            writeUnit,
            seconds,
            rateVal,
            rateUnit);
}

static inline uint64_t preadFull(int device,
                                 void *buffer,
                                 uint64_t bytesToRead,
                                 off_t deviceOffset,
                                 struct stateStruct *stateSt)
{
    uint8_t *bufferPtr = buffer;
    uint64_t totalBytesRead = 0;

    while (totalBytesRead < bytesToRead)
    {

        struct iovec vector;

        vector.iov_base = bufferPtr + totalBytesRead;
        vector.iov_len = bytesToRead - totalBytesRead;

        ssize_t bytesRead = preadv(device,
                                   &vector,
                                   1,
                                   deviceOffset + totalBytesRead);

        if (bytesRead < 0)
        {

            if (errno == EINTR)
                continue;

            stateSt->miscSt.returnVal = errno;
            return errno;
        }

        if (bytesRead == 0)
        {
            stateSt->miscSt.returnVal = EIO;
            return EIO;
        }

        totalBytesRead += bytesRead;
    }

    return 0;
}

static inline uint64_t pwriteFull(int device,
                                  void *buffer,
                                  uint64_t bytesToWrite,
                                  off_t deviceOffset,
                                  struct stateStruct *stateSt)
{
    uint8_t *bufferPtr = buffer;
    uint64_t totalBytesWritten = 0;

    while (totalBytesWritten < bytesToWrite)
    {

        struct iovec vector;

        vector.iov_base = bufferPtr + totalBytesWritten;
        vector.iov_len = bytesToWrite - totalBytesWritten;

        ssize_t bytesWritten = pwritev(device,
                                       &vector,
                                       1,
                                       deviceOffset + totalBytesWritten);

        if (bytesWritten < 0)
        {

            if (errno == EINTR)
                continue;

            stateSt->miscSt.returnVal = errno;
            return errno;
        }

        if (bytesWritten == 0)
        {
            stateSt->miscSt.returnVal = EIO;
            return EIO;
        }

        totalBytesWritten += bytesWritten;
    }

    return 0;
}

static inline void adaptive_readahead(
    int fd,
    struct readaheadStruct *readaheadSt,
    struct timespec t1,
    struct timespec t2,
    size_t chunkSize,
    size_t chunksThisIter,
    uint64_t remainingBytes,
    off_t deviceOffset)
{
    double latency =
        (t2.tv_sec - t1.tv_sec) +
        (t2.tv_nsec - t1.tv_nsec) / 1e9;

    if (readaheadSt->lastLatency > 0)
    {

        double ratio = latency / readaheadSt->lastLatency;

        /* Device getting slower → reduce pipeline slightly */
        if (ratio > 1.30 && readaheadSt->readaheadChunks > 4)
        {

            size_t shrink = (readaheadSt->readaheadChunks / 8) + 1;

            if (readaheadSt->readaheadChunks > shrink)
                readaheadSt->readaheadChunks -= shrink;
            else
                readaheadSt->readaheadChunks = 4;
        }

        /* Device handling pipeline easily → grow faster */
        else if (ratio < 0.80 && readaheadSt->readaheadChunks < 48)
        {

            size_t growth = (readaheadSt->readaheadChunks / 4) + 1;

            readaheadSt->readaheadChunks += growth;

            if (readaheadSt->readaheadChunks > 48)
                readaheadSt->readaheadChunks = 48;
        }
    }

    readaheadSt->lastLatency = latency;

    uint64_t raBytes = chunkSize * readaheadSt->readaheadChunks;

    if (raBytes > remainingBytes)
        raBytes = remainingBytes;

    if (remainingBytes > raBytes)
    {

        off_t raOffset =
            deviceOffset + chunkSize * chunksThisIter;

        if (readahead(fd, raOffset, raBytes) != 0)
        {

            if (errno != EINVAL && errno != ENOSYS)
                PRINT_SYS_ERROR(errno);
        }
    }
}

static inline ssize_t findFirstMismatch(
    const uint8_t *restrict a,
    const uint8_t *restrict b,
    size_t len)
{
    size_t i = 0;

    /* Compare 8 bytes at a time */
    const uint64_t *a64 = (const uint64_t *)a;
    const uint64_t *b64 = (const uint64_t *)b;

    size_t words = len / sizeof(uint64_t);

    for (i = 0; i < words; i++)
    {
        if (a64[i] != b64[i])
        {

            /* Narrow down to byte level */
            const uint8_t *ab = (const uint8_t *)&a64[i];
            const uint8_t *bb = (const uint8_t *)&b64[i];

            for (size_t j = 0; j < 8; j++)
            {
                if (ab[j] != bb[j])
                    return (i * 8) + j;
            }
        }
    }

    /* Remaining bytes */
    size_t offset = words * 8;

    for (i = offset; i < len; i++)
    {
        if (a[i] != b[i])
            return i;
    }

    return -1; /* buffers identical */
}

uint64_t getDeviceSize(const char *device);
uint64_t getLogicalBlockSize(int device, const char *deviceName);
uint64_t greatestCommonDenominator(uint64_t a, uint64_t b);
uint64_t leastCommonDenominator(uint64_t a, uint64_t b);
uint8_t printSyntax(char *arg);
uint64_t parseBufferSize(const char *arg);
void parseOptions(int argc, char *argv[], struct stateStruct *stateSt);

void diffDup(int sourceDevice,
             int destinationDevice,
             uint64_t sourceDeviceSize,
             struct stateStruct *stateSt);

void installSignalHandlers(void);

int sigusr1Pending(void);
int sigintCount(void);
void clearSigusr1(void);

void autoTuneIO(int fd, struct stateStruct *stateSt);
