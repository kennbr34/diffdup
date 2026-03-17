/*
 * diffdup - incremental block device duplication
 *
 * Copyright (c) 2026 Kenneth Brown
 * Licensed under the MIT License.
 */

#define _GNU_SOURCE /* See feature_test_macros(7) */
#include "lib.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <time.h>

void diffDup(int sourceDevice,
             int destinationDevice,
             uint64_t sourceDeviceSize,
             struct dataStruct *st)
{
    uint64_t available = sourceDeviceSize - st->cryptSt.sourceDeviceStart;

    uint64_t remainingBytes;

    if (st->optSt.outputAmountGiven)
        remainingBytes = st->cryptSt.outputAmount;
    else
        remainingBytes = available;

    if (remainingBytes > available)
        remainingBytes = available;

    uint64_t chunkSize = st->cryptSt.dataBufSize;
    size_t pageSize = getpagesize();

    const int VECTOR_WIDTH = st->cryptSt.numVectors;
    uint8_t **inBuffer = NULL;
    uint8_t **outBuffer = NULL;

    inBuffer = malloc(sizeof(uint8_t *) * VECTOR_WIDTH);
    outBuffer = malloc(sizeof(uint8_t *) * VECTOR_WIDTH);

    if (!inBuffer || !outBuffer)
    {
        PRINT_SYS_ERROR(errno);
        PRINT_ERROR("Could not allocate buffer pointer arrays");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < VECTOR_WIDTH; i++)
    {

        if (posix_memalign((void **)&inBuffer[i], pageSize, chunkSize) != 0)
        {
            PRINT_SYS_ERROR(errno);
            PRINT_ERROR("Could not allocate memory input buffer");
            exit(EXIT_FAILURE);
        }

        if (posix_memalign((void **)&outBuffer[i], pageSize, chunkSize) != 0)
        {
            PRINT_SYS_ERROR(errno);
            PRINT_ERROR("Could not allocate memory output buffer");
            exit(EXIT_FAILURE);
        }

        memset(inBuffer[i], 0, chunkSize);
        memset(outBuffer[i], 0, chunkSize);
    }

    struct iovec *srcVec = NULL;
    struct iovec *dstVec = NULL;

    srcVec = malloc(sizeof(struct iovec) * VECTOR_WIDTH);
    dstVec = malloc(sizeof(struct iovec) * VECTOR_WIDTH);

    if (!srcVec || !dstVec)
    {
        PRINT_SYS_ERROR(errno);
        PRINT_ERROR("Could not allocate iovec arrays");
        exit(EXIT_FAILURE);
    }

    off_t sourceDeviceOffset = 0;
    if (st->optSt.sourceStartGiven == true)
    {
        sourceDeviceOffset = st->cryptSt.sourceDeviceStart;
    }

    off_t destinationDeviceOffset = 0;
    if (st->optSt.destinationStartGiven == true)
    {
        destinationDeviceOffset = st->cryptSt.destinationDeviceStart;
    }

    uint64_t totalBytesRead = 0;
    uint64_t totalBytesWritten = 0;

    struct timespec startTime;
    if (clock_gettime(CLOCK_MONOTONIC, &startTime) != 0)
    {
        PRINT_SYS_ERROR(errno);
        PRINT_ERROR("clock_gettime failed");
        exit(EXIT_FAILURE);
    }

    struct timespec lastPrintTime = startTime;
    struct timespec sourceTimer1, sourceTimer2;
    struct timespec destinationTimer1, destinationTimer2;

    st->miscSt.sourceRaSt.raChunks = 8;
    st->miscSt.sourceRaSt.lastLatency = 0;

    st->miscSt.destinationRaSt.raChunks = 8;
    st->miscSt.destinationRaSt.lastLatency = 0;

    int gracefulStop = 0;

    while (remainingBytes > 0)
    {

        /* ----- Periodic progress display (low-overhead) ----- */

        if (st->optSt.printProgress)
        {

            struct timespec now;

            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            {
                PRINT_SYS_ERROR(errno);
                PRINT_ERROR("clock_gettime failed");
                exit(EXIT_FAILURE);
            }

            if ((now.tv_sec - lastPrintTime.tv_sec) >= 1)
            {

                fprintf(stderr, "\r\033[K");

                printStats(totalBytesRead,
                           totalBytesWritten,
                           startTime);

                fflush(stderr);

                lastPrintTime = now;
            }
        }

        /* ----- Signal Handling ----- */

        if (sigusr1Pending())
        {
            printStats(totalBytesRead,
                       totalBytesWritten,
                       startTime);
            clearSigusr1();
        }

        if (sigintCount() == 1 && !gracefulStop)
        {
            PRINT_ERROR("\nInterrupt received, stopping after current block...\n");
            gracefulStop = 1;
        }

        if (sigintCount() >= 2)
        {
            PRINT_ERROR("\nSecond interrupt received, exiting immediately.\n");
            exit(EXIT_FAILURE);
        }

        if (gracefulStop)
            break;

        /* ----- Determine chunks for this iteration ----- */

        int chunksThisIter = VECTOR_WIDTH;

        if (remainingBytes < chunkSize * VECTOR_WIDTH)
        {
            chunksThisIter = remainingBytes / chunkSize;

            if (remainingBytes % chunkSize)
                chunksThisIter++;
        }

        /* ----- Prepare iovecs ----- */

        for (int i = 0; i < chunksThisIter; i++)
        {

            uint64_t thisChunk = chunkSize;

            if (thisChunk > remainingBytes - (i * chunkSize))
                thisChunk = remainingBytes - (i * chunkSize);

            srcVec[i].iov_base = inBuffer[i];
            srcVec[i].iov_len = thisChunk;

            dstVec[i].iov_base = outBuffer[i];
            dstVec[i].iov_len = thisChunk;
        }

        /* ----- Vector read destination ----- */

        clock_gettime(CLOCK_MONOTONIC, &destinationTimer1);

        for (int i = 0; i < chunksThisIter; i++)
        {
            if (preadFull(destinationDevice,
                          outBuffer[i],
                          dstVec[i].iov_len,
                          destinationDeviceOffset + (i * chunkSize),
                          st) != 0)
            {
                PRINT_SYS_ERROR(st->miscSt.returnVal);
                PRINT_ERROR("\nCould not read from destination device\n");
                exit(EXIT_FAILURE);
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &destinationTimer2);

        /* ----- Manual readahead for destination ----- */

        if (st->optSt.enableManualReadahead)
        {

            adaptive_readahead(
                destinationDevice,
                &st->miscSt.destinationRaSt,
                destinationTimer1,
                destinationTimer2,
                chunkSize,
                chunksThisIter,
                remainingBytes,
                destinationDeviceOffset);
        }

        /* ----- Vector read source ----- */

        clock_gettime(CLOCK_MONOTONIC, &sourceTimer1);

        for (int i = 0; i < chunksThisIter; i++)
        {
            if (preadFull(sourceDevice,
                          inBuffer[i],
                          srcVec[i].iov_len,
                          sourceDeviceOffset + (i * chunkSize),
                          st) != 0)
            {
                PRINT_SYS_ERROR(st->miscSt.returnVal);
                PRINT_ERROR("\nCould not read from source device\n");
                exit(EXIT_FAILURE);
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &sourceTimer2);

        /* ----- Manual readahead for source ----- */

        if (st->optSt.enableManualReadahead)
        {

            adaptive_readahead(
                sourceDevice,
                &st->miscSt.sourceRaSt,
                sourceTimer1,
                sourceTimer2,
                chunkSize,
                chunksThisIter,
                remainingBytes,
                sourceDeviceOffset);
        }

        /* ----- Process each chunk ----- */

        for (int i = 0; i < chunksThisIter; i++)
        {

            uint64_t thisChunk = srcVec[i].iov_len;

            totalBytesRead += thisChunk;

            ssize_t mismatchIndex =
                findFirstMismatch(inBuffer[i], outBuffer[i], thisChunk);

            int blocksDiffer = (mismatchIndex >= 0);

            if (blocksDiffer)
            {

                if (st->optSt.verifyIntegrity)
                {

                    uint64_t absoluteOffset =
                        sourceDeviceOffset + (i * chunkSize) + mismatchIndex;

                    fprintf(stderr,
                            "Integrity check failed at absolute offset %" PRIu64
                            " (0x%016" PRIx64 ")\n",
                            absoluteOffset,
                            absoluteOffset);

                    exit(EXIT_FAILURE);
                }

                if (pwriteFull(destinationDevice,
                               inBuffer[i],
                               thisChunk,
                               destinationDeviceOffset + (i * chunkSize),
                               st) != 0)
                {

                    PRINT_SYS_ERROR(st->miscSt.returnVal);
                    PRINT_ERROR("Could not write to destination device");
                    exit(EXIT_FAILURE);
                }

                totalBytesWritten += thisChunk;

                if (st->optSt.verifyWrites)
                {

                    if (preadFull(destinationDevice,
                                  outBuffer[i],
                                  thisChunk,
                                  destinationDeviceOffset + (i * chunkSize),
                                  st) != 0)
                    {

                        PRINT_SYS_ERROR(st->miscSt.returnVal);
                        PRINT_ERROR("\nCould not re-read destination for verification\n");
                        exit(EXIT_FAILURE);
                    }

                    ssize_t mismatchIndex =
                        findFirstMismatch(inBuffer[i],
                                          outBuffer[i],
                                          thisChunk);

                    if (mismatchIndex >= 0)
                    {

                        uint64_t absoluteOffset =
                            sourceDeviceOffset + (i * chunkSize) + mismatchIndex;

                        fprintf(stderr,
                                "\nVerification failed at absolute offset %" PRIu64
                                " (0x%016" PRIx64 ")\n"
                                "Expected: 0x%02x  Found: 0x%02x\n",
                                absoluteOffset,
                                absoluteOffset,
                                (unsigned char)inBuffer[i][mismatchIndex],
                                (unsigned char)outBuffer[i][mismatchIndex]);

                        exit(EXIT_FAILURE);
                    }
                }
            }

            remainingBytes -= thisChunk;
        }

        sourceDeviceOffset += chunkSize * chunksThisIter;
        destinationDeviceOffset += chunkSize * chunksThisIter;

        size_t bytesProcessed = chunkSize * chunksThisIter;

        posix_fadvise(sourceDevice,
                      sourceDeviceOffset - bytesProcessed,
                      bytesProcessed,
                      POSIX_FADV_DONTNEED);

        posix_fadvise(destinationDevice,
                      destinationDeviceOffset - bytesProcessed,
                      bytesProcessed,
                      POSIX_FADV_DONTNEED);
    }

    /* ----- Final sync ----- */

    if (!st->optSt.verifyIntegrity && totalBytesWritten > 0)
    {

        if (fsync(destinationDevice) != 0)
        {
            PRINT_SYS_ERROR(errno);
            PRINT_ERROR("\nfsync failed on destination device\n");
            exit(EXIT_FAILURE);
        }
    }

    /* ----- Final stats ----- */

    printStats(totalBytesRead,
               totalBytesWritten,
               startTime);
    printf("\n");

    for (int i = 0; i < VECTOR_WIDTH; i++)
    {
        DDFREE(free, inBuffer[i]);
        DDFREE(free, outBuffer[i]);
    }

    DDFREE(free, inBuffer);
    DDFREE(free, outBuffer);

    DDFREE(free, srcVec);
    DDFREE(free, dstVec);

    if (st->optSt.verifyIntegrity)
    {
        printf("\nIntegrity verification completed successfully.\n");
    }
}
