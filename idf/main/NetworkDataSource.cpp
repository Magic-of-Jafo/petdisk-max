#include "NetworkDataSource.h"
#include "hardware.h"
#include "Settings.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>

#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "HTTPClient.h"

#define TAG "http"

namespace bitfixer {

bool NetworkDataSource::init()
{
    return true;
}

bool NetworkDataSource::isInitialized()
{
    return true;
}

void NetworkDataSource::clearState()
{
    _path[0] = 0;
    _readName[0] = 0;
    _writeName[0] = 0;
    _receiveUrl[0] = 0;
    _params[0] = 0;
    _dirPage[0] = 0;
}

void NetworkDataSource::copyUrlEscapedString(char* dest, int destSize, const char* src)
{
    char* destptr = dest;
    char* destEnd = dest + destSize - 1;

    for (const char* p = src; *p != 0; p++)
    {
        // unsigned, so PET graphics characters (>= 0x80) escape as %XX
        // instead of a sign-extended %FFFFFFXX
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            if (destptr + 1 > destEnd)
            {
                break;
            }
            *destptr++ = c;
        }
        else
        {
            if (destptr + 3 > destEnd)
            {
                break;
            }
            sprintf(destptr, "%%%02X", c);
            destptr += 3;
        }
    }
    *destptr = 0x00;
}

// Escape "<current folder>/<fileName>" into dest (NDS_ESCAPED_MAX bytes).
bool NetworkDataSource::escapeWithPath(char* dest, const char* fileName)
{
    char raw[NDS_PATH_MAX + 64];
    int n = snprintf(raw, sizeof(raw), _path[0] ? "%s/%s" : "%s%s", _path, fileName);
    if (n < 0 || n >= (int)sizeof(raw))
    {
        dest[0] = 0;
        return false;
    }
    copyUrlEscapedString(dest, NDS_ESCAPED_MAX, raw);
    return true;
}

void NetworkDataSource::openFileForWriting(unsigned char* fileName)
{
    // open file for writing
    // blocks of data will be sent to the server.
    // no need to make a request here
    _firstBlockWritten = false;
    escapeWithPath(_writeName, (const char*)fileName);
    _blockData = _writeBlock;

    _readBufferSize = 512;
    _writeBufferSize = 512;
}

bool NetworkDataSource::openFileForReading(unsigned char* fileName)
{
    escapeWithPath(_readName, (const char*)fileName);

    int receiveUrlLength = _settings.getUrl(_receiveUrl);
    snprintf(&_receiveUrl[receiveUrlLength], sizeof(_receiveUrl) - receiveUrlLength, "?file=%s", _readName);

    urlInfo* info = (urlInfo*)_dataBuffer;
    _settings.getHost(info->host);
    int port = _settings.getPort();

    _fileSize = _http->getSize((const char*)info->host, port, _receiveUrl, _dataBuffer, _dataBufferSize);

    _currentBlockByte = 0;
    _currentOutputByte = 0;
    _blockData = 0;

    _readBufferSize = 512;
    _writeBufferSize = 512;

    if (_fileSize <= 0)
    {
        return false;
    }

    // get first block
    int bytesToFetch = _readBufferSize;
    if (bytesToFetch > (int)_fileSize) {
        bytesToFetch = _fileSize;
    }

    if (!fetchBlock(0, bytesToFetch))
    {
        return false;
    }
    _currentBlockByte += bytesToFetch;

    return true;
}

bool NetworkDataSource::openDirectory(const char* dirName)
{
    if (dirName == NULL || dirName[0] == 0)
    {
        return true;
    }

    // go up one folder
    if (strcmp(dirName, "..") == 0)
    {
        char* slash = strrchr(_path, '/');
        if (slash != NULL)
        {
            *slash = 0;
        }
        else
        {
            _path[0] = 0;
        }
        return true;
    }

    // a leading slash starts from the top folder
    const char* name = dirName;
    char base[NDS_PATH_MAX];
    strcpy(base, _path);
    if (name[0] == '/')
    {
        base[0] = 0;
        while (name[0] == '/')
        {
            name++;
        }
        if (name[0] == 0)
        {
            _path[0] = 0;
            return true;
        }
    }

    char newPath[NDS_PATH_MAX];
    int n = snprintf(newPath, sizeof(newPath), base[0] ? "%s/%s" : "%s%s", base, name);
    if (n < 0 || n >= (int)sizeof(newPath))
    {
        return false;
    }
    // drop trailing slashes
    while (n > 0 && newPath[n-1] == '/')
    {
        newPath[--n] = 0;
    }

    // check the folder exists: the server answers 404 for a missing folder
    // (servers without folder support list the top folder instead)
    urlInfo* info = (urlInfo*)_dataBuffer;
    _settings.getHost(info->host);
    _settings.getUrl(info->url);
    int port = _settings.getPort();

    char escapedPath[NDS_ESCAPED_MAX];
    copyUrlEscapedString(escapedPath, sizeof(escapedPath), newPath);
    snprintf(_params, sizeof(_params), "?d=1&p=0&dir=%s", escapedPath);

    int size = 0;
    uint8_t* listing = _http->makeRequest(
        (const char*)info->host,
        port,
        (const char*)info->url,
        (const char*)_params,
        _dataBuffer,
        _dataBufferSize,
        &size);
    if (listing == NULL)
    {
        log_e("no folder %s", newPath);
        return false;
    }

    strcpy(_path, newPath);
    _dirPtr = 0;
    _currentDirectoryPage = 0;
    return true;
}

uint16_t NetworkDataSource::requestReadBufferSize(uint16_t requestedReadBufferSize)
{
    if (requestedReadBufferSize > 512)
    {
        requestedReadBufferSize = 512;
    }
    _readBufferSize = requestedReadBufferSize;
    return _readBufferSize;
}

uint16_t NetworkDataSource::requestWriteBufferSize(uint16_t requestedWriteBufferSize)
{
    if (requestedWriteBufferSize > 512)
    {
        requestedWriteBufferSize = 512;
    }
    _writeBufferSize = requestedWriteBufferSize;
    return _writeBufferSize;
}

// Fetch bytes start..end-1 into _readBlock. The server sends at most up to the
// end of the file. Returns false if the request failed or came back short.
bool NetworkDataSource::fetchBlock(uint32_t start, uint32_t end)
{
    int rangeSize = 0;

    urlInfo* info = (urlInfo*)_dataBuffer;
    int hostLength = _settings.getHost(info->host);
    int port = _settings.getPort();
    info->host[hostLength] = 0;

    uint8_t* data = _http->getRange(
        (const char*)info->host,
        port,
        (const char*)_receiveUrl,
        start,
        end,
        _dataBuffer,
        _dataBufferSize,
        &rangeSize);

    uint32_t available = (end > _fileSize) ? _fileSize : end;
    uint32_t expected = (available > start) ? available - start : 0;

    if (data == NULL || rangeSize < (int)expected)
    {
        log_e("short read %d of %" PRIu32 " bytes at %" PRIu32, rangeSize, expected, start);
        _blockData = 0;
        return false;
    }

    // keep our own copy: the response buffer is reused by the next request,
    // from this drive or any other
    uint32_t want = end - start;
    if (want > NDS_BLOCK_MAX)
    {
        want = NDS_BLOCK_MAX;
    }
    uint32_t got = ((uint32_t)rangeSize < want) ? (uint32_t)rangeSize : want;
    memcpy(_readBlock, data, got);
    if (got < want)
    {
        memset(&_readBlock[got], 0, want - got);
    }
    _blockData = _readBlock;
    return true;
}

uint32_t NetworkDataSource::seek(uint32_t pos)
{
    // round to closest block
    uint32_t q_pos = (pos / (uint32_t)readBufferSize()) * (uint32_t)readBufferSize();
    _currentOutputByte = q_pos;
    _currentBlockByte = 0;
    return _currentOutputByte;
}

uint16_t NetworkDataSource::getNextFileBlock()
{
    if (_currentOutputByte < _currentBlockByte)
    {
        uint16_t bytes = _currentBlockByte - _currentOutputByte;
        _currentOutputByte = _currentBlockByte;
        return bytes;
    }

    // retrieve range from server
    uint32_t start = _currentOutputByte;
    uint32_t end = start + _readBufferSize;

    // TODO: remove +1 ?
    if (end > _fileSize + 1)
    {
        end = _fileSize + 1;
    }

    if (fetchBlock(start, end))
    {
        _currentBlockByte = end;
        _currentOutputByte = _currentBlockByte;
        return end-start;
    }

    return 0;
}

bool NetworkDataSource::isLastBlock()
{
    if (_currentOutputByte >= _fileSize)
    {
        return true;
    }

    return false;
}

bool NetworkDataSource::getNextDirectoryEntry()
{
    // if there is no directory information
    // or if we have reached the end of this page,
    // fetch the next page from the server.
    if (_dirPtr == 0 || _dirPtr[0] == '\n')
    {
        // prepare address
        urlInfo* info = (urlInfo*)_dataBuffer;
        _settings.getHost(info->host);
        _settings.getUrl(info->url);
        int port = _settings.getPort();

        // sub=1 asks for folders too (as "NAME/"); servers without folder
        // support ignore it and the dir parameter
        if (_path[0])
        {
            char escapedPath[NDS_ESCAPED_MAX];
            copyUrlEscapedString(escapedPath, sizeof(escapedPath), _path);
            snprintf(_params, sizeof(_params), "?d=1&p=%d&sub=1&dir=%s", _currentDirectoryPage++, escapedPath);
        }
        else
        {
            snprintf(_params, sizeof(_params), "?d=1&p=%d&sub=1", _currentDirectoryPage++);
        }

        int size = 0;
        uint8_t* page = _http->makeRequest(
            (const char*)info->host,
            port,
            (const char*)info->url,
            (const char*)_params,
            _dataBuffer,
            _dataBufferSize,
            &size);

        if (page == NULL || size <= 0)
        {
            // no directory
            _dirPtr = 0;
            return false;
        }

        // copy the page: the response buffer is reused by the next request
        if (size > NDS_DIRPAGE_MAX)
        {
            size = NDS_DIRPAGE_MAX;
        }
        memcpy(_dirPage, page, size);
        _dirPage[size] = 0;
        _dirPtr = _dirPage;
    }

    if (_dirPtr[0] == '\n' || _dirPtr[0] == 0)
    {
        // if the first character is newline, this is the end of the directory.
        _dirPtr = 0;
        return false;
    }

    return true;
}

void NetworkDataSource::writeBufferToFile(uint16_t numBytes)
{
    // data is in _writeBlock, copy it to where postBlock encodes it from
    urlInfo* info = (urlInfo*)_dataBuffer;
    if (numBytes > sizeof(info->blockData))
    {
        numBytes = sizeof(info->blockData);
    }
    memcpy(info->blockData, _writeBlock, numBytes);

    // specify url parameters
    _settings.getHost(info->host);
    _settings.getUrl(info->url);
    int port = _settings.getPort();

    bool first = !_firstBlockWritten;
    snprintf(_params, sizeof(_params), first ? "?f=%s&n=1" : "?f=%s", _writeName);
    _firstBlockWritten = true;

    bool ok = _http->postBlock(
        info->host,
        port,
        info->url,
        _params,
        _dataBuffer,
        _dataBufferSize,
        numBytes);

    // the first block replaces the file, so it is safe to send again;
    // appends are not retried, a lost reply could duplicate data
    if (!ok && first)
    {
        log_e("retrying first block of %s", _writeName);
        ok = _http->postBlock(info->host, port, info->url, _params, _dataBuffer, _dataBufferSize, numBytes);
    }
    if (!ok)
    {
        log_e("network write failed for %s", _writeName);
    }
}

void NetworkDataSource::updateBlock()
{
    // write specific block to file
    // data is the block last read, modified in place
    urlInfo* info = (urlInfo*)_dataBuffer;
    uint16_t numBytes = _writeBufferSize;
    if (numBytes > sizeof(info->blockData))
    {
        numBytes = sizeof(info->blockData);
    }
    if (_blockData == 0)
    {
        log_e("updateBlock without data");
        return;
    }
    memmove(info->blockData, _blockData, numBytes);

    // specify url parameters
    _settings.getHost(info->host);
    _settings.getUrl(info->url);
    int port = _settings.getPort();

    uint32_t endByte = _currentOutputByte + numBytes;
    snprintf(_params, sizeof(_params), "?f=%s&u=1&s=%" PRIu32 "&e=%" PRIu32, _readName, _currentOutputByte, endByte);

    if (!_http->postBlock(
        info->host,
        port,
        info->url,
        _params,
        _dataBuffer,
        _dataBufferSize,
        numBytes))
    {
        log_e("network block update failed for %s", _readName);
    }
}

void NetworkDataSource::closeFile()
{
    // close the file,
    // no need for additional request
}

void NetworkDataSource::openCurrentDirectory()
{
    _dirPtr = 0;
    _currentDirectoryPage = 0;
}

unsigned char* NetworkDataSource::getFilename()
{
    uint8_t* thisFilename = _dirPtr;
    // find end of this filename (stop at the end of the page too)
    while (*_dirPtr != '\n' && *_dirPtr != 0)
    {
        _dirPtr++;
    }

    // set null terminator and advance to next filename
    bool endOfPage = (*_dirPtr == 0);
    _dirPtr[0] = 0;
    if (!endOfPage)
    {
        _dirPtr++;
    }

    // folders are listed as "NAME/"
    int len = strlen((const char*)thisFilename);
    _entryIsDirectory = (len > 0 && thisFilename[len-1] == '/');
    if (_entryIsDirectory)
    {
        thisFilename[len-1] = 0;
    }

    return thisFilename;
}

unsigned char* NetworkDataSource::getBuffer()
{
    return _blockData;
}

bool NetworkDataSource::getCurrentDateTime(int* year, int* month, int* day, int* hour, int* minute, int* second)
{
    // Ask for the time directly. This used to go through openFileForReading,
    // which replaced the state of any file open for reading on this drive.
    urlInfo* info = (urlInfo*)_dataBuffer;
    _settings.getHost(info->host);
    _settings.getUrl(info->url);
    int port = _settings.getPort();

    int size = 0;
    uint8_t* data = _http->makeRequest(
        (const char*)info->host,
        port,
        (const char*)info->url,
        "?file=TIME&s=0&e=20",
        _dataBuffer,
        _dataBufferSize,
        &size);
    if (data == NULL || size < 19)
    {
        return false;
    }

    char tmp[32];
    int n = size < (int)sizeof(tmp) - 1 ? size : (int)sizeof(tmp) - 1;
    memcpy(tmp, data, n);
    tmp[n] = 0;
    return sscanf(tmp, "%d-%d-%d %d:%d:%d", year, month, day, hour, minute, second) == 6;
}

}
