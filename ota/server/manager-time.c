/* Small read-only clock service over the existing management connection. */
#define _POSIX_C_SOURCE 200809L
#include "manager-time.h"
#include "clock-control.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

bool manager_time_route(int client, const struct server_context *context, const struct http_request *request)
{
    if(clock_control_route(client,context,request))return true;
    const char *prefix="/api/time/";
    if(strncmp(request->path,prefix,strlen(prefix)))return false;
    if(strcmp(request->method,"GET")&&!request->head){
        http_text(client,405,"Method Not Allowed","Use GET\n",request->head);return true;
    }
    const char *nonce=request->path+strlen(prefix);
    if(strlen(nonce)!=32||request->length){
        http_text(client,400,"Bad Request","Invalid time request\n",request->head);return true;
    }
    for(int i=0;i<32;i++)if(!((nonce[i]>='0'&&nonce[i]<='9')||(nonce[i]>='a'&&nonce[i]<='f'))){
        http_text(client,400,"Bad Request","Invalid time nonce\n",request->head);return true;
    }
    struct timespec now;
    if(clock_gettime(CLOCK_REALTIME,&now)||now.tv_sec<1577836800LL||now.tv_sec>=4102444800LL){
        http_text(client,503,"Unavailable","Server clock unavailable\n",request->head);return true;
    }
    char body[128];
    snprintf(body,sizeof(body),"DMTIME1 %s %lld %ld\n",nonce,(long long)now.tv_sec,now.tv_nsec);
    http_text(client,200,"OK",body,request->head);return true;
}
