#include <stdio.h>

int main() {
    int z = 0x12345678;
    char *p = (char *) &z;
	*((unsigned short *) p) = 0xf9f8;
	printf("%p %p %08x\n", &z, p, z);

}


