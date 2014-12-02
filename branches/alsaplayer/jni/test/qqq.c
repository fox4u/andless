#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>

int main() {
    off_t flen;
    int fd;
    char *f = "/storage/sdcard1/muzon/Russian/DDT/1992/ДДТ - Актриса Весна, 1992.flac";
    void *mm;

	fd = open(f, O_RDONLY);
	if(fd < 0) return printf("failed to open\n");
	flen = lseek(fd, 0, SEEK_END);
	if(flen < 0) return printf("lseek failed\n");
	mm = mmap(0, flen, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if(mm == MAP_FAILED) return printf("failed to mmap\n");
	printf("okay, %08X\n", mm);

}


