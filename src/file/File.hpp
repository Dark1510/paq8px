#pragma once

#include "../Array.hpp"
#include "../String.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#ifndef PAQ8PX_FILEUTILS_HPP
#define PAQ8PX_FILEUTILS_HPP

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include "../String.hpp"
#include "../SystemDefines.hpp"

//////////////////// IO functions and classes ///////////////////
// Wrappers to utf8 vs. wchar functions
// Linux i/o works with utf8 char* but on windows it's wchar_t*.
// We'll be using utf8 char* in all our functions, so we need to convert to/from
// wchar_t* when on windows.


// The preferred slash for displaying
// We will change the BADSLASH to GOODSLASH before displaying a path string to the user
#ifdef WINDOWS
#define BADSLASH '/'
#define GOODSLASH '\\'
#else
#define BADSLASH '\\'
#define GOODSLASH '/'
#endif

//only for Windows: a class encapsulating a wchar string converted from a utf8 string
//purpose: to properly free the allocated string buffer on destruction
#ifdef WINDOWS
class WcharStr
{
  private:
  //convert a utf8 string to a wchar string
  wchar_t *utf8_to_wchar_str(const char *utf8_str)
  {
    int buffersize     = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, NULL, 0);
    wchar_t *wchar_str = new wchar_t[buffersize];
    MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, wchar_str, buffersize);
    return wchar_str;
  }

  public:
  wchar_t *wchar_str;
  WcharStr(const char *utf8_str)
  {
    wchar_str = utf8_to_wchar_str(utf8_str);
  }
  ~WcharStr()
  {
    delete[] wchar_str;
  }
};
#endif

//only for Windows: a class encapsulating a utf8 string converted from a wchar string
//purpose: to properly free the allocated string buffer on destruction
#ifdef WINDOWS
class Utf8Str
{
  private:
  //convert a wchar string to a utf8 string
  char *wchar_to_utf8_str(const wchar_t *wchar_str)
  {
    int buffersize = WideCharToMultiByte(CP_UTF8, 0, wchar_str, -1, NULL, 0, NULL, NULL);
    char *utf8_str = new char[buffersize];
    WideCharToMultiByte(CP_UTF8, 0, wchar_str, -1, utf8_str, buffersize, NULL, NULL);
    return utf8_str;
  }

  public:
  char *utf8_str;
  Utf8Str(const wchar_t *wchar_str)
  {
    utf8_str = wchar_to_utf8_str(wchar_str);
  }
  ~Utf8Str()
  {
    delete[] utf8_str;
  }
};
#endif

#ifdef WINDOWS
#define STAT __stat64
#else
#define STAT stat
#endif

static constexpr int READ = 0;
static constexpr int WRITE = 1;
static constexpr int APPEND = 2;

/**
 * Wrapper function (Linux vs Windows) to open a file
 * @param filename
 * @param mode
 * @return
 */
static FILE* openFile(const char *filename, const int mode)
{
  FILE *file = nullptr;
#ifdef WINDOWS
  file = _wfopen(WcharStr(filename).wchar_str, mode == READ ? L"rb" : mode == WRITE ? L"wb+" : L"a");
#else
  file = fopen(filename, mode == READ ? "rb" : mode == WRITE ? "wb+" : "a");
#endif
  return file;
}

/**
 * Wrapper function (Linux vs Windows) to examine a path
 * @param path
 * @param status
 * @return
 */
static bool statPath(const char *path, struct STAT &status)
{
#ifdef WINDOWS
  return _wstat64(WcharStr(path).wchar_str, &status);
#else
  return stat(path, &status) != 0;
#endif
}

/**
 * examines given "path" and returns:
 * 0: on error
 * 1: when exists and is a file
 * 2: when exists and is a directory
 * 3: when does not exist, but looks like a file       /abcd/efgh
 * 4: when does not exist, but looks like a directory  /abcd/efgh/
 * @param path
 * @return
 */
static int examinePath(const char *path)
{
  struct STAT status {};
  const bool success = static_cast<int>(statPath(path, status)) == 0;
  if(!success)
  {
    if(errno == ENOENT) //no such file or directory
    {
      const int len = static_cast<int>(strlen(path));
      if(len == 0)
      {
        return 0; //error: path is an empty string
      }
      const char lastChar = path[len - 1];
      if( lastChar != '/' && lastChar != '\\' )
      {
        return 3; //looks like a file
      }

      return 4; //looks like a directory
    }
    return 0; //error
  }
  if((status.st_mode & S_IFREG) != 0 )
  {
    return 1; //existing file
  }
  if((status.st_mode & S_IFDIR) != 0 )
  {
    return 2; //existing directory
  }
  return 0; //error: "path" may be a socket, symlink, named pipe, etc.
}

/**
 * Creates a directory if it does not exist
 * @param dir
 * @return
 */
static int makeDir(const char *dir)
{
  if(examinePath(dir) == 2) //existing directory
  {
    return 2; //2: directory already exists, no need to create
  }
#ifdef WINDOWS
  const bool created = (CreateDirectoryW(WcharStr(dir).wchar_str, nullptr) == TRUE);
#else
#ifdef UNIX
  const bool created = (mkdir(dir, 0777) == 0);
#else
#error Unknown target system
#endif
#endif
  return created ? 1 : 0; //0: failed, 1: created successfully
}

/**
 * Creates directories recursively if they don't exist.
 */
static void makeDirectories(const char *filename)
{
  String path(filename);
  uint64_t start = 0;
  if(path[1] == ':')
  {
    start = 2; //skip drive letter (c:)
  }
  if(path[start] == '/' || path[start] == '\\')
  {
    start++; //skip leading slashes (root dir)
  }
  for( uint64_t i = start; path[i] != 0; ++i )
  {
    if( path[i] == '/' || path[i] == '\\' )
    {
      char saveChar = path[i];
      path[i] = 0;
      const char *dirName = path.c_str();
      const int created = makeDir(dirName);
      if(created == 0)
      {
        printf("Unable to create directory %s", dirName);
        quit();
      }
      if( created == 1 )
      {
        printf("Created directory %s\n", dirName);
      }
      path[i] = saveChar;
    }
  }
}

#endif //PAQ8PX_FILEUTILS_HPP


/**
 * This is an abstract class for all the required file operations.
 * The main purpose of these classes is to keep temporary files in RAM as mush as possible. The default behaviour is to simply
 * pass function calls to the operating system - except in case of temporary files.
 */
class File
{
public:
  virtual ~File() = default;
  virtual bool open(const char *filename, bool mustSucceed) = 0;
  virtual void create(const char *filename) = 0;
  virtual void close() = 0;
  virtual int getchar() = 0;
  virtual void putChar(uint8_t c) = 0;
  void append(const char *s);
  virtual uint64_t blockRead(uint8_t *ptr, uint64_t count) = 0;
  virtual void blockWrite(uint8_t *ptr, uint64_t count) = 0;
  uint32_t get32();
  void put32(uint32_t x);
  uint64_t getVLI();
  void putVLI(uint64_t i);
  virtual void setpos(uint64_t newPos) = 0;
  virtual void setEnd() = 0;
  virtual uint64_t curPos() = 0;
  virtual bool eof() = 0;
};

/**
 * A class to represent a filename.
 */
class FileName:
public String
{
public:
  explicit FileName(const char *s = "");
  int lastSlashPos() const;
  void keepFilename();
  void keepPath();

  /**
    * Prepare path string for screen output.
    */
  void replaceSlashes();
};


/**
 * This class is responsible for files on disk.
 * It simply passes function calls to stdio.
 */
class FileDisk:
public File
{
private:
  /**
    * Helper function: create a temporary file
    *
    * On Windows when using tmpFile() the temporary file may be created
    * in the root directory causing access denied error when User Account Control (UAC) is on.
    * To avoid this issue with tmpFile() we simply use fopen() instead.
    * We create the temporary file in the directory where the executable is launched from.
    * Luckily the MS c runtime library provides two (MS specific) fopen() flags: "T"emporary and "d"elete.
    * @return
    */
  static FILE* makeTmpFile();
protected:
  FILE *file;

public:
  FileDisk();
  ~FileDisk() override;
  bool open(const char *filename, bool mustSucceed) override;
  void create(const char *filename) override;
  void createTmp();
  void close() override;
  int getchar() override;
  void putChar(uint8_t c) override;
  uint64_t blockRead(uint8_t *ptr, uint64_t count) override;
  void blockWrite(uint8_t *ptr, uint64_t count) override;
  void setpos(uint64_t newPos) override;
  void setEnd() override;
  uint64_t curPos() override;
  bool  eof() override;
};

#ifdef __APPLE__

#include <mach-o/dyld.h>

#endif

namespace ofmf
{
  static char myPathError[] = "Can't determine my path.";
} // namespace ofmf

/**
 * Helper class: open a file from the executable's folder
 */
class OpenFromMyFolder
{
private:
public:
  /**
    * This static method will open the executable itself for reading
    * @param f The @ref FileDisk to read the contents of the file into.
    */
  static void myself(FileDisk *f);

  /**
    * This static method will open a file from the executable's folder.
    * Only ASCII file names are supported.
    * @param f The @ref FileDisk to read the contents of the file into.
    * @param filename The name of the file to open
    */
  static void anotherFile(FileDisk *f, const char *filename);
};

// FileTmp.hpp

class FileTmp:
public File
{
private:
  Array<uint8_t> *contentInRam; /**< content of file */
  uint64_t filePos;
  uint64_t fileSize;
  FileDisk *fileOnDisk;

  void forgetContentInRam();
  void forgetFileOnDisk();

#ifdef NDEBUG
  static constexpr uint32_t MAX_RAM_FOR_TMP_CONTENT = 64 * 1024 * 1024; //64 MB (per file)
#else
  static constexpr uint32_t MAX_RAM_FOR_TMP_CONTENT = 64; // to trigger switching to disk earlier - for testing
#endif
  /**
    * switch: ram->disk
    */
  void ramToDisk();
public:
  FileTmp();
  ~FileTmp() override;

  /**
    *  This method is forbidden for temporary files.
    */
  bool open(const char * /*filename*/, bool /*mustSucceed*/) override;

  /**
    *  This method is forbidden for temporary files.
    */
  void create(const char * /*filename*/) override;
  void close() override;
  int getchar() override;
  void putChar(uint8_t c) override;
  uint64_t  blockRead(uint8_t *ptr, uint64_t count) override;
  void blockWrite(uint8_t *ptr, uint64_t count) override;
  void setpos(uint64_t newPos) override;
  void setEnd() override;
  uint64_t curPos() override;
  bool  eof() override;
};

// fileUtils2.hpp

/**
 * Verify that the specified file exists and is readable, determine file size
 * @todo Large file support
 * @param filename
 * @return
 */
static uint64_t getFileSize(const char *filename)
{
  FileDisk f;
  f.open(filename, true);
  f.setEnd();
  const uint64_t fileSize = f.curPos();
  f.close();
  if ((fileSize >> 31) != 0)
  {
    quit("Large files not supported.");
  }
  return fileSize;
}

static void appendToFile(const char *filename, const char *s)
{
  FILE *f = openFile(filename, APPEND);
  if( f == nullptr )
  {
    printf("Warning: Could not log compression results to %s\n", filename);
  }
  else
  {
    fprintf(f, "%s", s);
    fclose(f);
  }
}

// ListOfFiles.hpp

class ListOfFiles
{
private:
  enum class ParseState
  {
      IN_HEADER, FINISHED_A_FILENAME, FINISHED_A_LINE, PROCESSING_FILENAME
  };
  ParseState state; /**< parsing state */
  FileName basePath;
  String listOfFiles; /**< path/file list in first column, columns separated by tabs, rows separated by newlines, with header in 1st row */
  Array<FileName *> names; /**< all file names parsed from listOfFiles */
public:
  ListOfFiles();
  ~ListOfFiles();
  void setBasePath(const char *s);
  void addChar(char c);
  int getCount();
  const char* getfilename(int i);
  String* getString();
};
