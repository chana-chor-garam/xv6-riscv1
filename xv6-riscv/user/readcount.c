#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

int
main(int argc, char *argv[])
{
  uint64 initial_count;
  uint64 final_count;
  int fd;
  char buf[100];
  int bytes_read;

  // Create a file to read from
  fd = open("testfile", O_CREATE | O_WRONLY);
  if (fd < 0) {
    fprintf(2, "readcount: cannot open testfile for writing\n");
    exit(1);
  }
  // Write some data to ensure the file has at least 100 bytes
  write(fd, "This is a test file with some content to verify the readcount system call.", 75);
  close(fd);

  // Open the file for reading
  fd = open("testfile", O_RDONLY);
  if (fd < 0) {
    fprintf(2, "readcount: cannot open testfile for reading\n");
    exit(1);
  }

  // Get the initial read count before reading from the file.
  initial_count = getreadcount();
  fprintf(1, "Initial read count: %lu\n", initial_count);

  // Read 100 bytes from the file.
  bytes_read = read(fd, buf, 100);
  if (bytes_read < 0) {
    fprintf(2, "readcount: read failed\n");
    close(fd);
    exit(1);
  }

  // Get the final read count after the read operation.
  final_count = getreadcount();
  fprintf(1, "Read %d bytes.\n", bytes_read);
  fprintf(1, "Final read count: %lu\n", final_count);

  // Verify that the read count increased by the number of bytes read.
  if (final_count == initial_count + bytes_read) {
    fprintf(1, "Success: The read count increased as expected.\n");
  } else {
    fprintf(1, "Failure: The read count did not increase correctly.\n");
  }

  close(fd);
  unlink("testfile"); // Clean up the created file

  exit(0);
}
