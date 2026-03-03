# diffdup
A device-level incremental data duplicator

Imagine you wanted to mirror your encrypted-thumbdrive to another one, but you didn't want to duplicate the entirety of the data every time you did so. You could use various tools like rsync, rclone, etc. but they all work at the filesystem level, and what if you don't want to enter your key and decrypt the drives in order to use filesystem level utilities to do an incremental backup?

This program simply reads ever n bytes from both the source and destination device and compares them, and then only writes the bytes from the source to the destination if the chunk differs. It still requires reading the entirety of the source drive, but can speed things up a bit and prevent as much wear to the backup device, while providing incremental backup of opaquely encrypted data.
