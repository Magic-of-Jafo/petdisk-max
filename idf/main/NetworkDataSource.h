#ifndef __network_data_source_h__
#define __network_data_source_h__

#include "DataSource.h"
#include "EspHttp.h"
#include "Settings.h"

namespace bitfixer {

// Sizes for per-drive state. Escaped names can be 3x the raw length.
#define NDS_PATH_MAX     128    // current folder, e.g. "GAMES/ARCADE"
#define NDS_ESCAPED_MAX  460    // url-escaped folder + file name
#define NDS_URL_MAX      540    // script url + query string
#define NDS_BLOCK_MAX    520    // one read or write block (512) plus slack
#define NDS_DIRPAGE_MAX  1024   // one directory listing page (512 from the server)

class NetworkDataSource : public DataSource
{
public:
    NetworkDataSource()
    : _http(NULL)
    , _fileSize(0)
    , _currentBlockByte(0)
    , _currentOutputByte(0)
    , _currentDirectoryPage(0)
    , _blockData(0)
    , _dataBuffer(NULL)
    , _dataBufferSize(NULL)
    , _dirPtr(0)
    , _entryIsDirectory(false)
    , _firstBlockWritten(false)
    , _readBufferSize(512)
    , _writeBufferSize(512)
    {
        clearState();
    }

    NetworkDataSource(EspHttp* http, uint8_t* buffer, uint16_t* bufferSize)
    : NetworkDataSource()
    {
        initWithParams(http, buffer, bufferSize);
    }

    ~NetworkDataSource() {}

    void initWithParams(EspHttp* http, uint8_t* buffer, uint16_t* bufferSize)
    {
        _http = http;
        _dataBuffer = buffer;
        _dataBufferSize = bufferSize;
    }

    void setUrlData(void* eepromHost, int eepromHostLength, int port, void* eepromUrl, int eepromUrlLength)
    {
        _settings.initWithParams(eepromHost, eepromHostLength, port, eepromUrl, eepromUrlLength);
    }

    bool init();
    bool isInitialized();
    void openFileForWriting(unsigned char* fileName);
    bool openFileForReading(unsigned char* fileName);
    bool openDirectory(const char* dirName);
    uint16_t getNextFileBlock();
    bool isLastBlock();
    bool getNextDirectoryEntry();

    void writeBufferToFile(uint16_t numBytes);
    void updateBlock();
    void closeFile();
    void openCurrentDirectory();
    bool isDirectory() { return _entryIsDirectory; }
    unsigned char* getFilename();
    unsigned char* getBuffer();

    bool getCurrentDateTime(int* year, int* month, int* day, int* hour, int* minute, int* second);

    uint32_t seek(uint32_t pos);

    uint16_t readBufferSize()
    {
        return _readBufferSize;
    }

    uint16_t writeBufferSize()
    {
        return _writeBufferSize;
    }

    uint16_t requestReadBufferSize(uint16_t requestedReadBufferSize);
    uint16_t requestWriteBufferSize(uint16_t requestedWriteBufferSize);

    Settings _settings;
private:
    EspHttp* _http;
    uint32_t _fileSize;
    uint32_t _currentBlockByte;
    uint32_t _currentOutputByte;
    int _currentDirectoryPage;
    uint8_t* _blockData;
    uint8_t* _dataBuffer;
    uint16_t* _dataBufferSize;
    uint8_t* _dirPtr;
    bool _entryIsDirectory;
    bool _firstBlockWritten;

    uint16_t _readBufferSize;
    uint16_t _writeBufferSize;

    // Per-drive state. This used to live inside the 1K buffer shared by all
    // network drives (and HTTP responses), so drives overwrote each other.
    char _path[NDS_PATH_MAX];                // current folder, "" = top
    char _readName[NDS_ESCAPED_MAX];         // escaped name of the file being read
    char _writeName[NDS_ESCAPED_MAX];        // escaped name of the file being written
    char _receiveUrl[NDS_URL_MAX];           // "<url>?file=<readName>"
    char _params[NDS_URL_MAX];               // query string scratch
    uint8_t _readBlock[NDS_BLOCK_MAX];       // last block read
    uint8_t _writeBlock[NDS_BLOCK_MAX];      // block being filled for writing
    uint8_t _dirPage[NDS_DIRPAGE_MAX + 1];   // current directory page

    void clearState();
    bool fetchBlock(uint32_t start, uint32_t end);
    bool escapeWithPath(char* dest, const char* fileName);
    static void copyUrlEscapedString(char* dest, int destSize, const char* src);
};

}

#endif
