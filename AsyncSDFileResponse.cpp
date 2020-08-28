// Serve files using SdFat 
// Modified from https://gist.github.com/pim-borst

#include <Arduino.h>

#include <SPI.h>
#include "SdFat.h"
#define FS_NO_GLOBALS

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>


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

AsyncSDFileResponse::~AsyncSDFileResponse(){
  if(_content)
    _content.close();
}

void AsyncSDFileResponse::_setContentType(const String& path){
  if (path.endsWith(".html")) _contentType = "text/html";
  else if (path.endsWith(".htm")) _contentType = "text/html";
  else if (path.endsWith(".css")) _contentType = "text/css";
  else if (path.endsWith(".csv")) _contentType = "text/csv";
  else if (path.endsWith(".json")) _contentType = "text/json";
  else if (path.endsWith(".js")) _contentType = "application/javascript";
  else if (path.endsWith(".png")) _contentType = "image/png";
  else if (path.endsWith(".gif")) _contentType = "image/gif";
  else if (path.endsWith(".jpg")) _contentType = "image/jpeg";
  else if (path.endsWith(".ico")) _contentType = "image/x-icon";
  else if (path.endsWith(".svg")) _contentType = "image/svg+xml";
  else if (path.endsWith(".eot")) _contentType = "font/eot";
  else if (path.endsWith(".woff")) _contentType = "font/woff";
  else if (path.endsWith(".woff2")) _contentType = "font/woff2";
  else if (path.endsWith(".ttf")) _contentType = "font/ttf";
  else if (path.endsWith(".txt")) _contentType = "text/plain";
  else if (path.endsWith(".xml")) _contentType = "text/xml";
  else if (path.endsWith(".pdf")) _contentType = "application/pdf";
  else if (path.endsWith(".zip")) _contentType = "application/zip";
  else if(path.endsWith(".gz")) _contentType = "application/x-gzip";
  else _contentType = "application/octet-stream";
}

bool AsyncSDFileResponse::SD_exists(SdFat &sd, String path) {
  // For some reason SD.exists(filename) reboots the ESP...
  // So we test by opening the file
  bool exists = false;
  File test = sd.open(path);
  if(test){
    test.close();
    exists = true;
  }
  return exists;
}

AsyncSDFileResponse::AsyncSDFileResponse(SdFat &sd, const String& path, const String& contentType, bool download){
  _code = 200;
  _path = path;
  
  if(!download && !SD_exists(sd, _path) && SD_exists(sd, _path+".gz")){
    _path = _path+".gz";
    addHeader("Content-Encoding", "gzip");
  }

  _content = sd.open(_path, O_READ);
  _contentLength = _content.size();
  _sourceIsValid = _content;

  if(contentType == "")
    _setContentType(path);
  else
    _contentType = contentType;

  int filenameStart = path.lastIndexOf('/') + 1;
  char buf[26+path.length()-filenameStart];
  char* filename = (char*)path.c_str() + filenameStart;

  if(download) {
    // set filename and force download
    snprintf(buf, sizeof (buf), "attachment; filename=\"%s\"", filename);
  } else {
    // set filename and force rendering
    snprintf(buf, sizeof (buf), "inline; filename=\"%s\"", filename);
  }
  addHeader("Content-Disposition", buf);

}


AsyncSDFileResponse::AsyncSDFileResponse(File content, const String& path, const String& contentType, bool download){
  _code = 200;
  _path = path;
  _content = content;
  _contentLength = _content.size();

  if(!download && String(_content.name()).endsWith(".gz") && !path.endsWith(".gz"))
    addHeader("Content-Encoding", "gzip");

  if(contentType == "")
    _setContentType(path);
  else
    _contentType = contentType;

  int filenameStart = path.lastIndexOf('/') + 1;
  char buf[26+path.length()-filenameStart];
  char* filename = (char*)path.c_str() + filenameStart;

  if(download) {
    snprintf(buf, sizeof (buf), "attachment; filename=\"%s\"", filename);
  } else {
    snprintf(buf, sizeof (buf), "inline; filename=\"%s\"", filename);
  }
  addHeader("Content-Disposition", buf);
}

size_t AsyncSDFileResponse::_fillBuffer(uint8_t *data, size_t len){
  _content.read(data, len);
  return len;
}
