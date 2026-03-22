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

    struct stateStruct stateSt = {0};

    /* ----- Set Default Parmaters ----- */

    stateSt.configSt.dataBufSize = DEFAULT_BUFFER_SIZE;
    stateSt.configSt.numVectors = DEFAULT_VECTOR_NUM;
    stateSt.optSt.tuneIO = false;
    stateSt.optSt.enableManualReadahead = true;

    /* ----- Set User-Configured Parmaters ----- */

    parseOptions(argc, argv, &stateSt);

    /* ----- Automatically tune IO Parmaters ----- */

    int sourceDevice, destinationDevice;

    if (stateSt.optSt.tuneIO == true)
    {

        sourceDevice = open(stateSt.deviceNameSt.sourceDeviceName, O_RDONLY | O_CLOEXEC | O_DIRECT);
        if (sourceDevice < 0)
        {
            PRINT_DEVICE_ERROR(stateSt.deviceNameSt.sourceDeviceName, errno);
            exit(EXIT_FAILURE);
        }

        destinationDevice = open(stateSt.deviceNameSt.destinationDeviceName, O_RDWR | O_CLOEXEC | O_DIRECT);
        if (destinationDevice < 0)
        {
            PRINT_DEVICE_ERROR(stateSt.deviceNameSt.destinationDeviceName, errno);
            close(sourceDevice);
            exit(EXIT_FAILURE);
        }

        printf("\nAutomatically tuning best parameters for source...\n");
        autoTuneIO(sourceDevice, &stateSt);

        printf("\nAutomatically tuning best parametersfor destination...\n");
        autoTuneIO(destinationDevice, &stateSt);
        close(sourceDevice);
        close(destinationDevice);
    }

    /* ----- Open Devices ----- */

    sourceDevice = open(stateSt.deviceNameSt.sourceDeviceName, O_RDONLY | O_CLOEXEC);
    if (sourceDevice < 0)
    {
        PRINT_DEVICE_ERROR(stateSt.deviceNameSt.sourceDeviceName, errno);
        exit(EXIT_FAILURE);
    }

    if (stateSt.optSt.sourceStartGiven == true)
    {
        lseek(sourceDevice, stateSt.configSt.sourceDeviceStart, SEEK_SET);
    }
    else
    {
        lseek(sourceDevice, 0L, SEEK_SET);
    }

    destinationDevice = open(stateSt.deviceNameSt.destinationDeviceName, O_RDWR | O_CLOEXEC);
    if (destinationDevice < 0)
    {
        PRINT_DEVICE_ERROR(stateSt.deviceNameSt.destinationDeviceName, errno);
        close(sourceDevice);
        exit(EXIT_FAILURE);
    }

    if (stateSt.optSt.destinationStartGiven == true)
    {
        lseek(destinationDevice, stateSt.configSt.destinationDeviceStart, SEEK_SET);
    }
    else
    {
        lseek(destinationDevice, 0L, SEEK_SET);
    }

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

        fprintf(stderr,"\nSource and destination refer to the same device\n");
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
	    getDeviceSize(stateSt.deviceNameSt.sourceDeviceName);
	
	uint64_t destinationDeviceSize =
	    getDeviceSize(stateSt.deviceNameSt.destinationDeviceName);
	
	/* ----- Compute effective ranges ----- */
	
	uint64_t sourceStart = stateSt.configSt.sourceDeviceStart;
	uint64_t destinationStart = stateSt.configSt.destinationDeviceStart;
	
	/* Validate source start */
	if (sourceStart > sourceDeviceSize)
	{
	    fprintf(stderr, "\nSource start offset exceeds source size\n");
	    close(sourceDevice);
	    close(destinationDevice);
	    exit(EXIT_FAILURE);
	}
	
	/* Bytes available from source after offset */
	uint64_t amountAvailable = sourceDeviceSize - sourceStart;
	
	/* Determine how much we actually intend to copy */
	uint64_t amountNeeded = amountAvailable;
	
	if (stateSt.optSt.outputAmountGiven)
	{
	    if (stateSt.configSt.outputAmount > amountAvailable)
	    {
	        fprintf(stderr, "\nRequested amount exceeds available bytes from source offset\n");
	        close(sourceDevice);
	        close(destinationDevice);
	        exit(EXIT_FAILURE);
	    }
	
	    amountNeeded = stateSt.configSt.outputAmount;
	}
	
	/* ----- Validate destination capacity ----- */
	
	/* Skip size enforcement during integrity-only verification */
	if (!stateSt.optSt.verifyIntegrity)
	{
	    if (destinationStart > destinationDeviceSize ||
	        amountNeeded > destinationDeviceSize - destinationStart)
	    {
	        fprintf(stderr, "\nDestination not large enough for requested write range\n");
	        close(sourceDevice);
	        close(destinationDevice);
	        exit(EXIT_FAILURE);
	    }
	}

    /* ----- Get block sizes ----- */

    uint64_t sourceLogicalBlockSize =
        getLogicalBlockSize(sourceDevice, stateSt.deviceNameSt.sourceDeviceName);

    uint64_t destinationLogicalBlockSize =
        getLogicalBlockSize(destinationDevice, stateSt.deviceNameSt.destinationDeviceName);

    uint64_t requiredAlignment =
        leastCommonDenominator(sourceLogicalBlockSize, destinationLogicalBlockSize);

    /* Enforce minimum alignment */

    if (stateSt.configSt.dataBufSize < requiredAlignment)
    {
        stateSt.configSt.dataBufSize = requiredAlignment;
    }
    else
    {
        uint64_t remainder =
            stateSt.configSt.dataBufSize % requiredAlignment;

        if (remainder != 0)
        {
            stateSt.configSt.dataBufSize += requiredAlignment - remainder;
        }
    }

    if (stateSt.configSt.dataBufSize == 0)
    {
        PRINT_ERROR("dataBufSize resolved to zero after alignment");
        exit(EXIT_FAILURE);
    }

    /* ----- Do Differential Duplication ----- */

    diffDup(sourceDevice,
            destinationDevice,
            sourceDeviceSize,
            &stateSt);

    if (stateSt.optSt.verifyAfter)
    {
        printf("\nVerifying written data...\n");

        stateSt.optSt.verifyIntegrity = true;
        stateSt.optSt.verifyWrites = false;

        diffDup(sourceDevice,
                destinationDevice,
                sourceDeviceSize,
                &stateSt);
    }

    /* ----- Close Devices ----- */

    close(sourceDevice);
    close(destinationDevice);

    /* ----- Cleanup ----- */

    DDFREE(free, stateSt.deviceNameSt.sourceDeviceName);

    DDFREE(free, stateSt.deviceNameSt.destinationDeviceName);

    return EXIT_SUCCESS;
}
