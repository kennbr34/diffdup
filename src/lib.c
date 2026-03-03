#include "lib.h"
#include <getopt.h>
#include <stdint.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <stdlib.h>

uint64_t preadFull(int device,
                   void *buffer,
                   uint64_t bytesToRead,
                   off_t deviceOffset,
                   struct dataStruct *st)
{
    uint8_t *bufferPtr = buffer;
    uint64_t totalBytesRead = 0;

    while (totalBytesRead < bytesToRead) {

        ssize_t bytesRead = pread(device,
                                  bufferPtr + totalBytesRead,
                                  bytesToRead - totalBytesRead,
                                  deviceOffset + totalBytesRead);

        if (bytesRead < 0) {

            if (errno == EINTR)
                continue;

            st->miscSt.returnVal = errno;
            return errno;
        }

        if (bytesRead == 0) {
            /* Unexpected EOF */
            st->miscSt.returnVal = EIO;
            return EIO;
        }

        totalBytesRead += bytesRead;
    }

    st->miscSt.freadAmt = totalBytesRead;

    return 0;
}

uint64_t pwriteFull(int device,
                    void *buffer,
                    uint64_t bytesToWrite,
                    off_t deviceOffset,
                    struct dataStruct *st)
{
    uint8_t *bufferPtr = buffer;
    uint64_t totalBytesWritten = 0;

    while (totalBytesWritten < bytesToWrite) {

        ssize_t bytesWritten = pwrite(device,
                                      bufferPtr + totalBytesWritten,
                                      bytesToWrite - totalBytesWritten,
                                      deviceOffset + totalBytesWritten);

        if (bytesWritten < 0) {

            if (errno == EINTR)
                continue;

            st->miscSt.returnVal = errno;
            return errno;
        }

        if (bytesWritten == 0) {
            st->miscSt.returnVal = EIO;
            return EIO;
        }

        totalBytesWritten += bytesWritten;
    }

    return 0;
}

uint64_t getDeviceSize(const char *deviceName)
{
    int device = open(deviceName, O_RDONLY | O_CLOEXEC);
    if (device < 0) {
        PRINT_SYS_ERROR(errno);
        exit(EXIT_FAILURE);
    }

    struct stat deviceStat;
    if (fstat(device, &deviceStat) != 0) {
        PRINT_SYS_ERROR(errno);
        close(device);
        exit(EXIT_FAILURE);
    }

    uint64_t deviceSize = 0;

    if (S_ISREG(deviceStat.st_mode)) {

        deviceSize = (uint64_t)deviceStat.st_size;

    } else if (S_ISBLK(deviceStat.st_mode)) {

        if (ioctl(device, BLKGETSIZE64, &deviceSize) != 0) {
            PRINT_SYS_ERROR(errno);
            close(device);
            exit(EXIT_FAILURE);
        }

    } else {

        fprintf(stderr, "Unsupported file type: %s\n", deviceName);
        close(device);
        exit(EXIT_FAILURE);
    }

    close(device);
    return deviceSize;
}

size_t getBufSizeMultiple(char *value)
{

#define MAX_DIGITS 13
    char valString[MAX_DIGITS] = {0};
    /* Compiling without optimization results in extremely slow speeds, but this will be optimized
     * out if not set to volatile.
     */
    volatile int valueLength = 0;
    volatile int multiple = 1;

    /* value from getsubopt is not null-terminated so must copy and get the length manually without
     * string functions
     */
    for (valueLength = 0; valueLength < MAX_DIGITS; valueLength++) {
        if (isdigit(value[valueLength])) {
            valString[valueLength] = value[valueLength];
            continue;
        } else if (isalpha(value[valueLength])) {
            valString[valueLength] = value[valueLength];
            valueLength++;
            break;
        }
    }

    if (valString[valueLength - 1] == 'b' || valString[valueLength - 1] == 'B')
        multiple = 1;
    if (valString[valueLength - 1] == 'k' || valString[valueLength - 1] == 'K')
        multiple = 1024;
    if (valString[valueLength - 1] == 'm' || valString[valueLength - 1] == 'M')
        multiple = 1024 * 1024;
    if (valString[valueLength - 1] == 'g' || valString[valueLength - 1] == 'G')
        multiple = 1024 * 1024 * 1024;

    return multiple;
}

void makeMultipleOf(size_t *numberToChange, size_t multiple)
{
    if (*numberToChange > multiple && *numberToChange % multiple != 0) {
        *numberToChange = *numberToChange - (*numberToChange % multiple);
    } else if (*numberToChange > multiple && *numberToChange % multiple == 0) {
        *numberToChange = *numberToChange;
    }
}

void bytesPrefixed(char *prefixedString, unsigned long long bytes)
{
    if (bytes <= 1023) {
        sprintf(prefixedString, "%llu bytes", bytes);
    } else if (bytes >= 1024 && bytes < 1048576) {
        sprintf(prefixedString, "%llu Kb", bytes / 1024);
    } else if (bytes >= 1048576 && bytes < 1073741824) {
        sprintf(prefixedString, "%llu Mb", bytes / 1048576);
    } else if (bytes >= 1073741824) {
        sprintf(prefixedString, "%llu Gb", bytes / 1073741824);
    }
}

uint8_t printSyntax(char *arg)
{
    printf("\
\nUse: \
\n\n%s -i source -o destination [-b]\
\n-s,--source-device - source device\
\n-d,--destination-device - destination device.\
\n\t Size of input/output buffers to use in bytes, kilobytes or megabytes\
\n",arg);
    return EXIT_FAILURE;
}

void parseOptions(
    int argc,
    char *argv[],
    struct dataStruct *st)
{
    int c;
    int errflg = 0;
    char binName[MAX_FILE_NAME_SIZE];
    snprintf(binName, MAX_FILE_NAME_SIZE, "%s", argv[0]);

    while (1) {
        int option_index = 0;
        static struct option long_options[] = {
            {"help", no_argument, 0, 'h'},
            {"source-device", required_argument, 0, 's'},
            {"destination-device", required_argument, 0, 's'},
            {"buffer-size", required_argument, 0, 'b'},
            {0, 0, 0, 0}};

        c = getopt_long(argc, argv, "hs:d:b",
                        long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {

        case 'h':
            printSyntax(binName);
            exit(EXIT_FAILURE);
            break;
        case 's':
            if (optarg[0] == '-' && strlen(optarg) == 2) {
                fprintf(stderr, "Option -i requires an argument\n");
                errflg++;
                break;
            } else {
                st->optSt.sourceDeviceGiven = true;
                st->deviceNameSt.sourceDeviceName = strdup(optarg);
            }
            break;
        case 'd':
            if (optarg[0] == '-' && strlen(optarg) == 2) {
                fprintf(stderr, "Option -o requires an argument\n");
                errflg++;
                break;
            } else {
                st->optSt.destinationDeviceGiven = true;
                st->deviceNameSt.destinationDeviceName = strdup(optarg);
            }
            break;
        case 'b':
            if (optarg[0] == '-' && strlen(optarg) == 2) {
                fprintf(stderr, "Option -b requires an argument\n");
                errflg++;
                break;
            } else {
                st->optSt.dataBufSizeGiven = true;
                st->cryptSt.dataBufSize = atol(optarg) * sizeof(uint8_t) * getBufSizeMultiple(optarg);
            }
            break;
        case ':':
            fprintf(stderr, "Option -%c requires an argument\n", optopt);
            errflg++;
            break;
        case '?':
            errflg++;
            break;
        }
    }

    if (!st->optSt.sourceDeviceGiven || !st->optSt.destinationDeviceGiven) {
        fprintf(stderr, "Must specify a source and destination device\n");
        errflg++;
    }

    if (!strcmp(st->deviceNameSt.sourceDeviceName, st->deviceNameSt.destinationDeviceName)) {
        fprintf(stderr, "Source and destination device are the same\n");
        errflg++;
    }

    if (errflg) {
        printSyntax(binName);
        exit(EXIT_FAILURE);
    }
}
