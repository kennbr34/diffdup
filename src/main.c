#include "lib.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <signal.h>

int main(int argc, char *argv[])
{
    if (argc == 1) {
        printSyntax(argv[0]);
        exit(EXIT_FAILURE);
    }
    
    installSignalHandlers();
    
    struct dataStruct st = {0};

    parseOptions(argc, argv, &st);
    
    int sourceDevice = open(st.deviceNameSt.sourceDeviceName, O_RDONLY | O_CLOEXEC);
	if (sourceDevice < 0) {
	    PRINT_DEVICE_ERROR(st.deviceNameSt.sourceDeviceName, errno);
	    exit(EXIT_FAILURE);
	}
	
	int destinationDevice = open(st.deviceNameSt.destinationDeviceName, O_RDWR | O_CLOEXEC);
	if (destinationDevice < 0) {
	    PRINT_DEVICE_ERROR(st.deviceNameSt.destinationDeviceName, errno);
	    close(sourceDevice);
	    exit(EXIT_FAILURE);
	}

    uint64_t sourceDeviceSize =
    getDeviceSize(st.deviceNameSt.sourceDeviceName);

	uint64_t destinationDeviceSize =
	    getDeviceSize(st.deviceNameSt.destinationDeviceName);
	
	if (sourceDeviceSize > destinationDeviceSize) {
	    PRINT_ERROR("Destination not large enough for source");
	    close(sourceDevice);
	    close(destinationDevice);
	    exit(EXIT_FAILURE);
	}
	
	uint64_t sourceLogicalBlockSize = 0;
	uint64_t destinationLogicalBlockSize = 0;
	
	if (ioctl(sourceDevice, BLKSSZGET, &sourceLogicalBlockSize) != 0) {
		PRINT_ERROR("Could not get source device's logical block size");
	    PRINT_SYS_ERROR(errno);
	    exit(EXIT_FAILURE);
	}
	
	if (ioctl(destinationDevice, BLKSSZGET, &destinationLogicalBlockSize) != 0) {
		PRINT_ERROR("Could not get destination device's logical block size");
	    PRINT_SYS_ERROR(errno);
	    exit(EXIT_FAILURE);
	}
	
	uint64_t requiredAlignment =
	    leastCommonDenominator(sourceLogicalBlockSize, destinationLogicalBlockSize);
	
	/* Enforce minimum alignment */
	
	if (st.cryptSt.dataBufSize < requiredAlignment) {
	    st.cryptSt.dataBufSize = requiredAlignment;
	} else {
	    uint64_t remainder =
	        st.cryptSt.dataBufSize % requiredAlignment;
	
	    if (remainder != 0) {
	        st.cryptSt.dataBufSize -= remainder;
	    }
	}
	
	if (st.cryptSt.dataBufSize == 0) {
	    PRINT_ERROR("dataBufSize resolved to zero after alignment");
	    exit(EXIT_FAILURE);
	}
    
	diffDup(sourceDevice,
        destinationDevice,
        sourceDeviceSize,
        &st);
	
	close(sourceDevice);
	close(destinationDevice);
	
    DDFREE(free, st.deviceNameSt.sourceDeviceName);

    DDFREE(free, st.deviceNameSt.destinationDeviceName);

    return EXIT_SUCCESS;
}
