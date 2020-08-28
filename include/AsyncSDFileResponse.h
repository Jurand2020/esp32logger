// Serve files using SdFat 
// Modified from https://gist.github.com/pim-borst
/**
 * \file
 * \brief AsyncSDFileResponse class
 */

#ifndef __AsyncSDFileResponse__
#define __AsyncSDFileResponse__

#include <Arduino.h>

#include <SPI.h>
#include "SdFat.h"
#define FS_NO_GLOBALS

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

//==============================================================================
/**
 * \class AsyncSDFileResponse
 * \brief SdFat file response for ESPAsyncWebServer
 */
class AsyncSDFileResponse: public AsyncAbstractResponse {
  private:
    File _content;
    String _path;
    void _setContentType(const String& path);
    bool _sourceIsValid;
    bool SD_exists(SdFat &sd, String path);
  public:
    AsyncSDFileResponse(SdFat &sd, const String& path, const String& contentType=String(), bool download=false);
    AsyncSDFileResponse(File content, const String& path, const String& contentType=String(), bool download=false);
    ~AsyncSDFileResponse();
    bool _sourceValid() const { return _sourceIsValid; } 
    virtual size_t _fillBuffer(uint8_t *buf, size_t maxLen) override;
};

#endif