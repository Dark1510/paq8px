#include "File.hpp"
#include "../CharacterNames.hpp"
#include "../SystemDefines.hpp"

void File::append(const char *s)
{
  for(int i = 0; s[i] != 0; i++)
  {
    putChar(static_cast<uint8_t>(s[i]));
  }
}

uint32_t  File::get32()
{
    return (getchar() << 24) | (getchar() << 16) | (getchar() << 8) | (getchar());
}

void File::put32(uint32_t x)
{
  putChar((x >> 24) & 255);
  putChar((x >> 16) & 255);
  putChar((x >> 8) & 255);
  putChar(x & 255);
}

uint64_t File::getVLI()
{
  uint64_t i = 0;
  int k = 0;
  uint8_t b = 0;
  do
  {
    b = getchar();
    i |= static_cast<uint64_t>(b & 0x7F) << k;
    k += 7;
  }
  while((b >> 7) > 0 );
  return i;
}

void File::putVLI(uint64_t i)
{
  while(i > 0x7F)
  {
    putChar(0x80 | (i & 0x7F));
    i >>= 7;
  }
  putChar(uint8_t(i));
}

ListOfFiles::ListOfFiles():
state(ParseState::IN_HEADER), names(0) {}

ListOfFiles::~ListOfFiles()
{
  for (int i = 0; i < static_cast<int>(names.size()); i++)
  {
    delete names[i];
  }
}

void ListOfFiles::setBasePath(const char *s)
{
  assert(basePath.strsize() == 0);
  basePath += s;
}

void ListOfFiles::addChar(char c)
{
  if( c != EOF)
  {
    listOfFiles += c;
  }
  if( c == 10 || c == 13 || c == EOF) //got a newline
  {
    state = ParseState::FINISHED_A_LINE; //empty lines / extra newlines (cr, crlf or lf) are ignored
  }
  else if( state == ParseState::IN_HEADER )
  {
    return; //ignore anything in header
  }
  else if( c == TAB ) //got a tab
  {
    state = ParseState::FINISHED_A_FILENAME; //ignore the rest (other columns)
  }
  else // got a character
  {
    if (state == ParseState::FINISHED_A_FILENAME)
    {
      return; //ignore the rest (other columns)
    }
    if(state == ParseState::FINISHED_A_LINE)
    {
      names.pushBack(new FileName(basePath.c_str()));
      state = ParseState::PROCESSING_FILENAME;
      if( c == '/' || c == '\\' ) {
        quit("For security reasons absolute paths are not allowed in the file list.");
      }
      // TODO: prohibit parent folder references in path ('/../')
    }
    if( c == 0 || c == ':' || c == '?' || c == '*' ) {
      printf("\nIllegal character ('%c') in file list.", c);
      quit();
    }
    if (c == BADSLASH)
    {
      c = GOODSLASH;
    }
    (*names[names.size() - 1]) += c;
  }
}

int ListOfFiles::getCount() {return static_cast<int>(names.size()); }

const char* ListOfFiles::getfilename(int i) { return names[i]->c_str(); }

String* ListOfFiles::getString() { return &listOfFiles; }


void OpenFromMyFolder::myself(FileDisk *f)
{
#ifdef WINDOWS
  int i;
      Array<wchar_t> myFileName(MAX_PATH);
      if ((i = GetModuleFileNameW(nullptr, &myFileName[0], MAX_PATH)) && i < MAX_PATH && i != 0)
      {
        f->open(Utf8Str(&myFileName[0]).utf8_str, true);
      } else
        quit(ofmf::myPathError);
#endif
#ifdef __APPLE__
  char myFileName[PATH_MAX];
  uint32_t size = sizeof(myFileName);
  if( _NSGetExecutablePath(myFileName, &size) == 0 )
  {
    f->open(&myFileName[0], true);
  } else {
    quit(ofmf::myPathError);
  }
#endif
#ifdef UNIX
#ifndef __APPLE__
  Array<char> myFileName(PATH_MAX + 1);
      if (readlink("/proc/self/exe", &myFileName[0], PATH_MAX) != -1)
        f->open(&myFileName[0], true);
      else
        quit(ofmf::myPathError);
#endif
#endif
}

void OpenFromMyFolder::anotherFile(FileDisk *f, const char *filename)
{
  const uint64_t fLength = strlen(filename) + 1;
#ifdef WINDOWS
  int i;
  Array<wchar_t> myFileNameW(MAX_PATH + fLength);
  if ((i = GetModuleFileNameW(nullptr, &myFileNameW[0], MAX_PATH)) && i < MAX_PATH && i != 0)
  {
    Array<char> myFileName(MAX_PATH + fLength);
    strcpy(&myFileName[0], Utf8Str(&myFileNameW[0]).utf8_str);
    char *endOfPath = strrchr(&myFileName[0], '\\');
#endif
#ifdef __APPLE__
  char myFileName[PATH_MAX + fLength];
  uint32_t size = sizeof(myFileName);
  if( _NSGetExecutablePath(myFileName, &size) == 0 )
  {
    char *endOfPath = strrchr(&myFileName[0], '/');
#endif
#ifdef UNIX
#ifndef __APPLE__
    char myFileName[PATH_MAX + fLength];
        if(readlink("/proc/self/exe", myFileName, PATH_MAX) != -1 )
        {
          char *endOfPath = strrchr(&myFileName[0], '/');
#endif
#endif
    if (endOfPath == nullptr)
    {
      quit(ofmf::myPathError);
    }
    endOfPath++;
    strcpy(endOfPath, filename); //append filename to my path
    f->open(&myFileName[0], true);
        } else {
    quit(ofmf::myPathError);
  }
}


FILE* FileDisk::makeTmpFile()
{
#if defined(WINDOWS)
  wchar_t szTempFileName[MAX_PATH];
  const UINT uRetVal = GetTempFileNameW(L".", L"tmp", 0, szTempFileName);
  if (uRetVal == 0)
      return nullptr;
  return fopen(Utf8Str(szTempFileName).utf8_str, "w+bTD");
#else
  return tmpfile();
#endif
}

FileDisk::FileDisk() { file = nullptr; }

FileDisk::~FileDisk() { close(); }

bool FileDisk::open(const char *filename, bool mustSucceed)
{
  assert(file == nullptr);
  file = openFile(filename, READ);
  const bool success = (file != nullptr);
  if( !success && mustSucceed )
  {
    printf("Unable to open file %s (%s)", filename, strerror(errno));
    quit();
  }
  return success;
}

void FileDisk::create(const char *filename)
{
  assert(file == nullptr);
  makeDirectories(filename);
  file = openFile(filename, WRITE);
  if( file == nullptr )
  {
    printf("Unable to create file %s (%s)", filename, strerror(errno));
    quit();
  }
}

void FileDisk::createTmp()
{
  assert(file == nullptr);
  file = makeTmpFile();
  if( file == nullptr )
  {
    printf("Unable to create temporary file (%s)", strerror(errno));
    quit();
  }
}

void FileDisk::close()
{
  if( file != nullptr )
  {
    fclose(file);
  }
  file = nullptr;
}

int FileDisk::getchar() { return fgetc(file); }

void FileDisk::putChar(uint8_t c) { fputc(c, file); }

uint64_t FileDisk::blockRead(uint8_t *ptr, uint64_t count) { return fread(ptr, 1, count, file); }

void FileDisk::blockWrite(uint8_t *ptr, uint64_t count) { fwrite(ptr, 1, count, file); }

void FileDisk::setpos(uint64_t newPos) { fseeko(file, newPos, SEEK_SET); }

void FileDisk::setEnd() { fseeko(file, 0, SEEK_END); }

uint64_t FileDisk::curPos() { return ftello(file); }

bool FileDisk::eof() { return feof(file) != 0; }

FileName::FileName(const char *s) : String(s) {}

int FileName::lastSlashPos() const
{
  int pos = findLast('/');
  if(pos < 0)
  {
    pos = findLast('\\');
  }
  return pos; //has no path when -1
}

void FileName::keepFilename()
{
  const int pos = lastSlashPos();
  stripStart(pos + 1); //don't keep last slash
}

void FileName::keepPath()
{
  const int pos = lastSlashPos();
  stripEnd(strsize() - pos - 1); //keep last slash
}

void FileName::replaceSlashes()
{
  const uint64_t sSize = strsize();
  for (uint64_t i = 0; i < sSize; i++)
  {
    if ((*this)[i] == BADSLASH )
    {
      (*this)[i] = GOODSLASH;
    }
  }
  chk_consistency();
}




void FileTmp::forgetContentInRam()
{
  if (contentInRam != nullptr)
  {
    delete contentInRam;
    contentInRam = nullptr;
    filePos = 0;
    fileSize = 0;
  }
}

void FileTmp::forgetFileOnDisk()
{
  if (fileOnDisk != nullptr)
  {
    fileOnDisk->close();
    delete fileOnDisk;
    fileOnDisk = nullptr;
  }
}

void FileTmp::ramToDisk()
{
  assert(fileOnDisk == nullptr);
  fileOnDisk = new FileDisk();
  fileOnDisk->createTmp();
  if (fileSize > 0)
  {
    fileOnDisk->blockWrite(&((*contentInRam)[0]), fileSize);
  }
  fileOnDisk->setpos(filePos);
  forgetContentInRam();
}

FileTmp::FileTmp()
{
  contentInRam = new Array<uint8_t>(0);
  filePos = 0;
  fileSize = 0;
  fileOnDisk = nullptr;
}

FileTmp::~FileTmp() { close(); }

bool FileTmp::open(const char * /*filename*/, bool /*mustSucceed*/)
{
  assert(false);
  return false;
}

void FileTmp::create(const char * /*filename*/) { assert(false); }

void FileTmp::close()
{
  forgetContentInRam();
  forgetFileOnDisk();
}

int FileTmp::getchar()
{
  if(contentInRam != nullptr)
  {
    if(filePos >= fileSize)
    {
      return EOF;
    }
    const uint8_t c = (*contentInRam)[filePos];
    filePos++;
    return c;
  }
  return fileOnDisk->getchar();
}

void FileTmp::putChar(uint8_t c)
{
  if (contentInRam != nullptr)
  {
    if (filePos < MAX_RAM_FOR_TMP_CONTENT)
    {
      if (filePos == fileSize)
      {
        contentInRam->pushBack(c);
        fileSize++;
      }
      else
      {
        (*contentInRam)[filePos] = c;
      }
      filePos++;
      return;
    }
    ramToDisk();
  }
  fileOnDisk->putChar(c);
}

uint64_t FileTmp::blockRead(uint8_t *ptr, uint64_t count)
{
  if(contentInRam != nullptr)
  {
    const uint64_t available = fileSize - filePos;
    if (available < count)
    {
      count = available;
    }
    if(count > 0)
    {
      memcpy(ptr, &((*contentInRam)[filePos]), count);
    }
    filePos += count;
    return count;
  }
  return fileOnDisk->blockRead(ptr, count);
}

void FileTmp::blockWrite(uint8_t *ptr, uint64_t count)
{
  if(contentInRam != nullptr)
  {
    if(filePos + count <= MAX_RAM_FOR_TMP_CONTENT )
    {
      contentInRam->resize((filePos + count));
      if( count > 0 )
      {
        memcpy(&((*contentInRam)[filePos]), ptr, count);
      }
      fileSize += count;
      filePos += count;
      return;
    }
    ramToDisk();
  }
  fileOnDisk->blockWrite(ptr, count);
}

void FileTmp::setpos(uint64_t newPos)
{
  if(contentInRam != nullptr)
  {
    if(newPos > fileSize)
    {
      ramToDisk(); //panic: we don't support seeking past end of file (but stdio does) - let's switch to disk
    }
    else
    {
      filePos = newPos;
      return;
    }
  }
  fileOnDisk->setpos(newPos);
}

void FileTmp::setEnd()
{
  if (contentInRam != nullptr)
  {
    filePos = fileSize;
  }
  else
  {
    fileOnDisk->setEnd();
  }
}

uint64_t FileTmp::curPos()
{
  if(contentInRam != nullptr)
  {
    return filePos;
  }

  return fileOnDisk->curPos();
}

bool FileTmp::eof()
{
  if (contentInRam != nullptr)
  {
    return filePos >= fileSize;
  }

  return fileOnDisk->eof();
}
