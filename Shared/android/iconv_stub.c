/* Stub libiconv for Android (bionic has no iconv and the vcpkg arm64-android
 * libiconv build is broken). Conversions report failure; the game logs and
 * falls back. Provides both iconv_* and libiconv_* symbol names. */
#include <stddef.h>
#include <errno.h>

typedef void* iconv_t;

iconv_t iconv_open(const char* to_code, const char* from_code)
{
	(void)to_code;
	(void)from_code;
	return (iconv_t)-1;
}

size_t iconv(iconv_t cd, char** inbuf, size_t* inbytesleft, char** outbuf, size_t* outbytesleft)
{
	(void)cd;
	(void)inbuf;
	(void)inbytesleft;
	(void)outbuf;
	(void)outbytesleft;
	errno = EILSEQ;
	return (size_t)-1;
}

int iconv_close(iconv_t cd)
{
	(void)cd;
	return 0;
}

iconv_t libiconv_open(const char* to_code, const char* from_code)
{
	return iconv_open(to_code, from_code);
}

size_t libiconv(iconv_t cd, char** inbuf, size_t* inbytesleft, char** outbuf, size_t* outbytesleft)
{
	return iconv(cd, inbuf, inbytesleft, outbuf, outbytesleft);
}

int libiconv_close(iconv_t cd)
{
	return iconv_close(cd);
}
