#include "lib.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void diffDup(int sourceDevice,
             int destinationDevice,
             uint64_t sourceDeviceSize,
             struct dataStruct *st)
{
    uint64_t remainingBytes = sourceDeviceSize;
    uint64_t chunkSize = st->cryptSt.dataBufSize;

    uint8_t *inBuffer  = calloc(chunkSize, sizeof(*inBuffer));
    uint8_t *outBuffer = calloc(chunkSize, sizeof(*outBuffer));

    if (inBuffer == NULL || outBuffer == NULL) {
        PRINT_SYS_ERROR(errno);
        PRINT_ERROR("Could not allocate memory input/output buffers");
        exit(EXIT_FAILURE);
    }

    off_t deviceOffset = 0;

    while (remainingBytes > 0) {

        uint64_t thisChunk = chunkSize;
        if (thisChunk > remainingBytes)
            thisChunk = remainingBytes;

        /* Read source */
        if (preadFull(sourceDevice,
                      inBuffer,
                      thisChunk,
                      deviceOffset,
                      st) != 0) {

            PRINT_SYS_ERROR(st->miscSt.returnVal);
            PRINT_ERROR("Could not read from source device");
            exit(EXIT_FAILURE);
        }

        /* Read destination */
        if (preadFull(destinationDevice,
                      outBuffer,
                      thisChunk,
                      deviceOffset,
                      st) != 0) {

            PRINT_SYS_ERROR(st->miscSt.returnVal);
            PRINT_ERROR("Could not read from destination device");
            exit(EXIT_FAILURE);
        }

        /* Compare */
        if (memcmp(inBuffer, outBuffer, thisChunk) != 0) {

            if (pwriteFull(destinationDevice,
                           inBuffer,
                           thisChunk,
                           deviceOffset,
                           st) != 0) {

                PRINT_SYS_ERROR(st->miscSt.returnVal);
                PRINT_ERROR("Could not write to destination device");
                exit(EXIT_FAILURE);
            }
        }

        deviceOffset   += thisChunk;
        remainingBytes -= thisChunk;
    }

    DDFREE(free, inBuffer);
    DDFREE(free, outBuffer);

    if (fsync(destinationDevice) != 0) {
        PRINT_SYS_ERROR(errno);
        PRINT_ERROR("fsync failed on destination device");
        exit(EXIT_FAILURE);
    }
}
