/************************************************************************
 * Shared libcurl process initialization for EDK domain libraries
 ************************************************************************/

#include "dm_http.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>

static pthread_once_t dm_http_once = PTHREAD_ONCE_INIT;
static int dm_http_init_ret;

static void dm_http_init_once(void)
{
	dm_http_init_ret = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ?
		0 : -EIO;
}

int dm_http_global_init(void)
{
	int ret = pthread_once(&dm_http_once, dm_http_init_once);

	return ret ? -ret : dm_http_init_ret;
}
