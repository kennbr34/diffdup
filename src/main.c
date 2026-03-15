/*
 * diffdup - incremental block device duplication
 *
 * Copyright (c) 2026 Kenneth Brown
 * Licensed under the MIT License.
 */

#define _GNU_SOURCE
#include "lib.h"
#include <fcntl.h>
#include <linux/fs.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    if (argc == 1)
    {
        printSyntax(argv[0]);
        exit(EXIT_FAILURE);
    }

    installSignalHandlers();

    struct dataStruct st = {0};

    /* ----- Set Default Parmaters ----- */

    st.cryptSt.dataBufSize = DEFAULT_BUFFER_SIZE;
    st.cryptSt.numVectors = DEFAULT_VECTOR_NUM;
    st.optSt.tuneIO = false;
    st.optSt.enableManualReadahead = true;

    /* ----- Set User-Configured Parmaters ----- */

    parseOptions(argc, argv, &st);

    /* ----- Automatically tune IO Parmaters ----- */

    int sourceDevice, destinationDevice;

    if (st.optSt.tuneIO == true)
    {

        sourceDevice = open(st.deviceNameSt.sourceDeviceName, O_RDONLY | O_CLOEXEC | O_DIRECT);
        if (sourceDevice < 0)
        {
            PRINT_DEVICE_ERROR(st.deviceNameSt.sourceDeviceName, errno);
            exit(EXIT_FAILURE);
        }

        destinationDevice = open(st.deviceNameSt.destinationDeviceName, O_RDWR | O_CLOEXEC | O_DIRECT);
        if (destinationDevice < 0)
        {
            PRINT_DEVICE_ERROR(st.deviceNameSt.destinationDeviceName, errno);
            close(sourceDevice);
            exit(EXIT_FAILURE);
        }

        printf("Automatically tuning best parameters for source...\n");
        autoTuneIO(sourceDevice, &st);

        printf("Automatically tuning best parametersfor destination...\n");
        autoTuneIO(destinationDevice, &st);
        close(sourceDevice);
        close(destinationDevice);
    }

    /* ----- Open Devices ----- */

    sourceDevice = open(st.deviceNameSt.sourceDeviceName, O_RDONLY | O_CLOEXEC);
    if (sourceDevice < 0)
    {
        PRINT_DEVICE_ERROR(st.deviceNameSt.sourceDeviceName, errno);
        exit(EXIT_FAILURE);
    }

    lseek(sourceDevice, 0, SEEK_SET);

    destinationDevice = open(st.deviceNameSt.destinationDeviceName, O_RDWR | O_CLOEXEC);
    if (destinationDevice < 0)
    {
        PRINT_DEVICE_ERROR(st.deviceNameSt.destinationDeviceName, errno);
        close(sourceDevice);
        exit(EXIT_FAILURE);
    }

    lseek(destinationDevice, 0, SEEK_SET);

    /* ----- Verify source and destination aren't the same ----- */

    struct stat sourceDeviceStat;
    struct stat destinationDeviceStat;

    if (fstat(sourceDevice, &sourceDeviceStat) != 0)
    {
        PRINT_SYS_ERROR(errno);
        exit(EXIT_FAILURE);
    }

    if (fstat(destinationDevice, &destinationDeviceStat) != 0)
    {
        PRINT_SYS_ERROR(errno);
        exit(EXIT_FAILURE);
    }

    if (sourceDeviceStat.st_dev == destinationDeviceStat.st_dev &&
        sourceDeviceStat.st_ino == destinationDeviceStat.st_ino)
    {

        PRINT_ERROR("Source and destination refer to the same device");
        exit(EXIT_FAILURE);
    }

    /* ----- Advise POSIX we will be doing sequential I/O ----- */

    int returnCode = posix_fadvise(sourceDevice, 0, 0, POSIX_FADV_SEQUENTIAL);
    if (returnCode != 0)
    {
        PRINT_SYS_ERROR(returnCode);
    }

    returnCode = posix_fadvise(destinationDevice, 0, 0, POSIX_FADV_SEQUENTIAL);
    if (returnCode != 0)
    {
        PRINT_SYS_ERROR(returnCode);
    }

    /* ----- Get Sizes ----- */

    uint64_t sourceDeviceSize =
        getDeviceSize(st.deviceNameSt.sourceDeviceName);

    uint64_t destinationDeviceSize =
        getDeviceSize(st.deviceNameSt.destinationDeviceName);

    /* ----- Make sure destination is big enough ----- */

    if (sourceDeviceSize > destinationDeviceSize && st.optSt.verifyIntegrity != true)
    {
        PRINT_ERROR("Destination not large enough for source");
        close(sourceDevice);
        close(destinationDevice);
        exit(EXIT_FAILURE);
    }

    /* ----- Get block sizes ----- */

    uint64_t sourceLogicalBlockSize =
        getLogicalBlockSize(sourceDevice, st.deviceNameSt.sourceDeviceName);

    uint64_t destinationLogicalBlockSize =
        getLogicalBlockSize(destinationDevice, st.deviceNameSt.destinationDeviceName);

    uint64_t requiredAlignment =
        leastCommonDenominator(sourceLogicalBlockSize, destinationLogicalBlockSize);

    /* Enforce minimum alignment */

    if (st.cryptSt.dataBufSize < requiredAlignment)
    {
        st.cryptSt.dataBufSize = requiredAlignment;
    }
    else
    {
        uint64_t remainder =
            st.cryptSt.dataBufSize % requiredAlignment;

        if (remainder != 0)
        {
            st.cryptSt.dataBufSize += requiredAlignment - remainder;
        }
    }

    if (st.cryptSt.dataBufSize == 0)
    {
        PRINT_ERROR("dataBufSize resolved to zero after alignment");
        exit(EXIT_FAILURE);
    }

    /* ----- Do Differential Duplication ----- */

    diffDup(sourceDevice,
            destinationDevice,
            sourceDeviceSize,
            &st);

    if (st.optSt.verifyAfter)
    {
        printf("Verifying written data...\n");

        st.optSt.verifyIntegrity = true;
        st.optSt.verifyWrites = false;

        diffDup(sourceDevice,
                destinationDevice,
                sourceDeviceSize,
                &st);
    }

    /* ----- Close Devices ----- */

    close(sourceDevice);
    close(destinationDevice);

    /* ----- Cleanup ----- */

    DDFREE(free, st.deviceNameSt.sourceDeviceName);

    DDFREE(free, st.deviceNameSt.destinationDeviceName);

    return EXIT_SUCCESS;
}
