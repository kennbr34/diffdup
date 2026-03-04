#include "lib.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <inttypes.h>

void diffDup(int sourceDevice,
             int destinationDevice,
             uint64_t sourceDeviceSize,
             struct dataStruct *st)
{
    uint64_t remainingBytes = sourceDeviceSize;
    uint64_t chunkSize      = st->cryptSt.dataBufSize;

    uint8_t *inBuffer  = calloc(chunkSize, sizeof(*inBuffer));
    uint8_t *outBuffer = calloc(chunkSize, sizeof(*outBuffer));

    if (inBuffer == NULL || outBuffer == NULL) {
        PRINT_SYS_ERROR(errno);
        PRINT_ERROR("Could not allocate memory input/output buffers");
        exit(EXIT_FAILURE);
    }

    off_t deviceOffset = 0;

    uint64_t totalBytesRead    = 0;
    uint64_t totalBytesWritten = 0;

    struct timespec startTime;
    if (clock_gettime(CLOCK_MONOTONIC, &startTime) != 0) {
        PRINT_SYS_ERROR(errno);
        PRINT_ERROR("clock_gettime failed");
        exit(EXIT_FAILURE);
    }

    int gracefulStop = 0;

    while (remainingBytes > 0) {

        /* ----- Signal Handling ----- */

        if (sigusr1Pending()) {
            printStats(totalBytesRead,
                       totalBytesWritten,
                       startTime);
            clearSigusr1();
        }

        if (sigintCount() == 1 && !gracefulStop) {
            PRINT_ERROR("Interrupt received, stopping after current block...");
            gracefulStop = 1;
        }

        if (sigintCount() >= 2) {
            PRINT_ERROR("Second interrupt received, exiting immediately.");
            exit(EXIT_FAILURE);
        }

        if (gracefulStop)
            break;

        /* ----- Determine chunk size ----- */

        uint64_t thisChunk = chunkSize;
        if (thisChunk > remainingBytes)
            thisChunk = remainingBytes;

        /* ----- Read source ----- */

        if (preadFull(sourceDevice,
                      inBuffer,
                      thisChunk,
                      deviceOffset,
                      st) != 0) {

            PRINT_SYS_ERROR(st->miscSt.returnVal);
            PRINT_ERROR("Could not read from source device");
            exit(EXIT_FAILURE);
        }

        totalBytesRead += thisChunk;

        /* ----- Read destination ----- */

        if (preadFull(destinationDevice,
                      outBuffer,
                      thisChunk,
                      deviceOffset,
                      st) != 0) {

            PRINT_SYS_ERROR(st->miscSt.returnVal);
            PRINT_ERROR("Could not read from destination device");
            exit(EXIT_FAILURE);
        }

        /* ----- Compare ----- */

        if (memcmp(inBuffer, outBuffer, thisChunk) == 0) {
            /* Blocks are identical — nothing to do */
            deviceOffset   += thisChunk;
            remainingBytes -= thisChunk;
            continue;
        }

        /* ----- Blocks differ ----- */

        if (st->optSt.verifyIntegrity) {

		    uint64_t mismatchIndex = 0;
		
		    for (mismatchIndex = 0; mismatchIndex < thisChunk; mismatchIndex++) {
		        if (inBuffer[mismatchIndex] != outBuffer[mismatchIndex])
		            break;
		    }
		
		    uint64_t absoluteOffset = deviceOffset + mismatchIndex;
		
		    fprintf(stderr,
            "Integrity check failed at absolute offset %" PRIu64
            " (0x%016" PRIx64 ")\n",
            absoluteOffset,
            absoluteOffset);
            exit(EXIT_FAILURE);
		}

        /* ----- Diff mode: write differing block ----- */

        if (pwriteFull(destinationDevice,
                       inBuffer,
                       thisChunk,
                       deviceOffset,
                       st) != 0) {

            PRINT_SYS_ERROR(st->miscSt.returnVal);
            PRINT_ERROR("Could not write to destination device");
            exit(EXIT_FAILURE);
        }

        totalBytesWritten += thisChunk;

        /* ----- Optional verify-after-write ----- */

        if (st->optSt.verifyWrites) {

            if (preadFull(destinationDevice,
                          outBuffer,
                          thisChunk,
                          deviceOffset,
                          st) != 0) {

                PRINT_SYS_ERROR(st->miscSt.returnVal);
                PRINT_ERROR("Could not re-read destination for verification");
                exit(EXIT_FAILURE);
            }

            if (memcmp(inBuffer, outBuffer, thisChunk) != 0) {
                PRINT_ERROR("Verification failed: written data does not match source");
                exit(EXIT_FAILURE);
            }
        }

        deviceOffset   += thisChunk;
        remainingBytes -= thisChunk;
    }

    /* ----- Final sync (only if we performed writes) ----- */

    if (!st->optSt.verifyIntegrity && totalBytesWritten > 0) {
        if (fsync(destinationDevice) != 0) {
            PRINT_SYS_ERROR(errno);
            PRINT_ERROR("fsync failed on destination device");
            exit(EXIT_FAILURE);
        }
    }

    /* ----- Final stats ----- */

    printStats(totalBytesRead,
               totalBytesWritten,
               startTime);

    DDFREE(free, inBuffer);
    DDFREE(free, outBuffer);

    /* Optional success message for integrity-only mode */
    if (st->optSt.verifyIntegrity) {
        printf("Integrity verification completed successfully.\n");
    }
}
