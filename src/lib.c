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
#include <time.h>
#include <signal.h>
#include <inttypes.h>

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

uint64_t greatestCommonDenominator(uint64_t a, uint64_t b)
{
    while (b != 0) {
        uint64_t temp = b;
        b = a % b;
        a = temp;
    }
    return a;
}

uint64_t leastCommonDenominator(uint64_t a, uint64_t b)
{
    return (a / greatestCommonDenominator(a, b)) * b;
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

static double elapsedTime(struct timespec startTime)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0.0;

    return (double)(now.tv_sec - startTime.tv_sec) +
           (double)(now.tv_nsec - startTime.tv_nsec) / 1e9;
}

static void humanReadable(double bytes,
                          double *value,
                          const char **unit)
{
    static const char *units[] = {
        "B", "KiB", "MiB", "GiB", "TiB", "PiB"
    };

    int i = 0;

    while (bytes >= 1024.0 && i < 5) {
        bytes /= 1024.0;
        i++;
    }

    *value = bytes;
    *unit  = units[i];
}

void printStats(uint64_t totalBytesRead,
                uint64_t totalBytesWritten,
                struct timespec startTime)
{
    double seconds = elapsedTime(startTime);

    if (seconds <= 0.0)
        seconds = 1e-9;

    double rate = (double)totalBytesRead / seconds;

    double readVal, rateVal;
    const char *readUnit;
    const char *rateUnit;

    humanReadable((double)totalBytesRead, &readVal, &readUnit);
    humanReadable(rate, &rateVal, &rateUnit);

    fprintf(stderr,
            "%" PRIu64 " bytes (%.2f %s) read, "
            "%" PRIu64 " bytes written, "
            "%.2f s, "
            "%.2f %s/s\n",
            totalBytesRead,
            readVal,
            readUnit,
            totalBytesWritten,
            seconds,
            rateVal,
            rateUnit);
}

uint8_t printSyntax(char *arg)
{
    printf("\
\nUse: \
\n\n%s -i source -o destination [-b|-w|-i]\
\n-w,--verify-writes - verify when writes are made for extra assurance\
\n-i,--verify-integrity - verify that destination matches source\
\n-s,--source-device - source device\
\n-d,--destination-device - destination device.\
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
            {"verify-integrity", no_argument, 0, 'i'},
            {"verify-writes", no_argument, 0, 'w'},
            {"source-device", required_argument, 0, 's'},
            {"destination-device", required_argument, 0, 's'},
            {0, 0, 0, 0}};

        c = getopt_long(argc, argv, "hiws:d:",
                        long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {

        case 'h':
            printSyntax(binName);
            exit(EXIT_FAILURE);
            break;
        case 'i':
            st->optSt.verifyIntegrity = true;
            break;
        case 'w':
            st->optSt.verifyWrites = true;
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
