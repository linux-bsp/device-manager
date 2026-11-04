#ifndef DM_TIME_SOURCE_H
#define DM_TIME_SOURCE_H
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* A source is an origin, never a command, credential or redirect URL. */
static inline int dm_source_parse(const char *source,char host[256],char port[6])
{
    const char *p;int ntp=0;
    if(!strncmp(source,"http://",7))p=source+7;
    else if(!strncmp(source,"https://",8))p=source+8;
    else if(!strncmp(source,"ntp://",6)){p=source+6;ntp=1;}
    else return -1;
    if(strlen(source)>511||!*p)return -1;
    const char *end=p,*service=NULL;size_t length;
    if(*p=='['){end=strchr(++p,']');if(!end)return -1;
        if(end[1]){if(end[1]!=':')return -1;service=end+2;}
    }else{while(*end&&*end!=':')end++;if(*end)service=end+1;}
    length=(size_t)(end-p);if(!length||length>255)return -1;
    for(size_t i=0;i<length;i++)if(!(isalnum((unsigned char)p[i])||p[i]=='.'||p[i]=='-'||p[i]==':'))return -1;
    if(*p=='-'||p[length-1]=='-')return -1;
    memcpy(host,p,length);host[length]=0;
    if(service){
        if(!*service||strlen(service)>5)return -1;
        for(const char *s=service;*s;s++)if(*s<'0'||*s>'9')return -1;
        unsigned long n=strtoul(service,NULL,10);if(!n||n>65535)return -1;
        snprintf(port,6,"%lu",n);
    }else strcpy(port,ntp?"123":!strncmp(source,"https://",8)?"443":"80");
    return ntp;
}
static inline int dm_source_valid(const char *source)
{char host[256],port[6];return !strcmp(source,"-")||dm_source_parse(source,host,port)>=0;}
#endif
