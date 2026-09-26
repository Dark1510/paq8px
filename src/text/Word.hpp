#pragma once

#include "../Hash.hpp"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>

class Word
{
private:
  uint64_t calculateHash();
public:
  constexpr static int maxWordSize = 64;
  constexpr static int wordEmbeddingSize = 3;
  uint8_t letters[maxWordSize] {};
  uint8_t start {};
  uint8_t end {};
  uint64_t Hash[2] {};
  uint32_t type {};
  uint32_t language {};
  uint32_t embedding {};
  Word();
  void reset();
  bool operator==(const char *s) const;
  bool operator!=(const char *s) const;
  void operator+=(char c);
  uint32_t operator-(Word w) const;
  uint32_t operator+(Word w) const;
  uint8_t operator[](uint8_t i) const;
  uint8_t operator()(uint8_t i) const;
  uint32_t length() const;
  uint32_t distanceTo(Word w) const;
  void calculateWordHash();

  /**
    * Called by a stemmer after stemming
    */
  void calculateStemHash();
  bool changeSuffix(const char *oldSuffix, const char *newSuffix);
  bool matchesAny(const char **a, int count);
  bool endsWith(const char *suffix) const;
  bool startsWith(const char *prefix) const;
  void print() const;
};

class Segment
{
public:
  Word firstWord; /**< useful following questions */
  uint32_t wordCount {};
  uint32_t numCount {};
};

class Sentence:
public Segment
{
public:
  enum Types  // possible sentence types, excluding Imperative
  {Declarative, Interrogative, Exclamative, Count};
  Types type;
  uint32_t segmentCount {};
  uint32_t verbIndex {}; /**< relative position of last detected verb */
  uint32_t nounIndex {}; /**< relative position of last detected noun */
  uint32_t capitalIndex {}; /**< relative position of last capitalized word, excluding the initial word of this sentence */
  Word lastVerb;
  Word lastNoun;
  Word lastCapital;
};

class Paragraph
{
public:
  uint32_t sentenceCount;
  uint32_t typeCount[Sentence::Types::Count];
  uint32_t typeMask;
};

class Stemmer
{
protected:
  uint32_t getRegion(const Word *w, uint32_t from);
  static bool suffixInRn(const Word *w, uint32_t rn, const char *suffix);
  static bool charInArray(char c, const char a[], int len);
public:
  virtual ~Stemmer() = default;
  virtual bool isVowel(char c) = 0;
  virtual bool stem(Word *w) = 0;
};

class Language
{
public:
  enum Flags
  {Verb = (1 << 0), Noun = (1 << 1)};
  enum Ids
  {Unknown, English, French, German, Count};

  virtual ~Language() = default;
  virtual bool isAbbreviation(Word *w) = 0;
};
