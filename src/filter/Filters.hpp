#pragma once

#include "../Array.hpp"
#include "../BlockType.hpp"
#include "../CharacterNames.hpp"
#include "../Encoder.hpp"
#include "../TransformOptions.hpp"
#include "../file/File.hpp"
#include "../Utils.hpp"
#include "../Block.hpp"
#include "Filter.hpp"
#include "TextParserStateInfo.hpp"
#include "LZWDictionary.hpp"
#include "DecAlphaFilter.hpp"
#include <cctype>
#include <cstdint>
#include <cstring>


/////////////////////////// Filters /////////////////////////////////
//@todo: Update this documentation
//
// Before compression, data is encoded in blocks with the following format:
//
//   <type> <size> <encoded-data>
//
// type is 1 byte (type BlockType): DEFAULT=0, JPEG, EXE
// size is 4 bytes in big-endian format.
// Encoded-data decodes to <size> bytes.  The encoded size might be
// different.  Encoded data is designed to be more compressible.
//
//   void encode(File *in, File *out, int n);
//
// Reads n bytes of in (open in "rb" mode) and encodes one or
// more blocks to temporary file out (open in "wb+" mode).
// The file pointer of in is advanced n bytes.  The file pointer of
// out is positioned after the last byte written.
//
//   en.setFile(File *out);
//   int decode(Encoder& en);
//
// Decodes and returns one byte.  Input is from en.decompressByte(), which
// reads from out if in COMPRESS mode.  During compression, n calls
// to decode() must exactly match n bytes of in, or else it is compressed
// as type 0 without encoding.
//
//   BlockType detect(File *in, int n, BlockType type);
//
// Reads n bytes of in, and detects when the type changes to
// something else.  If it does, then the file pointer is repositioned
// to the start of the change and the new type is returned.  If the type
// does not change, then it repositions the file pointer n bytes ahead
// and returns the old type.
//
// For each type X there are the following 2 functions:
//
//   void encode_X(File *in, File *out, int n, ...);
//
// encodes n bytes from in to out.
//
//   int decode_X(Encoder& en);
//
// decodes one byte from en and returns it.  decode() and decode_X()
// maintain state information using static variables.

namespace base64
{
  constexpr bool isdigit(int8_t c)
  {
    return c >= '0' && c <= '9';
  }

  constexpr bool islower(int8_t c)
  {
    return c >= 'a' && c <= 'z';
  }

  constexpr bool isupper(int8_t c)
  {
    return c >= 'A' && c <= 'Z';
  }

  constexpr bool isalpha(int8_t c)
  {
    return islower(c) || isupper(c);
  }

  constexpr bool isalnum(int8_t c)
  {
    return isalpha(c) || isdigit(c);
  }

  static constexpr char table1[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
} // namespace base64

class Base64Filter: Filter
{
private:
  static char valueB(char c)
  {
    const char* p = strchr(base64::table1, c);
    if (p != nullptr)
    {
      return static_cast<char>(p - base64::table1);
    }
    return 0;
  }

  static bool isBase64(uint8_t c)
  {
    return (base64::isalnum(c) || (c == '+') || (c == '/') || (c == 10) || (c == 13));
  }

public:
  void encode(File* in, File* out, uint64_t size, int  /*info*/, int& /*headerSize*/) override
  {
    uint64_t inLen = 0;
    int i = 0;
    int lineSize = 0;
    uint8_t b = 0;
    uint8_t tlf = 0;
    uint8_t src[4];
    uint64_t b64Mem = (size >> 2) * 3 + 10;
    Array<uint8_t> ptr(b64Mem);
    int olen = 5;

    while (b = in->getchar(), inLen++, (b != '=') && isBase64(b) && inLen <= size)
    {
      if (b == 13 || b == 10)
      {
        if (lineSize == 0)
        {
          lineSize = inLen;
          tlf = b;
        }
        if (tlf != b)
        {
          tlf = 0;
        }
        continue;
      }
      src[i++] = b;
      if (i == 4) {
        for (int j = 0; j < 4; j++)
        {
          src[j] = valueB(src[j]);
        }
        src[0] = (src[0] << 2) + ((src[1] & 0x30) >> 4);
        src[1] = ((src[1] & 0xf) << 4) + ((src[2] & 0x3c) >> 2);
        src[2] = ((src[2] & 0x3) << 6) + src[3];

        ptr[olen++] = src[0];
        ptr[olen++] = src[1];
        ptr[olen++] = src[2];
        i = 0;
      }
    }

    if (i != 0)
    {
      for (int j = i; j < 4; j++)
      {
        src[j] = 0;
      }

      for (int j = 0; j < 4; j++)
      {
        src[j] = valueB(src[j]);
      }

      src[0] = (src[0] << 2) + ((src[1] & 0x30) >> 4);
      src[1] = ((src[1] & 0xf) << 4) + ((src[2] & 0x3c) >> 2);
      src[2] = ((src[2] & 0x3) << 6) + src[3];

      for (int j = 0; (j < i - 1); j++)
      {
        ptr[olen++] = src[j];
      }
    }
    ptr[0] = lineSize & 255;
    ptr[1] = size & 255;
    ptr[2] = (size >> 8) & 255;
    ptr[3] = (size >> 16) & 255;
    if (tlf != 0)
    {
      if (tlf == 10)
      {
        ptr[4] = 128;
      } else {
        ptr[4] = 64;
      }
    } else {
      ptr[4] = (size >> 24) & 63; //1100 0000
    }
    out->blockWrite(&ptr[0], olen);
  }

  uint64_t decode(File* in, File* out, FMode fMode, uint64_t /*size*/, uint64_t& diffFound) override
  {
    uint8_t inn[3];
    int i = 0;
    int len = 0;
    int blocksOut = 0;
    int fle = 0;
    int lineSize = in->getchar();
    int outLen = in->getchar();
    outLen += (in->getchar()) << 8;
    outLen += (in->getchar()) << 16;
    uint8_t tlf = in->getchar();
    outLen += (tlf & 63) << 24;
    Array<uint8_t> ptr((outLen >> 2) * 4 + 10);
    tlf = (tlf & 192);
    if (tlf == 128)
    {
      tlf = 10; // LF: 10
    }
    else if (tlf == 64)
    {
      tlf = 13; // CR: 13
    } else {
      tlf = 0;
    }

    while (fle < outLen)
    {
      len = 0;
      for (i = 0; i < 3; i++)
      {
        int c = in->getchar();
        if (c != EOF)
        {
          inn[i] = static_cast<uint8_t>(c);
          len++;
        } else {
          inn[i] = 0;
        }
      }
      if (len != 0)
      {
        uint8_t in0 = inn[0];
        uint8_t in1 = inn[1];
        uint8_t in2 = inn[2];
        ptr[fle++] = (base64::table1[in0 >> 2]);
        ptr[fle++] = (base64::table1[((in0 & 0x03) << 4) | ((in1 & 0xf0) >> 4)]);
        ptr[fle++] = ((len > 1 ? base64::table1[((in1 & 0x0f) << 2) | ((in2 & 0xc0) >> 6)] : '='));
        ptr[fle++] = ((len > 2 ? base64::table1[in2 & 0x3f] : '='));
        blocksOut++;
      } else {
        if (fMode == FMode::FDECOMPRESS)
        {
          quit("Unexpected Base64 decoding state");
        }
        else if (fMode == FMode::FCOMPARE)
        {
          diffFound = fle;
          break; // give up
        }
      }
      if (blocksOut >= (lineSize / 4) && lineSize != 0) //no lf if lineSize==0
      {
        if ((blocksOut != 0) && !in->eof() && fle <= outLen) //no lf if eof
        {
          if (tlf != 0)
          {
            ptr[fle++] = tlf;
          } else {
            ptr[fle++] = 13;
            ptr[fle++] = 10;
          }
        }
        blocksOut = 0;
      }
    }
    //Write out or compare
    if (fMode == FMode::FDECOMPRESS)
    {
      out->blockWrite(&ptr[0], outLen);
    }
    else if (fMode == FMode::FCOMPARE)
    {
      for (i = 0; i < outLen; i++)
      {
        uint8_t b = ptr[i];
        if (b != out->getchar() && (diffFound == 0))
        {
          diffFound = static_cast<int>(out->curPos());
        }
      }
    }
    return outLen;
  }
};


constexpr int powers[5] = { 85 * 85 * 85 * 85, 85 * 85 * 85, 85 * 85, 85, 1 };

class Base85Filter:
Filter
{
public:
  void encode(File* in, File* out, uint64_t size, int  /*info*/, int& /*headerSize*/) override
  {
    int lfp = 0;
    int tlf = 0;
    int b85mem = (size >> 2) * 5 + 100;
    Array<uint8_t, 1> ptr(b85mem);
    int olen = 5;
    int c;
    int count = 0;
    uint32_t tuple = 0;
    for (int f = 0; f < size; f++)
    {
      c = in->getchar();
      if (olen + 10 > b85mem)
      {
        count = 0;
        break;
      }
      if (c == CARRIAGE_RETURN || c == NEW_LINE)
      {
        if (lfp == 0)
        {
          lfp = f;
          tlf = c;
        }
        if (tlf != c)
          tlf = 0;
        continue;
      }
      if (c == 'z' && count == 0)
      {
        if (olen + 10 > b85mem)
        {
          count = 0;
          break;
        }
        for (int i = 1; i < 5; i++)
          ptr[olen++] = 0;
        continue;
      }
      if (c == EOF)
      {
        if (olen + 10 > b85mem)
        {
          count = 0;
          break;
        }
        if (count > 0)
        {
          tuple += powers[count - 1];
          for (int i = 1; i < count; i++)
            ptr[olen++] = tuple >> ((4 - i) * 8);
        }
        break;
      }
      tuple += (c - '!') * powers[count++];
      if (count == 5)
      {
        if (olen > b85mem + 10)
        {
          count = 0;
          break;
        }
        for (int i = 1; i < count; i++)
          ptr[olen++] = tuple >> ((4 - i) * 8);
        tuple = 0;
        count = 0;
      }
    }
    if (count > 0)
    {
      tuple += powers[count - 1];
      for (int i = 1; i < count; i++)
        ptr[olen++] = tuple >> ((4 - i) * 8);
    }
    ptr[0] = lfp & 255; //nl lenght
    ptr[1] = size & 255;
    ptr[2] = size >> 8 & 255;
    ptr[3] = size >> 16 & 255;
    if (tlf != 0)
    {
      if (tlf == 10)
        ptr[4] = 128;
      else ptr[4] = 64;
    }
    else
      ptr[4] = size >> 24 & 63; //1100 0000
    out->blockWrite(&ptr[0], olen);
  }

  uint64_t decode(File* in, File* out, FMode fMode, uint64_t /*size*/, uint64_t& diffFound) override
  {
    int i;
    int fle = 0;
    int nlsize = 0;
    int outlen = 0;
    int tlf = 0;
    nlsize = in->getchar();
    outlen = in->getchar();
    outlen += (in->getchar() << 8);
    outlen += (in->getchar() << 16);
    tlf = (in->getchar());
    outlen += ((tlf & 63) << 24);
    Array<uint8_t, 1> ptr((outlen >> 2) * 5 + 10);
    tlf = (tlf & 192);
    if (tlf == 128)
      tlf = NEW_LINE;
    else if (tlf == 64)
      tlf = CARRIAGE_RETURN;
    else
      tlf = 0;
    int c;
    int count = 0;
    int lenlf = 0;
    uint32_t tuple = 0;

    while (fle < outlen)
    {
      c = in->getchar();
      if (c != EOF)
      {
        tuple |= ((uint32_t)c) << ((3 - count++) * 8);
        if (count < 4)
            continue;
      }
      else if (count == 0)
          break;
      int i;
      int lim;
      char out[5];
      if (tuple == 0 && count == 4) // for 0x00000000
      {
        if (nlsize && lenlf >= nlsize)
        {
          if (tlf)
            ptr[fle++] = (tlf);
          else
          {
            ptr[fle++] = CARRIAGE_RETURN;
            ptr[fle++] = NEW_LINE;
          }
          lenlf = 0;
        }
        ptr[fle++] = 'z';
      } else {
        for (i = 0; i < 5; i++)
        {
          out[i] = tuple % 85 + '!';
          tuple /= 85;
        }
        lim = 4 - count;
        for (i = 4; i >= lim; i--)
        {
          if (nlsize && lenlf >= nlsize && ((outlen - fle) >= 5)) // skip nl if only 5 bytes left
          {
            if (tlf)
              ptr[fle++] = (tlf);
            else
            {
              ptr[fle++] = CARRIAGE_RETURN;
              ptr[fle++] = NEW_LINE;
            }
            lenlf = 0;
          }
          ptr[fle++] = out[i];
          lenlf++;
        }
      }
      if (c == EOF)
          break;
      tuple = 0;
      count = 0;
    }
    if (fMode == FMode::FDECOMPRESS)
    {
      out->blockWrite(&ptr[0], outlen);
    }
    else if (fMode == FMode::FCOMPARE)
    {
      for (i = 0; i < outlen; i++)
      {
        uint8_t b = ptr[i];
        if (b != out->getchar() && !diffFound)
            diffFound = out->curPos();
      }
    }
    return outlen;
  }
};



// Function eccCompute(), edcCompute() and eccedcInit() taken from
// ** UNECM - Decoder for ECM (Error code Modeler) format.
// ** version 1.0
// ** Copyright (c) 2002 Neill Corlett

/* LUTs used for computing ECC/EDC */
static uint8_t eccFLut[256];
static uint8_t eccBLut[256];
static uint32_t edcLut[256];
static bool tablesInit = false;

static void eccedcInit() {
  if( tablesInit ) {
    return;
  }
  uint32_t i = 0;
  uint32_t j = 0;
  uint32_t edc = 0;
  for( i = 0; i < 256; i++ ) {
    j = (i << 1) ^ ((i & 0x80) != 0 ? 0x11D : 0);
    eccFLut[i] = j;
    eccBLut[i ^ j] = i;
    edc = i;
    for( j = 0; j < 8; j++ ) {
      edc = (edc >> 1) ^ ((edc & 1) != 0 ? 0xD8018001 : 0);
    }
    edcLut[i] = edc;
  }
  tablesInit = true;
}

static void eccCompute(const uint8_t *src, uint32_t majorCount, uint32_t minorCount, uint32_t majorMult, uint32_t minorInc, uint8_t *dest) {
  uint32_t size = majorCount * minorCount;
  uint32_t major = 0;
  uint32_t minor = 0;
  for( major = 0; major < majorCount; major++ ) {
    uint32_t index = (major >> 1) * majorMult + (major & 1);
    uint8_t eccA = 0;
    uint8_t eccB = 0;
    for( minor = 0; minor < minorCount; minor++ ) {
      uint8_t temp = src[index];
      index += minorInc;
      if( index >= size ) {
        index -= size;
      }
      eccA ^= temp;
      eccB ^= temp;
      eccA = eccFLut[eccA];
    }
    eccA = eccBLut[eccFLut[eccA] ^ eccB];
    dest[major] = eccA;
    dest[major + majorCount] = eccA ^ eccB;
  }
}

static uint32_t edcCompute(const uint8_t *src, int size) {
  uint32_t edc = 0;
  while((size--) != 0 ) {
    edc = (edc >> 8) ^ edcLut[(edc ^ (*src++)) & 0xff];
  }
  return edc;
}


/**
 * @todo Large file support
 */
class CdFilter:
Filter
{
public:
  static int expandCdSector(uint8_t *data, int address, int test)
  {
    uint8_t d2[2352];
    eccedcInit();
    //sync pattern: 00 FF FF FF FF FF FF FF FF FF FF 00
    d2[0] = d2[11] = 0;
    for( int i = 1; i < 11; i++ )
    {
      d2[i] = 255;
    }
    //determine Mode and Form
    int fMode = (data[15] != 1 ? 2 : 1);
    int form = (data[15] == 3 ? 2 : 1);
    //address (Minutes, Seconds, Sectors)
    if( address == -1 )
    {
      for( int i = 12; i < 15; i++ )
      {
        d2[i] = data[i];
      }
    } else {
      int c1 = (address & 15) + ((address >> 4) & 15) * 10;
      int c2 = ((address >> 8) & 15) + ((address >> 12) & 15) * 10;
      int c3 = ((address >> 16) & 15) + ((address >> 20) & 15) * 10;
      c1 = (c1 + 1) % 75;
      if( c1 == 0 )
      {
        c2 = (c2 + 1) % 60;
        if( c2 == 0 )
        {
          c3++;
        }
      }
      d2[12] = (c3 % 10) + 16 * (c3 / 10);
      d2[13] = (c2 % 10) + 16 * (c2 / 10);
      d2[14] = (c1 % 10) + 16 * (c1 / 10);
    }
    d2[15] = fMode;
    if( fMode == 2 )
    {
      for( int i = 16; i < 24; i++ )
      {
        d2[i] = data[i - 4 * static_cast<int>(i >= 20)]; //8 byte subheader
      }
    }
    if( form == 1 )
    {
      if( fMode == 2 )
      {
        d2[1] = d2[12], d2[2] = d2[13], d2[3] = d2[14];
        d2[12] = d2[13] = d2[14] = d2[15] = 0;
      } else {
        for( int i = 2068; i < 2076; i++ )
        {
          d2[i] = 0; //Mode1: reserved 8 (zero) bytes
        }
      }
      for( int i = 16 + 8 * static_cast<int>(fMode == 2); i < 2064 + 8 * static_cast<int>(fMode == 2); i++ )
      {
        d2[i] = data[i]; //data bytes
      }
      uint32_t edc = edcCompute(d2 + 16 * static_cast<int>(fMode == 2), 2064 - 8 * static_cast<int>(fMode == 2));
      for( int i = 0; i < 4; i++ ) {
        d2[2064 + 8 * static_cast<int>(fMode == 2) + i] = (edc >> (8 * i)) & 0xff;
      }
      eccCompute(d2 + 12, 86, 24, 2, 86, d2 + 2076);
      eccCompute(d2 + 12, 52, 43, 86, 88, d2 + 2248);
      if( fMode == 2 ) {
        d2[12] = d2[1], d2[13] = d2[2], d2[14] = d2[3], d2[15] = 2;
        d2[1] = d2[2] = d2[3] = 255;
      }
    }
    for( int i = 0; i < 2352; i++ )
    {
      if( d2[i] != data[i] && (test != 0))
      {
        form = 2;
      }
    }
    if( form == 2 )
    {
      for( int i = 24; i < 2348; i++ )
      {
        d2[i] = data[i]; //data bytes
      }
      uint32_t edc = edcCompute(d2 + 16, 2332);
      for( int i = 0; i < 4; i++ ) {
        d2[2348 + i] = (edc >> (8 * i)) & 0xff; //EDC
      }
    }
    for( int i = 0; i < 2352; i++ )
    {
      if( d2[i] != data[i] && (test != 0))
      {
        return 0;
      }
      data[i] = d2[i];
    }
    return fMode + form - 1;
  }

  void encode(File *in, File *out, uint64_t size, int info, int & /*headerSize*/) override
  {
    const int block = 2352;
    uint8_t blk[block];
    uint64_t blockResidual = size % block;
    assert(blockResidual < 65536);
    out->putChar((blockResidual >> 8) & 255);
    out->putChar(blockResidual & 255);
    for( uint64_t offset = 0; offset < size; offset += block )
    {
      if( offset + block > size ) //residual
      {
        in->blockRead(&blk[0], size - offset);
        out->blockWrite(&blk[0], size - offset);
      } else { //normal sector
        in->blockRead(&blk[0], block);
        if( info == 3 )
        {
          blk[15] = 3; //indicate Mode2/Form2
        }
        if( offset == 0 )
        {
          out->blockWrite(&blk[12], 4 + 4 * static_cast<int>(blk[15] != 1)); //4-byte address + 4 bytes from the 8-byte subheader goes only to the first sector
        }
        out->blockWrite(&blk[16 + 8 * static_cast<int>(blk[15] != 1)],
                        2048 + 276 * static_cast<int>(info == 3)); //user data goes to all sectors
        if( offset + block * 2 > size && blk[15] != 1 )
        {
          out->blockWrite(&blk[16], 4); //in Mode2 4 bytes from the 8-byte subheader goes after the last sector
        }
      }
    }
  }

  /**
    * @todo Large file support
    * @param in
    * @param out
    * @param fMode
    * @param size
    * @param diffFound
    * @return
    */
  uint64_t decode(File *in, File *out, FMode fMode, uint64_t size, uint64_t &diffFound) override
  {
    const int block = 2352;
    uint8_t blk[block];
    uint64_t i = 0; //*in position
    uint64_t nextBlockPos = 0;
    int address = -1;
    int dataSize = 0;
    uint64_t residual = (static_cast<uint64_t>(in->getchar()) << 8) + in->getchar();
    size -= 2;
    while( i < size )
    {
      if( size - i == residual ) //residual data after last sector
      {
        in->blockRead(blk, residual);
        if( fMode == FMode::FDECOMPRESS )
        {
          out->blockWrite(blk, residual);
        } else if( fMode == FMode::FCOMPARE )
        {
          for( int j = 0; j < static_cast<int>(residual); ++j )
          {
            if( blk[j] != out->getchar() && (diffFound == 0))
            {
              diffFound = nextBlockPos + j + 1;
            }
          }
        }
        return nextBlockPos + residual;
      }
      if( i == 0 ) //first sector
      {
        in->blockRead(blk + 12, 4); //header (4 bytes) consisting of address (Minutes, Seconds, Sectors) and fMode (1 = Mode1, 2 = Mode2/Form1, 3 = Mode2/Form2)
        if( blk[15] != 1 )
        {
          in->blockRead(blk + 16, 4); //Mode2: 4 bytes from the read 8-byte subheader
        }
        dataSize = 2048 + static_cast<int>(blk[15] == 3) * 276; //user data bytes: Mode1 and Mode2/Form1: 2048 (ECC is present) or Mode2/Form2: 2048+276=2324 bytes (ECC is not present)
        i += 4 + 4 * static_cast<int>(blk[15] != 1); //4 byte header + ( Mode2: 4 bytes from the 8-byte subheader )
      } else { //normal sector
        address = (blk[12] << 16) + (blk[13] << 8) + blk[14]; //3-byte address (Minutes, Seconds, Sectors)
      }
      in->blockRead(blk + 16 + static_cast<int>(blk[15] != 1) * 8,
                    dataSize); //read data bytes, but skip 8-byte subheader in Mode 2 (which we processed already above)
      i += dataSize;
      if( dataSize > 2048 )
      {
        blk[15] = 3; //indicate Mode2/Form2
      }
      if( blk[15] != 1 && size - residual - i == 4 ) { //Mode 2: we are at the last sector - grab the 4 subheader bytes
        in->blockRead(blk + 16, 4);
        i += 4;
      }
      expandCdSector(blk, address, 0);
      if( fMode == FMode::FDECOMPRESS )
      {
        out->blockWrite(blk, block);
      }
      else if( fMode == FMode::FCOMPARE )
      {
        for( int j = 0; j < block; ++j )
        {
          if( blk[j] != out->getchar() && (diffFound == 0))
          {
            diffFound = nextBlockPos + j + 1;
          }
        }
      }
      nextBlockPos += block;
    }
    return nextBlockPos;
  }
};



#define LZW_TABLE_SIZE 9221

#define LZW_FIND(k) \
  { \
    offset     = ( int ) finalize64((k) * PHI64, 13); \
    int stride = (offset > 0) ? LZW_TABLE_SIZE - offset : 1; \
    while (true) { \
      if ((index = table[offset]) < 0) { \
        index = -offset - 1; \
        break; \
      } else if (dict[index] == int(k)) { \
        break; \
      } \
      offset -= stride; \
      if (offset < 0) \
        offset += LZW_TABLE_SIZE; \
    } \
  }

#define LZW_RESET \
  { \
    for (int i = 0; i < LZW_TABLE_SIZE; table[i] = -1, i++) \
      ; \
  }

static int encodeGif(File *in, File *out, uint64_t len, int &headerSize)
{
  int codeSize = in->getchar();
  int diffPos = 0;
  int clearPos = 0;
  int bsize = 0;
  int code = 0;
  int offset = 0;
  uint64_t beginIn = in->curPos();
  uint64_t beginOut = out->curPos();
  Array<uint8_t> output(4096);
  headerSize = 6;
  out->putChar(headerSize >> 8);
  out->putChar(headerSize & 255);
  out->putChar(bsize);
  out->putChar(clearPos >> 8);
  out->putChar(clearPos & 255);
  out->putChar(codeSize);
  Array<int> table(LZW_TABLE_SIZE);
  for( int phase = 0; phase < 2; phase++ )
  {
    in->setpos(beginIn);
    int bits = codeSize + 1;
    int shift = 0;
    int buffer = 0;
    int blockSize = 0;
    int maxcode = (1u << codeSize) + 1;
    int last = -1;
    Array<int> dict(4096);
    LZW_RESET
    bool end = false;
    while((blockSize = in->getchar()) > 0 && in->curPos() - beginIn < len && !end )
    {
      for( int i = 0; i < blockSize; i++ )
      {
        buffer |= in->getchar() << shift;
        shift += 8;
        while( shift >= bits && !end )
        {
          code = buffer & ((1u << bits) - 1);
          buffer >>= bits;
          shift -= bits;
          if((bsize == 0) && code != (1u << codeSize))
          {
            headerSize += 4;
            out->put32(0);
          }
          if( bsize == 0 )
          {
            bsize = blockSize;
          }
          if( code == (1 << codeSize))
          {
            if( maxcode > (1 << codeSize) + 1 )
            {
              if((clearPos != 0) && clearPos != 69631 - maxcode )
              {
                return 0;
              }
              clearPos = 69631 - maxcode;
            }
            bits = codeSize + 1, maxcode = (1u << codeSize) + 1, last = -1;
            LZW_RESET
          }
          else if( code == (1u << codeSize) + 1 )
          {
            end = true;
          }
          else if( code > maxcode + 1 )
          {
            return 0;
          } else {
            int j = (code <= maxcode ? code : last);
            int size = 1;
            while( j >= (1 << codeSize))
            {
              output[4096 - (size++)] = dict[j] & 255;
              j = dict[j] >> 8;
            }
            output[4096 - size] = j;
            if( phase == 1 )
            {
              out->blockWrite(&output[4096 - size], size);
            } else {
              diffPos += size;
            }
            if( code == maxcode + 1 )
            {
              if( phase == 1 )
              {
                out->putChar(j);
              } else {
                diffPos++;
              }
            }
            if( last != -1 )
            {
              if( ++maxcode >= 8191 )
              {
                return 0;
              }
              if( maxcode <= 4095 )
              {
                int key = (static_cast<uint32_t>(last) << 8) + j;
                int index = -1;
                LZW_FIND(key)
                dict[maxcode] = key;
                table[(index < 0) ? -index - 1 : offset] = maxcode;
                if( phase == 0 && index > 0 )
                {
                  headerSize += 4;
                  j = diffPos - size - static_cast<int>(code == maxcode);
                  out->put32(j);
                  diffPos = size + static_cast<int>(code == maxcode);
                }
              }
              if( maxcode >= ((1 << bits) - 1) && bits < 12 )
              {
                bits++;
              }
            }
            last = code;
          }
        }
      }
    }
  }
  diffPos = static_cast<int>(out->curPos());
  out->setpos(beginOut);
  out->putChar(headerSize >> 8);
  out->putChar(headerSize & 255);
  out->putChar(255 - bsize);
  out->putChar((clearPos >> 8) & 255);
  out->putChar(clearPos & 255);
  out->setpos(diffPos);
  return static_cast<int>(in->curPos() - beginIn == len - 1);
}

#define GIF_WRITE_BLOCK(count) \
  { \
    output[0] = (count); \
    if (mode == FMode::FDECOMPRESS) \
      out->blockWrite(&output[0], (count) + 1); \
    else if (mode == FMode::FCOMPARE) \
      for (int j = 0; j < (count) + 1; j++) \
        if (output[j] != out->getchar() && ! diffFound) { \
          diffFound = outsize + j + 1; \
          return 1; \
        } \
    outsize += (count) + 1; \
    blockSize = 0; \
  }

#define GIF_WRITE_CODE(c) \
  { \
    buffer += (c) << shift; \
    shift += bits; \
    while (shift >= 8) { \
      output[++blockSize] = buffer & 255; \
      buffer >>= 8; \
      shift -= 8; \
      if (blockSize == bsize) GIF_WRITE_BLOCK(bsize); \
    } \
  }

static int decodeGif(File *in, uint64_t size, File *out, FMode mode, uint64_t &diffFound)
{
  int diffCount = in->getchar();
  int curDiff = 0;
  Array<int> diffPos(4096);
  diffCount = ((diffCount << 8) + in->getchar() - 6) / 4;
  int bsize = 255 - in->getchar();
  int clearPos = in->getchar();
  clearPos = (clearPos << 8) + in->getchar();
  clearPos = (69631 - clearPos) & 0xffff;
  int codesize = in->getchar();
  int bits = codesize + 1;
  int shift = 0;
  int buffer = 0;
  int blockSize = 0;
  if( diffCount > 4096 || clearPos <= (1 << codesize) + 2 )
  {
    return 1;
  }
  int maxcode = (1u << codesize) + 1;
  int input = 0;
  int code = 0;
  int offset = 0;
  Array<int> dict(4096);
  Array<int> table(LZW_TABLE_SIZE);
  LZW_RESET
  for( int i = 0; i < diffCount; i++ )
  {
    diffPos[i] = in->getchar();
    diffPos[i] = (diffPos[i] << 8) + in->getchar();
    diffPos[i] = (diffPos[i] << 8) + in->getchar();
    diffPos[i] = (diffPos[i] << 8) + in->getchar();
    if( i > 0 ) {
      diffPos[i] += diffPos[i - 1];
    }
  }
  Array<uint8_t> output(256);
  size -= 6 + diffCount * 4;
  int last = in->getchar();
  int total = static_cast<int>(size) + 1;
  int outsize = 1;
  if( mode == FMode::FDECOMPRESS )
  {
    out->putChar(codesize);
  }
  else if( mode == FMode::FCOMPARE )
  {
    if( codesize != out->getchar() && (diffFound == 0))
    {
      diffFound = 1;
    }
  }
  if( diffCount == 0 || diffPos[0] != 0 )
      GIF_WRITE_CODE(1u << codesize)
  else
  {
    curDiff++;
  }
  while( size != 0 && (input = in->getchar()) != EOF)
  {
    size--;
    int key = (static_cast<uint32_t>(last) << 8) + input;
    int index = (code = -1);
    if( last < 0 )
    {
      index = input;
    }
    else
        LZW_FIND(key)
    code = index;
    if( curDiff < diffCount && total - static_cast<int>(size) > diffPos[curDiff] )
    {
      curDiff++, code = -1;
    }
    if( code < 0 )
    {
      GIF_WRITE_CODE(last)
      if( maxcode == clearPos )
      {
        GIF_WRITE_CODE(1u << codesize)
        bits = codesize + 1, maxcode = (1u << codesize) + 1;
        LZW_RESET
      } else {
        ++maxcode;
        if( maxcode <= 4095 )
        {
          dict[maxcode] = key;
          table[(index < 0) ? -index - 1 : offset] = maxcode;
        }
        if( maxcode >= (1 << bits) && bits < 12 )
        {
          bits++;
        }
      }
      code = input;
    }
    last = code;
  }
  GIF_WRITE_CODE(last)
  GIF_WRITE_CODE((1u << codesize) + 1)
  if( shift > 0 )
  {
    output[++blockSize] = buffer & 255;
    if( blockSize == bsize ) GIF_WRITE_BLOCK(bsize)
  }
  if( blockSize > 0 )
      GIF_WRITE_BLOCK(blockSize)
  if( mode == FMode::FDECOMPRESS )
  {
    out->putChar(0);
  }
  else if( mode == FMode::FCOMPARE )
  {
    if( 0 != out->getchar() && (diffFound == 0))
    {
      diffFound = outsize + 1;
    }
  }
  return outsize + 1;
}



class MrbRleFilter:
Filter
{
private:
  int width;
  int height;

  int encodeRLE(uint8_t* dst, uint8_t* ptr, int src_end, int maxlen)
  {
    int i = 0;
    int ind = 0;
    for (ind = 0; ind < src_end; )
    {
      if (i > maxlen)
          return i;
      if (ptr[ind + 0] != ptr[ind + 1] || ptr[ind + 1] != ptr[ind + 2])
      {
        // Guess how many non repeating bytes we have
        int j = 0;
        for (j = ind + 1; j < (src_end); j++)
          if ((ptr[j + 0] == ptr[j + 1] && ptr[j + 2] == ptr[j + 0]) || ((j - ind) >= 127))
              break;
        int pixels = j - ind;
        if (j + 1 == src_end && pixels < 8)
            pixels++;
        dst[i++] = 0x80 | pixels;
        for (int cnt = 0; cnt < pixels; cnt++)
        {
          dst[i++] = ptr[ind + cnt];
          if (i > maxlen)
              return i;
        }
        ind = ind + pixels;
      } else {
        // Get the number of repeating bytes
        int j = 0;
        for (j = ind + 1; j < (src_end); j++)
          if (ptr[j + 0] != ptr[j + 1])
              break;
        int pixels = j - ind + 1;
        if (j == src_end && pixels < 4)
        {
          pixels--;
          dst[i] = uint8_t(0x80 | pixels);
          i++;
          if (i > maxlen)
              return i;
          for (int cnt = 0; cnt < pixels; cnt++)
          {
            dst[i] = ptr[ind + cnt];
            i++;
            if (i > maxlen)
                return i;
          }
          ind = ind + pixels;
        }
        else {
          j = pixels;
          while (pixels > 127)
          {
            dst[i++] = 127;
            dst[i++] = ptr[ind];
            if (i > maxlen)
                return i;
            pixels = pixels - 127;
          }
          if (pixels > 0)
          {
            if (j == src_end)
                pixels--;
            dst[i++] = pixels;
            dst[i++] = ptr[ind];
            if (i > maxlen)
                return i;
          }
          ind = ind + j;
        }
      }
    }
    return i;
  }

public:
  void setInfo(int width, int height)
  {
    this->width = width;
    this->height = height;
  }
  void encode(File* in, File* out, uint64_t size, int info, int& /*headerSize*/) override
  {
    uint64_t savepos = in->curPos();
    int totalSize = (width)*height;
    Array<uint8_t, 1> ptrin(totalSize + 4);
    Array<uint8_t, 1> ptr(size + 4);
    Array<uint32_t> diffpos(4096);
    uint32_t count = 0;
    uint8_t value = 0;
    int diffcount = 0;
    // decode RLE
    for (int i = 0; i < totalSize; ++i)
    {
      if ((count & 0x7F) == 0)
      {
        count = in->getchar();
        value = in->getchar();
      }
      else if (count & 0x80)
      {
        value = in->getchar();
      }
      count--;
      ptrin[i] = value;
    }
    // encode RLE
    int a = encodeRLE(&ptr[0], &ptrin[0], totalSize, size);
    assert(a < (size + 4));
    // compare to original and output diff data
    in->setpos(savepos);
    for (int i = 0; i < size; i++)
    {
      uint8_t b = ptr[i], c = in->getchar();
      if (diffcount == 4095 || diffcount > (size / 2) || i > 0xFFFFFF)
          return; // fail
      if (b != c)
      {
        if (diffcount < 4095)
            diffpos[diffcount++] = c + (i << 8);
      }
    }
    out->putChar((diffcount >> 8) & 255); out->putChar(diffcount & 255);
    if (diffcount > 0)
      out->blockWrite((uint8_t*)&diffpos[0], diffcount * 4);
    out->put32(size);
    out->blockWrite(&ptrin[0], totalSize);
  }

  uint64_t decode(File* in, File* out, FMode fMode, uint64_t  size, uint64_t& diffFound) override {
    if (size == 0)
    {
      diffFound = 1;
      return 0;
    }
    Array<uint32_t> diffpos(4096);
    int diffcount = 0;
    diffcount = (in->getchar() << 8) + in->getchar();
    if (diffcount > 0)
        in->blockRead((uint8_t*)&diffpos[0], diffcount * 4);
    int len = in->get32();
    Array<uint8_t, 1> fptr(size + 4);
    Array<uint8_t, 1> ptr(size + 4);
    in->blockRead(&fptr[0], size);
    encodeRLE(&ptr[0], &fptr[0], size - 2 - 4 - diffcount * 4, len); //size - header
    //Write out or compare
    if (fMode == FMode::FDECOMPRESS)
    {
      int diffo = diffpos[0] >> 8;
      int diffp = 0;
      for (int i = 0; i < len; i++)
      {
        if (i == diffo && diffcount)
        {
          ptr[i] = diffpos[diffp] & 255, diffp++, diffo = diffpos[diffp] >> 8;
        }
      }
      out->blockWrite(&ptr[0], len);
    }
    else if (fMode == FMode::FCOMPARE)
    {
      int diffo = diffpos[0] >> 8;
      int diffp = 0;
      for (int i = 0; i < len; i++)
      {
        if (i == diffo && diffcount)
        {
          ptr[i] = diffpos[diffp] & 255, diffp++, diffo = diffpos[diffp] >> 8;
        }
        uint8_t b = ptr[i];
        if (b != out->getchar() && !diffFound)
            diffFound = out->curPos();
      }
    }
    assert(len < size);
    return len;
  }
};



/**
 * 8/24/32-bit png image data encode/decode
 * filter bytes from individual lines go to a separate header
 */
class PngFilter:
public Filter
{
private:
  int stride = 3; //1: Gray/Indexed, 3: RGB, 4: RGBA
  int width = 0;
public:

  void setWidth(int w)
  {
    this->width = w;
  }
  void setStride(int stride)
  {
    this->stride = stride;
  }

  void encode(File *in, File *out, uint64_t size, int width, int & headerSize) override
  {
    int lineWidth = width + 1; //including filter byte
    headerSize = static_cast<int>(size / lineWidth); // = number of rows
    RingBuffer<uint8_t> filterBuffer(nextPowerOf2(headerSize));
    RingBuffer<uint8_t> pixelBuffer(nextPowerOf2(size - headerSize));
    assert(filterBuffer.size() >= headerSize);
    assert(pixelBuffer.size() >= size - headerSize);
    for( int line = 0; line < headerSize; line++ )
    {
      uint8_t filter = in->getchar();
      filterBuffer.add(filter);
      for (int x = 0; x < width; x++)
      {
        uint8_t c1 = in->getchar();
        switch (filter)
        {
          case 0:
        {
            break;
          }
          case 1:
        {
            c1=(static_cast<uint8_t>(c1 + (x < stride ? 0 : pixelBuffer(stride))));
            break;
          }
          case 2:
        {
            c1=(static_cast<uint8_t>(c1 + (line == 0 ? 0 : pixelBuffer(width))));
            break;
          }
          case 3:
        {
            c1 = (static_cast<uint8_t>(c1 + (((line == 0 ? 0 : pixelBuffer(width)) + (x < stride ? 0 : pixelBuffer(stride))) >> 1)));
            break;
          }
          case 4:
        {
            c1 = (static_cast<uint8_t>(c1 + paeth(
              x < stride ? 0 : pixelBuffer(stride),
              line == 0 ? 0 : pixelBuffer(width),
              line == 0 || x < stride ? 0 : pixelBuffer(width + stride))));
            break;
          }
          default:
            //fail: unexpected filter code.
            return;
        }
        pixelBuffer.add(c1);
      }
    }
    uint32_t len1 = filterBuffer.getpos();
    uint32_t len2 = pixelBuffer.getpos();
    for (uint32_t i = 0; i < len1; i++)
      out->putChar(filterBuffer[i]);
    for (uint32_t i = 0; i < len2; i++)
      out->putChar(pixelBuffer[i]);
  }

  uint64_t decode(File *in, File *out, FMode fMode, uint64_t size, uint64_t &diffFound) override {
    int lineWidth = width + 1; //including filter byte
    int headerSize = static_cast<int>(size / lineWidth); // = number of rows
    RingBuffer<uint8_t> filterBuffer(nextPowerOf2(headerSize));
    RingBuffer<uint8_t> pixelBuffer(nextPowerOf2(size - headerSize));
    assert(filterBuffer.size() >= headerSize);
    assert(pixelBuffer.size() >= size - headerSize);
    for (int line = 0; line < headerSize; line++)
    {
      uint8_t filter = in->getchar();
      filterBuffer.add(filter);
    }
    uint32_t p = 0;
    for (int line = 0; line < headerSize; line++)
    {
      uint8_t filter = filterBuffer[line];
      if (fMode == FMode::FDECOMPRESS)
      {
        out->putChar(filter);
      }
      else if (fMode == FMode::FCOMPARE)
      {
        p++;
        if (filter != out->getchar() && (diffFound == 0))
        {
          diffFound = p;
        }
      }
      for (int x = 0; x < width; x++)
      {
        uint8_t c1 = in->getchar();
        uint8_t c = c1;
        switch (filter)
        {
          case 0:
        {
            break;
          }
          case 1:
        {
            c1 = (static_cast<uint8_t>(c1 - (x < stride ? 0 : pixelBuffer(stride))));
            break;
          }
          case 2:
        {
            c1 = (static_cast<uint8_t>(c1 - (line == 0 ? 0 : pixelBuffer(width))));
            break;
          }
          case 3:
        {
            c1 = (static_cast<uint8_t>(c1 - (((line == 0 ? 0 : pixelBuffer(width)) + (x < stride ? 0 : pixelBuffer(stride))) >> 1)));
            break;
          }
          case 4:
        {
            c1 = (static_cast<uint8_t>(c1 - paeth(
              x < stride ? 0 : pixelBuffer(stride),
              line == 0 ? 0 : pixelBuffer(width),
              line == 0 || x < stride ? 0 : pixelBuffer(width + stride))));
            break;
          }
          default:
            //fail: unexpected filter code.
            break;
        }
        pixelBuffer.add(c);
        if (fMode == FMode::FDECOMPRESS)
        {
          out->putChar(c1);
        }
        else if (fMode == FMode::FCOMPARE)
        {
          p++;
          if (c1 != out->getchar() && (diffFound == 0))
          {
            diffFound = p;
          }
        }
      }
    }
    return size;
  }
};



/**
 * UStar (Unix Standard TAR) detection and transformation
 *
 */
class TarFilter:
public Filter
{
private:

  struct TARheader // 512 bytes
  {
    char name[100];       //   0 | file name
    char mode[8];         // 100 | file mode (permissions)
    char uid[8];          // 108 | owner user id (octal)
    char gid[8];          // 116 | owner group id (octal)
    char size[12];        // 124 | file size in bytes (octal)
    char mtime[12];       // 136 | last modification time in numeric Unix time format (octal)
    char chksum[8];       // 148 | sum of unsigned characters in header block, filled with spaces while calculating (octal)
    char typeflag;        // 156 | link flag / file type
    char linkname[100];   // 157 | name of linked file
    char magic[8];        // 257 | "ustar\000" in calgary.tar created by windows 10 and gnu tar
                          //       "ustar  \0" in mozilla, samba, xml or in calgary.tar created by total commander or gnu tar
    char uname[32];       // 265 | owner user name (string)
    char gname[32];       // 297 | owner group name (string)
    char devmajor[8];     // 329 | device major number
    char devminor[8];     // 337 | device minor number
    char prefix[155];     // 345 | filename prefix
    char padding[12];     // 500 | padding

    int oct2bin(const char* p, int size)
    {
      while (*p == SPACE) //skip leading spaces
      {
        ++p;
        --size;
      }
      int i = 0;
      while (size > 0)
      {
        if (*p == 0) //the last char must be a \0
          break;
        if (*p == SPACE)
          break;
        if (*p < '0' || *p > '7')
          return -1; //fail
        i *= 8;
        i += *p - '0';
        ++p;
        --size;
      }
      return i;
    }

    int calculateChecksum()
    {
      const char* p = &name[0];
      constexpr int chksumOffset = offsetof(TARheader, chksum);
      constexpr int chksumSize = 8;
      int u = 0;
      for (int n = 0; n < sizeof(TARheader); ++n)
      {
        if (n < chksumOffset || n >= chksumOffset + chksumSize) //exluding the checksum bytes
          u += ((uint8_t*)p)[n];
        else
          u += SPACE; // emulate spaces in place of the checksum bytes
      }
      return u;
    }

    // checksum format examples:
    // "0012201\0" (as in calgary.tar as created by total commander)
    // "012201\0 " (as in samba or calgary.tar created by windows 10 or gnu tar)
    // " 12201\0 " (as in mozilla and xml)

    void clearChecksum()
    {
      //look up the terminating \0 and fill it's left side with either spaces or '0's to preserve
      //information about the format (see the checksum format examples above).
      char filler = chksum[0] == SPACE ? SPACE : '0';
      int i = 0;
      for (; i < sizeof(tarh.chksum); i++)
        if (chksum[i + 1] == 0)
            break;
      //now i points to the last digit of the checksum
      for (; i >= 0; i--)
        chksum[i] = filler;
    }

    void generateChecksum()
    {
      //look up the terminating \0 and fill it's left side with the checksum (octal)
      int checksum = calculateChecksum();
      int i = 0;
      for (; i < sizeof(tarh.chksum); i++)
        if (chksum[i + 1] == 0)
            break;
      //now i points to the last digit of the checksum
      for (; i >= 0; i--)
      {
        chksum[i] = (checksum & 7) + '0';
        checksum >>= 3;
        if (checksum == 0)
          break;
      }
    }

    bool verifyChecksum()
    {
      return calculateChecksum() == oct2bin(&chksum[0], 8);
    }

    bool isEmptySector()
    {
      const char* p = &name[0];
      for (int n = 511; n >= 0; --n)
        if (p[n] != 0)
          return false;
      return true;
    }

  } tarh;

  Array<uint64_t> detectedSectorStartPositions{0};
  Array<uint64_t> detectedFileStartPositions{0};
  Array<uint64_t> detectedFileLengths{0};
  uint64_t detectedEmptySectorCount{0};

  //detect tar content
  //a tar file is: hdr+filecontent + hdr+filecontent + etc...
  //this function figures out where each file starts and how long they are
  //we ignore files in tar having garbage in the padding area
  bool process(File* in, uint64_t maxFilePos)
  {
    uint64_t sectorStartPos = this->detectedStartPos;
    while (true)
    {
      if (sectorStartPos == maxFilePos)
      {
        //no empty sectors at the end - that'll be ok
        //this usually happens when we ignore files in a tar with garbage in the padding area
        this->detectedEndPos = sectorStartPos;
        return true;
      }
      else if (sectorStartPos > maxFilePos)
        return false; //fail
      in->setpos(sectorStartPos);
      int bytesRead = in->blockRead((uint8_t*)&tarh, sizeof(tarh));
      if (bytesRead != sizeof(tarh))
      {
        return false; //fail
      }
      if (tarh.isEmptySector())
      {
        do
        {
          detectedEmptySectorCount++;
          sectorStartPos += sizeof(tarh);
          int bytesRead = in->blockRead((uint8_t*)&tarh, sizeof(tarh));
          if (bytesRead != sizeof(tarh))
            break;
        }
        while (tarh.isEmptySector());
        this->detectedEndPos = sectorStartPos;
        return true;
      }
      //verify if all fields look octal that should look octal
      if (!tarh.verifyChecksum())
        return false; //fail
      if (tarh.oct2bin(tarh.mode, sizeof(tarh.mode)) < 0)
        return false; //fail
      if (tarh.oct2bin(tarh.uid, sizeof(tarh.uid)) < 0)
        return false; //fail
      if (tarh.oct2bin(tarh.gid, sizeof(tarh.gid)) < 0)
        return false; //fail
      if (tarh.oct2bin(tarh.size, sizeof(tarh.size)) < 0)
        return false; //fail
      if (tarh.oct2bin(tarh.mtime, sizeof(tarh.mtime)) < 0)
        return false; //fail
      if (tarh.oct2bin(tarh.devmajor, sizeof(tarh.devmajor)) < 0)
        return false; //fail
      if (tarh.oct2bin(tarh.devminor, sizeof(tarh.devminor)) < 0)
        return false; //fail

      detectedSectorStartPositions.pushBack(sectorStartPos);

      int fileSize = tarh.oct2bin(tarh.size, sizeof(tarh.size));
      if (fileSize != 0)
      {
        //detect if file is properly padded
        int filePaddingSize = (512 - (fileSize & 511)) & 511;
        in->setpos(sectorStartPos + sizeof(TARheader) + fileSize);
        for (int i = 0; i < filePaddingSize; i++)
        {
          int c = in->getchar();
          if (c != 0)
          {
            if (detectedFileStartPositions.size() > 0)
            {
              this->detectedEndPos = sectorStartPos;
              return true; //accept what we have so far (files with proper padding)
            }
            return false; //fail, there is not properly padded files so far
          }
        }
        detectedFileStartPositions.pushBack(sectorStartPos + sizeof(TARheader));
        detectedFileLengths.pushBack(fileSize);
      }

      int sectorsToJump = (fileSize + 511) >> 9;
      sectorStartPos += sizeof(TARheader) * (sectorsToJump + 1);
    }
  }

  void Print()
  {
    for (size_t i = 0; i < detectedSectorStartPositions.size(); i++)
    {
      printf("tar sector position: %d\n", (int)detectedSectorStartPositions[i]);
    }
    for (size_t i = 0; i < detectedFileStartPositions.size(); i++)
    {
      printf("file position: %d, length: %d\n", (int)detectedFileStartPositions[i], (int)detectedFileLengths[i]);
    }
    printf("empty sectors: %d, length: %d\n", (int)detectedEmptySectorCount, (int)(detectedEmptySectorCount * sizeof(tarh)));
  }

public:
  uint64_t detectedStartPos{};
  uint64_t detectedEndPos{};

  bool detect(File* in, uint64_t maxFilePos)
  {
    uint64_t userNamePos = in->curPos();
    this->detectedStartPos = userNamePos - offsetof(TARheader, uname);
    return process(in, maxFilePos);
  }

  void encode(File *in, File *out, uint64_t size, int width, int & headerSize) override
  {
    this->detectedStartPos = in->curPos();
    uint64_t maxFilePos = this->detectedStartPos + size;
    bool success = process(in, maxFilePos);
    if (!success)
      quit("Internal error in TAR detection.");

    //for debugging
    //Print();

    out->putVLI(detectedSectorStartPositions.size());
    out->putVLI(detectedEmptySectorCount);

    for (size_t i = 0; i < detectedSectorStartPositions.size(); i++)
    {
      in->setpos(detectedSectorStartPositions[i]);
      int bytesRead = in->blockRead((uint8_t*)&tarh, sizeof(tarh));
      if (bytesRead != sizeof(tarh))
      {
        quit("Internal error in TAR transformation.");
      }
      tarh.clearChecksum();
      out->blockWrite((uint8_t*)&tarh, sizeof(tarh));
    }

    Array<uint8_t, 1> fileData{0};
    for (size_t i = 0; i < detectedFileStartPositions.size(); i++)
    {
      fileData.resize(detectedFileLengths[i]);
      in->setpos(detectedFileStartPositions[i]);
      in->blockRead(&fileData[0], detectedFileLengths[i]);
      out->blockWrite(&fileData[0], detectedFileLengths[i]);
    }

    return;
  }

  uint64_t decode(File* in, File* out, FMode fMode, uint64_t size, uint64_t& diffFound) override
  {
    size_t sectorCount = in->getVLI();
    size_t emptySectorCount = in->getVLI();
    size_t curPos = in->curPos();
    Array<TARheader, 1> headerData{sectorCount};
    Array<uint8_t, 1> fileData{0};
    uint64_t p = 0;
    uint64_t fileDataStartPos = curPos + sectorCount * sizeof(tarh);
    for (size_t i = 0; i < sectorCount; i++)
    {
      in->setpos(curPos + i * sizeof(tarh));
      int bytesRead = in->blockRead((uint8_t*)&headerData[i], sizeof(tarh));
      if (bytesRead != sizeof(tarh))
      {
        if (fMode == FMode::FCOMPARE)
          diffFound = p + 1;
        return 0;
      }
      headerData[i].generateChecksum();

      if (fMode == FMode::FDECOMPRESS)
      {
        p += sizeof(tarh);
        out->blockWrite((uint8_t*)&headerData[i], sizeof(tarh));
      }
      else if (fMode == FMode::FCOMPARE)
      {
        for (int j = 0; j < sizeof(tarh); j++)
        {
          p++;
          int c1 = out->getchar();
          int c2 = ((uint8_t*)&headerData[i])[j];
          if (c1 != c2 && (diffFound == 0))
          {
            diffFound = p;
          }
        }
      }

      int fileSize = tarh.oct2bin(headerData[i].size, 12);

      if (fileSize != 0)
      {
        in->setpos(fileDataStartPos);
        fileData.resize(fileSize);
        int bytesRead = in->blockRead(&fileData[0], fileSize);
        if (bytesRead != fileSize)
        {
          if (fMode == FMode::FCOMPARE)
            diffFound = p;
          return p;
        }
        if (fMode == FMode::FDECOMPRESS)
        {
          p += fileSize;
          out->blockWrite(&fileData[0], fileSize);
        }
        else if (fMode == FMode::FCOMPARE)
        {
          for (int j = 0; j < fileSize; j++)
          {
            p++;
            if (fileData[j] != out->getchar() && (diffFound == 0))
            {
              diffFound = p;
            }
          }
        }
        fileDataStartPos += fileSize;

        //padding sector with 0 when needed
        while ((fileSize & 511) != 0)
        {
          p++;
          fileSize++;
          if (fMode == FMode::FDECOMPRESS)
          {
            out->putChar(0);
          }
          else if (fMode == FMode::FCOMPARE)
          {
            if (out->getchar() != 0 && (diffFound == 0))
            {
              diffFound = p;
            }
          }
        }
      }
    }

    //write empty sectors at the end
    for (size_t i = 0; i < emptySectorCount * sizeof(tarh); i++)
    {
      p++;
      if (fMode == FMode::FDECOMPRESS)
      {
        out->putChar(0);
      }
      else if (fMode == FMode::FCOMPARE)
      {
        if (out->getchar() != 0 && (diffFound == 0))
        {
          diffFound = p;
        }
      }
    }

    in->setpos(fileDataStartPos);

    return p;
  }

  void getFilePositions(File* in, Array<uint64_t,1> &filePositions)
  {
    assert(filePositions.size() == 0);
    size_t sectorCount = in->getVLI();
    size_t emptySectorCount = in->getVLI();
    size_t curPos = in->curPos();
    uint64_t fileDataStartPos = curPos + sectorCount * sizeof(tarh);
    filePositions.pushBack(fileDataStartPos); //the first entry is the first file position
    for (size_t i = 0; i < sectorCount; i++)
    {
      int bytesRead = in->blockRead((uint8_t*)&tarh, sizeof(tarh));
      assert(bytesRead == sizeof(tarh));

      int fileSize = tarh.oct2bin(tarh.size, 12);
      assert(fileSize >= 0);

      if (fileSize != 0)
      {
        fileDataStartPos += fileSize;
        filePositions.pushBack(fileDataStartPos); //the last entry is exactly the tempfile size (it points past to the last file)
      }
    }
  }

};



#define LZW_RESET_CODE 256
#define LZW_EOF_CODE 257

class LZWFilter:
Filter
{
public:
  void encode(File *in, File *out, uint64_t  /*size*/, int  /*info*/, int & /*headerSize*/) override
  {
    LZWDictionary dic;
    int parent = -1;
    int code = 0;
    int buffer = 0;
    int bitsPerCode = 9;
    int bitsUsed = 0;
    bool done = false;
    while( !done )
    {
      buffer = in->getchar();
      if( buffer < 0 )
      {
        return;// 0;
      }
      for( int j = 0; j < 8; j++ )
      {
        code += code + ((buffer >> (7 - j)) & 1), bitsUsed++;
        if( bitsUsed >= bitsPerCode )
        {
          if( code == LZW_EOF_CODE )
          {
            done = true;
            break;
          }
          if( code == LZW_RESET_CODE )
          {
            dic.reset();
            parent = -1;
            bitsPerCode = 9;
          } else {
            if( code < dic.index )
            {
              if( parent != -1 )
              {
                dic.addEntry(parent, dic.dumpEntry(out, code));
              } else {
                out->putChar(code);
              }
            }
            else if( code == dic.index )
            {
              int a = dic.dumpEntry(out, parent);
              out->putChar(a);
              dic.addEntry(parent, a);
            } else {
              return;// 0;
            }
            parent = code;
          }
          bitsUsed = 0;
          code = 0;
          if((1u << bitsPerCode) == dic.index + 1 && dic.index < 4096 )
          {
            bitsPerCode++;
          }
        }
      }
    }
    // return 1;
  }

  uint64_t decode(File * /*in*/, File * /*out*/, FMode  /*fMode*/, uint64_t  /*size*/, uint64_t & /*diffFound*/) override
  {
    return 0;
  }

};

static int encodeLzw(File *in, File *out, uint64_t size, int &headerSize)
{
  LZWDictionary dic;
  int parent = -1;
  int code = 0;
  int buffer = 0;
  int bitsPerCode = 9;
  int bitsUsed = 0;
  bool done = false;
  while( !done )
  {
    buffer = in->getchar();
    if( buffer < 0 )
    {
      return 0;
    }
    for( int j = 0; j < 8; j++ )
    {
      code += code + ((buffer >> (7 - j)) & 1), bitsUsed++;
      if( bitsUsed >= bitsPerCode )
      {
        if( code == LZW_EOF_CODE )
        {
          done = true;
          break;
        }
        if( code == LZW_RESET_CODE )
        {
          dic.reset();
          parent = -1;
          bitsPerCode = 9;
        } else {
          if( code < dic.index )
          {
            if( parent != -1 )
            {
              dic.addEntry(parent, dic.dumpEntry(out, code));
            } else {
              out->putChar(code);
            }
          }
          else if( code == dic.index )
          {
            int a = dic.dumpEntry(out, parent);
            out->putChar(a);
            dic.addEntry(parent, a);
          } else {
            return 0;
          }
          parent = code;
        }
        bitsUsed = 0;
        code = 0;
        if((1 << bitsPerCode) == dic.index + 1 && dic.index < 4096 )
        {
          bitsPerCode++;
        }
      }
    }
  }
  return 1;
}

static inline void writeCode(File *f, const FMode mode, int *buffer, uint64_t *pos, int *bitsUsed, const int bitsPerCode, const int code, uint64_t *diffFound)
{
  *buffer <<= bitsPerCode;
  *buffer |= code;
  (*bitsUsed) += bitsPerCode;
  while((*bitsUsed) > 7 )
  {
    const uint8_t b = *buffer >> (*bitsUsed -= 8);
    (*pos)++;
    if( mode == FMode::FDECOMPRESS )
    {
      f->putChar(b);
    }
    else if( mode == FMode::FCOMPARE && b != f->getchar())
    {
      *diffFound = *pos;
    }
  }
}

static uint64_t decodeLzw(File *in, File *out, FMode mode, uint64_t &diffFound)
{
  LZWDictionary dic;
  uint64_t pos = 0;
  int parent = -1;
  int code = 0;
  int buffer = 0;
  int bitsPerCode = 9;
  int bitsUsed = 0;
  writeCode(out, mode, &buffer, &pos, &bitsUsed, bitsPerCode, LZW_RESET_CODE, &diffFound);
  while((code = in->getchar()) >= 0 && diffFound == 0 )
  {
    int index = dic.findEntry(parent, code);
    if( index < 0 ) // entry not found
    {
      writeCode(out, mode, &buffer, &pos, &bitsUsed, bitsPerCode, parent, &diffFound);
      if( dic.index > 4092 )
      {
        writeCode(out, mode, &buffer, &pos, &bitsUsed, bitsPerCode, LZW_RESET_CODE, &diffFound);
        dic.reset();
        bitsPerCode = 9;
      } else {
        dic.addEntry(parent, code, index);
        if( dic.index >= (1 << bitsPerCode))
        {
          bitsPerCode++;
        }
      }
      parent = code;
    } else {
      parent = index;
    }
  }
  if( parent >= 0 )
  {
    writeCode(out, mode, &buffer, &pos, &bitsUsed, bitsPerCode, parent, &diffFound);
  }
  writeCode(out, mode, &buffer, &pos, &bitsUsed, bitsPerCode, LZW_EOF_CODE, &diffFound);
  if( bitsUsed > 0 ) // flush buffer
  {
    const uint8_t b = uint8_t(buffer << (8 - bitsUsed));  // shift remaining bits to MSB
    pos++;
    if( mode == FMode::FDECOMPRESS )
    {
      out->putChar(b);
    }
    else if( mode == FMode::FCOMPARE && b != out->getchar())
    {
      diffFound = pos;
    }
  }
  return pos;
}





#ifndef DISABLE_ZLIB

#include "../Utils.hpp"
#include <zlib.h>

static int parseZlibHeader(int header)
{
  switch(header)
  {
    case 0x2815:
      return 0;
    case 0x2853:
      return 1;
    case 0x2891:
      return 2;
    case 0x28cf:
      return 3;
    case 0x3811:
      return 4;
    case 0x384f:
      return 5;
    case 0x388d:
      return 6;
    case 0x38cb:
      return 7;
    case 0x480d:
      return 8;
    case 0x484b:
      return 9;
    case 0x4889:
      return 10;
    case 0x48c7:
      return 11;
    case 0x5809:
      return 12;
    case 0x5847:
      return 13;
    case 0x5885:
      return 14;
    case 0x58c3:
      return 15;
    case 0x6805:
      return 16;
    case 0x6843:
      return 17;
    case 0x6881:
      return 18;
    case 0x68de:
      return 19;
    case 0x7801:
      return 20;
    case 0x785e:
      return 21;
    case 0x789c:
      return 22;
    case 0x78da:
      return 23;
    default:
      return -1;
  }
}

static int zlibInflateInit(z_streamp strm, int zh)
{
  if( zh == -1 ) {
    return inflateInit2(strm, -MAX_WBITS);
  }
  return inflateInit(strm);
}

MTFList mtf(81);

static int encodeZlib(File *in, File *out, uint64_t len, int &headerSize)
{
  const int block = 1u << 16;
  const int limit = 128;
  uint8_t zin[block * 2];
  uint8_t zOut[block];
  uint8_t zRec[block * 2];
  uint8_t diffByte[81 * limit];
  uint64_t diffPos[81 * limit];

  // Step 1 - parse offset type form zlib stream header
  uint64_t posBackup = in->curPos();
  uint32_t h1 = in->getchar();
  uint32_t h2 = in->getchar();
  in->setpos(posBackup);
  int zh = parseZlibHeader(h1 * 256 + h2);
  int memLevel = 0;
  int cLevel = 0;
  int cType = zh % 4;
  int window = zh == -1 ? 0 : MAX_WBITS + 10 + zh / 4;
  int minCLevel = window == 0 ? 1 : cType == 3 ? 7 : cType == 2 ? 6 : cType == 1 ? 2 : 1;
  int maxCLevel = window == 0 ? 9 : cType == 3 ? 9 : cType == 2 ? 6 : cType == 1 ? 5 : 1;
  int index = -1;
  int nTrials = 0;
  bool found = false;

  // Step 2 - check recompressibility, determine parameters and save differences
  z_stream mainStrm;
  z_stream recStrm[81];
  int diffCount[81];
  int recPos[81];
  int mainRet = Z_STREAM_END;
  mainStrm.zalloc = Z_NULL;
  mainStrm.zfree = Z_NULL;
  mainStrm.opaque = Z_NULL;
  mainStrm.next_in = Z_NULL;
  mainStrm.avail_in = 0;
  if( zlibInflateInit(&mainStrm, zh) != Z_OK )
  {
    return 0;
  }
  for( int i = 0; i < 81; i++ )
  {
    cLevel = (i / 9) + 1;
    // Early skip if invalid parameter
    if( cLevel < minCLevel || cLevel > maxCLevel )
    {
      diffCount[i] = limit;
      continue;
    }
    memLevel = (i % 9) + 1;
    recStrm[i].zalloc = Z_NULL;
    recStrm[i].zfree = Z_NULL;
    recStrm[i].opaque = Z_NULL;
    recStrm[i].next_in = Z_NULL;
    recStrm[i].avail_in = 0;
    int ret = deflateInit2(&recStrm[i], cLevel, Z_DEFLATED, window - MAX_WBITS, memLevel, Z_DEFAULT_STRATEGY);
    diffCount[i] = (ret == Z_OK) ? 0 : limit;
    recPos[i] = block * 2;
    diffPos[i * limit] = 0xFFFFFFFFFFFFFFFF;
    diffByte[i * limit] = 0;
  }

  for( uint64_t i = 0; i < len; i += block )
  {
    uint32_t blSize = min(uint32_t(len - i), block);
    nTrials = 0;
    for( int j = 0; j < 81; j++ )
    {
      if( diffCount[j] >= limit )
      {
        continue;
      }
      nTrials++;
      if( recPos[j] >= block )
      {
        recPos[j] -= block;
      }
    }
    // early break if nothing left to test
    if( nTrials == 0 ) {
      break;
    }
    memmove(&zRec[0], &zRec[block], block);
    memmove(&zin[0], &zin[block], block);
    in->blockRead(&zin[block], blSize); // Read block from input file

    // Decompress/inflate block
    mainStrm.next_in = &zin[block];
    mainStrm.avail_in = blSize;
    do
    {
      mainStrm.next_out = &zOut[0];
      mainStrm.avail_out = block;
      mainRet = inflate(&mainStrm, Z_FINISH);
      nTrials = 0;

      // Recompress/deflate block with all possible parameters
      for( int j = mtf.getFirst(); j >= 0; j = mtf.getNext())
      {
        if( diffCount[j] >= limit )
        {
          continue;
        }
        nTrials++;
        recStrm[j].next_in = &zOut[0];
        recStrm[j].avail_in = block - mainStrm.avail_out;
        recStrm[j].next_out = &zRec[recPos[j]];
        recStrm[j].avail_out = block * 2 - recPos[j];
        int ret = deflate(&recStrm[j], mainStrm.total_in == len ? Z_FINISH : Z_NO_FLUSH);
        if( ret != Z_BUF_ERROR && ret != Z_STREAM_END && ret != Z_OK )
        {
          diffCount[j] = limit;
          continue;
        }

        // Compare
        int end = 2 * block - static_cast<int>(recStrm[j].avail_out);
        int tail = max(mainRet == Z_STREAM_END ? static_cast<int>(len) - static_cast<int>(recStrm[j].total_out) : 0, 0);
        for( int k = recPos[j]; k < end + tail; k++ )
        {
          if((k < end && i + k - block < len && zRec[k] != zin[k]) || k >= end )
          {
            if( ++diffCount[j] < limit )
            {
              const int p = j * limit + diffCount[j];
              diffPos[p] = i + k - block;
              assert(k < int(sizeof(zin) / sizeof(*zin)));
              diffByte[p] = zin[k];
            }
          }
        }
        // Early break on perfect match
        if( mainRet == Z_STREAM_END && diffCount[j] == 0 )
        {
          index = j;
          found = true;
          break;
        }
        recPos[j] = 2U * block - recStrm[j].avail_out;
      }
    }
    while( mainStrm.avail_out == 0 && mainRet == Z_BUF_ERROR && nTrials > 0 );
    if((mainRet != Z_BUF_ERROR && mainRet != Z_STREAM_END) || nTrials == 0 )
    {
      break;
    }
  }
  int minCount = (found) ? 0 : limit;
  for( int i = 80; i >= 0; i-- )
  {
    cLevel = (i / 9) + 1;
    if( cLevel >= minCLevel && cLevel <= maxCLevel )
    {
      deflateEnd(&recStrm[i]);
    }
    if( !found && diffCount[i] < minCount )
    {
      minCount = diffCount[index = i];
    }
  }
  inflateEnd(&mainStrm);
  if( minCount == limit ) {
    return 0;
  }
  mtf.moveToFront(index);

  // Step 3 - write parameters, differences and precompressed (inflated) data
  out->putChar(diffCount[index]);
  out->putChar(window);
  out->putChar(index);
  for( int i = 0; i <= diffCount[index]; i++ )
  {
    const int v = i == diffCount[index] ? int(len - diffPos[index * limit + i]) :
                  int(diffPos[index * limit + i + 1] - diffPos[index * limit + i]) - 1;
    out->put32(v);
  }
  for( int i = 0; i < diffCount[index]; i++ )
  {
    out->putChar(diffByte[index * limit + i + 1]);
  }

  in->setpos(posBackup);
  mainStrm.zalloc = Z_NULL;
  mainStrm.zfree = Z_NULL;
  mainStrm.opaque = Z_NULL;
  mainStrm.next_in = Z_NULL;
  mainStrm.avail_in = 0;
  if( zlibInflateInit(&mainStrm, zh) != Z_OK )
  {
    return 0;
  }
  for( uint64_t i = 0; i < len; i += block )
  {
    uint32_t blSize = min(uint32_t(len - i), block);
    in->blockRead(&zin[0], blSize);
    mainStrm.next_in = &zin[0];
    mainStrm.avail_in = blSize;
    do
    {
      mainStrm.next_out = &zOut[0];
      mainStrm.avail_out = block;
      mainRet = inflate(&mainStrm, Z_FINISH);
      out->blockWrite(&zOut[0], block - mainStrm.avail_out);
    }
    while( mainStrm.avail_out == 0 && mainRet == Z_BUF_ERROR);
    if( mainRet != Z_BUF_ERROR && mainRet != Z_STREAM_END )
    {
      break;
    }
  }
  inflateEnd(&mainStrm);
  headerSize = diffCount[index] * 5 + 7;
  return static_cast<int>(mainRet == Z_STREAM_END);
}

static int decodeZlib(File *in, uint64_t size, File *out, FMode mode, uint64_t &diffFound)
{
  const int block = 1u << 16;
  const int limit = 128;
  uint8_t zin[block];
  uint8_t zOut[block];
  int diffCount = min(in->getchar(), limit - 1);
  int window = in->getchar() - MAX_WBITS;
  int index = in->getchar();
  int memLevel = (index % 9) + 1;
  int cLevel = (index / 9) + 1;
  int len = 0;
  int diffPos[limit];
  diffPos[0] = -1;
  for( int i = 0; i <= diffCount; i++ )
  {
    int v = in->get32();
    if( i == diffCount ) {
      len = v + diffPos[i];
    } else {
      diffPos[i + 1] = v + diffPos[i] + 1;
    }
  }
  uint8_t diffByte[limit];
  diffByte[0] = 0;
  for( int i = 0; i < diffCount; i++ )
  {
    diffByte[i + 1] = in->getchar();
  }
  size -= 7 + 5 * diffCount;

  z_stream recStrm;
  int diffIndex = 1;
  int recPos = 0;
  recStrm.zalloc = Z_NULL;
  recStrm.zfree = Z_NULL;
  recStrm.opaque = Z_NULL;
  recStrm.next_in = Z_NULL;
  recStrm.avail_in = 0;
  int ret = deflateInit2(&recStrm, cLevel, Z_DEFLATED, window, memLevel, Z_DEFAULT_STRATEGY);
  if( ret != Z_OK )
  {
    return 0;
  }
  for( uint64_t i = 0; i < size; i += block )
  {
    uint32_t blSize = min(uint32_t(size - i), block);
    in->blockRead(&zin[0], blSize);
    recStrm.next_in = &zin[0];
    recStrm.avail_in = blSize;
    do
    {
      recStrm.next_out = &zOut[0];
      recStrm.avail_out = block;
      ret = deflate(&recStrm, i + blSize == size ? Z_FINISH : Z_NO_FLUSH);
      if( ret != Z_BUF_ERROR && ret != Z_STREAM_END && ret != Z_OK ) {
        break;
      }
      const int have = min(block - recStrm.avail_out, len - recPos);
      while( diffIndex <= diffCount && diffPos[diffIndex] >= recPos && diffPos[diffIndex] < recPos + have )
      {
        zOut[diffPos[diffIndex] - recPos] = diffByte[diffIndex];
        diffIndex++;
      }
      if( mode == FMode::FDECOMPRESS )
      {
        out->blockWrite(&zOut[0], have);
      }
      else if( mode == FMode::FCOMPARE )
      {
        for( int j = 0; j < have; j++ )
        {
          if( zOut[j] != out->getchar() && (diffFound == 0))
          {
            diffFound = recPos + j + 1;
          }
        }
      }
      recPos += have;

    } while( recStrm.avail_out == 0 );
  }
  while( diffIndex <= diffCount )
  {
    if( mode == FMode::FDECOMPRESS )
    {
      out->putChar(diffByte[diffIndex]);
    }
    else if( mode == FMode::FCOMPARE )
    {
      if( diffByte[diffIndex] != out->getchar() && (diffFound == 0))
      {
        diffFound = recPos + 1;
      }
    }
    diffIndex++;
    recPos++;
  }
  deflateEnd(&recStrm);
  return recPos == len ? len : 0;
}


#endif //DISABLE_ZLIB

static bool isGrayscalePalette(File *in, int n = 256, int isRGBA = 0)
{
  uint64_t offset = in->curPos();
  int stride = 3 + isRGBA;
  int res = (n > 0) << 8;
  int order = 1;
  for( int i = 0; (i < n * stride) && ((res >> 8) != 0); i++ )
  {
    int b = in->getchar();
    if( b == EOF)
    {
      res = 0;
      break;
    }
    if (i == 0)
    {
      res = 0x100 | b;
      order = 1 - 2 * static_cast<int>(b > int(ilog2(n) / 4));
      continue;
    }
    //"j" is the index of the current byte in this color entry
    int j = i % stride;
    if (j == 0)
    {
      // load first component of this entry
      int k = (b - (res & 0xFF)) * order;
      res = res & (static_cast<int>(k >= 0 && k <= 8) << 8);
      res |= (res) != 0 ? b : 0;
    }
    else if (j == 3)
    {
    res &= (static_cast<int>((b == 0) || (b == 0xFF)) * 0x1FF); // alpha/attribute component must be zero or 0xFF
    }
    else
    {
    res &= (static_cast<int>(b == (res & 0xFF)) * 0x1FF);
    }
  }
  in->setpos(offset);
  return (res >> 8) > 0;
}

// Checks whether a TIFF ColorMap (tag 320) represents a grayscale palette.
// Assumes little-endian TIFF ("II")
// TIFF ColorMap layout: all R entries (uint16 LE), then all G, then all B.
// nEntries is the number of entries per channel (e.g. 256 for an 8-bit image).
// fileOffset is the absolute file offset to the start of the ColorMap data.
// Returns true if R==G==B for all entries (i.e. the palette is grayscale).
static bool isTiffGrayscaleColorMap(File* in, uint64_t fileOffset, int nEntries)
{
  in->setpos(fileOffset);
  Array<uint8_t> rHi(nEntries);
  for (int e = 0; e < nEntries; ++e)
  {
    int lo = in->getchar();
    int hi = in->getchar();
    if (lo == EOF || hi == EOF)
        return false;
    rHi[e] = static_cast<uint8_t>(hi);
  }
  for (int e = 0; e < nEntries; ++e)
  {
    int lo = in->getchar();
    int hi = in->getchar();
    if (lo == EOF || hi == EOF)
        return false;
    if (static_cast<uint8_t>(hi) != rHi[e])
        return false;
  }
  for (int e = 0; e < nEntries; ++e)
  {
    int lo = in->getchar();
    int hi = in->getchar();
    if (lo == EOF || hi == EOF)
        return false;
    if (static_cast<uint8_t>(hi) != rHi[e])
        return false;
  }
  return true;
}

//for MRB detection:
//read compressed word,dword
uint16_t GetCWord(File* f)
{
  uint8_t b = f->getchar();
  if (b & 1)
      return ((f->getchar() << 8) | b) >> 1;
  return b >> 1;
}
uint32_t GetCDWord(File* f)
{
  uint16_t w = f->getchar();
  w = w | (f->getchar() << 8);
  if (w & 1)
  {
    uint16_t w1 = f->getchar();
    w1 = w1 | (f->getchar() << 8);
    return ((w1 << 16) | w) >> 1;
  }
  return w >> 1;
}

ALWAYS_INLINE
static bool is_base85(unsigned char c)
{
  return (isalnum(c) || (c == 13) || (c == 10) || (c == 'y') || (c == 'z') || (c >= '!' && c <= 'u'));
}

struct DetectionInfo
{
  uint64_t HeaderStart{};
  uint64_t HeaderLength{};
  uint64_t DataStart{};
  uint64_t DataLength{};
  BlockType Type{};
  int DataInfo{};

  void IMG_DET(uint64_t blockStart, BlockType type, uint64_t start_pos, uint32_t header_len, uint32_t width, uint32_t height)
  {
    Type = type;
    HeaderStart = blockStart + start_pos;
    HeaderLength = header_len;
    DataStart = HeaderStart + HeaderLength;
    DataLength = static_cast<uint64_t>(width) * height;
    DataInfo = width;
  }

  void AUD_DET(uint64_t blockStart, BlockType type, uint64_t start_pos, uint32_t header_len, uint64_t data_len, int wmode)
  {
    Type = type;
    HeaderStart = blockStart + start_pos;
    HeaderLength = header_len;
    DataStart = HeaderStart + HeaderLength;
    DataLength = data_len;
    DataInfo = wmode;
  }

  void MRB_DET(uint64_t blockStart, BlockType type, uint8_t packingMethod, uint16_t colorBits, uint64_t start_pos, uint32_t header_len, uint64_t data_len, int width, int height)
  {
    Type = type;
    HeaderStart = blockStart + start_pos;
    HeaderLength = header_len;
    DataStart = HeaderStart + HeaderLength;
    DataLength = data_len;
    DataInfo = (colorBits << 2 | packingMethod) << 24 | width << 12 | height;
  }

  void DBF_DET(uint64_t blockStart, BlockType type, uint64_t start_pos, uint32_t header_len, uint64_t data_len, int recordLength)
  {
    Type = type;
    HeaderStart = blockStart + start_pos;
    HeaderLength = header_len;
    DataStart = HeaderStart + HeaderLength;
    DataLength = data_len;
    DataInfo = recordLength;
  }

  bool SizeVerificationPassed(uint64_t nextBlockStart)
  {
    bool passed = DataStart + DataLength <= nextBlockStart;
    if (!passed)
      memset(this, 0, sizeof(DetectionInfo));
    return passed;
  }
};

struct TextDetectionInfo
{
  uint64_t DataStart{};
  uint64_t DataLength{};
  BlockType Type{}; // DEFAULT / TEXT / TEXT_EOL
};

// Detect text blocks (TEXT/TEXT_EOL) inside a DEFAULT block
static TextDetectionInfo detectText(File* in, uint64_t blockStart, uint64_t blockSize)
{
  TextDetectionInfo detectionInfo;

  TextParserStateInfo textParser;
  textParser.reset(0);

  in->setpos(blockStart);
  const uint64_t n = blockSize;
  uint32_t buf0 = 0;

  for (uint64_t i = 0; i < n; ++i)
  {
    int c = in->getchar();
    if (c == EOF)
    {
      quit("detectText(): Unexpected end of file");
    }
    uint8_t pc = buf0 & 0xff;
    buf0 = buf0 << 8 | c;

    uint32_t t = TextParserStateInfo::utf8StateTable[c];
    textParser.UTF8State =
      t == TextParserStateInfo::utf8Reject ? TextParserStateInfo::utf8Reject : //don't accept the non-pritable ascii chars
      TextParserStateInfo::utf8StateTable[256 + textParser.UTF8State + t];

    //some exceptions we still accept
    if (textParser.UTF8State == TextParserStateInfo::utf8Reject) // illegal state
    {
      if (c == 0 && pc >= 32 && pc <= 127 /* asciiz */)
      {
        textParser.UTF8State = TextParserStateInfo::utf8Accept;
      }
    }

    if (c == NEW_LINE)
    {
      if (pc != CARRIAGE_RETURN)
      {
        textParser.EOLType = 2; // mixed or LF-only
      }
      else if (textParser.EOLType == 0)
      {
        textParser.EOLType = 1; // CRLF-only
      }
    }

    if (textParser.UTF8State == TextParserStateInfo::utf8Accept)
    {
      textParser.invalidCount = textParser.invalidCount * (TextParserStateInfo::TEXT_ADAPT_RATE - 1) / TextParserStateInfo::TEXT_ADAPT_RATE;
      if (textParser.invalidCount == 0)
      {
        textParser.End = i;
      }
    }

    if (textParser.UTF8State == TextParserStateInfo::utf8Reject) // illegal state
    {
      textParser.invalidCount = textParser.invalidCount * (TextParserStateInfo::TEXT_ADAPT_RATE - 1) / TextParserStateInfo::TEXT_ADAPT_RATE + TextParserStateInfo::TEXT_ADAPT_RATE;
      textParser.UTF8State = TextParserStateInfo::utf8Accept; // reset state
      if (textParser.invalidCount >= TextParserStateInfo::TEXT_MAX_MISSES * TextParserStateInfo::TEXT_ADAPT_RATE)
      {

        //end of text block
        //if we have a large enough valid textblock, get it
        if (textParser.Start == 0 && textParser.isLargeText())
        {
          detectionInfo.Type = textParser.EOLType == 1 ? BlockType::TEXT_EOL : BlockType::TEXT;
          detectionInfo.DataStart = blockStart;
          detectionInfo.DataLength = textParser.End + 1;
          return detectionInfo;
        }

        textParser.reset(i + 1); // it's not text (or not long enough) - start over
      }
    }

    //early stop
    //we have a large enough text block, but it's not the first block
    if (textParser.Start != 0 && textParser.isLargeText())
    {
      detectionInfo.Type = BlockType::DEFAULT;
      detectionInfo.DataStart = blockStart;
      detectionInfo.DataLength = textParser.Start;
      return detectionInfo;
    }
  }

  //TEXT
  if (textParser.Start == 0 && (
    textParser.End + 1 == n || /*< the whole block is text, no matter how small */
    textParser.isSmallText())) /*< the whole block is a large enough text block with some garbage */
  {
    detectionInfo.Type = textParser.EOLType == 1 ? BlockType::TEXT_EOL : BlockType::TEXT;
    detectionInfo.DataStart = blockStart;
    detectionInfo.DataLength = n;
    return detectionInfo;
  }

  //DEFAULT (->TEXT)
  if (textParser.Start != 0 && textParser.isLargeText())
  {
    detectionInfo.Type = BlockType::DEFAULT;
    detectionInfo.DataStart = blockStart;
    detectionInfo.DataLength = textParser.Start; //could overshoot depending on how the inalidcount decays
    return detectionInfo;
  }

  //no text found at all, or it is too small
  //DEFAULT
  detectionInfo.Type = BlockType::DEFAULT;
  detectionInfo.DataStart = blockStart;
  detectionInfo.DataLength = n;
  return detectionInfo;
}

struct dBASE
{
  uint32_t nRecords{};
  uint16_t RecordLength{};
  uint16_t HeaderLength{};
  uint8_t Version{};
};

// Detect blocks
static DetectionInfo detect(File *in, uint64_t blockSize, const TransformOptions* const transformOptions)
{

  DetectionInfo detectionInfo;

  int textParserState = 0;
  uint64_t blockHash = 0;

  // TODO: Large file support
  const uint64_t n = blockSize;

  // last 16 bytes
  uint32_t buf3 = 0;
  uint32_t buf2 = 0;
  uint32_t buf1 = 0;
  uint32_t buf0 = 0;

  uint64_t start = 0;

  start = in->curPos(); // start of the current block

  // For EXE detection
  Array<uint64_t> absPos(256); // CALL/JMP abs. address. low byte -> last offset
  Array<uint64_t> relPos(256); // CALL/JMP relative address. low byte -> last offset
  int e8e9count = 0; // number of consecutive CALL/JMPs
  uint64_t e8e9pos = 0; // offset of first CALL or JMP instruction
  uint64_t e8e9last = 0; // offset of most recent CALL or JMP

  // For DEC Alpha detection
  int decAlpha = 0;
  uint64_t decAlphaHeaderStart = 0;
  uint16_t decAlphaNumberOfSections = 0;
  uint64_t decAlphaNextExpectedOffset = 0;
  uint64_t decAlphaSectionStart = 0;
  uint64_t decAlphaSectionLen = 0;

  // For JPEG detection
  uint64_t soi = 0; // Start Of Image
  uint64_t sof = 0; // Start Of Frame
  uint64_t sos = 0; // Start Of Scan
  uint64_t app = 0; // Application-specific marker
  uint64_t firstSoi = 0;

  // For WAVE detection
  uint64_t wavi = 0;
  int wavSize = 0; // filesize
  int wavch = 0; // number of channels: 1 or 2
  int wavbps = 0; // bits per sample: 8 or 16
  int wavm = 0;
  int wavType = 0; // 1 for WAV, 2 for SF2
  int wavLen = 0;
  uint64_t wavlist = 0; //position of a LIST info chunk (if present)

  // For AIFF detection
  uint64_t aiff = 0;
  int aiffm = 0;
  int aiffs = 0;

  // For S3M detection
  uint64_t s3mi = 0;
  int s3Mno = 0;
  int s3Mni = 0;

  // For MRB detection
  uint64_t mrb = 0;
  uint8_t mrbPictureType = 0;
  uint8_t mrbPackingMethod = 0;
  uint16_t mrbmulti = 0; // number of pictures in multi-resolution-bitmap minus 1

  // For BMP detection
  uint64_t bmpi = 0;
  uint64_t dibi = 0;
  int imgbpp = 0;
  int bmpx = 0;
  int bmpy = 0;
  int bmpof = 0;
  int bmps = 0;
  int nColors = 0;

  // For RGB detection
  uint64_t rgbi = 0;
  int rgbx = 0;
  int rgby = 0;

  // For TGA detection
  uint64_t tga = 0;
  int tgax = 0;
  int tgay = 0;
  int tgaz = 0;
  int tgat = 0;
  int tgaid = 0;
  int tgamap = 0;

  // For PBM (Portable BitMap), PGM (Portable GrayMap), PPM (Portable PixMap), PAM (Portable Arbitrary Map) detection
  uint64_t pgm = 0;
  int pgmComment = 0; // flag for presence of a comment line
  int pgmw = 0; // width
  int pgmh = 0; // height
  int pgmc = 0;  // color depth
  int pgmn = 0; // format: 4: PBM, 5: PGM 6: PPM, 7: PAM
  int pamatr = 0; // currently processed PAM attribute
  int pamd = 0; // PAM color depth (number of color channes)
  uint64_t pgmdata = 0; // image data start
  uint64_t pgmDataSize = 0; // image data size in bytes
  int pgmPtr = 0; // index in pgmBuf
  char pgmBuf[32];

  // For CD sectors detection
  uint64_t cdi = 0;
  int cda = 0;
  int cdm = 0;
  uint32_t cdf = 0;

  // For ZLIB stream detection
  uint8_t zBuf[256 + 32] = {0};
  uint8_t zin[1 << 16] = {0};
  uint8_t zout[1 << 16] = {0};

  // For DBF detection
  dBASE dbase{};
  uint64_t dbasei = 0;

  int zBufPos = 0;
  uint64_t zZipPos = UINT64_MAX;
  int histogram[256] = {0};
  int pdfIm = 0;
  int pdfImW = 0;
  int pdfImH = 0;
  int pdfImB = 0;
  int pdfGray = 0;
  uint64_t pdfimp = 0;

  // For base64 detection
  int b64S = 0;
  uint64_t b64I = 0;
  uint64_t b64Line = 0;
  uint64_t b64Nl = 0;
  uint64_t b64End = 0;

  // For base85 (ascii85) detection
  int b85state = 0; //0 or 1
  int b85linelength = 0;
  uint64_t base85start = 0;
  uint64_t base85end = 0;

  // For GIF detection
  uint64_t gifi = 0;
  uint64_t gifa = 0;
  int gif = 0;
  int gifw = 0;
  int gifc = 0;
  int gifb = 0;
  int gifplt = 0;
  static bool gifGray = false;

  // For PNG detection
  uint64_t png = 0;
  int pngw = 0;
  int pngh = 0;
  int pngbps = 0;
  int pngType = 0;
  int pnggray = 0;
  int lastChunk = 0;
  uint64_t nextChunk = 0;

  // Static state for multi-strip TIFF LZW detection.
  // Persists across detect() calls to emit one strip per call, similar to GIF's dett mechanism.
  static const int MAX_TIFF_STRIPS = 256;
  static struct
  {
    uint64_t offsets[MAX_TIFF_STRIPS];
    int      sizes[MAX_TIFF_STRIPS];
    int      info;
    int      count;
    int      next;
  } tiffStrips = {};

  // Continue emitting pending TIFF LZW strips from a previous detect() call
  if (tiffStrips.next > 0 && tiffStrips.next < tiffStrips.count)
  {
    const int s = tiffStrips.next++;
    detectionInfo.Type = BlockType::LZW;
    detectionInfo.DataInfo = tiffStrips.info;
    detectionInfo.DataStart = tiffStrips.offsets[s];
    detectionInfo.DataLength = tiffStrips.sizes[s];
    if (tiffStrips.next >= tiffStrips.count)
      tiffStrips.next = 0; // all strips emitted, reset
    return detectionInfo;
  }

  for (uint64_t i = 0; i < n; ++i)
  {
    int c = in->getchar();
    if (c == EOF)
    {
      quit("detect(): Unexpected end of file");
    }

    blockHash = hash(blockHash, c);

    buf3 = buf3 << 8 | buf2 >> 24;
    buf2 = buf2 << 8 | buf1 >> 24;
    buf1 = buf1 << 8 | buf0 >> 24;
    buf0 = buf0 << 8 | c;

    // helper for .pbm .pgm .ppm .pam detection
    uint32_t t = TextParserStateInfo::utf8StateTable[c];
    textParserState = TextParserStateInfo::utf8StateTable[256 + textParserState + t];

    // detect PNG images
    if ((png == 0) && buf3 == 0x89504E47 /*%PNG*/ && buf2 == 0x0D0A1A0A && buf1 == 0x0000000D && buf0 == 0x49484452)
    {
      png = i, pngType = -1, lastChunk = buf3;
    }
    if (png != 0)
    {
      const uint64_t p = i - png;
      if (p == 12)
      {
        pngw = buf2;
        pngh = buf1;
        pngbps = buf0 >> 24;
        pngType = static_cast<uint8_t>(buf0 >> 16); //Color type codes represent sums of the following values: 1 (palette used), 2 (color used), and 4 (alpha channel used). Valid values are 0, 2, 3, 4, and 6.
        pnggray = 0;
        png *= static_cast<int>((buf0 & 0xFFFF) == 0 && (pngw != 0) && (pngh != 0) && pngbps == 8 &&
          ((pngType == 0) || pngType == 2 || pngType == 3 || pngType == 4 || pngType == 6));
      }
      else if (p > 12 && pngType < 0)
      {
        png = 0;
      }
      else if (p == 17)
      {
        png *= static_cast<int>((buf1 & 0xFF) == 0);
        nextChunk = (png) != 0 ? i + 8 : 0;
      }
      else if (p > 17 && i == nextChunk)
      {
        nextChunk += buf1 + 4 /*CRC*/ + 8 /*Chunk length+id*/;
        lastChunk = buf0;
        png *= static_cast<int>(lastChunk != 0x49454E44 /*IEND*/);
        if (lastChunk == 0x504C5445 /*PLTE*/)
        {
          png *= static_cast<int>(buf1 % 3 == 0);
          pnggray = static_cast<int>((png != 0) && isGrayscalePalette(in, buf1 / 3));
        }
      }
    }

    // ZLIB stream detection
#ifndef DISABLE_ZLIB
    histogram[c]++;
    if (i >= 256)
      histogram[zBuf[zBufPos]]--;
    zBuf[zBufPos] = c;
    if (zBufPos < 32)
      zBuf[zBufPos + 256] = c;
    zBufPos = (zBufPos + 1) & 0xFF;

    int zh = parseZlibHeader(((int)zBuf[(zBufPos - 32) & 0xFF]) * 256 + (int)zBuf[(zBufPos - 32 + 1) & 0xFF]);
    bool valid = (i >= 31 && zh != -1);
    if (!valid && transformOptions->useBruteForceDeflateDetection && i >= 255)
    {
      uint8_t bType = (zBuf[zBufPos] & 7) >> 1;
      if ((valid = (bType == 1 || bType == 2)))
      {
        int maximum = 0, used = 0, offset = zBufPos;
        for (int i = 0; i < 4; i++, offset += 64)
        {
          for (int j = 0; j < 64; j++)
          {
            int freq = histogram[zBuf[(offset + j) & 0xFF]];
            used += (freq > 0);
            maximum += (freq > maximum);
          }
          if (maximum >= ((12 + i) << i) || used * (6 - i) < (i + 1) * 64)
          {
            valid = false;
            break;
          }
        }
      }
    }
    if (valid || zZipPos == i)
    {
      int streamLength = 0, ret = 0, brute = (zh == -1 && zZipPos != i);

      // Quick check possible stream by decompressing first 32 bytes
      z_stream strm;
      strm.zalloc = Z_NULL;
      strm.zfree = Z_NULL;
      strm.opaque = Z_NULL;
      strm.next_in = Z_NULL;
      strm.avail_in = 0;
      if (zlibInflateInit(&strm, zh) == Z_OK)
      {
        strm.next_in = &zBuf[(zBufPos - (brute ? 0 : 32)) & 0xFF];
        strm.avail_in = 32;
        strm.next_out = zout;
        strm.avail_out = 1 << 16;
        ret = inflate(&strm, Z_FINISH);
        ret = (inflateEnd(&strm) == Z_OK && (ret == Z_STREAM_END || ret == Z_BUF_ERROR) && strm.total_in >= 16);
      }
      if (ret)
      {
        // Verify valid stream and determine stream length
        const uint64_t savedpos = in->curPos();
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.next_in = Z_NULL;
        strm.avail_in = 0;
        strm.total_in = strm.total_out = 0;
        if (zlibInflateInit(&strm, zh) == Z_OK)
        {
          uint64_t blstart = static_cast<uint64_t>(std::max<int64_t>(i - (brute ? 255 : 31), 0));
          for (uint64_t j = blstart; j < n; j += 1 << 16)
          {
            uint32_t blsize = static_cast<uint32_t>(min(n - j, UINT64_C(1) << 16));
            in->setpos(start + j);
            if (in->blockRead(zin, blsize) != blsize)
              break;
            strm.next_in = zin;
            strm.avail_in = blsize;
            do
            {
              strm.next_out = zout;
              strm.avail_out = 1 << 16;
              ret = inflate(&strm, Z_FINISH);
            } while (strm.avail_out == 0 && ret == Z_BUF_ERROR);
            if (ret == Z_STREAM_END)
              streamLength = strm.total_in;
            if (ret != Z_BUF_ERROR)
              break;
          }
          if (inflateEnd(&strm) != Z_OK)
            streamLength = 0;
        }
        in->setpos(savedpos);
      }
      if (streamLength > (brute << 7))
      {
        int info = 0;
        if (pdfImW > 0 && pdfImW < 0x1000000 && pdfImH > 0)
        {
          if (pdfImB == 8 && (int)strm.total_out == pdfImW * pdfImH)
            info = ((pdfGray ? BlockType::IMAGE8GRAY : BlockType::IMAGE8) << 24) | pdfImW;
          if (pdfImB == 8 && (int)strm.total_out == pdfImW * pdfImH * 3)
            info = (BlockType::IMAGE24 << 24) | pdfImW * 3;
          if (pdfImB == 4 && (int)strm.total_out == ((pdfImW + 1) / 2) * pdfImH)
            info = (BlockType::IMAGE4 << 24) | ((pdfImW + 1) / 2);
          if (pdfImB == 1 && (int)strm.total_out == ((pdfImW + 7) / 8) * pdfImH)
            info = (BlockType::IMAGE1 << 24) | ((pdfImW + 7) / 8);
          pdfGray = 0;
        }
        else if (png && pngw < 0x1000000 && lastChunk == 0x49444154 /*IDAT*/)
        {
          if (pngbps == 8 && pngType == 2 /*color, no alpha*/ && (int)strm.total_out == (pngw * 3 + 1) * pngh)
            info = (BlockType::PNG24 << 24) | (pngw * 3), png = 0;
          else if (pngbps == 8 && pngType == 6 /*color, with alpha*/ && (int)strm.total_out == (pngw * 4 + 1) * pngh)
            info = (BlockType::PNG32 << 24) | (pngw * 4), png = 0;
          else if (pngbps == 8 && (pngType == 0 /*no color channels, no palette, i.e. grayscale*/ || pngType == 3 /*palette, color*/) && (int)strm.total_out == (pngw + 1) * pngh)
            info = (((pngType == 0 /*grayscale png*/ || pnggray) ? BlockType::PNG8GRAY : BlockType::PNG8) << 24) | (pngw), png = 0;
        }
        detectionInfo.Type = BlockType::ZLIB;
        detectionInfo.DataInfo = info;
        detectionInfo.DataLength = streamLength;
        detectionInfo.DataStart = start + i - (brute ? 255 : 31);
        return detectionInfo;
      }
    }
    if (zh == -1 && zBuf[(zBufPos - 32) & 0xFF] == 'P' && zBuf[(zBufPos - 32 + 1) & 0xFF] == 'K' &&
      zBuf[(zBufPos - 32 + 2) & 0xFF] == '\x3' && zBuf[(zBufPos - 32 + 3) & 0xFF] == '\x4' && zBuf[(zBufPos - 32 + 8) & 0xFF] == '\x8' &&
      zBuf[(zBufPos - 32 + 9) & 0xFF] == '\0')
    {
      int nlen = (int)zBuf[(zBufPos - 32 + 26) & 0xFF] + ((int)zBuf[(zBufPos - 32 + 27) & 0xFF]) * 256 +
        (int)zBuf[(zBufPos - 32 + 28) & 0xFF] + ((int)zBuf[(zBufPos - 32 + 29) & 0xFF]) * 256;
      if (nlen < 256 && i + 30 + nlen < n)
        zZipPos = i + 30 + nlen;
    }
#endif //DISABLE_ZLIB

    // dBASE VERSIONS
    //  '02' > FoxBase
    //  '03' > dBase III without memo file
    //  '04' > dBase IV without memo file
    //  '05' > dBase V without memo file
    //  '07' > Visual Objects 1.x
    //  '30' > Visual FoxPro
    //  '31' > Visual FoxPro with AutoIncrement field
    //  '43' > dBASE IV SQL table files, no memo
    //  '63' > dBASE IV SQL system files, no memo
    //  '7b' > dBase IV with memo file
    //  '83' > dBase III with memo file
    //  '87' > Visual Objects 1.x with memo file
    //  '8b' > dBase IV with memo file
    //  '8e' > dBase IV with SQL table
    //  'cb' > dBASE IV SQL table files, with memo
    //  'f5' > FoxPro with memo file - tested
    //  'fb' > FoxPro without memo file
    //
    if (dbasei == 0 && ((c & 7) == 3 /*dBase level 3-5*/ || (c & 7) == 4 /*dBase level 7*/ || (c >> 4) == 3 || c == 0xf5 || c == 0x30)) {
      dbasei = i + 1;
      dbase.Version = ((c >> 4) == 3) ? 3 : c & 7;
    }
    if (dbasei)
    {
      const int p = int(i - dbasei + 1);
      //1-2-3: Date of last update; in YYMMDD format
      if (p == 1)
      { if (c < 83) dbasei = 0; } //year (the DBF file type was introduced with dBASE II in 1983.)
      else if (p == 2)
      { if (!(c > 0 && c < 13)) dbasei = 0; } //month
      else if (p == 3)
      { if (!(c > 0 && c < 32)) dbasei = 0; }//day
      //4-7: Number of records in the table. (Least significant byte first.)
      else if (p == 7)
      { if (!((dbase.nRecords = bswap(buf0)) > 0 && dbase.nRecords < 0x40000000)) dbasei = 0; }
      //8-9: Number of bytes in the header. (Least significant byte first.)
      else if (p == 9)
      {
          if (!((dbase.HeaderLength = ((buf0 >> 8) & 0xff) | (c << 8)) > 32 && (((dbase.HeaderLength - 32 - 1) % 32) == 0 || (dbase.HeaderLength > 255 + 8 && (((dbase.HeaderLength -= 255 + 8) - 32 - 1) % 32) == 0))))
              dbasei = 0;
      }
      //10-11: Number of bytes in the record. (Least significant byte first.)
      else if (p == 11)
      {
          if (!(((dbase.RecordLength = ((buf0 >> 8) & 0xff) | (c << 8))) > 8 && dbase.HeaderLength + dbase.nRecords * dbase.RecordLength < blockSize))
              dbasei = 0;
      }
      //12-13: Reserved; filled with zeros.
      //14: Flag indicating incomplete dBASE IV transaction. 0 or 1.
      //15: dBASE IV encryption flag. 0 or 1.
      else if (p == 15)
      {
          if ((buf0 & 0xfffffefe) != 0)
              dbasei = 0;
      }
      //16-27: Reserved for multi - user processing.
      //28: Production .mdx file flag; 1 if there is a production .mdx file, 0 if not
      else if (p == 28)
      {
          if ((c & 0xfe) != 0)
          dbasei = 0;
      }
      //30-31: Reserved; filled with zeros.
      else if (p == 31)
      {
          if ((buf0 & 0xffff) != 0)
              dbasei = 0;
      }
      //32: for dBase III, IV and 5 the 'Field descriptor array' starts now and is n*32 bytes, for level 7 it starts at 68 and is n*48 bytes
      else if (p == 32)
      {
        uint64_t savedpos = in->curPos();
        in->setpos(savedpos - 34 + dbase.HeaderLength);
        uint8_t marker = in->getchar(); // field descriptor array terminator, it must be 0x0d
        if (marker != 0x0d)
        {
          dbasei = 0;
          in->setpos(savedpos);
        }
        else
        {
          uint32_t endPos = dbase.nRecords * dbase.RecordLength;
          uint64_t seekpos = endPos + in->curPos();
          in->setpos(seekpos);
          marker = in->getchar(); // file end marker, it must be 0x1a
          if (marker != 0x1a)
          {
            dbasei = 0;
            in->setpos(savedpos);
          } else {
            //success
            in->setpos(savedpos);
            detectionInfo.DBF_DET(start, BlockType::DBF, dbasei - 1, dbase.HeaderLength, dbase.nRecords* dbase.RecordLength + 1, dbase.RecordLength);
            return detectionInfo;
          }
        }
      }
    }

    if (i - pdfimp > 1024)
    {
      pdfIm = pdfImW = pdfImH = pdfImB = pdfGray = 0; // fail
    }
    if (pdfIm > 1 && !((isspace(c) != 0) || (isdigit(c) != 0)))
    {
      pdfIm = 1;
    }
    if (pdfIm == 2 && (isdigit(c) != 0))
    {
      pdfImW = pdfImW * 10 + (c - '0');
    }
    if (pdfIm == 3 && (isdigit(c) != 0))
    {
      pdfImH = pdfImH * 10 + (c - '0');
    }
    if (pdfIm == 4 && (isdigit(c) != 0))
    {
      pdfImB = pdfImB * 10 + (c - '0');
    }
    if ((buf0 & 0xffff) == 0x3c3c)
    {
      pdfimp = i, pdfIm = 1; // <<
    }
    if ((pdfIm != 0) && (buf1 & 0xffff) == 0x2f57 && buf0 == 0x69647468)
    {
      pdfIm = 2, pdfImW = 0; // /Width
    }
    if ((pdfIm != 0) && (buf1 & 0xffffff) == 0x2f4865 && buf0 == 0x69676874)
    {
      pdfIm = 3, pdfImH = 0; // /Height
    }
    if ((pdfIm != 0) && buf3 == 0x42697473 && buf2 == 0x50657243 && buf1 == 0x6f6d706f && buf0 == 0x6e656e74 &&
        zBuf[(zBufPos - 32 + 15) & 0xFF] == '/')
    {
      pdfIm = 4, pdfImB = 0; // /BitsPerComponent
    }
    if ((pdfIm != 0) && (buf2 & 0xFFFFFF) == 0x2F4465 && buf1 == 0x76696365 && buf0 == 0x47726179)
    {
      pdfGray = 1; // /DeviceGray
    }

    // CD sectors detection (mode 1 and mode 2 form 1+2 - 2352 bytes)
    if (buf1 == 0x00ffffff && buf0 == 0xffffffff && (cdi == 0))
    {
      cdi = i, cda = -1, cdm = 0;
    }
    if ((cdi != 0) && i > cdi)
    {
      const int p = (i - cdi) % 2352;
      if (p == 8 && (buf1 != 0xffffff00 || ((buf0 & 0xff) != 1 && (buf0 & 0xff) != 2)))
      {
        cdi = 0;
      }
      else if (p == 16 && i + 2336 < n)
      {
        uint8_t data[2352];
        const uint64_t savedPos = in->curPos();
        in->setpos(start + i - 23);
        in->blockRead(data, 2352);
        in->setpos(savedPos);
        int t = CdFilter::expandCdSector(data, cda, 1);
        if (t != cdm) {
          cdm = t * static_cast<int>(i - cdi < 2352);
        }
        if ((cdm != 0) && cda != 10 && (cdm == 1 || buf0 == buf1))
        {
          if (detectionInfo.Type != BlockType::CD)
          {
            detectionInfo.Type = BlockType::CD;
            detectionInfo.DataStart = start + cdi - 7;
            detectionInfo.DataInfo = cdm;
          }
          cda = (data[12] << 16) + (data[13] << 8) + data[14];
          if (cdm != 1 && i - cdi > 2352 && buf0 != cdf)
          {
            cda = 10;
          }
          if (cdm != 1) {
            cdf = buf0;
          }
        }
        else {
          cdi = 0;
        }
      }
      if ((i + 1 == n || cdi == 0) && detectionInfo.Type == BlockType::CD)
      {
        detectionInfo.DataLength = (start + i - p - 7) - detectionInfo.DataStart;
        return detectionInfo;
      }
    }
    if (detectionInfo.Type == BlockType::CD)
    {
      continue;
    }

    // Detect JPEG by code SOI APPx (FF D8 FF Ex) followed by
    // SOF0 (FF C0 xx xx 08) and SOS (FF DA) within a reasonable distance.
    // Detect end by any code other than RST0-RST7 (FF D9-D7) or
    // a byte stuff (FF 00).

    if ((soi == 0) && i >= 3 && (buf0 & 0xffffff00) == 0xffd8ff00 && ((buf0 & 0xFE) == 0xC0 || static_cast<uint8_t>(buf0) == 0xC4 ||
        (static_cast<uint8_t>(buf0) >= 0xDB && static_cast<uint8_t>(buf0) <= 0xFE)))
    {
      soi = i, app = i + 2, sos = sof = 0;
    }
    if (soi != 0)
    {
      if (app == i && (buf0 >> 24) == 0xff && ((buf0 >> 16) & 0xff) > 0xc1 && ((buf0 >> 16) & 0xff) < 0xff)
      {
        app = i + (buf0 & 0xffff) + 2;
      }
      if (app < i && (buf1 & 0xff) == 0xff && (buf0 & 0xfe0000ff) == 0xc0000008)
      {
        sof = i;
      }
      if ((sof != 0) && sof > soi && i - sof < 0x1000 && (buf0 & 0xffff) == 0xffda)
      {
        sos = i;
        if (firstSoi == 0)
        {
          firstSoi = soi;
        }
      }
      if (i - soi > 0x40000 && (sos == 0)) { // fail
        soi = 0;
      }
    }
    if (firstSoi != 0 && (sos != 0) && i > sos && (buf0 & 0xff00) == 0xff00 && (buf0 & 0xff) != 0 && (buf0 & 0xf8) != 0xd0)
    {
      detectionInfo.Type = BlockType::JPEG;
      detectionInfo.DataStart = start + (firstSoi - 3);
      detectionInfo.DataLength = i + 1 - (firstSoi - 3);
      return detectionInfo;
    }

    // Detect .wav file header
    if (buf0 == 0x52494646 /*RIFF*/)
    {
      wavi = i;
      wavm = wavLen = 0;
    }
    if (wavi != 0)
    {
      uint64_t p = i - wavi;
      if (p == 4)
      {
        wavSize = bswap(buf0); //fileSize
      }
      else if (p == 8)
      {
        wavType = (buf0 == 0x57415645 /*WAVE*/) ? 1 : (buf0 == 0x7366626B /*sfbk*/) ? 2 : 0;
        if (wavType == 0)
        {
          wavi = 0;
        }
      }
      else if (wavType != 0)
      {
        if (wavType == 1) // type: WAVE
        {
          if (p == 16 + wavLen && (buf1 != 0x666d7420 /*"fmt "*/ || ((wavm = bswap(buf0) - 16) & 0xFFFFFFFD) != 0))
          {
            wavLen = ((bswap(buf0) + 1) & (-2)) + 8, wavi *= static_cast<int>(buf1 == 0x666d7420 /*"fmt "*/ && (wavm & 0xFFFFFFFD) != 0);
          }
          else if (p == 22 + wavLen)
          {
            wavch = bswap(buf0) & 0xffff; // number of channels: 1 or 2
          }
          else if (p == 34 + wavLen)
          {
            wavbps = bswap(buf0) & 0xffff; // bits per sample: 8 or 16
          }
          else if (p == 40 + wavLen + wavm && buf1 != 0x64617461 /*"data"*/)
          {
            wavm += ((bswap(buf0) + 1) & (-2)) + 8, wavi = (wavm > 0xfffff ? 0 : wavi);
          }
          else if (p == 40 + wavLen + wavm)  // The "data" subchunk contains the actual audio data
          {
            int wavD = bswap(buf0); // size of data section
            wavLen = 0;
            if ((wavch == 1 || wavch == 2) && (wavbps == 8 || wavbps == 16) && wavD > 0 && wavSize >= wavD + 36 &&
                wavD % ((wavbps / 8) * wavch) == 0)
            {
              detectionInfo.AUD_DET(start, (wavbps == 8) ? BlockType::AUDIO : BlockType::AUDIO_LE, wavi - 3, 44 + wavm, wavD, wavch + wavbps / 4 - 3);
              if (detectionInfo.SizeVerificationPassed(start + n))
                return detectionInfo;
            }
            wavi = 0;
          }
        } else { // format: SF2
          if ((p == 16 && buf1 != 0x4C495354 /*LIST*/) || (p == 20 && buf0 != 0x494E464F /*INFO*/))
          {
            wavi = 0;
          }
          else if (p > 20 && buf1 == 0x4C495354 /*LIST*/) {
            wavLen = bswap(buf0);
            if (wavLen != 0) {
              wavlist = i;
            }
            else {
              wavi = 0; // fail; bad format
            }
          }
          else if (wavlist != 0) {
            p = i - wavlist;
            if (p == 8 && (buf1 != 0x73647461 /*sdta*/ || buf0 != 0x736D706C /*smpl*/)) {
              wavi = 0; // fail; bad format
            }
            else if (p == 12) {
              int wavD = bswap(buf0); // size of data section
              if ((wavD != 0) && (wavD + 12) == wavLen) {
                detectionInfo.AUD_DET(start, BlockType::AUDIO_LE, wavi - 3, (12 + wavlist - (wavi - 3) + 1) & ~1, wavD, 1 + 16 / 4 - 3 /*mono, 16-bit*/);
                if (detectionInfo.SizeVerificationPassed(start + n))
                  return detectionInfo;
              }
              wavi = 0; // fail; bad format
            }
          }
        }
      }
    }

    // Detect .aiff file header
    if (buf0 == 0x464f524d /*FORM*/)
    {
      aiff = i, aiffs = 0;
    }
    if (aiff != 0) {
      const uint64_t p = i - aiff;
      if (p == 12 && (buf1 != 0x41494646 /*AIFF*/ || buf0 != 0x434f4d4d /*COMM*/))
      {
        aiff = 0; // fail
      }
      else if (p == 24) {
        const int bits = buf0 & 0xffff;
        const int chn = buf1 >> 16;
        if ((bits == 8 || bits == 16) && (chn == 1 || chn == 2)) {
          aiffm = chn + bits / 4 - 3 + 4;
        }
        else {
          aiff = 0; //fail
        }
      }
      else if (p == 42 + aiffs && buf1 != 0x53534e44 /*SSND*/) {
        aiffs += (buf0 + 8) + (buf0 & 1);
        if (aiffs > 0x400) {
          aiff = 0; //fail
        }
      }
      else if (p == 42 + aiffs) { // Sound Data Chunk
        detectionInfo.AUD_DET(start, BlockType::AUDIO, aiff - 3, 54 + aiffs, buf0 - 8, aiffm);
        if (detectionInfo.SizeVerificationPassed(start + n))
          return detectionInfo;
        else
          aiff = 0; //fail
      }
    }

    // Detect .mod file header
    if ((buf0 == 0x4d2e4b2e || buf0 == 0x3643484e || buf0 == 0x3843484e // m.K. 6CHN 8CHN
        || buf0 == 0x464c5434 || buf0 == 0x464c5438) && (buf1 & 0xc0c0c0c0) == 0 && i >= 1083) {
      const uint64_t savedPos = in->curPos();
      const int chn = ((buf0 >> 24) == 0x36 ? 6 : (((buf0 >> 24) == 0x38 || (buf0 & 0xff) == 0x38) ? 8 : 4));
      int len = 0; // total length of samples
      int numPat = 1; // number of patterns
      for (int j = 0; j < 31; j++) {
        in->setpos(start + i - 1083 + 42 + j * 30);
        const int i1 = in->getchar();
        const int i2 = in->getchar();
        len += i1 * 512 + i2 * 2;
      }
      in->setpos(start + i - 131);
      for (int j = 0; j < 128; j++) {
        int x = in->getchar();
        if (x + 1 > numPat) {
          numPat = x + 1;
        }
      }
      if (numPat < 65) {
        detectionInfo.AUD_DET(start, BlockType::AUDIO, i - 1083, 1084 + numPat * 256 * chn, len, 4 /*mono, 8-bit*/);
        if (detectionInfo.SizeVerificationPassed(start + n))
          return detectionInfo;
      }
      in->setpos(savedPos);
    }

    // Detect .s3m file header
    if (buf0 == 0x1a100000 && i >= 31) { //0x1A: signature byte, 0x10: song type, 0x0000: reserved
      s3mi = i, s3Mno = s3Mni = 0;
    }
    if (s3mi != 0) {
      const uint64_t p = i - s3mi;
      if (p == 4) {
        s3Mno = bswap(buf0) & 0xffff; //Number of entries in the order table, should be even
        s3Mni = (bswap(buf0) >> 16); //Number of instruments in the song
      }
      else if (p == 16 && (((buf1 >> 16) & 0xff) != 0x13 || buf0 != 0x5343524d /*SCRM*/)) {
        s3mi = 0;
      }
      else if (p == 16) {
        const uint64_t savedPos = in->curPos();
        int b[31];
        int samStart = (1 << 16);
        int samEnd = 0;
        int ok = 1;
        for (int j = 0; j < s3Mni; j++) {
          in->setpos(start + s3mi - 31 + 0x60 + s3Mno + j * 2);
          int i1 = in->getchar();
          i1 += in->getchar() * 256;
          in->setpos(start + s3mi - 31 + i1 * 16);
          i1 = in->getchar();
          if (i1 == 1) { // type: sample
            for (int k = 0; k < 31; k++) {
              b[k] = in->getchar();
            }
            int len = b[15] + (b[16] << 8);
            int ofs = b[13] + (b[14] << 8);
            if (b[30] > 1) {
              ok = 0;
            }
            if (ofs * 16 < samStart) {
              samStart = ofs * 16;
            }
            if (ofs * 16 + len > samEnd) {
              samEnd = ofs * 16 + len;
            }
          }
        }
        if ((ok != 0) && samStart < (1 << 16)) {
          detectionInfo.AUD_DET(start, BlockType::AUDIO, s3mi - 31, samStart, samEnd - samStart, 0 /*mono, 8-bit*/);
          if (detectionInfo.SizeVerificationPassed(start + n))
            return detectionInfo;
        }
        s3mi = 0;
        in->setpos(savedPos);
      }
    }


    //detect uncompressed and rle encoded mrb files inside windows hlp files 506C
    //we support only single images
    if (!mrb && ((buf0 & 0xFFFF) == 0x6c70 || (buf0 & 0xFFFF) == 0x6C50) && !b64S && !cdi) { //Magic: 0x506C (SHG,lP) or 0x706C (MRB,lp)
      mrb = i;
      mrbmulti = 0;
    }
    if (mrb != 0) {
      const int p = int(i - mrb) - mrbmulti * 4; // p: offset of first picture descriptor
      if (p == 1 && c > 1 && c < 4 && mrbmulti == 0) mrbmulti = c - 1; //c: NumberOfPictures
      else if (p == 1 && c == 0) mrb = 0; // fail
      else if (p == 7) {
        if ((c == 5 || c == 6)) mrbPictureType = c; // 5=DDB   6=DIB   8=metafile
        else mrb = 0; // fail
      }
      else if (p == 8) {
        if (c <= 3) mrbPackingMethod = c; // c: 0=uncompressed 1=RunLen 2=LZ77 3=both
        else mrb = 0; // fail
      }
      else if (p == 10) {
        if (mrbPictureType == 6) { // DIB
          uint64_t mrbTell = in->curPos() - 2; //save curPos so we can restore it
          in->setpos(mrbTell);
          uint32_t Xdpi = GetCDWord(in);
          uint32_t Ydpi = GetCDWord(in);
          uint16_t Planes = GetCWord(in);
          uint16_t BitCount = GetCWord(in);  //4: 16 colors, 8: 256 colors
          uint32_t mrbw = GetCDWord(in);
          uint32_t mrbh = GetCDWord(in);
          uint32_t ColorsUsed = GetCDWord(in);
          uint32_t ColorsImportant = GetCDWord(in);
          uint32_t mrbcsize = GetCDWord(in); // compressedSize
          uint32_t HotspotSize = GetCDWord(in);
          uint32_t CompressedOffset = bswap(in->get32());
          uint32_t HotspotOffset = bswap(in->get32());
          uint64_t mrbsize = mrbcsize + in->curPos() - mrbTell + 10 + (1 << BitCount) * 4; // ignore HotspotSize
          mrbTell = mrbTell + 2;
          in->setpos(mrbTell);
          int pixelBytes = (mrbw * mrbh * BitCount) >> 3;
          //debug:
          //printf("MRB: %d, %d, %d, %d, %d, %d, %d, %d\n", mrbPictureType, mrbPackingMethod, BitCount, ColorsUsed, mrbw, mrbh, mrbcsize, mrbmulti);
          if (!(BitCount == 1 || BitCount == 4 || BitCount == 8) || mrbw < 4 || mrbh < 4 || mrbw > 1024 || mrbh >= 4096 || mrbsize == 0 || mrbPackingMethod > 1) {
            mrb = 0; // fail
          }
          else if (mrbPackingMethod <= 1 && pixelBytes < 360) {
            //debug:
            //printf("MRB: skipping\n");
            mrb = 0; // block is too small to be worth processing as a new block
          }
          else {
            // success
            detectionInfo.MRB_DET(start, BlockType::MRB, mrbPackingMethod, BitCount, mrb - 1, mrbsize - mrbcsize, mrbcsize, mrbw, mrbh);
            if (detectionInfo.SizeVerificationPassed(start + n))
              return detectionInfo;
            else
              mrb = 0;
          }
        }
        else {
          //unsupported format
          mrb = 0; // fail
        }
      }
    }

    // Detect .bmp image
    if ((bmpi == 0) && (dibi == 0)) {
      if ((buf0 & 0xffff) == 16973) { // 'BM'
        bmpi = i; // header start: bmpi-1
        dibi = i - 1 + 18; // we expect a DIB header to come
      }
      else if (buf0 == 0x28000000) { // headerless (DIB-only)
        dibi = i + 1;
      }
    }
    else {
      const uint64_t p = i - dibi + 1 + 18;
      if (p == 10 + 4) {
        bmpof = bswap(buf0), bmpi = (bmpof < 54 || start + blockSize < bmpi - 1 + bmpof) ? (dibi = 0)
          : bmpi; //offset of pixel data (this field is still located in the BMP Header)
      }
      else if (p == 14 + 4 && buf0 != 0x28000000) {
        bmpi = dibi = 0; //BITMAPINFOHEADER (0x28)
      }
      else if (p == 18 + 4) {
        bmpx = bswap(buf0), bmpi = (((bmpx & 0xff000000) != 0 || bmpx == 0) ? (dibi = 0) : bmpi); //width
      }
      else if (p == 22 + 4) {
        bmpy = abs(int(bswap(buf0))), bmpi = (((bmpy & 0xff000000) != 0 || bmpy == 0) ? (dibi = 0) : bmpi); //height
      }
      else if (p == 26 + 2) {
        bmpi = ((bswap(buf0 << 16)) != 1) ? (dibi = 0) : bmpi; //number of color planes (must be 1)
      }
      else if (p == 28 + 2) {
        imgbpp = bswap(buf0 << 16), bmpi = ((imgbpp != 1 && imgbpp != 4 && imgbpp != 8 && imgbpp != 24 && imgbpp != 32) ? (dibi = 0)
          : bmpi); //color depth
      }
      else if (p == 30 + 4) {
        bmpi = ((buf0 != 0) ? (dibi = 0) : bmpi); //compression method must be: BI_RGB (uncompressed)
      }
      else if (p == 34 + 4) {
        bmps = bswap(buf0); //image size or 0
        //else if (p==38+4) ; // the horizontal resolution of the image (ignored)
        //else if (p==42+4) ; // the vertical resolution of the image (ignored)
      }
      else if (p == 46 + 4) {
        nColors = bswap(buf0); // the number of colors in the color palette, or 0 to default to (1<<imgbpp)
        if (nColors == 0 && imgbpp <= 8) {
          nColors = 1 << imgbpp;
        }
        if (nColors > (1 << imgbpp) || (imgbpp > 8 && nColors > 0)) {
          bmpi = dibi = 0;
        }
      }
      else if (p == 50 + 4) { //the number of important colors used
        if (bswap(buf0) <= static_cast<uint32_t>(nColors) || bswap(buf0) == 0x10000000) {
          if (bmpi == 0 /*headerless*/ && (bmpx * 2 == bmpy) && imgbpp > 1 && // possible icon/cursor?
            ((bmps > 0 && bmps == ((bmpx * bmpy * (imgbpp + 1)) >> 4)) || (((bmps == 0) || bmps < ((bmpx * bmpy * imgbpp) >> 3)) &&
              ((bmpx == 8)  || (bmpx == 10) || (bmpx == 14) || (bmpx == 16) ||
               (bmpx == 20) || (bmpx == 22) || (bmpx == 24) ||
               (bmpx == 32) || (bmpx == 40) || (bmpx == 48) ||
               (bmpx == 60) || (bmpx == 64) || (bmpx == 72) ||
               (bmpx == 80) || (bmpx == 96) || (bmpx == 128) ||
               (bmpx == 256))))) {
            bmpy = bmpx;
          }

          BlockType blockType = BlockType::DEFAULT;
          int widthInBytes = 0;
          if (imgbpp == 1) {
            blockType = BlockType::IMAGE1;
            widthInBytes = (((bmpx - 1) >> 5) + 1) * 4;
          }
          else if (imgbpp == 4) {
            blockType = BlockType::IMAGE4;
            widthInBytes = ((bmpx * 4 + 31) >> 5) * 4;
          }
          else if (imgbpp == 8) {
            blockType = BlockType::IMAGE8;
            widthInBytes = (bmpx + 3) & -4;
          }
          else if (imgbpp == 24) {
            blockType = BlockType::IMAGE24;
            widthInBytes = ((bmpx * 3) + 3) & -4;
          }
          else if (imgbpp == 32) {
            blockType = BlockType::IMAGE32;
            widthInBytes = bmpx * 4;
          }

          if (imgbpp == 8) {
            const uint64_t colorPalettePos = dibi - 18 + 54;
            const uint64_t savedPos = in->curPos();
            in->setpos(colorPalettePos);
            if (isGrayscalePalette(in, nColors, 1)) {
              blockType = BlockType::IMAGE8GRAY;
            }
            in->setpos(savedPos);
          }

          const uint64_t headerPos = bmpi > 0 ? bmpi - 1 : dibi - 4;
          const uint64_t minHeaderSize = (bmpi > 0 ? 54 : 54 - 14) + nColors * 4;
          const uint64_t headerSize = bmpi > 0 ? bmpof : minHeaderSize;

          // some final sanity checks
          if (bmps != 0 &&
            bmps < widthInBytes * bmpy) { /*printf("\nBMP guard: image is larger than reported in header\n",bmps,widthInBytes*bmpy);*/
          }
          else if (start + blockSize < headerPos + headerSize + widthInBytes * bmpy) { /*printf("\nBMP guard: cropped data\n");*/
          }
          else if (headerSize == (bmpi > 0 ? 54 : 54 - 14) && nColors > 0) { /*printf("\nBMP guard: missing palette\n");*/
          }
          else if (bmpi > 0 && bmpof < minHeaderSize) { /*printf("\nBMP guard: overlapping color palette\n");*/
          }
          else if (bmpi > 0 && uint64_t(bmpi) - 1 + bmpof + widthInBytes * bmpy >
            start + blockSize) { /*printf("\nBMP guard: reported pixel data offset is incorrect\n");*/
          }
          else if (widthInBytes * bmpy <= 64) { /*printf("\nBMP guard: too small\n");*/
          } // too small - not worthy to use the image models
          else {
            detectionInfo.IMG_DET(start, blockType, headerPos, headerSize, widthInBytes, bmpy);
            if (detectionInfo.SizeVerificationPassed(start + n))
              return detectionInfo;
          }
        }
        bmpi = dibi = 0;
      }
    }

    // Detect binary .pbm .pgm .ppm .pam images
    if ((buf0 & 0xfff0ff) == 0x50300a) { //"Px" + line feed, where "x" shall be a number
      pgmn = (buf0 & 0xf00) >> 8; // extract "x"
      if ((pgmn >= 4 && pgmn <= 6) || pgmn == 7) {
        pgm = i, pgmdata = pgmDataSize = 0; // "P4" (pbm), "P5" (pgm), "P6" (ppm), "P7" (pam)
        pgmw = pgmh = pgmc = pamd = pgmComment = 0;
        pgmPtr = 0;
        pamatr = 0;
      }
    }
    if (pgm != 0) {
      if (pgmdata == 0) { // parse header
        if (i - pgm == 1 && c == '#') {
          pgmComment = 1; // # (pgm comment)
        }
        if ((pgmComment == 0) && (pgmPtr != 0)) {
          uint64_t s = 0;
          if (pgmn == 7) {
            if ((buf1 & 0xdfdf) == 0x5749 && (buf0 & 0xdfdfdfff) == 0x44544820) {
              pgmPtr = 0, pamatr = 1; // WIDTH
            }
            if ((buf1 & 0xdfdfdf) == 0x484549 && (buf0 & 0xdfdfdfff) == 0x47485420) {
              pgmPtr = 0, pamatr = 2; // HEIGHT
            }
            if ((buf1 & 0xdfdfdf) == 0x4d4158 && (buf0 & 0xdfdfdfff) == 0x56414c20) {
              pgmPtr = 0, pamatr = 3; // MAXVAL
            }
            if ((buf1 & 0xdfdf) == 0x4445 && (buf0 & 0xdfdfdfff) == 0x50544820) {
              pgmPtr = 0, pamatr = 4; // DEPTH
            }
            if ((buf2 & 0xdf) == 0x54 && (buf1 & 0xdfdfdfdf) == 0x55504c54 && (buf0 & 0xdfdfdfff) == 0x59504520) {
              pgmPtr = 0, pamatr = 5; // TUPLTYPE
            }
            if ((buf1 & 0xdfdfdf) == 0x454e44 && (buf0 & 0xdfdfdfff) == 0x4844520a) {
              pgmPtr = 0, pamatr = 6; // ENDHDR
            }
            if (c == 0x0a) {
              if (pamatr == 0) {
                pgm = 0;
              }
              else if (pamatr < 5) {
                s = pamatr;
              }
              if (pamatr != 6) {
                pamatr = 0;
              }
            }
          }
          else if (c == 0x20 && (pgmw == 0)) {
            s = 1;
          }
          else if (c == 0x0a && (pgmh == 0)) {
            s = 2;
          }
          else if (c == 0x0a && (pgmc == 0) && pgmn != 4) {
            s = 3;
          }
          if (s != 0) {
            pgmBuf[pgmPtr++] = 0;
            int v = atoi(pgmBuf); // parse width/height/depth/maxval value
            if (s == 1) {
              pgmw = v;
            }
            else if (s == 2) {
              pgmh = v;
            }
            else if (s == 3) {
              pgmc = v;
            }
            else if (s == 4) {
              pamd = v;
            }
            if (v == 0 || (s == 3 && v > 255)) {
              pgm = 0;
            }
            else {
              pgmPtr = 0;
            }
          }
        }
        if (pgmComment == 0) {
          pgmBuf[pgmPtr++] = c;
        }
        if (pgmPtr >= 32) {
          pgm = 0;
        }
        if ((pgmComment != 0) && c == 0x0a) {
          pgmComment = 0;
        }
        if ((pgmw != 0) && (pgmh != 0) && (pgmc == 0) && pgmn == 4) {
          pgmdata = i;
          pgmDataSize = (pgmw + 7) / 8 * pgmh;
          textParserState = 0; //start monitoring pixel data
        }
        if ((pgmw != 0) && (pgmh != 0) && (pgmc != 0) && (pgmn == 5 || (pgmn == 7 && pamd == 1 && pamatr == 6))) {
          pgmdata = i;
          pgmDataSize = pgmw * pgmh;
          textParserState = 0; //start monitoring pixel data
        }
        if ((pgmw != 0) && (pgmh != 0) && (pgmc != 0) && (pgmn == 6 || (pgmn == 7 && pamd == 3 && pamatr == 6))) {
          pgmdata = i;
          pgmDataSize = pgmw * 3 * pgmh;
          textParserState = 0; //start monitoring pixel data
        }
        if ((pgmw != 0) && (pgmh != 0) && (pgmc != 0) && (pgmn == 7 && pamd == 4 && pamatr == 6)) {
          pgmdata = i;
          pgmDataSize = pgmw * 4 * pgmh;
          textParserState = 0; //start monitoring pixel data
        }
      }
      else { // pixel data
        if (textParserState == TextParserStateInfo::utf8Reject || // for any sign of non-text data in pixel area
          (pgm - 2 == 0 && n - pgmDataSize == i)) // or the image is the whole file/block -> FINISH (success)
        {
          if ((pgmw != 0) && (pgmh != 0) && (pgmc == 0) && pgmn == 4) {
            detectionInfo.IMG_DET(start, BlockType::IMAGE1, pgm - 2, pgmdata - pgm + 3, (pgmw + 7) / 8, pgmh);
            if (detectionInfo.SizeVerificationPassed(start + n))
              return detectionInfo;
          }
          if ((pgmw != 0) && (pgmh != 0) && (pgmc != 0) && (pgmn == 5 || (pgmn == 7 && pamd == 1 && pamatr == 6))) {
            detectionInfo.IMG_DET(start, BlockType::IMAGE8GRAY, pgm - 2, pgmdata - pgm + 3, pgmw, pgmh);
            if (detectionInfo.SizeVerificationPassed(start + n))
              return detectionInfo;
          }
          if ((pgmw != 0) && (pgmh != 0) && (pgmc != 0) && (pgmn == 6 || (pgmn == 7 && pamd == 3 && pamatr == 6))) {
            detectionInfo.IMG_DET(start, BlockType::IMAGE24, pgm - 2, pgmdata - pgm + 3, pgmw * 3, pgmh);
            if (detectionInfo.SizeVerificationPassed(start + n))
              return detectionInfo;
          }
          if ((pgmw != 0) && (pgmh != 0) && (pgmc != 0) && (pgmn == 7 && pamd == 4 && pamatr == 6)) {
            detectionInfo.IMG_DET(start, BlockType::IMAGE32, pgm - 2, pgmdata - pgm + 3, pgmw * 4, pgmh);
            if (detectionInfo.SizeVerificationPassed(start + n))
              return detectionInfo;
          }
          pgm = 0; //fail
        }
        else if ((--pgmDataSize) == 0) {
          pgm = 0; // all data was probably text in pixel area: fail
        }
      }
    }

    // Detect .rgb image
    if ((buf0 & 0xffff) == 0x01da) {
      rgbi = i, rgbx = rgby = 0;
    }
    if (rgbi != 0) {
      const uint64_t p = i - rgbi;
      if (p == 1 && c != 0) {
        rgbi = 0;
      }
      else if (p == 2 && c != 1) {
        rgbi = 0;
      }
      else if (p == 4 && (buf0 & 0xffff) != 1 && (buf0 & 0xffff) != 2 && (buf0 & 0xffff) != 3) {
        rgbi = 0;
      }
      else if (p == 6) {
        rgbx = buf0 & 0xffff, rgbi = (rgbx == 0 ? 0 : rgbi);
      }
      else if (p == 8) {
        rgby = buf0 & 0xffff, rgbi = (rgby == 0 ? 0 : rgbi);
      }
      else if (p == 10) {
        int z = buf0 & 0xffff;
        if ((rgbx != 0) && (rgby != 0) && (z == 1 || z == 3 || z == 4)) {
          detectionInfo.IMG_DET(start, BlockType::IMAGE8, rgbi - 1, 512, rgbx, rgby * z);
          if (detectionInfo.SizeVerificationPassed(start + n))
            return detectionInfo;
        }
        rgbi = 0;
      }
    }

    // Detect .tiff file header (little-endian (II), uncompressed or LZW-compressed,
    // 1/4/8/24-bit color, single or multi-strip).
    // Note: big-endian (Motorola) is not supported
    if (buf1 == 0x49492a00 && n > i + static_cast<int>(bswap(buf0))) { // "II*\0"
      const uint64_t savedPos = in->curPos();
      // Seek to the Image File Directory (IFD); buf0 holds the IFD offset (4 bytes, LE).
      // We subtract 7 because 'i' points to the last byte of the 8-byte TIFF header,
      in->setpos(start + i + static_cast<uint64_t>(bswap(buf0)) - 7);

      // IFD layout: 2-byte entry count (read as two separate bytes here),
      // followed by 12-byte entries: tag(2) + type(2) + count(4) + value/offset(4)
      int dirSize = in->getchar(); // low byte of entry count
      int tifX = 0; // tag 256: image width in pixels
      int tifY = 0; // tag 257: image height in pixels
      int tifZ = 0; // tag 277: samples per pixel (1 = grayscale/palette, 3 = RGB)
      int tifZb = 0; // tag 258: bits per sample (1, 4, 8)
      int tifC = 0; // tag 259: compression (1 = none, 5 = LZW)
      int tifofval = 0; // 1 if strip offsets are already resolved (inline or read from array)

      // Tag 320 (ColorMap): only present for palette-color images (PhotometricInterpretation==3).
      // Stores 3 * 2^BitsPerSample uint16 values: all R entries, then all G, then all B.
      int tifColorMapOfs = 0; // absolute file offset to the ColorMap data, 0 = absent
      int tifColorMapLen = 0; // number of palette entries per channel (e.g. 256 for 8-bit)

      // Tag 273 (StripOffsets) and tag 279 (StripByteCounts): a TIFF image may be split
      // into multiple horizontal strips, each independently compressed.
      // We support up to 256 strips.
      uint64_t tifStripOffsets[MAX_TIFF_STRIPS] = {}; // absolute file offset of each strip
      int      tifStripSizes[MAX_TIFF_STRIPS] = {}; // compressed byte count of each strip
      int      tifStripCount = 0;                     // number of strips

      int b[12];
      if (in->getchar() == 0) { // high byte of entry count must be 0 (max 255 entries)
        for (int i = 0; i < dirSize; i++) {
          for (int j = 0; j < 12; j++) {
            b[j] = in->getchar();
          }
          if (b[11] == EOF)
            break;
          int tag = b[0] + (b[1] << 8);           // IFD tag identifier
          int tagFmt = b[2] + (b[3] << 8);        // data type: 3=SHORT(uint16), 4=LONG(uint32)
          int tagLen = b[4] + (b[5] << 8) + (b[6] << 16) + (b[7] << 24); // number of values
          int tagVal = b[8] + (b[9] << 8) + (b[10] << 16) + (b[11] << 24); // value or file offset to value array
          if (tagFmt == 3 || tagFmt == 4) {
            if (tag == 256) {
              tifX = tagVal; // ImageWidth
            }
            else if (tag == 257) {
              tifY = tagVal; // ImageLength (height)
            }
            else if (tag == 258) {
              // BitsPerSample: bits per channel.
              // For multi-channel images tagLen > 1 (one value per channel),
              // but all channels have the same depth so we just use 8 as default.
              tifZb = tagLen == 1 ? tagVal : 8; // bits per component
            }
            else if (tag == 259) {
              tifC = tagVal; // Compression: 1 = uncompressed, 5 = LZW
            }
            else if (tag == 273 && (tagFmt == 3 || tagFmt == 4)) {
              // StripOffsets: file offsets to each strip's data.
              // If tagLen == 1 the single value fits inline in the IFD entry.
              // If tagLen > 1 the value is a file offset to an array of offsets.
              const int bytesPerVal = (tagFmt == 3) ? 2 : 4; // SHORT or LONG
              if (tagLen == 1) {
                tifStripOffsets[0] = tagVal; // single strip, value is inline
                tifStripCount = 1;
                tifofval = 1; // already resolved, no indirection needed
              }
              else if (tagLen <= MAX_TIFF_STRIPS) {
                // tagVal is the file offset to the array of strip offsets
                const uint64_t savedPos2 = in->curPos();
                in->setpos(tagVal);
                tifStripCount = tagLen;
                for (int s = 0; s < tagLen; s++) {
                  uint64_t ofs = in->getchar();
                  ofs += (uint64_t)in->getchar() << 8;
                  if (bytesPerVal == 4) {
                    ofs += (uint64_t)in->getchar() << 16;
                    ofs += (uint64_t)in->getchar() << 24;
                  }
                  tifStripOffsets[s] = ofs;
                }
                in->setpos(savedPos2);
                tifofval = 1; // offsets already resolved from array
              }
            }
            else if (tag == 277) {
              tifZ = tagVal; // SamplesPerPixel: components per pixel
            }
            else if (tag == 279) {
              // StripByteCounts: compressed (or uncompressed) byte count for each strip.
              // If tagLen == 1 the single value fits inline in the IFD entry.
              // If tagLen > 1 the value is a file offset to an array of counts.
              const int bytesPerVal = (tagFmt == 3) ? 2 : 4; // SHORT or LONG
              if (tagLen == 1) {
                tifStripSizes[0] = tagVal; // single strip, value is inline
              }
              else if (tagLen <= MAX_TIFF_STRIPS) {
                // tagVal is the file offset to the array of strip byte counts
                const uint64_t savedPos2 = in->curPos();
                in->setpos(tagVal);
                for (int s = 0; s < tagLen; s++) {
                  int v = in->getchar();
                  v += in->getchar() << 8;
                  if (bytesPerVal == 4) {
                    v += in->getchar() << 16;
                    v += in->getchar() << 24;
                  }
                  tifStripSizes[s] = v;
                }
                in->setpos(savedPos2);
              }
            }
            // ColorMap tag (320): only present for palette-color images
            // (PhotometricInterpretation == 3).
            // tagFmt == 3 (SHORT), tagLen == 3 * 2^BitsPerSample entries total
            // (e.g. 768 for 8-bit: 256 R + 256 G + 256 B uint16 values).
            // Since tagLen > 2 always for a valid palette, tagVal is a file offset
            // to the palette data (doesn't fit inline in the 4-byte IFD value field).
            else if (tag == 320 && tagFmt == 3 && tagLen > 2) {
              tifColorMapOfs = tagVal;  // absolute file offset to the ColorMap data
              tifColorMapLen = tagLen / 3; // entries per channel (e.g. 256 for 8-bit)
            }
          }
        }
      }

      // Compute total compressed size across all strips
      int tifTotalSize = 0;
      for (int s = 0; s < tifStripCount; s++)
        tifTotalSize += tifStripSizes[s];

      const uint64_t tifofs = tifStripCount > 0 ? tifStripOffsets[0] : 0;

      if ((tifX != 0) && (tifY != 0) && (tifZb != 0) && (tifZ == 1 || tifZ == 3) &&
        ((tifC == 1) || (tifC == 5 && tifTotalSize > 0)) &&
        (tifofs != 0) && tifofs < (1 << 18) && tifofs + i < n)
      {
        if (tifC == 1) {
          // Uncompressed (BI_RGB): pixel data starts at tifofs
          if (tifZ == 1 && tifZb == 1) {
            detectionInfo.IMG_DET(start, BlockType::IMAGE1, i - 7, tifofs, ((tifX - 1) >> 3) + 1, tifY);
            return detectionInfo;
          }
          if (tifZ == 1 && tifZb == 8) {
            // 8-bit single channel: grayscale unless a ColorMap says otherwise
            BlockType tifType = BlockType::IMAGE8GRAY; // default: no ColorMap = grayscale
            if (tifColorMapOfs != 0 && tifColorMapLen > 0) {
              // ColorMap is present: seek to it and inspect all entries.
              // TIFF ColorMap layout is planar (all R, then all G, then all B),
              // unlike interleaved RGB — so we use isTiffGrayscaleColorMap()
              // rather than the generic isGrayscalePalette().
              const uint64_t cmapEnd = static_cast<uint64_t>(tifColorMapOfs) + tifColorMapLen * 6u;
              if (cmapEnd <= start + n) {
                tifType = isTiffGrayscaleColorMap(in, tifColorMapOfs, tifColorMapLen)
                  ? BlockType::IMAGE8GRAY
                  : BlockType::IMAGE8;
              }
              else {
                // ColorMap offset is past end of file: corrupted?
              }
            }
            detectionInfo.IMG_DET(start, tifType, i - 7, tifofs, tifX, tifY);
            return detectionInfo;
          }
          if (tifZ == 3 && tifZb == 8) {
            detectionInfo.IMG_DET(start, BlockType::IMAGE24, i - 7, tifofs, tifX * 3, tifY);
            return detectionInfo;
          }
        }
        else if (tifC == 5 && tifTotalSize > 0) {
          // LZW compressed: determine the image type and uncompressed row stride.
          // Each strip is an independent LZW stream (with its own RESET and EOI codes),
          // so multi-strip images are emitted one strip at a time via tiffStrips state.
          int widthInBytes;
          BlockType tifType;
          if (tifZb == 1) {
            tifType = BlockType::IMAGE1;
            widthInBytes = ((tifX - 1) / 8 + 1) * tifZ; // ceil(width/8) bytes per row
          }
          else if (tifZb == 4) {
            tifType = BlockType::IMAGE4;
            widthInBytes = (tifX + 1) / 2 * tifZ; // ceil(width/2) bytes per row, 2 pixels per byte
          }
          else { // tifZb == 8
            tifType = (tifZ == 1) ? BlockType::IMAGE8 : BlockType::IMAGE24;
            widthInBytes = tifX * tifZ; // one byte per channel per pixel
          }
          // Pack image type into high byte and row stride into low 24 bits,
          // matching the info encoding used by other image block types
          const int info = (static_cast<int>(tifType) << 24) | widthInBytes;

          if (tifStripCount == 1) {
            // Single strip: emit directly, no static state needed
            detectionInfo.Type = BlockType::LZW;
            detectionInfo.DataInfo = info;
            detectionInfo.DataStart = tifStripOffsets[0];
            detectionInfo.DataLength = tifStripSizes[0];
            return detectionInfo;
          }
          else {
            // Multi-strip: TIFF does not guarantee strips are contiguous in the file.
            // We only handle the contiguous case (strips laid out sequentially with
            // no gaps), since the LZW decoder expects a single linear byte range.
            // Non-contiguous strips fall through to DEFAULT.
            bool contiguous = true;
            for (int s = 1; s < tifStripCount; s++) {
              if (tifStripOffsets[s] != tifStripOffsets[s - 1] + tifStripSizes[s - 1]) {
                contiguous = false;
                break;
              }
            }
            if (contiguous) {
              // Store all strips in static state and emit the first strip now.
              // Subsequent detect() calls will emit strips 1..count-1 via the
              // early-return block at the top of detect().
              tiffStrips.count = tifStripCount;
              tiffStrips.info = info;
              tiffStrips.next = 1; // next call will emit strip 1
              for (int s = 0; s < tifStripCount; s++) {
                tiffStrips.offsets[s] = tifStripOffsets[s];
                tiffStrips.sizes[s] = tifStripSizes[s];
              }
              detectionInfo.Type = BlockType::LZW;
              detectionInfo.DataInfo = info;
              detectionInfo.DataStart = tiffStrips.offsets[0];
              detectionInfo.DataLength = tiffStrips.sizes[0];
              return detectionInfo;
            }
            // Non-contiguous strips: fall through to savedPos restore
          }
        }
      }
      in->setpos(savedPos);
    }

    // Detect .tga image (8-bit 256 colors or 24-bit uncompressed)
    if ((buf1 & 0xFFF7FF) == 0x00010100 && (buf0 & 0xFFFFFFC7) == 0x00000100 && (c == 16 || c == 24 || c == 32)) {
      tga = i, tgax = tgay, tgaz = 8, tgat = (buf1 >> 8) & 0xF, tgaid = buf1 >> 24, tgamap = c / 8;
    }
    else if ((buf1 & 0xFFFFFF) == 0x00000200 && buf0 == 0x00000000) {
      tga = i, tgax = tgay, tgaz = 24, tgat = 2;
    }
    else if ((buf1 & 0xFFF7FF) == 0x00000300 && buf0 == 0x00000000) {
      tga = i, tgax = tgay, tgaz = 8, tgat = (buf1 >> 8) & 0xF;
    }
    if (tga != 0) {
      if (i - tga == 8) {
        tga = (buf1 == 0 ? tga : 0), tgax = (bswap(buf0) & 0xffff), tgay = (bswap(buf0) >> 16);
      }
      else if (i - tga == 10) {
        if ((buf0 & 0xFFF7) == 32 << 8) {
          tgaz = 32;
        }
        if ((tgaz << 8) == static_cast<int>(buf0 & 0xFFD7) && (tgax != 0) && (tgay != 0) && uint32_t(tgax * tgay) < 0xFFFFFFF) {
          if (tgat == 1) {
            in->setpos(start + tga + 11 + tgaid);
            bool isGray = isGrayscalePalette(in);
            detectionInfo.IMG_DET(start, isGray ? BlockType::IMAGE8GRAY : BlockType::IMAGE8, tga - 7, 18 + tgaid + 256 * tgamap, tgax, tgay);
            if (detectionInfo.SizeVerificationPassed(start + n))
              return detectionInfo;
          }
          if (tgat == 2) {
            detectionInfo.IMG_DET(start, (tgaz == 24) ? BlockType::IMAGE24 : BlockType::IMAGE32, tga - 7, 18 + tgaid, tgax * (tgaz >> 3), tgay);
            if (detectionInfo.SizeVerificationPassed(start + n))
              return detectionInfo;
          }
          if (tgat == 3) {
            detectionInfo.IMG_DET(start, BlockType::IMAGE8GRAY, tga - 7, 18 + tgaid, tgax, tgay);
            if (detectionInfo.SizeVerificationPassed(start + n))
              return detectionInfo;
          }
          if (tgat == 9 || tgat == 11) {
            int info;
            const uint64_t savedPos = in->curPos();
            in->setpos(start + tga + 11 + tgaid);
            if (tgat == 9) {
              bool isGray = isGrayscalePalette(in);
              info = (isGray ? BlockType::IMAGE8GRAY : BlockType::IMAGE8) << 24;
              in->setpos(start + tga + 11 + tgaid + 256 * tgamap);
            }
            else {
              info = BlockType::IMAGE8GRAY << 24;
            }
            info |= tgax;
            // now detect compressed image data size
            uint32_t detd = 0;
            int c = in->getchar();
            int b = 0;
            int total = tgax * tgay;
            int line = 0;
            while (total > 0 && c >= 0 && (++detd, b = in->getchar()) >= 0) {
              if (c == 0x80) {
                c = b;
                continue;
              }
              if (c > 0x7F) {
                total -= (c = (c & 0x7F) + 1);
                line += c;
                c = in->getchar();
                detd++;
              }
              else {
                in->setpos(in->curPos() + c);
                detd += ++c;
                total -= c;
                line += c;
                c = in->getchar();
              }
              if (line > tgax) {
                break;
              }
              if (line == tgax) {
                line = 0;
              }
            }
            if (total == 0) {
              detectionInfo.Type = BlockType::RLE;
              detectionInfo.DataInfo = info;
              detectionInfo.DataStart = start + tga + 11 + tgaid + 256 * tgamap;
              detectionInfo.DataLength = detd;
              if (detectionInfo.SizeVerificationPassed(start + n))
                return detectionInfo;
            }
            in->setpos(savedPos);
          }
        }
        tga = 0;
      }
    }

    static BlockType dett = BlockType::DEFAULT;

    // Detect .gif
    if (detectionInfo.Type == BlockType::DEFAULT && dett == BlockType::GIF && i == 0) {
      dett = BlockType::DEFAULT;
      if (c == 0x2c || c == 0x21) { //image section or extension block
        gif = 2; // flag: image section or extension block
        gifi = 2; //jump 2 bytes
      }
      else {
        gifGray = 0;
      }
    }
    if ((gif == 0) && (buf1 & 0xffff) == 0x4749 /*GI*/ && (buf0 == 0x46383961 /*F89a*/ || buf0 == 0x46383761 /*F87a*/)) {
      gif = 1; // flag: found header
      gifi = i + 5; // position after the header
    }
    if (gif != 0) { //we are in a GIF file
      if (gif == 1 && i == gifi) {
        gif = 2;
        gifplt = (c & 128) != 0 ? (3 * (2 << (c & 7))) : 0;
        gifi = i + 5 + gifplt;
      }
      if (gif == 2 && (gifplt != 0) && i == gifi - gifplt - 3) {
        gifGray = isGrayscalePalette(in, gifplt / 3);
        gifplt = 0;
      }
      if (gif == 2 && i == gifi) {
        if ((buf0 & 0xff0000) == 0x210000) {
          gif = 5;
          gifi = i;
        }
        else if ((buf0 & 0xff0000) == 0x2c0000) {
          gif = 3;
          gifi = i;
        }
        else {
          gif = 0; //failed
        }
      }
      if (gif == 3 && i == gifi + 6) {
        gifw = (bswap(buf0) & 0xffff);
      }
      if (gif == 3 && i == gifi + 7) {
        gif = 4;
        gifc = gifb = 0;
        gifplt = (c & 128) != 0 ? (3 * (2 << (c & 7))) : 0;
        gifa = gifi = i + 2 + gifplt;
      }
      if (gif == 4 && (gifplt != 0)) {
        gifGray = isGrayscalePalette(in, gifplt / 3);
        gifplt = 0;
      }
      if (gif == 4 && i == gifi) {
        if (c > 0 && gifb != 0 && gifc != gifb) {
          gifw = 0;
        }
        if (c > 0) {
          gifb = gifc;
          gifc = c;
          gifi += c + 1;
        }
        else if (gifw == 0) {
          gif = 2;
          gifi = i + 3;
        }
        else {
          dett = BlockType::GIF;
          detectionInfo.Type = BlockType::GIF;
          detectionInfo.DataStart = start + gifa - 1;
          detectionInfo.DataLength = i - gifa + 2;
          detectionInfo.DataInfo = ((gifGray ? BlockType::IMAGE8GRAY : BlockType::IMAGE8) << 24) | gifw;
          return detectionInfo;
        }
      }
      if (gif == 5 && i == gifi) {
        if (c > 0) {
          gifi += c + 1;
        }
        else {
          gif = 2;
          gifi = i + 3;
        }
      }
    }

    // Detect x86/64 if the low order byte (little-endian) XX is more
    // recently seen (and within 4K) if a relative to absolute address
    // conversion is done in the context CALL/JMP (E8/E9) XX xx xx 00/FF
    // 4 times in a row.  Detect end of EXE at the last
    // place this happens when it does not happen for 64KB.

    if (((buf1 & 0xfe) == 0xe8 || (buf1 & 0xfff0) == 0x0f80) && ((buf0 + 1) & 0xfe) == 0) {
      uint64_t r = buf0 >> 24; // relative address low 8 bits
      uint64_t a = ((buf0 >> 24) + i) & 0xff; // absolute address low 8 bits
      uint64_t rDist = i - relPos[r];
      uint64_t aDist = i - absPos[a];
      if (aDist < rDist && aDist < 0x800 && absPos[a] > 5) {
        e8e9last = i;
        ++e8e9count;
        if (e8e9pos == 0 || e8e9pos > absPos[a]) {
          e8e9pos = absPos[a];
        }
      }
      else {
        e8e9count = 0;
      }
      if (detectionInfo.Type == BlockType::DEFAULT && e8e9count >= 4 && e8e9pos > 5) {
        detectionInfo.Type = BlockType::EXE;
        detectionInfo.DataStart = start + e8e9pos - 5;
      }
      absPos[a] = i;
      relPos[r] = i;
    }
    if (i + 1 == n || i - e8e9last > 0x4000) {
      // TODO: Large file support
      if (detectionInfo.Type == BlockType::EXE) {
        detectionInfo.DataInfo = static_cast<int>(detectionInfo.DataStart);
        detectionInfo.DataLength = start + e8e9last - detectionInfo.DataStart;
        return detectionInfo;
      }
      e8e9pos = 0;
      e8e9count = 0;
    }

    //DEC Alpha code section detection
    //Based on the Tru64 Object file format
    //see: https://www3.physnet.uni-hamburg.de/physnet/Tru64-Unix/HTML/APS31DTE/DOCU_013.HTM
    //note: this document^ doesn't cover the same version as in silesia/mozilla
    //luckily the 3 sections containing DEC Alpha code are always adjacent to each other
    //so we can treat them as one continuous section.

    if (decAlpha == 0 && ((buf3 >> 16) == 0x8301 /*Target-machine magic number*/ || buf1 == 0 && buf0 == 0 /*no symbolic header*/)) {
      uint16_t numberOfSections = bswap(buf3) >> 16;
      if (numberOfSections >= 3 && numberOfSections <= 24) {
        decAlpha = 1;
        decAlphaHeaderStart = i - 15;
        decAlphaNumberOfSections = numberOfSections;
      }
    }

    if (decAlpha != 0) {
      if (decAlpha == 1) { // sanity checks
        if (i == decAlphaHeaderStart + 23) {
          //size of optional header is always 80
          uint16_t sizeOfOptionalHeader = bswap(buf0)&0xffff;
          //flags:
          //0x2005/0x2007 - Dynamically shared object (so)
          //0x3007        - Dynamic executable file
          uint16_t flags = bswap(buf0) >> 16;
          if (sizeOfOptionalHeader != 80 || !(flags == 0x2005 || flags == 0x2007 || flags == 0x3007)) {
            decAlpha = 0; //fail
          }
        }
        else if (i == decAlphaHeaderStart + 24 + 80) {
          if (c != '.') { //section names start with a dot (this is the first section)
            decAlpha = 0; //fail
          }
        }
        else if (i > decAlphaHeaderStart + 24 + 80 + decAlphaNumberOfSections * 64) {
          decAlpha = 0; //fail
        }
      }

      //we look for the 3 (adjacent) sections containing program code
      if (decAlpha == 1 && buf1 == 0x2E746578 && buf0 == 0x74000000) { // ".text   "
        decAlpha = 2;
        decAlphaNextExpectedOffset = i + 32;
      }
      else if (decAlpha == 2 && i == decAlphaNextExpectedOffset)
      {
        decAlphaSectionStart = ((uint64_t)bswap(buf0)) << 32 | bswap(buf1);
        decAlphaSectionLen = ((uint64_t)bswap(buf2)) << 32 | bswap(buf3);
        decAlpha = 3;
      }
      else if (decAlpha == 3 && buf1 == 0x2E696E69 && buf0 == 0x74000000) // ".init   "
      {
        decAlpha = 4;
        decAlphaNextExpectedOffset = i + 32;
      }
      else if (decAlpha == 4 && i == decAlphaNextExpectedOffset)
      {
        uint64_t initstart = ((uint64_t)bswap(buf0)) << 32 | bswap(buf1);
        if (initstart == decAlphaSectionStart + decAlphaSectionLen)
        {
          decAlphaSectionLen += ((uint64_t)bswap(buf2)) << 32 | bswap(buf3);
          decAlpha = 5;
        }
        else
        {
          //printf("DECALPHA - found .text and .init sections are not adjacent");
          decAlpha = 0; //fail
        }
      }
      else if (decAlpha == 5 && buf1 == 0x2E66696E && buf0 == 0x69000000) // ".fini   "
      {
        decAlpha = 6;
        decAlphaNextExpectedOffset = i + 32;
      }
      else if (decAlpha == 6 && i == decAlphaNextExpectedOffset)
      {
        uint64_t finistart = ((uint64_t)bswap(buf0)) << 32 | bswap(buf1);
        if (finistart == decAlphaSectionStart + decAlphaSectionLen)
        {
          decAlphaSectionLen += ((uint64_t)bswap(buf2)) << 32 | bswap(buf3);
          if (decAlphaHeaderStart + decAlphaSectionStart + decAlphaSectionLen <= n)
          {
            detectionInfo.Type = BlockType::DEC_ALPHA;
            detectionInfo.DataStart = start + decAlphaHeaderStart + decAlphaSectionStart;
            detectionInfo.DataLength = decAlphaSectionLen;
            return detectionInfo;
          } else {
            //printf("DECALPHA - end of section is past end of file");
            decAlpha = 0; //fail
          }
        }
        else {
          //printf("DECALPHA - found .init and .fini sections are not adjavent");
          decAlpha = 0; //fail
        }
      }
    }

    // this is the old DEC Alpha detection logic
    // unused - kept for reference only
#ifdef USE_OLD_DECALPHA_DETECTION

    struct {
      Array<uint64_t> absPos{ 256 * 4 };
      Array<uint64_t> relPos{ 256 * 4 };
      uint32_t opcode = 0, idx = 0, count[4] = { 0 }, branches[4] = { 0 };
      uint64_t offset[4] = { 0 }, last[4] = { 0 };
    } DEC;

    DEC.idx = i & 3;
    DEC.opcode = bswap(buf0);
    bool isValidDecAlphaInstruction = (i >= 3) && DECAlpha::IsValidInstruction(DEC.opcode);
    if (DEC.count[DEC.idx] == 0 && buf0 == 0 && buf1 == 0 /*ignore lead-in padding*/)
      isValidDecAlphaInstruction = false;
    if (isValidDecAlphaInstruction && buf0 == 0 && buf1 == 0 && buf2 == 0 && buf3 == 0 /*stop when lead-out padding*/)
      isValidDecAlphaInstruction = false;
    DEC.count[DEC.idx] = isValidDecAlphaInstruction ? DEC.count[DEC.idx] + 1 : DEC.count[DEC.idx] = 0;
    DEC.opcode >>= 21;
    //test if bsr opcode and if last 4 opcodes are valid
    if (
      (DEC.opcode == (0x34 << 5) + 26) &&
      (DEC.count[DEC.idx] > 4) &&
      ((e8e9count == 0) && !soi && !pgm && !rgbi && !bmpi && !wavi && !tga)
      ) {
      uint32_t const absAddrLSB = DEC.opcode & 0xFF; // absolute address low 8 bits
      uint32_t const relAddrLSB = ((DEC.opcode & 0x1FFFFF) + static_cast<uint32_t>(i) / 4u) & 0xff; // relative address low 8 bits
      uint64_t const absPos = DEC.absPos[absAddrLSB * 4 + DEC.idx];
      uint64_t const relPos = DEC.relPos[relAddrLSB * 4 + DEC.idx];
      uint64_t const curPos = i - 3;
      if ((absPos > relPos) && (curPos < absPos + UINT64_C(0x8000)) && (absPos > 16) && (curPos > absPos + UINT64_C(16)) && (((curPos - absPos) & UINT64_C(3)) == 0)) {
        DEC.last[DEC.idx] = curPos;
        DEC.branches[DEC.idx]++;
        if ((DEC.offset[DEC.idx] == 0) || (DEC.offset[DEC.idx] > DEC.absPos[absAddrLSB])) {
          uint64_t const addr = curPos - (DEC.count[DEC.idx] - 1) * UINT64_C(4);
          DEC.offset[DEC.idx] = std::min<uint64_t>(DEC.absPos[absAddrLSB * 4 + DEC.idx], addr);
        }
      }
      else
        DEC.branches[DEC.idx] = 0;
      DEC.absPos[absAddrLSB * 4 + DEC.idx] = DEC.relPos[relAddrLSB * 4 + DEC.idx] = curPos;
    }

    if (DEC.last[DEC.idx] != 0) {
      if ((detectionInfo.Type == BlockType::DEFAULT) && (DEC.branches[DEC.idx] >= 16) && DEC.count[DEC.idx] >= 64) {
        detectionInfo.Type = BlockType::DEC_ALPHA;
        detectionInfo.DataStart = start + DEC.offset[DEC.idx];
        for (int i = 0; i < 4; i++) {
          if (i != DEC.idx) {
            DEC.last[i] = 0;
            DEC.offset[i] = 0;
            DEC.branches[i] = 0;
          }
        }
      }

      else if ((detectionInfo.Type == BlockType::DEFAULT) && (i > DEC.last[DEC.idx] + UINT64_C(0x8000))) {
        DEC.last[DEC.idx] = 0;
        DEC.offset[DEC.idx] = 0;
        DEC.branches[DEC.idx] = 0;
      }

      else if (detectionInfo.Type == BlockType::DEC_ALPHA
        && (start + DEC.offset[DEC.idx]-detectionInfo.DataStart) % 4 == 0
        && ((i + 4 >= n) || (i > DEC.last[DEC.idx] + UINT64_C(0x1000) && DEC.count[DEC.idx] == 0))) {
        detectionInfo.DataStart = start + DEC.offset[DEC.idx];
        detectionInfo.DataLength = (start + DEC.last[DEC.idx]) - detectionInfo.DataStart;
        return detectionInfo;
      }
    }

#endif //USE_OLD_DECALPHA_DETECTION

    // Detect base64 encoded data
    if( b64S == 0 && buf0 == 0x73653634 && ((buf1 & 0xffffff) == 0x206261 || (buf1 & 0xffffff) == 0x204261))
    {
      b64S = 1; b64I = i - 6; //' base64' ' Base64'
    }
    if( b64S == 0 && ((buf1 == 0x3b626173 && buf0 == 0x6536342c) || (buf1 == 0x215b4344 && buf0 == 0x4154415b)))
    {
      b64S = 3; b64I = i + 1; // ';base64,' '![CDATA['
    }
    if( b64S > 0 )
    {
      if( b64S == 1 && buf0 == 0x0d0a0d0a )
      {
        b64I = i + 1; b64Line = 0; b64S = 2;
      }
      else if( b64S == 2 && (buf0 & 0xffff) == 0x0d0a && b64Line == 0 )
      {
        b64Line = i + 1 - b64I; b64Nl = i;
      }
      else if( b64S == 2 && (buf0 & 0xffff) == 0x0d0a && b64Line > 0 && (buf0 & 0xffffff) != 0x3d0d0a )
      {
        if( i - b64Nl < b64Line && buf0 != 0x0d0a0d0a )
        {
          b64End = i - 1; b64S = 5;
        }
        else if( buf0 == 0x0d0a0d0a )
        {
          b64End = i - 3 /*remove the last 0d0a*/; b64S = 5;
        }
        else if( i - b64Nl == b64Line )
        {
          b64Nl = i;
        } else {
          b64S = 0;
        }
      }
      else if( b64S == 2 && (buf0 & 0xffffff) == 0x3d0d0a )
      {
        b64End = i - 1; b64S = 5; // '=' or '=='
      }
      else if( b64S == 2 && !(isalnum(c) || c == '+' || c == '/' || c == 10 || c == 13 || c == '='))
      {
        b64S = 0;
      }
      if( b64Line > 0 && (b64Line <= 4 || b64Line > 255))
      {
        b64S = 0;
      }
      if (b64S == 3 && i >= b64I && !(isalnum(c) || c == '+' || c == '/' || c == '='))
      {
        b64End = i;
        b64S = 4;
      }
      if((b64S == 4 && b64End - b64I > 32) || (b64S == 5 && b64End - b64I > 32 && b64End - b64I < (1 << 27)))
      {
        detectionInfo.Type = BlockType::BASE64;
        detectionInfo.DataStart = start + b64I;
        detectionInfo.DataLength = b64End - b64I;
        return detectionInfo;
      }
      if( b64S > 3 )
      {
        b64S = 0;
      }
      if( b64S == 1 && i - b64I >= 128 )
      {
        b64S = 0; // detect false positives after 128 bytes
      }
    }

    //detect base85 (ascii85) encoded data
    //headers: stream\n stream\r\n oNimage\n utimage\n \nimage\n
    if (b85state == 0 && ((buf0 == 0x65616D0A && (buf1 & 0xffffff) == 0x737472) || (buf0 == 0x616D0D0A && buf1 == 0x73747265) || (buf0 == 0x6167650A && buf1 == 0x6F4E696D) || (buf0 == 0x6167650A && buf1 == 0x7574696D) || (buf0 == 0x6167650A && (buf1 & 0xffffff) == 0x0A696D)))
    {
        b85state = 1;
        base85start = i;
        b85linelength = 0;
    }
    else if (b85state == 1)
    {
      if (c == CARRIAGE_RETURN && b85linelength == 0)
      {
        b85linelength = i - base85start; //capture line lenght
        if (b85linelength <= 25 || b85linelength > 255)
          b85state = 0; //fail
      }
      else if (c == '~') { //end marker
        base85end = i - 1;
        b85state = 0;
        if (((base85end - base85start) > 60) && ((base85end - base85start) < 0x8000000))
        {
          detectionInfo.Type = BlockType::BASE85;
          detectionInfo.DataStart = start + base85start + 1;
          detectionInfo.DataLength = base85end - base85start;
          return detectionInfo;
        }
      }
      else if (is_base85(c))
      {
        // still ok
      }
      else if (c == CARRIAGE_RETURN && b85linelength != 0)
      {
        if (b85linelength != i - base85start)
          b85state = 0; //fail
      } else {
        b85state = 0; //fail
      }
    }

    // UStar (Unix Standard TAR) detection
    // Notable uses: silesia/mozilla, silesia/samba, silesia/xml
    if (buf1 == 0x75737461 /* "usta" */ && (buf0 == 0x72202000 /* "r  \0" */ || buf0 == 0x72003030 /* "r\000" */))
    {
      uint64_t posBackup = in->curPos();
      TarFilter tarFilter;
      bool success = tarFilter.detect(in, start + blockSize);
      if (success)
      {
        detectionInfo.Type = BlockType::TAR;
        detectionInfo.DataStart = tarFilter.detectedStartPos;
        detectionInfo.DataLength = tarFilter.detectedEndPos - tarFilter.detectedStartPos;
        return detectionInfo;
      }
      in->setpos(posBackup);
    }

  }

  if (detectionInfo.Type != BlockType::DEFAULT)
    quit("detect(): detection didn't finish properly.");

  //special detections (using hash signature)
  //we could detect these *types* of content properly, but since
  //they are rare, it's just much simpler to detect only those files
  //we are targeting

  //printf("BlockHash: %" PRIu64 "\n", blockHash);

  if (blockHash == UINT64_C(16175250862432333790) && blockSize == 513216) //calgary/pic; canterbury/ptt5
  {
    detectionInfo.IMG_DET(start, BlockType::IMAGE1, 0, 0, 1728 / 8, 2376);
    return detectionInfo;
  }

  //nothing detected
  detectionInfo.Type = BlockType::DEFAULT;
  detectionInfo.DataStart = start + n;
  detectionInfo.DataLength = 0;

  return detectionInfo;
}


/**
 * 24/32-bit image data transforms, controlled by OPTION_SKIPRGB:
 *   - simple color transform (b, g, r) -> (g, g-r, g-b)
 *   - channel reorder only (b, g, r) -> (g, r, b)
 * Detects RGB565 to RGB888 conversions.
 */
class BmpFilter : public Filter {
private:
  int stride = 3; //3: RGB or BGR, 4: RGBA or BGRA
  int width = 0;
  bool skipRgb = false;
  bool isPossibleRgb565 = true;
  uint32_t rgb565Run = 0;
  static constexpr int rgb565MinRun = 63;
public:

  void setWidth(int w) {
    this->width = w;
  }
  void setSkipRgb(bool skipRgb) {
    this->skipRgb = skipRgb;
  }
  void setHasApha() {
    this->stride = 4;
    this->isPossibleRgb565 = false; //to fix false positives (and to keep compatibility with previous version)
  }

  void encode(File *in, File *out, uint64_t size, int width, int & /*headerSize*/) override {
    uint32_t r = 0;
    uint32_t g = 0; // green is always the middle channel in both RGB and BGR images, the transform is symmetric in r and b
    uint32_t b = 0;
    for( int i = 0; i < static_cast<int>(size / width); i++ ) {
      for( int j = 0; j < width / stride; j++ ) {
        b = in->getchar();
        g = in->getchar();
        r = in->getchar();
        if( isPossibleRgb565 ) {
          int rgb565RunPrevious = rgb565Run;
          rgb565Run = min(rgb565Run + 1, 0xFFFF) *
                  static_cast<int>((b & 7) == ((b & 8) - ((b >> 3) & 1)) && (g & 3) == ((g & 4) - ((g >> 2) & 1)) &&
                                    (r & 7) == ((r & 8) - ((r >> 3) & 1)));
          if( rgb565Run > rgb565MinRun || rgb565RunPrevious >= rgb565MinRun ) {
            b ^= (b & 8) - ((b >> 3) & 1);
            g ^= (g & 4) - ((g >> 2) & 1);
            r ^= (r & 8) - ((r >> 3) & 1);
          }
          isPossibleRgb565 = rgb565Run > 0;
        }
        if (!skipRgb) {
          r = g - r;
          b = g - b;
        }
        out->putChar(g);
        out->putChar(r);
        out->putChar(b);
        if (stride == 4) {
          out->putChar(in->getchar());
        }
      }
      for( int j = 0; j < width % stride; j++ ) {
        out->putChar(in->getchar());
      }
    }
    for( int i = size % width; i > 0; i-- ) {
      out->putChar(in->getchar());
    }
  }

  uint64_t decode(File * /*in*/, File *out, FMode fMode, uint64_t size, uint64_t &diffFound) override {
    uint32_t r = 0;
    uint32_t g = 0;
    uint32_t b = 0;
    uint32_t a = 0;
    uint32_t p = 0;
    for( int i = 0; i < static_cast<int>(size / width); i++ ) {
      p = i * width;
      for( int j = 0; j < width / stride; j++ ) {
        g = encoder->decompressByte(encoder->predictorMain);
        r = encoder->decompressByte(encoder->predictorMain);
        b = encoder->decompressByte(encoder->predictorMain);
        if (stride == 4) {
          a = encoder->decompressByte(encoder->predictorMain);
        }
        if( !skipRgb )
        {
          r = g - r;
          b = g - b;
        }
        if( isPossibleRgb565 )
        {
          if( rgb565Run >= rgb565MinRun )
          {
            b ^= (b & 8) - ((b >> 3) & 1);
            g ^= (g & 4) - ((g >> 2) & 1);
            r ^= (r & 8) - ((r >> 3) & 1);
          }
          rgb565Run = min(rgb565Run + 1, 0xFFFF) *
                  static_cast<uint32_t>((b & 7) == ((b & 8) - ((b >> 3) & 1)) && (g & 3) == ((g & 4) - ((g >> 2) & 1)) &&
                                        (r & 7) == ((r & 8) - ((r >> 3) & 1)));
          isPossibleRgb565 = rgb565Run > 0;
        }
        if( fMode == FMode::FDECOMPRESS )
        {
          out->putChar(b);
          out->putChar(g);
          out->putChar(r);
          if (stride == 4)
          {
            out->putChar(a);
          }
          if((j == 0) && ((i & 0xF) == 0))
          {
            encoder->printStatus();
          }
        }
        else if( fMode == FMode::FCOMPARE )
        {
          if((b & 255) != out->getchar() && (diffFound == 0))
          {
            diffFound = p + 1;
          }
          if( g != out->getchar() && (diffFound == 0))
          {
            diffFound = p + 2;
          }
          if((r & 255) != out->getchar() && (diffFound == 0))
          {
            diffFound = p + 3;
          }
          if (stride == 4)
          {
            if ((a & 255) != out->getchar() && (diffFound == 0))
            {
              diffFound = p + 4;
            }
          }
          p += stride;
        }
      }
      for( int j = 0; j < width % stride; j++ )
      {
        if( fMode == FMode::FDECOMPRESS )
        {
          out->putChar(encoder->decompressByte(encoder->predictorMain));
        }
        else if( fMode == FMode::FCOMPARE )
        {
          if( encoder->decompressByte(encoder->predictorMain) != out->getchar() && (diffFound == 0))
          {
            diffFound = p + j + 1;
          }
        }
      }
    }
    for( int i = size % width; i > 0; i-- )
    {
      if( fMode == FMode::FDECOMPRESS )
      {
        out->putChar(encoder->decompressByte(encoder->predictorMain));
      } else if( fMode == FMode::FCOMPARE )
      {
        if( encoder->decompressByte(encoder->predictorMain) != out->getchar() && (diffFound == 0))
        {
          diffFound = size - i;
          break;
        }
      }
    }
    return size;
  }
};



class EndiannessFilter : public Filter {
public:
  void encode(File *in, File *out, uint64_t size, int  /*info*/, int & /*headerSize*/) override {
    for( uint64_t i = 0, l = size >> 1; i < l; i++ ) {
      uint8_t b = in->getchar();
      out->putChar(in->getchar());
      out->putChar(b);
    }
    if((size & 1) > 0 ) {
      out->putChar(in->getchar());
    }
  }

  uint64_t decode(File * /*in*/, File *out, FMode fMode, uint64_t size, uint64_t &diffFound) override {
    for( uint64_t i = 0, l = size >> 1; i < l; i++ ) {
      uint8_t b1 = encoder->decompressByte(encoder->predictorMain);
      uint8_t b2 = encoder->decompressByte(encoder->predictorMain);
      if( fMode == FMode::FDECOMPRESS ) {
        out->putChar(b2);
        out->putChar(b1);
      } else if( fMode == FMode::FCOMPARE ) {
        bool ok = out->getchar() == b2;
        ok &= out->getchar() == b1;
        if( !ok && (diffFound == 0)) {
          diffFound = size - i * 2;
          break;
        }
      }
      if( fMode == FMode::FDECOMPRESS && ((i & 0x7FF) == 0)) {
        encoder->printStatus();
      }
    }
    if((diffFound == 0) && (size & 1) > 0 ) {
      if( fMode == FMode::FDECOMPRESS ) {
        out->putChar(encoder->decompressByte(encoder->predictorMain));
      } else if( fMode == FMode::FCOMPARE ) {
        if( out->getchar() != encoder->decompressByte(encoder->predictorMain)) {
          diffFound = size - 1;
        }
      }
    }
    return size;
  }

};



/**
 * End of line transform
 */
class EolFilter:
public Filter
{
public:
  void encode(File *in, File *out, uint64_t size, int /*info*/, int & /*headerSize*/) override
  {
    uint8_t b = 0;
    uint8_t pB = 0;
    for( uint64_t i = 0; i < size; i++ )
    {
      b = in->getchar();
      if( pB == CARRIAGE_RETURN && b != NEW_LINE )
      {
        out->putChar(pB);
      }
      if( b != CARRIAGE_RETURN )
      {
        out->putChar(b);
      }
      pB = b;
    }
    if( b == CARRIAGE_RETURN )
    {
      out->putChar(b);
    }
  }

  uint64_t decode(File * /*in*/, File *out, FMode fMode, uint64_t size, uint64_t &diffFound) override
  {
    uint8_t b = 0;
    uint64_t count = 0;
    for( uint64_t i = 0; i < size; i++, count++ )
    {
      if((b = encoder->decompressByte(encoder->predictorMain)) == NEW_LINE )
      {
        if( fMode == FMode::FDECOMPRESS )
        {
          out->putChar(CARRIAGE_RETURN);
        }
        else if( fMode == FMode::FCOMPARE )
        {
          if( out->getchar() != CARRIAGE_RETURN && (diffFound == 0))
          {
            diffFound = size - i;
            break;
          }
        }
        count++;
      }
      if( fMode == FMode::FDECOMPRESS )
      {
        out->putChar(b);
      }
      else if( fMode == FMode::FCOMPARE )
      {
        if( b != out->getchar() && (diffFound == 0))
        {
          diffFound = size - i;
          break;
        }
      }
      if( fMode == FMode::FDECOMPRESS && ((i & 0xFFF) == 0))
      {
        encoder->printStatus();
      }
    }
    return count;
  }
};



/**
 * EXE transform: <encoded-size> <begin> <block>...
 * Encoded-size is 4 bytes, MSB first.
 * begin is the offset of the start of the input file, 4 bytes, MSB first.
 * Each block applies the e8e9 transform to strings falling entirely
 * within the block starting from the end and working backwards.
 * The 5 byte pattern is E8/E9 xx xx xx 00/FF (x86 CALL/JMP xxxxxxxx)
 * where xxxxxxxx is a relative address LSB first.  The address is
 * converted to an absolute address by adding the offset mod 2^25
 * (in range +-2^24).
 */
class ExeFilter : public Filter {
private:
  constexpr static int block = 0x10000; /**< block size */
  int info;
public:

void setBegin(int info) {
  this->info = info;
}

/**
    * @todo Large file support
    * @param in
    * @param out
    * @param size
    * @param info
    */
  void encode(File *in, File *out, uint64_t size, int info, int &/*headerSize*/) override {
    Array<uint8_t> blk(block);

    // Transform
    for( uint64_t offset = 0; offset < size; offset += block ) {
      uint32_t size1 = min(uint32_t(size - offset), block);
      int bytesRead = static_cast<int>(in->blockRead(&blk[0], size1));
      if( bytesRead != static_cast<int>(size1)) {
        quit("encodeExe read error");
      }
      for( int i = bytesRead - 1; i >= 5; --i ) {
        if((blk[i - 4] == 0xe8 || blk[i - 4] == 0xe9 || (blk[i - 5] == 0x0f && (blk[i - 4] & 0xf0) == 0x80)) &&
            (blk[i] == 0 || blk[i] == 0xff)) {
          int a = (blk[i - 3] | blk[i - 2] << 8 | blk[i - 1] << 16 | blk[i] << 24) + static_cast<int>(offset + info) + i + 1;
          a <<= 7;
          a >>= 7;
          blk[i] = a >> 24;
          blk[i - 1] = a ^ 176;
          blk[i - 2] = (a >> 8) ^ 176;
          blk[i - 3] = (a >> 16) ^ 176;
        }
      }
      out->blockWrite(&blk[0], bytesRead);
    }
  }

  /**
    * @todo Large file support
    * @param in
    * @param out
    * @param fMode
    * @param size
    * @param diffFound
    * @return
    */
  uint64_t decode(File */*in*/, File* out, FMode fMode, uint64_t size, uint64_t& diffFound) override {
    int offset = 6;
    int a = 0;
    uint8_t c[6];
    uint64_t begin = info;
    for( int i = 4; i >= 0; i-- ) {
      c[i] = encoder->decompressByte(encoder->predictorMain); // Fill queue
    }

    while( offset < static_cast<int>(size) + 6 ) {
      memmove(c + 1, c, 5);
      if( offset <= static_cast<int>(size)) {
        c[0] = encoder->decompressByte(encoder->predictorMain);
      }
      // E8E9 transform: E8/E9 xx xx xx 00/FF -> subtract location from x
      if((c[0] == 0x00 || c[0] == 0xFF) && (c[4] == 0xE8 || c[4] == 0xE9 || (c[5] == 0x0F && (c[4] & 0xF0) == 0x80)) &&
          (((offset - 1) ^ (offset - 6)) & -block) == 0 && offset <= static_cast<int>(size)) { // not crossing block boundary
        a = ((c[1] ^ 176) | (c[2] ^ 176) << 8 | (c[3] ^ 176) << 16 | c[0] << 24) - offset - static_cast<int>(begin);
        a <<= 7;
        a >>= 7;
        c[3] = a;
        c[2] = a >> 8;
        c[1] = a >> 16;
        c[0] = a >> 24;
      }
      if( fMode == FMode::FDECOMPRESS ) {
        out->putChar(c[5]);
      } else if( fMode == FMode::FCOMPARE && c[5] != out->getchar() && (diffFound == 0)) {
        diffFound = offset - 6 + 1;
      }
      if( fMode == FMode::FDECOMPRESS && ((offset & 0x0fff) == 0)) {
        encoder->printStatus();
      }
      offset++;
    }
    return size;
  }
};


class RleFilter:
Filter
{
private:

  int scanLineSize = 0;

  enum class RleState
  {
      BASE, LITERAL, RUN, LITERAL_RUN
  } state = RleState::BASE;

  void rleOutputRun(uint8_t byte, uint8_t* &outPtr, int &run)
  {
    while (run > 128)
    {
        *outPtr++ = 0xFF, *outPtr++ = byte;
        run -= 128;
    }
      *outPtr++ = (uint8_t)(0x80 | (run - 1)), *outPtr++ = byte;
  }


  void handleRun(uint8_t byte, uint8_t *&outPtr, uint8_t *&lastLiteral, int &run)
  {
    if( run > 1 )
    {
      state = RleState::RUN;
      rleOutputRun(byte, outPtr, run);
    } else {
      lastLiteral = outPtr;
      *outPtr++ = 0, *outPtr++ = byte;
      state = RleState::LITERAL;
    }
  }

  void handleLiteral(uint8_t byte, uint8_t *&outPtr, uint8_t *lastLiteral, int &run)
  {
    if( run > 1 )
    {
      state = RleState::LITERAL_RUN;
      rleOutputRun(byte, outPtr, run);
    } else {
      if( ++(*lastLiteral) == 127 )
      {
        state = RleState::BASE;
      }
      *outPtr++ = byte;
    }
  }

  uint8_t handleLiteralRun(uint8_t *outPtr, uint8_t *lastLiteral)
  {
    uint8_t loop = 0;
    if( outPtr[-2] == 0x81 && *lastLiteral < (125))
    {
      state = (((*lastLiteral) += 2) == 127) ? RleState::BASE : RleState::LITERAL;
      outPtr[-2] = outPtr[-1];
    } else {
      state = RleState::RUN;
    }
    loop = 1;
    return loop;
  }

public:

  void setScanLineSize(int scanLineSize)
  {
    this->scanLineSize = scanLineSize; // Run-length Packets should never encode pixels from more than one scan line (important for "decode")
  }

  static int VLICost(uint64_t n)
  {
    int cost = 1;
    while (n > 0x7F)
    {
      n >>= 7;
      cost++;
    }
    return cost;
  }
  void encode(File *in, File *out, uint64_t size, int /*info*/, int& headerSize) override
  {
    uint8_t b = 0;
    uint8_t c = in->getchar();
    int i = 1;
    out->putVLI(scanLineSize);
    headerSize = VLICost(scanLineSize);

    while( i < static_cast<int>(size))
    {
      b = in->getchar(), i++;
      if( c == 0x80 )
      {
        c = b;
        continue;
      }
      if( c > 0x7F )
      {
        for( uint32_t j = 0; j <= (c & 0x7F); j++ )
        {
          out->putChar(b);
        }
        c = in->getchar(), i++;
      }
      else
      {
        for(uint32_t j = 0; j <= c; j++, i++ )
        {
          out->putChar(b), b = in->getchar();
        }
        c = b;
      }
    }
  }

  uint64_t decode(File *in, File *out, FMode fMode, uint64_t  /*size*/, uint64_t &diffFound) override
  {
    uint8_t inBuffer[0x10000] = {0};
    uint8_t outBuffer[0x10200] = {0};
    uint64_t pos = 0;
    scanLineSize = static_cast<int>(in->getVLI());

    do
    {
      uint64_t remaining = in->blockRead(&inBuffer[0], scanLineSize);
      uint8_t *inPtr = (uint8_t *) inBuffer;
      uint8_t *outPtr = (uint8_t *) outBuffer;
      uint8_t *lastLiteral = nullptr;
      state = RleState::BASE;
      while( remaining > 0 )
      {
        uint8_t byte = *inPtr++;
        uint8_t loop = 0;
        int run = 1;
        for( remaining--; remaining > 0 && byte == *inPtr; remaining--, run++, inPtr++ )
        {}
        do
        {
          loop = 0;
          switch( state )
          {
            case RleState::BASE:
            case RleState::RUN:
            {
              handleRun(byte, outPtr, lastLiteral, run);
              break;
            }
            case RleState::LITERAL:
            {
              handleLiteral(byte, outPtr, lastLiteral, run);
              break;
            }
            case RleState::LITERAL_RUN:
            {
              loop = handleLiteralRun(outPtr, lastLiteral);
            }
          }
        }
        while( loop != 0 );
      }

      uint64_t length = outPtr - (&outBuffer[0]);
      if( fMode == FMode::FDECOMPRESS )
      {
        out->blockWrite(&outBuffer[0], length);
      }
      else if( fMode == FMode::FCOMPARE )
      {
        for(uint32_t j = 0; j < length; ++j )
        {
          if( outBuffer[j] != out->getchar() && (diffFound == 0))
          {
            diffFound = pos + j + 1;
            break;
          }
        }
      }
      pos += length;
    }
    while( !in->eof() && (diffFound == 0));
    return pos;
  }
};


//////////////////// Compress, Decompress ////////////////////////////

static void directEncodeBlock(BlockType type, File *in, uint64_t len, Encoder &en, int info)
{
  // TODO: Large file support
  Block::EncodeBlockHeader(&en, type, len, info);
  fprintf(stderr, "Compressing... ");
  for( uint64_t j = 0; j < len; ++j )
  {
    if((j & 0xfff) == 0 ) {
      en.printStatus(j, len);
    }
    en.compressByte(en.predictorMain, in->getchar());
  }
  fprintf(stderr, "\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
}

static void compressRecursive(File *in, uint64_t blockSize, Encoder &en, String &blstr, float p1, float p2, const TransformOptions* const transformOptions);
static void compressRecursiveForTar(File* in, uint64_t blockSize, Encoder& en, String& blstr, float p1, float p2, const TransformOptions* const transformOptions);

static uint64_t decodeFunc(BlockType type, Encoder &en, File *tmp, uint64_t len, int info, File *out, FMode mode, uint64_t &diffFound, const TransformOptions* const transformOptions) {
  if( type == BlockType::IMAGE24 )
  {
    auto f = BmpFilter();
    f.setWidth(info);
    f.setSkipRgb(transformOptions->skipRgb);
    f.setEncoder(en);
    return f.decode(tmp, out, mode, len, diffFound);
  }
  if( type == BlockType::IMAGE32 )
  {
    auto f = BmpFilter();
    f.setWidth(info);
    f.setSkipRgb(transformOptions->skipRgb);
    f.setHasApha();
    f.setEncoder(en);
    return f.decode(tmp, out, mode, len, diffFound);
  }
  if (type == BlockType::PNG8GRAY || type == BlockType::PNG8 || type == BlockType::PNG24|| type == BlockType::PNG32)
  {
    auto f = PngFilter();
    f.setWidth(info);
    auto stride = type == BlockType::PNG24 ? 3 : type == BlockType::PNG32 ? 4 : 1;
    f.setStride(stride);
    f.setEncoder(en);
    return f.decode(tmp, out, mode, len, diffFound);
  }
  if( type == BlockType::AUDIO_LE )
  {
    auto f = EndiannessFilter();
    f.setEncoder(en);
    return f.decode(tmp, out, mode, len, diffFound);
  } else if( type == BlockType::EXE ) {
    auto f = ExeFilter();
    f.setBegin(info);
    f.setEncoder(en);
    return f.decode(tmp, out, mode, len, diffFound);
  } else if( type == BlockType::TEXT_EOL ) {
    auto f = EolFilter();
    f.setEncoder(en);
    return f.decode(tmp, out, mode, len, diffFound);
  } else if( type == BlockType::CD ) {
    auto f = CdFilter();
    return f.decode(tmp, out, mode, len, diffFound);
#ifndef DISABLE_ZLIB
  } else if( type == BlockType::ZLIB ) {
    return decodeZlib(tmp, len, out, mode, diffFound);
#endif //DISABLE_ZLIB
  } else if( type == BlockType::BASE64 ) {
    auto f = Base64Filter();
    return f.decode(tmp, out, mode, len, diffFound);
  } else if (type == BlockType::BASE85) {
    auto f = Base85Filter();
    return f.decode(tmp, out, mode, len, diffFound);
  }
  else if( type == BlockType::GIF ) {
    return decodeGif(tmp, len, out, mode, diffFound);
  } else if( type == BlockType::RLE ) {
    auto f = RleFilter();
    //f.setScanLineSize(info & 0xFFFFFF); //now it self-encodes this info, but eventually we need to pass it
    return f.decode(tmp, out, mode, len, diffFound);
  } else if( type == BlockType::MRB) {
    uint8_t packingMethod = (info >> 24) & 3; //0..3
    if (packingMethod == 1 /*RLE*/) {
      auto f = MrbRleFilter();
      return f.decode(tmp, out, mode, len, diffFound);
    }
    else quit("MRB: not implemented");
  } else if( type == BlockType::LZW ) {
    return decodeLzw(tmp, out, mode, diffFound);
  } else if (type == BlockType::DEC_ALPHA) {
    auto f = DECAlphaFilter();
    f.setEncoder(en);
    return f.decode(tmp, out, mode, len, diffFound);
  } else if (type == BlockType::TAR) {
    auto f = TarFilter();
    f.setEncoder(en);
    return f.decode(tmp, out, mode, len, diffFound);
  } else {
    assert(false);
  }
  return 0;
}

static uint64_t encodeFunc(BlockType type, File *in, File *tmp, uint64_t len, int info, int &hdrsize, const TransformOptions* const transformOptions) {
  if( type == BlockType::IMAGE24 ) {
    auto f= BmpFilter();
    f.setSkipRgb(transformOptions->skipRgb);
    f.encode(in, tmp, len, info, hdrsize);
  } else if( type == BlockType::IMAGE32 ) {
    auto f = BmpFilter();
    f.setSkipRgb(transformOptions->skipRgb);
    f.setHasApha();
    f.encode(in, tmp, len, info, hdrsize);
  } else if (type == BlockType::PNG8GRAY || type == BlockType::PNG8 || type == BlockType::PNG24 || type == BlockType::PNG32) {
    auto f = PngFilter();
    auto stride = type == BlockType::PNG24 ? 3 : type == BlockType::PNG32 ? 4 : 1;
    f.setStride(stride);
    f.encode(in, tmp, len, info, hdrsize);
  } else if( type == BlockType::AUDIO_LE ) {
    auto f = EndiannessFilter();
    f.encode(in, tmp, len, info, hdrsize);
  } else if( type == BlockType::EXE ) {
    auto f = ExeFilter();
    f.setBegin(info);
    f.encode(in, tmp, len, info, hdrsize);
  } else if( type == BlockType::TEXT_EOL ) {
    auto f = EolFilter();
    f.encode(in, tmp, len, info, hdrsize);
  } else if( type == BlockType::CD ) {
    auto f = CdFilter();
    f.encode(in, tmp, len, info, hdrsize);
#ifndef DISABLE_ZLIB
  } else if( type == BlockType::ZLIB ) {
    return encodeZlib(in, tmp, len, hdrsize) ? 0 : 1;
#endif //DISABLE_ZLIB
  } else if( type == BlockType::BASE64 ) {
    auto f = Base64Filter();
    f.encode(in, tmp, len, info, hdrsize);
  } else if (type == BlockType::BASE85) {
    auto f = Base85Filter();
    f.encode(in, tmp, len, info, hdrsize);
  } else if( type == BlockType::GIF ) {
    return encodeGif(in, tmp, len, hdrsize) != 0 ? 0 : 1;
  } else if( type == BlockType::RLE ) {
    auto f = RleFilter();
    f.setScanLineSize(info & 0xFFFFFF);
    f.encode(in, tmp, len, info, hdrsize);
  } else if (type == BlockType::MRB) {
    const uint8_t packingMethod = (info >> 24) & 3; //0..3
    const uint16_t colorBits = (info >> 26); //1,4,8
    const int width = (info >> 12) & 0xfff;
    const int height = info & 0xfff;
    int widthInBytes;
    if (colorBits == 8) widthInBytes = ((width + 3) / 4) * 4;
    else if (colorBits == 4) widthInBytes = ((width + 3) / 4) * 2;
    else if (colorBits == 1) widthInBytes = ((width + 31) / 32) * 4;
    else quit("Unexpected colorBits for MRB");
    if (packingMethod == 1 /*RLE*/) {
      auto f = MrbRleFilter();
      f.setInfo(widthInBytes, height);
      f.encode(in, tmp, len, info, hdrsize);
    }
    else quit("MRB: not implemented");
  } else if( type == BlockType::LZW ) {
    return encodeLzw(in, tmp, len, hdrsize) != 0 ? 0 : 1;
  } else if (type == BlockType::DEC_ALPHA) {
    auto f = DECAlphaFilter();
    f.encode(in, tmp, len, info, hdrsize);
  } else if (type == BlockType::TAR) {
    auto f = TarFilter();
    f.encode(in, tmp, len, info, hdrsize);
  } else {
    assert(false);
  }
  return 0;
}

static void
transformEncodeBlock(BlockType type, File *in, uint64_t len, Encoder &en, int info, String &blstr, float p1, float p2, uint64_t begin, const TransformOptions* const transformOptions) {
  if( hasTransform(type, info)) {
    FileTmp tmp;
    int headerSize = 0;
    uint64_t diffFound = encodeFunc(type, in, &tmp, len, info, headerSize, transformOptions);
    const uint64_t tmpSize = tmp.curPos();
    tmp.setpos(tmpSize); //switch to read mode
    if( diffFound == 0 ) {
      tmp.setpos(0);
      en.setFile(&tmp);
      in->setpos(begin);
      decodeFunc(type, en, &tmp, tmpSize, info, in, FMode::FCOMPARE, diffFound, transformOptions);
    }
    // Test fails, compress without transform
    if( diffFound > 0 || tmp.getchar() != EOF) {
      printf("Transform fails at %" PRIu64 ", skipping...\n", diffFound - 1);
      in->setpos(begin);
      directEncodeBlock(BlockType::DEFAULT, in, len, en, -1);
    } else {
      tmp.setpos(0);
      if( hasRecursion(type)) {
        Block::EncodeBlockHeader(&en, type, tmpSize, info&0xffffff);
        BlockType type2 = static_cast<BlockType>((info >> 24) & 0xFF);
        if (isPNG(type)) {
            type2 =
                type == BlockType::PNG8GRAY ? BlockType::IMAGE8GRAY :
                type == BlockType::PNG8 ? BlockType::IMAGE8 :
                type == BlockType::PNG24 ? BlockType::IMAGE24 :
                type == BlockType::PNG32 ? BlockType::IMAGE32 : BlockType::DEFAULT;
        }
        if( type2 != BlockType::DEFAULT ) {
          String blstrSub0;
          blstrSub0 += blstr.c_str();
          blstrSub0 += "->";
          String blstrSub1;
          blstrSub1 += blstr.c_str();
          blstrSub1 += "-->";
          String blstrSub2;
          blstrSub2 += blstr.c_str();
          blstrSub2 += "-->";
          const char* exploded =
                           "exploded    ";
          const char* addedheader =
            isPNG(type)  ? "filter data " : // PNG ->Image
                           "added header";
          const char* dataname =
            isPNG(type2) ? "png data    " : // ZLIB -> PNG
            isPNG(type)  ? "pixel data  " : // PNG  -> Image
                           "data        ";
          if(!isPNG(type))
            printf(" %-11s | ->  %s |%10d bytes [%d - %d]\n", blstrSub0.c_str(), exploded, int(tmpSize), 0, int(tmpSize - 1));
          if (headerSize != 0) {
            printf(" %-11s | --> %s |%10d bytes [%d - %d]\n", blstrSub1.c_str(), addedheader, headerSize, 0, headerSize - 1);
            directEncodeBlock(BlockType::HDR, &tmp, headerSize, en, -1);
            printf(" %-11s | --> %s |%10d bytes [%d - %d]\n", blstrSub2.c_str(), dataname, int(tmpSize - headerSize), headerSize, int(tmpSize - 1));
          }
          transformEncodeBlock(type2, &tmp, tmpSize - headerSize, en, info & 0xffffff, blstr, p1, p2, headerSize, transformOptions);
        } else {
          if (type == BlockType::TAR) {
            compressRecursiveForTar(&tmp, tmpSize, en, blstr, p1, p2, transformOptions);
          }
          else {
            compressRecursive(&tmp, tmpSize, en, blstr, p1, p2, transformOptions);
          }
        }
      } else {
        if (type == BlockType::MRB) {
          String blstrSub0;
          blstrSub0 += blstr.c_str();
          blstrSub0 += "->";
          printf(" %-11s | ->  uncompressed |%10d bytes [%d - %d]\n", blstrSub0.c_str(), int(tmpSize), 0, int(tmpSize - 1));
        }
        directEncodeBlock(type, &tmp, tmpSize, en, info);
      }
    }
    tmp.close();
  } else {
    directEncodeBlock(type, in, len, en, info);
  }
}

static void composeSubBlockStringToPrint(String& blstr, String& blstrSub, int blNum) {
  //Compose block enumeration string
  blstrSub += blstr.c_str();
  if (blstrSub.strsize() != 0) {
    blstrSub += "-";
  }
  blstrSub += uint64_t(blNum);
}

static void printBlock(const uint64_t begin, const uint64_t len, const BlockType type, const int blockInfo, String& blstrSub) {
  static const char* typeNames[30] = { "default", "jpeg", "hdr", "1b-image", "4b-image", "8b-image", "8b-img-grayscale",
                                      "24b-image", "32b-image", "audio", "audio - le", "x86/64", "cd", "zlib", "base64", "gif", "png-8b",
                                      "png-8b-grayscale", "png-24b", "png-32b", "text", "text - eol", "rle", "lzw", "dec-alpha", "mrb",
                                      "dBase", "base85", "tar", "tar header"};
  static const char* audioTypes[4] = { "8b-mono", "8b-stereo", "16b-mono", "16b-stereo" };
  static const char* mrbTypes[4] = { "mrb-uncompressed", "mrb-rle", "mrb-lz77", "mrb-rle-lz77" };

  const char* typeName =
    type == BlockType::MRB ? mrbTypes[(blockInfo >> 24) & 3] :
    type == BlockType::ZLIB && isPNG(BlockType(blockInfo >> 24)) ? typeNames[blockInfo >> 24] :
    typeNames[(int)type];
  printf(" %-11s | %-16s |%10" PRIu64 " bytes [%" PRIu64 " - %" PRIu64 "]", blstrSub.c_str(), typeName, len, begin, (begin + len) - 1);
  if (type == BlockType::AUDIO || type == BlockType::AUDIO_LE) {
    printf(" (%s)", audioTypes[blockInfo % 4]);
  }
  else if (
    type == BlockType::IMAGE1 ||
    type == BlockType::IMAGE4 ||
    type == BlockType::IMAGE8 ||
    type == BlockType::IMAGE8GRAY ||
    type == BlockType::IMAGE24 ||
    type == BlockType::IMAGE32 ||
    (type == BlockType::ZLIB && isPNG(BlockType(blockInfo >> 24)))) {
    printf(" (width: %d)", (type == BlockType::ZLIB) ? (blockInfo & 0xFFFFFF) : blockInfo);
  }
  else if (type == BlockType::MRB) {
    const uint8_t packingMethod = (blockInfo >> 24) & 3; //0..3
    const uint16_t colorBits = (blockInfo >> 26); //1,4,8
    const int width = ((blockInfo >> 12) & 0xFFF);
    const int height = blockInfo & 0xFFF;
    printf(" (%d-bit image: %dx%d)", colorBits, width, height);
  }
  else if (hasRecursion(type) && (blockInfo >> 24) != (int)BlockType::DEFAULT) {
      printf(" (%s)", typeNames[blockInfo >> 24]);
  }
  else if (type == BlockType::CD) {
    printf(" (mode%d/form%d)", blockInfo == 1 ? 1 : 2, blockInfo != 3 ? 1 : 2);
  }
  else if (type == BlockType::DBF) {
    printf(" (record length: %d)", blockInfo);
  }
  printf("\n");
}

static void compressBlock(File* in, const uint64_t begin, const uint64_t len, int &blNum, BlockType type, int blockInfo, Encoder& en, String& blstr, float &p1, float &p2, const float pscale, const TransformOptions* const transformOptions) {
  p2 = p1 + pscale * len;
  en.setStatusRange(p1, p2);

  String blstrSub;
  composeSubBlockStringToPrint(blstr, blstrSub, blNum);
  printBlock(begin, len, type, blockInfo, blstrSub);
  transformEncodeBlock(type, in, len, en, blockInfo, blstrSub, p1, p2, begin, transformOptions);
  blNum++;

  p1 = p2;
}

static void compressRecursive(File *in, uint64_t bytesToProcess, Encoder &en, String &blstr, float p1, float p2, const TransformOptions* const transformOptions) {

  uint64_t begin = in->curPos();

  float pscale = bytesToProcess != 0 ? (p2 - p1) / bytesToProcess : 0;

  int blNum = 0;
  while(bytesToProcess > 0 ) {

    //detect a block
    DetectionInfo detectionInfo = detect(in, bytesToProcess, transformOptions); // Special blocktypes
    in->setpos(begin);

    uint64_t blockStart = detectionInfo.HeaderLength != 0 ? detectionInfo.HeaderStart : detectionInfo.DataStart;
    while(blockStart != begin) {
      TextDetectionInfo textDetectionInfo = detectText(in, begin, blockStart - begin); // DEFAULT / TEXT / TEXT_EOL
      in->setpos(begin);
      compressBlock(in, textDetectionInfo.DataStart, textDetectionInfo.DataLength, /*ref: */ blNum, textDetectionInfo.Type, 0, en, /*in: */ blstr, /*ref: */ p1, /*ref: */ p2, pscale, transformOptions);
      begin += textDetectionInfo.DataLength;
      bytesToProcess -= textDetectionInfo.DataLength;
    }

    if (detectionInfo.HeaderLength != 0) {
      compressBlock(in, detectionInfo.HeaderStart, detectionInfo.HeaderLength, /*ref: */ blNum, BlockType::HDR, 0, en, /*in: */ blstr, /*ref: */ p1, /*ref: */ p2, pscale, transformOptions);
      begin += detectionInfo.HeaderLength;
      bytesToProcess -= detectionInfo.HeaderLength;
    }

    if (begin != detectionInfo.DataStart)
      quit("Internal error in compressRecursive");

    if (detectionInfo.DataLength != 0) {
      compressBlock(in, detectionInfo.DataStart, detectionInfo.DataLength, /*ref: */ blNum, detectionInfo.Type, detectionInfo.DataInfo, en, /*in: */ blstr, /*ref: */ p1, /*ref: */ p2, pscale, transformOptions);
      begin += detectionInfo.DataLength;
      bytesToProcess -= detectionInfo.DataLength;
    }
  }
}

static void compressRecursiveForTar(File* in, uint64_t bytesToProcess, Encoder& en, String& blstr, float p1, float p2, const TransformOptions* const transformOptions) {
  Array<uint64_t, 1> filePositions{ 0 };
  TarFilter tarFilter{};
  in->setpos(0);
  tarFilter.getFilePositions(in, filePositions);
  assert(filePositions[filePositions.size() - 1] == bytesToProcess);
  in->setpos(0);

  int blNum = 0;
  float pscale = bytesToProcess != 0 ? (p2 - p1) / bytesToProcess : 0;

  auto headerSize = filePositions[0];
  compressBlock(in, 0, headerSize, /*ref: */ blNum, BlockType::TARHDR, 0, en, /*in: */ blstr, /*ref: */ p1, /*ref: */ p2, pscale, transformOptions);
  for (; blNum < filePositions.size(); blNum++) {
    uint64_t blockSize = filePositions[blNum] - filePositions[blNum - 1];
    p2 = p1 + pscale * blockSize;
    en.setStatusRange(p1, p2);
    String blstrSub;
    composeSubBlockStringToPrint(blstr, blstrSub, blNum);
    compressRecursive(in, blockSize, en, blstrSub, p1, p2, transformOptions);
    p1 = p2;
  }
}

// Compress a file. Split fileSize bytes into blocks by type.
// For each block, output
// <type> <size> and call encode_X to convert to type X.
// Test transform and compress.
static void compressfile(const Shared* const shared, const char *filename, uint64_t fileSize, Encoder &en, bool verbose) {
  assert(en.getMode() == COMPRESS);
  assert(filename && filename[0]);

  uint64_t start = en.size();
  Block::EncodeBlockSize(&en, fileSize);

  FileDisk in;
  in.open(filename, true);
  printf("Block segmentation:\n");
  String blstr;
  TransformOptions transformOptions(shared);
  BlockType forcedBlockType = shared->GetOptionDetectBlockAsBinary() ? BlockType::DEFAULT : shared->GetOptionDetectBlockAsText() ? BlockType::TEXT : BlockType::Count;
  if (forcedBlockType != BlockType::Count) {
    // skip blockType detection + compress with DEFAULT or TEXT
    const uint64_t begin = 0;
    int blNum = 0;
    const int info = -1;
    float p1 = 0.0f;
    float p2 = 1.0f;
    const float pscale = fileSize != 0 ? (p2 - p1) / fileSize : 0;
    compressBlock(&in, begin, fileSize, /*ref: */ blNum, forcedBlockType, info, en, /*in: */ blstr, /*ref: */ p1, /*ref: */ p2, pscale, &transformOptions);
  }
  else {
    // detect block types + compress
    compressRecursive(&in, fileSize, en, blstr, 0.0F, 1.0F, &transformOptions);
  }
  in.close();

  if (shared->GetOptionMultipleFileMode()) //multiple file mode
  {
    if( verbose )
    {
      printf("File size to encode   : 4\n"); //This string must be long enough. "Compressing ..." is still on screen, we need to overwrite it.
    }
    printf("File input size       : %" PRIu64 "\n", fileSize);
    printf("File compressed size  : %" PRIu64 "\n", en.size() - start);
  }
}

static uint64_t decompressRecursive(File *out, uint64_t blockSize, Encoder &en, FMode mode, TransformOptions *transformOptions)
{
  uint64_t i = 0;
  uint64_t diffFound = 0;
  while( i < blockSize )
  {

    uint64_t len = Block::DecodeBlockHeader(&en);
    BlockType type = en.predictorMain->shared->State.blockType;
    int info = en.predictorMain->shared->State.blockInfo;
    if (type == BlockType::MRB)
    {
      FileTmp tmp;
      for (uint64_t j = 0; j < len; ++j)
          tmp.putChar(en.decompressByte(en.predictorMain));
      if (mode != FMode::FDISCARD)
      {
        tmp.setpos(0);
        len = decodeFunc(type, en, &tmp, len, info, out, mode, diffFound, transformOptions);
      }
      tmp.close();
    }
    else if( hasRecursion(type))
    {
      FileTmp tmp;
      decompressRecursive(&tmp, len, en, FMode::FDECOMPRESS, transformOptions);
      if( mode != FMode::FDISCARD )
      {
        tmp.setpos(0);
        if( hasTransform(type, info))
        {
          len = decodeFunc(type, en, &tmp, len, info, out, mode, diffFound, transformOptions);
        }
      }
      tmp.close();
    }
    else if( hasTransform(type, info))
    {
      len = decodeFunc(type, en, nullptr, len, info, out, mode, diffFound, transformOptions);
    } else {
      for( uint64_t j = 0; j < len; ++j )
      {
        if((j & 0xfff) == 0 )
        {
          en.printStatus();
        }
        if( mode == FMode::FDECOMPRESS )
        {
          out->putChar(en.decompressByte(en.predictorMain));
        }
        else if( mode == FMode::FCOMPARE )
        {
          if( en.decompressByte(en.predictorMain) != out->getchar() && (diffFound == 0))
          {
            mode = FMode::FDISCARD;
            diffFound = i + j + 1;
          }
        } else {
          en.decompressByte(en.predictorMain);
        }
      }
    }
    i += len;
  }
  return diffFound;
}

// Decompress or compare a file
static void decompressFile(const Shared* const shared, const char* filename, FMode fMode, Encoder& en)
{
  assert(en.getMode() == DECOMPRESS);
  assert(filename && filename[0]);

  uint64_t fileSize = Block::DecodeBlockSize(&en);

  FileDisk f;
  if( fMode == FMode::FCOMPARE )
  {
    f.open(filename, true);
    printf("Comparing");
  } else { //mode==FDECOMPRESS;
    f.create(filename);
    printf("Extracting");
  }
  printf(" %s %" PRIu64 " bytes -> ", filename, fileSize);

  // Decompress/Compare
  TransformOptions transformOptions(shared);
  uint64_t r = decompressRecursive(&f, fileSize, en, fMode, &transformOptions);
  if( fMode == FMode::FCOMPARE && (r == 0) && f.getchar() != EOF)
  {
    printf("file is longer\n");
  }
  else if( fMode == FMode::FCOMPARE && (r != 0))
  {
    printf("differ at %" PRIu64 "\n", r - 1);
  }
  else if( fMode == FMode::FCOMPARE )
  {
    printf("identical\n");
  } else {
    printf("done   \n");
  }
  f.close();
}
