//uncomment the following define if you have only SSE, not AVX2
//#define USE_SSE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstdint>
#include <intrin.h>
#include <immintrin.h>

#if defined(_MSC_VER)
#define ALWAYS_INLINE  __forceinline
#elif (defined(__GNUC__) || defined(__clang__))
#define ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define ALWAYS_INLINE inline
#endif

#ifdef _MSC_VER
uint32_t ctz(uint32_t x) {
  unsigned long tmp = 0;
  if (x != 0) {
    _BitScanForward(&tmp, x);
  }
  return tmp;
}
#else
uint32_t ctz(uint32_t x) {
  return __builtin_ctz(x);
}
#endif

/*
 *  FileUtils
 */

void ReadAllBytes(const char* path, void* bytes, size_t numBytes) {
  FILE* file = fopen(path, "rb");
  if (file != NULL) {
    fread(bytes, sizeof(uint8_t), numBytes, file);
    fclose(file);
  }
  else {
    printf("Error opening file for reading: %s\n", path);
    exit(-1);
  }
}

void WriteAllBytes(const char* path, const void* bytes, size_t numBytes) {
  FILE* file = fopen(path, "wb");
  if (file != NULL) {
    fwrite(bytes, sizeof(uint8_t), numBytes, file);
    fclose(file);
  }
  else {
    exit(1);
  }
}

void AppendAllBytes(const char* path, const void* bytes, size_t numBytes) {
  FILE* file = fopen(path, "ab");
  if (file != NULL) {
    fwrite(bytes, sizeof(uint8_t), numBytes, file);
    fclose(file);
  }
  else {
    exit(1);
  }
}

alignas(32)
static uint8_t MTF_table[256];

#ifndef USE_SSE
ALWAYS_INLINE
static size_t findBytePositionAVX2(const uint8_t* byteArray, uint8_t byte) {
  __m256i target = _mm256_set1_epi8(byte);

  for (size_t i = 0; i < 256; i += 32) {
    __m256i chunk = _mm256_load_si256((__m256i*)(byteArray + i));
    __m256i compareResult = _mm256_cmpeq_epi8(chunk, target);
    int mask = _mm256_movemask_epi8(compareResult);
    //'likely' = small speed advantage: it's a side effect of MTF, that the byte we are looking for is at the beginning of the array
    if (mask != 0) [[likely]]
      return i + ctz(mask);
  }

  return -1; //never happens, only for consistency
}
#endif

#ifdef USE_SSE
ALWAYS_INLINE
static size_t findBytePositionSSE2(const uint8_t* byteArray, uint8_t byte) {
  __m128i target = _mm_set1_epi8(byte);
  for (size_t i = 0; i < 256; i += 16) {
    __m128i chunk = _mm_loadu_si128((const __m128i*)(byteArray + i));
    __m128i compareResult = _mm_cmpeq_epi8(chunk, target);
    int mask = _mm_movemask_epi8(compareResult);
    //'likely' = small speed advantage: it's a side effect of MTF, that the byte we are looking for is at the beginning of the array
    if (mask != 0) [[likely]]
      return i + ctz(mask);
  }

  return -1; //never happens, only for consistency
}
#endif

ALWAYS_INLINE
static uint8_t MTF_encode_byte(const uint8_t b) {
#ifdef USE_SSE
  size_t idx = findBytePositionSSE2(MTF_table, b);
#else
  size_t idx = findBytePositionAVX2(MTF_table, b);
#endif
  if (idx != 0) {
    memmove(MTF_table + 1, MTF_table, idx);
    MTF_table[0] = b;
  }
  return (uint8_t)idx;
}

ALWAYS_INLINE
static uint8_t MTF_decode_byte(const uint8_t idx) {
  uint8_t byte = MTF_table[idx];
  if (idx != 0)
    memmove(MTF_table + 1, MTF_table, idx);
  MTF_table[0] = byte;
  return byte;
}

ALWAYS_INLINE
void LEB_Encode(uint8_t* bytes1, uint8_t* bytes2, int* position1, int* position2, int value) {
  if (value <= 254) {
    [[likely]] //small speed advantage
    bytes1[(*position1)++] = (uint8_t)value;
    return;
  }

  bytes1[(*position1)++] = 255;
  value -= 254;
  do {
    uint8_t byteValue = (uint8_t)(value & 0x7F);
    value >>= 7;

    if (value != 0) {
      byteValue |= 0x80;
    }

    bytes2[(*position2)++] = byteValue;
  } while (value != 0);
}

ALWAYS_INLINE 
int LEB_Decode(const uint8_t* bytes1, const uint8_t* bytes2, int* position1, int* position2) {
  int result = 0;
  int shift = 0;

  uint8_t byteValue = bytes1[(*position1)++];
  if (byteValue <= 254)
    [[likely]] //small speed advantage
    return byteValue;

  while (1) {
    byteValue = bytes2[(*position2)++];
    result |= (byteValue & 0x7F) << shift;
    shift += 7;

    if ((byteValue & 0x80) == 0) {
      break;
    }
  }

  return result + 254;
}

void RLE3_Encode(const uint8_t* bytes, int length, uint8_t* result, uint8_t* rle1, uint8_t* rle2) {
  int index = 0;
  int resultIndex = 0;
  int rle1Index = 0;
  int rle2Index = 0;

  while (index < length) {
    uint8_t currentByte = bytes[index++];

    int runLength = 0;
    while (bytes[index + runLength] == currentByte)
      runLength++;

    result[resultIndex++] = MTF_encode_byte(currentByte);

    if (runLength >= 2) {
      result[resultIndex++] = 0; //no need to MTF - with previous byte repeating it's always 0
      result[resultIndex++] = 0; //no need to MTF - with previous byte repeating it's always 0
      LEB_Encode(rle1, rle2, &rle1Index, &rle2Index, runLength - 2);
      index += runLength;
    }
  }

}

void RLE3_Decode(const uint8_t* bytes, int length, const uint8_t* rle1, const uint8_t* rle2, int rle1Length, int rle2Length, uint8_t* result) {
  int resultIndex = 0;
  int rlePos1 = 0;
  int rlePos2 = 0;
  int index = 0;

  uint8_t currentByte = MTF_decode_byte(bytes[index++]);
  result[resultIndex++] = currentByte;

  currentByte = MTF_decode_byte(bytes[index++]);
  result[resultIndex++] = currentByte;

  while (index < length) {

    currentByte = MTF_decode_byte(bytes[index++]);
    result[resultIndex++] = currentByte;

    if (currentByte == result[resultIndex-2] && currentByte == result[resultIndex - 3]) {
      const int runLength = LEB_Decode(rle1, rle2, &rlePos1, &rlePos2);
      for (int j = 0; j < runLength; j++) {
        result[resultIndex++] = currentByte;
      }
    }
  }
}

int main(int argc, char **argv) {

  //parameters according to GDCC 2023 rules
  if (argc != 4 || (argv[1][0] != 't' && argv[1][0] != 'i')) {
    exit(1);
  }

  for (int i = 0; i < 256; i++)
    MTF_table[i] = i;

  //Encode
  if (argv[1][0] == 't') {
    //Note: GDCC 2023 rules don't require generality, so we can hardcode these sizes for simplicity
    const int filesize = 1073741828;
    uint8_t *bytes = (uint8_t*)malloc(filesize);
    ReadAllBytes(argv[2], bytes, filesize);

    uint8_t* encoded = (uint8_t*)malloc(375411081);
    uint8_t* rle1 = (uint8_t*)malloc(21499610);
    uint8_t* rle2 = (uint8_t*)malloc(151296);

    RLE3_Encode(bytes, 1073741828, encoded, rle1, rle2);

    WriteAllBytes(argv[3], rle1, 21499610);
    AppendAllBytes(argv[3], rle2, 151296);
    AppendAllBytes(argv[3], encoded, 375411081);
  }

  // Decode
  else {
    //Note: GDCC 2023 rules don't require generality, so we can hardcode these sizes for simplicity
    const int filesize = 397061987;
    uint8_t* bytes = (uint8_t*)malloc(filesize);
    ReadAllBytes(argv[2], bytes, filesize);

    uint8_t* decoded = (uint8_t*)malloc(1073741828);

    uint8_t* rle1 = bytes;
    uint8_t* rle2 = rle1 + 21499610;
    uint8_t* encoded = rle2 + 151296;

    RLE3_Decode(encoded, 375411081, rle1, rle2, 21499610, 151296, decoded);

    WriteAllBytes(argv[3], decoded, 1073741828);
  }

  return 0;
}

