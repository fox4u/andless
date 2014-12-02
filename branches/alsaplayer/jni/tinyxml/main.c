#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int xml_open_card(const char *xml_path, const char *card, void **result);
extern void *xml_close_card(void *ptr);
extern int xml_get_rate_ctl(void *ptr, int rate, char **name, char **value);
extern int xml_get_fmt_ctl(void *ptr, const char *format, char **name, char **value); 

int main(int argc, char **argv) 
{
    void *result;
    char *ctl, *val;

	if(argc != 3) return printf("bad cmd line\n");

 	if(xml_open_card(argv[1], argv[2], &result) != 0) return printf("cannot open card\n");

	if(xml_get_rate_ctl(result, 44100, &ctl, &val) == 0) printf("rate: %s -> %s\n", ctl, val);
	else printf("control for 44100 not found\n");

	if(xml_get_fmt_ctl(result, "SNDRV_PCM_FORMAT_S24_LE", &ctl, &val) == 0) printf("format: %s -> %s\n", ctl, val);
	else printf("control for SNDRV_PCM_FORMAT_S24_LE not found\n");

	xml_close_card(result);
	printf("done\n");
    return 0;
}



