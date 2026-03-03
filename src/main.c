#include "lib.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

int main(int argc, char *argv[])
{
    if (argc == 1) {
        printSyntax(argv[0]);
        exit(EXIT_FAILURE);
    }

    struct dataStruct st = {0};

    st.cryptSt.dataBufSize = DEFAULT_BUFF_SIZE;

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
