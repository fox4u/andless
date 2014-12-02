#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tinyxml2.h"

#if 0
<?xml version="1.0" encoding="ISO-8859-1"?>
<cards>
<card name="msm8974">
    <init_playback ctl="SLIMBUS_0_RX Audio Mixer MultiMedia%d" val="1"/> 
    <exit_playback ctl="SLIMBUS_0_RX Audio Mixer MultiMedia%d" val="0"/> 
    <set_rate_44100 ctl="SLIM_0_RX SampleRate" val="KHZ_48"/>	
    <set_rate_48000 ctl="SLIM_0_RX SampleRate" val="KHZ_48"/>	
    <set_rate_96000 ctl="SLIM_0_RX SampleRate" val="KHZ_96"/>	
    <set_rate_192000 ctl="SLIM_0_RX SampleRate" val="KHZ_192"/>
    <set_format_SNDRV_PCM_FORMAT_S16_LE ctl="SLIM_0_RX Format" val="S16_LE"/>
    <set_format_SNDRV_PCM_FORMAT_S24_LE ctl="SLIM_0_RX Format" val="S24_LE"/>
</card>
<card name="E-MU 0404 USB">
    <set_rate_44100 ctl="Clock rate Selector" val="0"/>	
    <set_rate_48000 ctl="Clock rate Selector" val="1"/>	
    <set_rate_88200 ctl="Clock rate Selector" val="2"/>	
    <set_rate_96000 ctl="Clock rate Selector" val="3"/>	
    <set_rate_176400 ctl="Clock rate Selector" val="4"/>	
    <set_rate_192000 ctl="Clock rate Selector" val="5"/>	
</card>
</cards>
#endif

struct card_ptr {
    tinyxml2::XMLDocument *xml;
    tinyxml2::XMLElement *card;
    card_ptr(tinyxml2::XMLDocument *x, tinyxml2::XMLElement *c) {
	xml = x; card = c;
    }
};

extern "C" int xml_open_card(const char *xml_path, const char *card, void **result) 
{
    const char *c;
    tinyxml2::XMLElement *e = 0;
    tinyxml2::XMLDocument *doc = new tinyxml2::XMLDocument();	
	if(!doc || doc->LoadFile(xml_path) != 0) return -1;
	for(e = doc->FirstChildElement(); e; e = e->NextSiblingElement()) {
	    if(strcmp(e->Name(),"card") != 0) continue;
	    c = e->Attribute("name");
	    if(c && strcmp(c, card) == 0) {
	 	*result = (void *) new card_ptr(doc, e);
		return 0;
	    }
	} 
    return -1;
}

extern "C" void *xml_close_card(void *ptr)
{
    card_ptr *x = (card_ptr *) ptr;	
    if(x) {
	delete x->xml;
	delete x;
    }		
}

extern "C" int xml_get_rate_ctl(void *ptr, int rate, char **name, char **value) 
{
    tinyxml2::XMLDocument *doc = ((card_ptr *) ptr)->xml;
    tinyxml2::XMLElement *card = ((card_ptr *) ptr)->card;
    tinyxml2::XMLElement *e = 0;
    char tmp[32];
    const char *ctl, *val;

	if(!doc || !card) return -1;
	sprintf(tmp, "set_rate_%d", rate);
	for(e = card->FirstChildElement(); e; e = e->NextSiblingElement()) {
	    if(strcmp(e->Name(), tmp) == 0) {
		ctl = e->Attribute("ctl");
		val = e->Attribute("val");
		if(ctl && val) {
		    *name = (char *) ctl;
		    *value = (char *) val;
		    return 0;	
		}
		return -1;
	    }
	}
    return -1;	
}

extern "C" int xml_get_fmt_ctl(void *ptr, const char *format, char **name, char **value) 
{
    tinyxml2::XMLDocument *doc = ((card_ptr *) ptr)->xml;
    tinyxml2::XMLElement *card = ((card_ptr *) ptr)->card;
    tinyxml2::XMLElement *e = 0;
    char tmp[128];
    const char *ctl, *val;

	if(!doc || !card) return -1;
	sprintf(tmp, "set_format_%s", format);
	for(e = card->FirstChildElement(); e; e = e->NextSiblingElement()) {
	    if(strcmp(e->Name(), tmp) == 0) {
		ctl = e->Attribute("ctl");
		val = e->Attribute("val");
		if(ctl && val) {
		    *name = (char *) ctl;
		    *value = (char *) val;
		    return 0;	
		}
		return -1;
	    }
	}
    return -1;	
}


void recurse_control(tinyxml2::XMLElement *e, const char *name, char **names, char **values)
{
	

}

int find_headphones(void *ptr, char ***names, char ***values)
{
     **names = 0;
     **values = 0;	

}


#if 0
int main(int argc, char **argv) 
{
    tinyxml2::XMLElement *e1 = 0, *e2 = 0, *e3 = 0;
    const char *c, *name, *value;

	if(argc != 2) return printf("bad cmd line\n");

	tinyxml2::XMLDocument *doc = new tinyxml2::XMLDocument();

	if(doc->LoadFile(argv[1]) != 0) return printf("error: %s\n", doc->GetErrorStr1());

	e1 = doc->FirstChildElement();
	if(!e1 || strcmp(e1->Name(), "mixer") != 0) return printf("not a mixer");

#if 0
	for(e2 = e1->FirstChildElement(); e2; e2 = e2->NextSiblingElement()) {
	    if(strcmp(e2->Name(),"path") != 0) continue;
	    c = e2->Attribute("name");
	    if(!c || strcmp(c, "headphones") != 0) continue;
	    for(e3 = e2->FirstChildElement(); e3; e3 = e3->NextSiblingElement()) {
		if(strcmp(e3->Name(),"ctl") != 0) continue;
		name = e3->Attribute("name");
		value = e3->Attribute("value");
		if(name && value) printf("\"%s\" -> \"%s\"\n", name, value);
	    }	
	} 
	delete doc;
#endif
}

#endif


