/*
 * diffdup - incremental block device duplication
 *
 * Copyright (c) 2026 Kenneth Brown
 * Licensed under the MIT License.
 */

#define _GNU_SOURCE

#include "lib.h"
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysinfo.h>
#include <sys/uio.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

uint64_t getDeviceSize(const char *deviceName)
{
    int device = open(deviceName, O_RDONLY | O_CLOEXEC);
    if (device < 0)
    {
        PRINT_SYS_ERROR(errno);
        exit(EXIT_FAILURE);
    }

    struct stat deviceStat;
    if (fstat(device, &deviceStat) != 0)
    {
        PRINT_SYS_ERROR(errno);
        close(device);
        exit(EXIT_FAILURE);
    }

    uint64_t deviceSize = 0;

    if (S_ISREG(deviceStat.st_mode))
    {

        deviceSize = (uint64_t)deviceStat.st_size;
    }
    else if (S_ISBLK(deviceStat.st_mode))
    {

        if (ioctl(device, BLKGETSIZE64, &deviceSize) != 0)
        {
            PRINT_SYS_ERROR(errno);
            close(device);
            exit(EXIT_FAILURE);
        }
    }
    else
    {

        fprintf(stderr, "Unsupported file type: %s\n", deviceName);
        close(device);
        exit(EXIT_FAILURE);
    }

    close(device);
    return deviceSize;
}

uint64_t getLogicalBlockSize(int device, const char *deviceName)
{
    struct stat deviceStat;

    if (fstat(device, &deviceStat) != 0)
    {
        PRINT_DEVICE_ERROR(deviceName, errno);
        exit(EXIT_FAILURE);
    }

    if (S_ISBLK(deviceStat.st_mode))
    {

        int logicalBlockSize = 0;

        if (ioctl(device, BLKSSZGET, &logicalBlockSize) != 0)
        {
            PRINT_ERROR("Could not get logical block size");
            PRINT_SYS_ERROR(errno);
            exit(EXIT_FAILURE);
        }

        if (logicalBlockSize <= 0 || logicalBlockSize > 65536)
        {
            PRINT_ERROR("Kernel returned invalid logical block size");
            exit(EXIT_FAILURE);
        }

        return (uint64_t)logicalBlockSize;
    }

    if (S_ISREG(deviceStat.st_mode))
    {

        /* Use filesystem block size */
        if (deviceStat.st_blksize > 0 && deviceStat.st_blksize <= 65536)
            return (uint64_t)deviceStat.st_blksize;

        return 4096;
    }

    PRINT_ERROR("Unsupported file type");
    exit(EXIT_FAILURE);
}

uint64_t greatestCommonDenominator(uint64_t number1, uint64_t number2)
{
    while (number2 != 0)
    {
        uint64_t temp = number2;
        number2 = number1 % number2;
        number1 = temp;
    }
    return number1;
}

uint64_t leastCommonDenominator(uint64_t number1, uint64_t number2)
{
    return (number1 / greatestCommonDenominator(number1, number2)) * number2;
}

static volatile sig_atomic_t g_sigusr1 = 0;
static volatile sig_atomic_t g_sigint_count = 0;

static void handle_sigusr1(int signo)
{
    (void)signo;
    g_sigusr1 = 1;
}

static void handle_sigint(int signo)
{
    (void)signo;
    g_sigint_count++;
}

void installSignalHandlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigusr1;
    sigaction(SIGUSR1, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
}

/* Accessors */

int sigusr1Pending(void)
{
    return g_sigusr1;
}

void clearSigusr1(void)
{
    g_sigusr1 = 0;
}

int sigintCount(void)
{
    return g_sigint_count;
}

static double getTimeSec()
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

static double benchmarkRead(int fd, size_t chunkSize, int numBuffers, int testIndex)
{
    struct iovec *ioVector = NULL;
    void **buffer = NULL;

    uint64_t totalBytes = 256ULL * 1024 * 1024;
    uint64_t processed = 0;

    ioVector = malloc(sizeof(struct iovec) * numBuffers);
    buffer = malloc(sizeof(void *) * numBuffers);

    if (!ioVector || !buffer)
        return 0;

    size_t pageSize = getpagesize();

    for (int i = 0; i < numBuffers; i++)
    {

        if (posix_memalign(&buffer[i], pageSize, chunkSize) != 0)
            goto cleanup;

        memset(buffer[i], 0, chunkSize);

        ioVector[i].iov_base = buffer[i];
        ioVector[i].iov_len = chunkSize;
    }

    /* Avoid cached pages affecting benchmark */
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);

    /* Move each test to a different region */
    off_t offset = (off_t)testIndex * 512ULL * 1024 * 1024;

    if (lseek(fd, offset, SEEK_SET) < 0)
        goto cleanup;

    double start = getTimeSec();

    while (processed < totalBytes)
    {

        ssize_t r = readv(fd, ioVector, numBuffers);

        if (r <= 0)
            break;

        processed += r;
    }

    double end = getTimeSec();

    /* Drop pages again after test */
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);

cleanup:

    if (buffer)
    {
        for (int i = 0; i < numBuffers; i++)
        {
            if (buffer[i])
                free(buffer[i]);
        }
        free(buffer);
    }

    if (ioVector)
        free(ioVector);

    if (processed == 0)
        return 0;

    return processed / (end - start);
}

void autoTuneIO(int fd, struct stateStruct *stateSt)
{
    size_t chunkOptions[] = {
        64 * 1024,
        128 * 1024,
        256 * 1024};

    int bufferOptions[] = {
        4,
        8};

    double bestSpeed = 0;
    int bestChunkIndex = 0;
    int bestBufferIndex = 0;

    int testIndex = 0;

    /* Disable kernel sequential heuristics */
    posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);

    for (int c = 0; c < 3; c++)
    {
        for (int b = 0; b < 2; b++)
        {

            double speed =
                benchmarkRead(fd,
                              chunkOptions[c],
                              bufferOptions[b],
                              testIndex++);

            printf("Test: chunk=%zu buffers=%d -> %.2f MB/s\n",
                   chunkOptions[c],
                   bufferOptions[b],
                   speed / (1024 * 1024));

            if (speed > bestSpeed)
            {

                bestSpeed = speed;
                bestChunkIndex = c;
                bestBufferIndex = b;

                if (stateSt->optSt.dataBufSizeGiven == false)
                    stateSt->configSt.dataBufSize = chunkOptions[bestChunkIndex];

                if (stateSt->optSt.numVectorsGiven == false)
                    stateSt->configSt.numVectors = bufferOptions[bestBufferIndex];
            }
        }
    }

    double baselineMBps = bestSpeed / (1024.0 * 1024.0);

    size_t chunk = chunkOptions[bestChunkIndex];

    /* --- Manual readahead benchmark --- */

    size_t testSize = 128 * 1024 * 1024; // 128MB
    uint8_t *buffer = malloc(chunk);

    off_t offset = 0;
    off_t end = testSize;

    struct timespec start, endt;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (offset < end)
    {

        /* pipeline prefetch ~4 chunks ahead */
        off_t readaheadOffset = offset + (chunk * 16);
        readahead(fd, readaheadOffset, chunk * 16);

        pread(fd, buffer, chunk, offset);

        offset += chunk;
    }

    clock_gettime(CLOCK_MONOTONIC, &endt);

    free(buffer);

    double seconds =
        (endt.tv_sec - start.tv_sec) +
        (endt.tv_nsec - start.tv_nsec) / 1e9;

    double readaheadMBps = (testSize / seconds) / (1024.0 * 1024.0);

    double improvement = readaheadMBps / baselineMBps;

    if (stateSt->optSt.enableManualReadahead == true)
    {

        if (improvement >= 1.01)
            stateSt->optSt.enableManualReadahead = true;
        else
            stateSt->optSt.enableManualReadahead = false;
    }

    if (stateSt->optSt.dataBufSizeGiven || stateSt->optSt.numVectorsGiven)
        printf("(USER CONFIGURED)");

    printf("\nSelected: chunk=%zu buffers=%d\n",
           stateSt->configSt.dataBufSize,
           stateSt->configSt.numVectors);

    printf("Detected throughput (baseline): %.2f MB/s\n", baselineMBps);
    printf("Detected throughput (manual readahead): %.2f MB/s\n", readaheadMBps);
    printf("Readahead improvement: %.2fx\n", improvement);

    if (stateSt->optSt.enableManualReadahead)
        printf("Manual readahead ENABLED (prefetch improves throughput)\n");
    else
        printf("Manual readahead DISABLED (no measurable benefit)\n");
}

uint8_t printSyntax(char *arg)
{
    printf("\
\nUse: \
\n\n%s -s source -d destination [-p|-t|-i|-w|-v|-r] [-n num] [-b num[b|k|m|g] [-S|-D|-C num[b|k|m|g|t]]\
\n-p,--progress - periodically print progress\
\n-t,--tune-parameters - automatically benchmark and set optimal buffer sizes and number of buffers and enable readahead for non-USB devices\
\n-i,--verify-integrity - verify that destination matches source\
\n-w,--verify-writes - verify when writes are made for extra assurance\
\n-v,--verify-integrity-after - verify that destination matches source after duplication\
\n-s,--source-device - source device\
\n-d,--destination-device - destination device.\
\n\tThe following options will override default or auto-tuned parameters\
\n-r,--toggle-readahead [yes|no|on|off] - toggle manual eadahead\
\n-n,--buffer-num - number of I/O buffers\
\n-b,--buffer-size - num[b|k|m|g]\
\n\t Size of input/output buffers to use in bytes, kilobytes, megabytes, or gigabytes\
\n-S,--source-start - num[b|k|m|g|t]\
\n\t The start of source device to begin duplicating from, given in kilobytes, megabytes, gigabytes or terabytes\
\n-D,--destination-start - num[b|k|m|g|t]\
\n\t The start of destination device to begin duplicating to, given in kilobytes, megabytes, gigabytes or terabytes\
\n-C,--output-amount - num[b|k|m|g|t]\
\n\t The amount of source to duplicate to destination, given in bytes, kilobytes, megabytes, gigabytes or terabytes\
\n Version %d.%d.%d\
\n",
           arg, MAJOR_VER, MINOR_VER, PATCH_VER);
    return EXIT_FAILURE;
}

uint64_t parseBufferSize(const char *arg)
{
    char *endptr;

    errno = 0;

    uint64_t value = strtoull(arg, &endptr, 10);

    if (errno != 0 || value == 0)
    {
        PRINT_ERROR("Invalid buffer size value");
        exit(EXIT_FAILURE);
    }

    uint64_t multiplier = 1;

    if (*endptr != '\0')
    {

        if (strcasecmp(endptr, "b") == 0)
            multiplier = 1;
        else if (strcasecmp(endptr, "k") == 0)
            multiplier = 1024ULL;
        else if (strcasecmp(endptr, "m") == 0)
            multiplier = 1024ULL * 1024ULL;
        else if (strcasecmp(endptr, "g") == 0)
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        else if (strcasecmp(endptr, "t") == 0)
            multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        else
        {
            PRINT_ERROR("Invalid buffer size suffix (use b/k/m/g/t)");
            exit(EXIT_FAILURE);
        }
    }

    return value * multiplier;
}

void parseOptions(
    int argc,
    char *argv[],
    struct stateStruct *stateSt)
{
    int c;
    int errflg = 0;
    char binName[MAX_FILE_NAME_SIZE];
    snprintf(binName, MAX_FILE_NAME_SIZE, "%s", argv[0]);

    while (1)
    {
        int option_index = 0;
        static struct option long_options[] = {
            {"help", no_argument, 0, 'h'},
            {"progress", no_argument, 0, 'p'},
            {"tune-parameters", no_argument, 0, 't'},
            {"verify-integrity", no_argument, 0, 'i'},
            {"direct-io", no_argument, 0, 'I'},
            {"verify-integrity-after", no_argument, 0, 'v'},
            {"verify-writes", no_argument, 0, 'w'},
            {"source-device", required_argument, 0, 's'},
            {"destination-device", required_argument, 0, 'd'},
            {"toggle-readahead", required_argument, 0, 'r'},
            {"buffer-num", required_argument, 0, 'n'},
            {"buffer-size", required_argument, 0, 'b'},
            {"source-start", required_argument, 0, 'S'},
            {"destination-start", required_argument, 0, 'D'},
            {"output-amount", required_argument, 0, 'C'},
            {0, 0, 0, 0}};

        c = getopt_long(argc, argv, "hptivws:d:r:n:b:S:D:C:",
                        long_options, &option_index);
        if (c == -1)
            break;

        switch (c)
        {

        case 'h':
            printSyntax(binName);
            exit(EXIT_FAILURE);
            break;
        case 'p':
            stateSt->optSt.printProgress = true;
            break;
        case 't':
            stateSt->optSt.tuneIO = true;
            break;
        case 'i':
            stateSt->optSt.verifyIntegrity = true;
            break;
        case 'w':
            stateSt->optSt.verifyWrites = true;
            break;
        case 'v':
            stateSt->optSt.verifyAfter = true;
            break;
        case 's':
            if (optarg[0] == '-' && strlen(optarg) == 2)
            {
                fprintf(stderr, "Option -s requires an argument\n");
                errflg++;
                break;
            }
            else
            {
                stateSt->optSt.sourceDeviceGiven = true;
                stateSt->deviceNameSt.sourceDeviceName = strdup(optarg);
            }
            break;
        case 'd':
            if (optarg[0] == '-' && strlen(optarg) == 2)
            {
                fprintf(stderr, "Option -d requires an argument\n");
                errflg++;
                break;
            }
            else
            {
                stateSt->optSt.destinationDeviceGiven = true;
                stateSt->deviceNameSt.destinationDeviceName = strdup(optarg);
            }
            break;
        case 'r':
            if (optarg[0] == '-' && strlen(optarg) == 2)
            {
                fprintf(stderr, "Option -r requires an argument\n");
                errflg++;
                break;
            }
            else
            {
                if (strncasecmp(optarg, "yes", 3) == 0 || strncasecmp(optarg, "on", 3) == 0)
                {
                    stateSt->optSt.enableManualReadahead = true;
                }
                else if (strncasecmp(optarg, "no", 3) == 0 || strncasecmp(optarg, "off", 3) == 0)
                {
                    stateSt->optSt.enableManualReadahead = false;
                }
                else
                {
                    fprintf(stderr, "Value %s is not valid for option -r", optarg);
                    errflg++;
                }
            }
            break;
        case 'n':
            if (optarg[0] == '-' && strlen(optarg) == 2)
            {
                fprintf(stderr, "Option -n requires an argument\n");
                errflg++;
                break;
            }
            else
            {
                stateSt->optSt.numVectorsGiven = true;
                stateSt->configSt.numVectors = atol(optarg);
            }
            break;
        case 'b':
            if (optarg[0] == '-' && strlen(optarg) == 2)
            {
                fprintf(stderr, "Option -b requires an argument\n");
                errflg++;
                break;
            }
            else
            {
                stateSt->optSt.dataBufSizeGiven = true;
                stateSt->configSt.dataBufSize = parseBufferSize(optarg);
            }
            break;
        case 'S':
            if (optarg[0] == '-' && strlen(optarg) == 2)
            {
                fprintf(stderr, "Option -S requires an argument\n");
                errflg++;
                break;
            }
            else
            {
                stateSt->optSt.sourceStartGiven = true;
                stateSt->configSt.sourceDeviceStart = parseBufferSize(optarg);
            }
            break;
        case 'D':
            if (optarg[0] == '-' && strlen(optarg) == 2)
            {
                fprintf(stderr, "Option -D requires an argument\n");
                errflg++;
                break;
            }
            else
            {
                stateSt->optSt.destinationStartGiven = true;
                stateSt->configSt.destinationDeviceStart = parseBufferSize(optarg);
            }
            break;
        case 'C':
            if (optarg[0] == '-' && strlen(optarg) == 2)
            {
                fprintf(stderr, "Option -C requires an argument\n");
                errflg++;
                break;
            }
            else
            {
                stateSt->optSt.outputAmountGiven = true;
                stateSt->configSt.outputAmount = parseBufferSize(optarg);
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

    if (!stateSt->optSt.sourceDeviceGiven || !stateSt->optSt.destinationDeviceGiven)
    {
        fprintf(stderr, "Must specify a source and destination device\n");
        errflg++;
    }

    /* Only compare if both were actually provided */
    if (stateSt->optSt.sourceDeviceGiven &&
        stateSt->optSt.destinationDeviceGiven)
    {

        if (strcmp(stateSt->deviceNameSt.sourceDeviceName,
                   stateSt->deviceNameSt.destinationDeviceName) == 0)
        {

            fprintf(stderr, "Source and destination device are the same\n");
            errflg++;
        }
    }

    if (errflg)
    {
        printSyntax(binName);
        exit(EXIT_FAILURE);
    }
}
