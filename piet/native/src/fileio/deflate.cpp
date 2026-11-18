// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from zlib 1.3.1 (Zlib).
// See THIRD_PARTY_NOTICES.
//
// The zlib deflate algorithm (deflate.c, trees.c) for one buffer given whole:
// hash chains over a 32 KiB window, greedy matching at levels 1-3 and lazy
// matching at levels 4-9 with the per-level limits of zlib's configuration
// table, blocks closed after 16383 symbols, and for each block the cheaper of
// stored, fixed-Huffman and dynamic-Huffman coding. The output is the same
// byte stream zlib gives, which is what makes a compressed size comparable
// with the one Stata's zip writer reports. Level 0 is the stored method only.
#include "fileio/deflate.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace piet::fileio {
namespace {

constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
constexpr int kMinLookahead = kMaxMatch + kMinMatch + 1;
constexpr int kWSize = 32768;
constexpr int kWMask = kWSize - 1;
constexpr int kHashBits = 15;
constexpr int kHashSize = 1 << kHashBits;
constexpr int kHashMask = kHashSize - 1;
constexpr int kHashShift = (kHashBits + kMinMatch - 1) / kMinMatch;
constexpr int kMaxDist = kWSize - kMinLookahead;
constexpr int kTooFar = 4096;
constexpr int kLitBufSize = 1 << 14;
constexpr int kWinInit = kMaxMatch;

constexpr int kLengthCodes = 29;
constexpr int kLiterals = 256;
constexpr int kLCodes = kLiterals + 1 + kLengthCodes;
constexpr int kDCodes = 30;
constexpr int kBlCodes = 19;
constexpr int kHeapSize = 2 * kLCodes + 1;
constexpr int kMaxBits = 15;
constexpr int kMaxBlBits = 7;
constexpr int kEndBlock = 256;
constexpr int kRep3_6 = 16;
constexpr int kRepz3_10 = 17;
constexpr int kRepz11_138 = 18;

const int kExtraLBits[kLengthCodes] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kExtraDBits[kDCodes] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                  6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const int kExtraBlBits[kBlCodes] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7};
const uint8_t kBlOrder[kBlCodes] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct Config {
  int good, lazy, nice, chain;
  bool slow;
};
const Config kConfig[10] = {{0, 0, 0, 0, false},      {4, 4, 8, 4, false},
                            {4, 5, 16, 8, false},     {4, 6, 32, 32, false},
                            {4, 4, 16, 16, true},     {8, 16, 32, 32, true},
                            {8, 16, 128, 128, true},  {8, 32, 128, 256, true},
                            {32, 128, 258, 1024, true}, {32, 258, 258, 4096, true}};

// One node of a Huffman tree. fc is the frequency, then the code; dl is the
// parent, then the code length (zlib overlays them the same way).
struct Ct {
  uint16_t fc = 0;
  uint16_t dl = 0;
};

unsigned biReverse(unsigned code, int len) {
  unsigned res = 0;
  do {
    res |= code & 1;
    code >>= 1;
    res <<= 1;
  } while (--len > 0);
  return res >> 1;
}

void genCodes(Ct* tree, int maxCode, const uint16_t* blCount) {
  uint16_t nextCode[kMaxBits + 1];
  unsigned code = 0;
  for (int bits = 1; bits <= kMaxBits; ++bits) {
    code = (code + blCount[bits - 1]) << 1;
    nextCode[bits] = static_cast<uint16_t>(code);
  }
  for (int n = 0; n <= maxCode; ++n) {
    const int len = tree[n].dl;
    if (len == 0) continue;
    tree[n].fc = static_cast<uint16_t>(biReverse(nextCode[len]++, len));
  }
}

struct StaticTables {
  uint8_t distCode[512];
  uint8_t lengthCode[256];
  int baseLength[kLengthCodes];
  int baseDist[kDCodes];
  Ct ltree[kLCodes + 2];
  Ct dtree[kDCodes];
  StaticTables() {
    int length = 0, code = 0;
    for (code = 0; code < kLengthCodes - 1; ++code) {
      baseLength[code] = length;
      for (int n = 0; n < (1 << kExtraLBits[code]); ++n)
        lengthCode[length++] = static_cast<uint8_t>(code);
    }
    lengthCode[length - 1] = static_cast<uint8_t>(code);
    int dist = 0;
    for (code = 0; code < 16; ++code) {
      baseDist[code] = dist;
      for (int n = 0; n < (1 << kExtraDBits[code]); ++n) distCode[dist++] = static_cast<uint8_t>(code);
    }
    dist >>= 7;
    for (; code < kDCodes; ++code) {
      baseDist[code] = dist << 7;
      for (int n = 0; n < (1 << (kExtraDBits[code] - 7)); ++n)
        distCode[256 + dist++] = static_cast<uint8_t>(code);
    }
    uint16_t blCount[kMaxBits + 1] = {0};
    int n = 0;
    while (n <= 143) ltree[n++].dl = 8, blCount[8]++;
    while (n <= 255) ltree[n++].dl = 9, blCount[9]++;
    while (n <= 279) ltree[n++].dl = 7, blCount[7]++;
    while (n <= 287) ltree[n++].dl = 8, blCount[8]++;
    genCodes(ltree, kLCodes + 1, blCount);
    for (n = 0; n < kDCodes; ++n) {
      dtree[n].dl = 5;
      dtree[n].fc = static_cast<uint16_t>(biReverse(static_cast<unsigned>(n), 5));
    }
  }
};

const StaticTables& tables() {
  static const StaticTables t;
  return t;
}

inline int dCode(unsigned dist) {
  const StaticTables& t = tables();
  return dist < 256 ? t.distCode[dist] : t.distCode[256 + (dist >> 7)];
}

struct StaticDesc {
  const Ct* staticTree;
  const int* extraBits;
  int extraBase;
  int elems;
  int maxLength;
};

struct TreeDesc {
  Ct* dynTree = nullptr;
  int maxCode = 0;
  const StaticDesc* stat = nullptr;
};

class Deflater {
 public:
  Deflater(const std::string& in, int level) : in_(in), level_(level) {
    const Config& c = kConfig[level];
    goodMatch_ = c.good;
    maxLazy_ = c.lazy;
    niceMatch_ = c.nice;
    maxChain_ = c.chain;
    window_.assign(2 * kWSize + 8, 0);
    head_.assign(kHashSize, 0);
    prev_.assign(kWSize, 0);
    symBuf_.reserve(static_cast<size_t>(kLitBufSize) * 3);
    static const StaticDesc lDesc = {tables().ltree, kExtraLBits, kLiterals + 1, kLCodes, kMaxBits};
    static const StaticDesc dDesc = {tables().dtree, kExtraDBits, 0, kDCodes, kMaxBits};
    static const StaticDesc blDesc = {nullptr, kExtraBlBits, 0, kBlCodes, kMaxBlBits};
    lDesc_.dynTree = dynLtree_;
    lDesc_.stat = &lDesc;
    dDesc_.dynTree = dynDtree_;
    dDesc_.stat = &dDesc;
    blDesc_.dynTree = blTree_;
    blDesc_.stat = &blDesc;
    initBlock();
  }

  std::string run() {
    if (level_ == 0) {
      stored();
    } else if (kConfig[level_].slow) {
      slow();
    } else {
      fast();
    }
    return std::move(out_);
  }

 private:
  const std::string& in_;
  size_t inPos_ = 0;
  int level_;
  int goodMatch_, maxLazy_, niceMatch_, maxChain_;

  std::vector<uint8_t> window_;
  std::vector<uint16_t> head_, prev_;
  unsigned insH_ = 0;
  long blockStart_ = 0;
  unsigned matchLength_ = kMinMatch - 1, prevLength_ = kMinMatch - 1;
  unsigned strStart_ = 0, matchStart_ = 0, lookahead_ = 0, prevMatch_ = 0;
  bool matchAvailable_ = false;
  unsigned insert_ = 0;
  unsigned long highWater_ = 0;

  // trees
  Ct dynLtree_[kHeapSize];
  Ct dynDtree_[2 * kDCodes + 1];
  Ct blTree_[2 * kBlCodes + 1];
  TreeDesc lDesc_, dDesc_, blDesc_;
  uint16_t blCount_[kMaxBits + 1];
  int heap_[2 * kLCodes + 1];
  int heapLen_ = 0, heapMax_ = 0;
  uint8_t depth_[2 * kLCodes + 1];
  std::vector<uint8_t> symBuf_;  // dist (2 bytes LE), lc (1 byte)
  unsigned long optLen_ = 0, staticLen_ = 0;
  unsigned matches_ = 0;

  // bit output
  std::string out_;
  uint64_t biBuf_ = 0;
  int biValid_ = 0;

  // ----- bit output -------------------------------------------------------
  void sendBits(unsigned value, int length) {
    biBuf_ |= static_cast<uint64_t>(value) << biValid_;
    biValid_ += length;
    while (biValid_ >= 8) {
      out_.push_back(static_cast<char>(biBuf_ & 0xff));
      biBuf_ >>= 8;
      biValid_ -= 8;
    }
  }
  void sendCode(int c, const Ct* tree) { sendBits(tree[c].fc, tree[c].dl); }
  void biWindup() {
    if (biValid_ > 0) out_.push_back(static_cast<char>(biBuf_ & 0xff));
    biBuf_ = 0;
    biValid_ = 0;
  }

  // ----- trees ------------------------------------------------------------
  void initBlock() {
    for (int n = 0; n < kLCodes; ++n) dynLtree_[n].fc = 0;
    for (int n = 0; n < kDCodes; ++n) dynDtree_[n].fc = 0;
    for (int n = 0; n < kBlCodes; ++n) blTree_[n].fc = 0;
    dynLtree_[kEndBlock].fc = 1;
    optLen_ = staticLen_ = 0;
    symBuf_.clear();
    matches_ = 0;
  }

  static bool smaller(const Ct* tree, int n, int m, const uint8_t* depth) {
    return tree[n].fc < tree[m].fc || (tree[n].fc == tree[m].fc && depth[n] <= depth[m]);
  }

  void pqDownHeap(const Ct* tree, int k) {
    const int v = heap_[k];
    int j = k << 1;
    while (j <= heapLen_) {
      if (j < heapLen_ && smaller(tree, heap_[j + 1], heap_[j], depth_)) ++j;
      if (smaller(tree, v, heap_[j], depth_)) break;
      heap_[k] = heap_[j];
      k = j;
      j <<= 1;
    }
    heap_[k] = v;
  }

  void genBitlen(TreeDesc* desc) {
    Ct* tree = desc->dynTree;
    const int maxCode = desc->maxCode;
    const Ct* stree = desc->stat->staticTree;
    const int* extra = desc->stat->extraBits;
    const int base = desc->stat->extraBase;
    const int maxLength = desc->stat->maxLength;
    int h, n, m, bits, xbits;
    int overflow = 0;
    for (bits = 0; bits <= kMaxBits; ++bits) blCount_[bits] = 0;
    tree[heap_[heapMax_]].dl = 0;
    for (h = heapMax_ + 1; h < kHeapSize; ++h) {
      n = heap_[h];
      bits = tree[tree[n].dl].dl + 1;
      if (bits > maxLength) bits = maxLength, ++overflow;
      tree[n].dl = static_cast<uint16_t>(bits);
      if (n > maxCode) continue;
      blCount_[bits]++;
      xbits = 0;
      if (n >= base) xbits = extra[n - base];
      const unsigned long f = tree[n].fc;
      optLen_ += f * static_cast<unsigned>(bits + xbits);
      if (stree) staticLen_ += f * static_cast<unsigned>(stree[n].dl + xbits);
    }
    if (overflow == 0) return;
    do {
      bits = maxLength - 1;
      while (blCount_[bits] == 0) --bits;
      blCount_[bits]--;
      blCount_[bits + 1] = static_cast<uint16_t>(blCount_[bits + 1] + 2);
      blCount_[maxLength]--;
      overflow -= 2;
    } while (overflow > 0);
    for (bits = maxLength; bits != 0; --bits) {
      n = blCount_[bits];
      while (n != 0) {
        m = heap_[--h];
        if (m > maxCode) continue;
        if (static_cast<unsigned>(tree[m].dl) != static_cast<unsigned>(bits)) {
          optLen_ += (static_cast<unsigned long>(bits) - tree[m].dl) * tree[m].fc;
          tree[m].dl = static_cast<uint16_t>(bits);
        }
        --n;
      }
    }
  }

  void buildTree(TreeDesc* desc) {
    Ct* tree = desc->dynTree;
    const Ct* stree = desc->stat->staticTree;
    const int elems = desc->stat->elems;
    int n, m;
    int maxCode = -1;
    int node;
    heapLen_ = 0;
    heapMax_ = kHeapSize;
    for (n = 0; n < elems; ++n) {
      if (tree[n].fc != 0) {
        heap_[++heapLen_] = maxCode = n;
        depth_[n] = 0;
      } else {
        tree[n].dl = 0;
      }
    }
    while (heapLen_ < 2) {
      node = heap_[++heapLen_] = (maxCode < 2 ? ++maxCode : 0);
      tree[node].fc = 1;
      depth_[node] = 0;
      --optLen_;
      if (stree) staticLen_ -= stree[node].dl;
    }
    desc->maxCode = maxCode;
    for (n = heapLen_ / 2; n >= 1; --n) pqDownHeap(tree, n);
    node = elems;
    do {
      n = heap_[1];
      heap_[1] = heap_[heapLen_--];
      pqDownHeap(tree, 1);
      m = heap_[1];
      heap_[--heapMax_] = n;
      heap_[--heapMax_] = m;
      tree[node].fc = static_cast<uint16_t>(tree[n].fc + tree[m].fc);
      depth_[node] = static_cast<uint8_t>((depth_[n] >= depth_[m] ? depth_[n] : depth_[m]) + 1);
      tree[n].dl = tree[m].dl = static_cast<uint16_t>(node);
      heap_[1] = node++;
      pqDownHeap(tree, 1);
    } while (heapLen_ >= 2);
    heap_[--heapMax_] = heap_[1];
    genBitlen(desc);
    genCodes(tree, maxCode, blCount_);
  }

  void scanTree(Ct* tree, int maxCode) {
    int prevlen = -1;
    int nextlen = tree[0].dl;
    int count = 0, maxCount = 7, minCount = 4;
    if (nextlen == 0) maxCount = 138, minCount = 3;
    tree[maxCode + 1].dl = 0xffff;
    for (int n = 0; n <= maxCode; ++n) {
      const int curlen = nextlen;
      nextlen = tree[n + 1].dl;
      if (++count < maxCount && curlen == nextlen) continue;
      else if (count < minCount) blTree_[curlen].fc = static_cast<uint16_t>(blTree_[curlen].fc + count);
      else if (curlen != 0) {
        if (curlen != prevlen) blTree_[curlen].fc++;
        blTree_[kRep3_6].fc++;
      } else if (count <= 10) blTree_[kRepz3_10].fc++;
      else blTree_[kRepz11_138].fc++;
      count = 0;
      prevlen = curlen;
      if (nextlen == 0) maxCount = 138, minCount = 3;
      else if (curlen == nextlen) maxCount = 6, minCount = 3;
      else maxCount = 7, minCount = 4;
    }
  }

  void sendTree(Ct* tree, int maxCode) {
    int prevlen = -1;
    int nextlen = tree[0].dl;
    int count = 0, maxCount = 7, minCount = 4;
    if (nextlen == 0) maxCount = 138, minCount = 3;
    for (int n = 0; n <= maxCode; ++n) {
      const int curlen = nextlen;
      nextlen = tree[n + 1].dl;
      if (++count < maxCount && curlen == nextlen) continue;
      else if (count < minCount) {
        do { sendCode(curlen, blTree_); } while (--count != 0);
      } else if (curlen != 0) {
        if (curlen != prevlen) {
          sendCode(curlen, blTree_);
          --count;
        }
        sendCode(kRep3_6, blTree_);
        sendBits(static_cast<unsigned>(count - 3), 2);
      } else if (count <= 10) {
        sendCode(kRepz3_10, blTree_);
        sendBits(static_cast<unsigned>(count - 3), 3);
      } else {
        sendCode(kRepz11_138, blTree_);
        sendBits(static_cast<unsigned>(count - 11), 7);
      }
      count = 0;
      prevlen = curlen;
      if (nextlen == 0) maxCount = 138, minCount = 3;
      else if (curlen == nextlen) maxCount = 6, minCount = 3;
      else maxCount = 7, minCount = 4;
    }
  }

  int buildBlTree() {
    scanTree(dynLtree_, lDesc_.maxCode);
    scanTree(dynDtree_, dDesc_.maxCode);
    buildTree(&blDesc_);
    int maxBlIndex;
    for (maxBlIndex = kBlCodes - 1; maxBlIndex >= 3; --maxBlIndex)
      if (blTree_[kBlOrder[maxBlIndex]].dl != 0) break;
    optLen_ += 3 * (static_cast<unsigned long>(maxBlIndex) + 1) + 5 + 5 + 4;
    return maxBlIndex;
  }

  void sendAllTrees(int lcodes, int dcodes, int blcodes) {
    sendBits(static_cast<unsigned>(lcodes - 257), 5);
    sendBits(static_cast<unsigned>(dcodes - 1), 5);
    sendBits(static_cast<unsigned>(blcodes - 4), 4);
    for (int rank = 0; rank < blcodes; ++rank) sendBits(blTree_[kBlOrder[rank]].dl, 3);
    sendTree(dynLtree_, lcodes - 1);
    sendTree(dynDtree_, dcodes - 1);
  }

  void compressBlock(const Ct* ltree, const Ct* dtree) {
    const StaticTables& t = tables();
    size_t sx = 0;
    if (!symBuf_.empty()) {
      do {
        unsigned dist = symBuf_[sx++];
        dist += static_cast<unsigned>(symBuf_[sx++]) << 8;
        int lc = symBuf_[sx++];
        if (dist == 0) {
          sendCode(lc, ltree);
        } else {
          unsigned code = t.lengthCode[lc];
          sendCode(static_cast<int>(code) + kLiterals + 1, ltree);
          int extra = kExtraLBits[code];
          if (extra != 0) {
            lc -= t.baseLength[code];
            sendBits(static_cast<unsigned>(lc), extra);
          }
          --dist;
          code = static_cast<unsigned>(dCode(dist));
          sendCode(static_cast<int>(code), dtree);
          extra = kExtraDBits[code];
          if (extra != 0) {
            dist -= static_cast<unsigned>(t.baseDist[code]);
            sendBits(dist, extra);
          }
        }
      } while (sx < symBuf_.size());
    }
    sendCode(kEndBlock, ltree);
  }

  void storedBlock(const uint8_t* buf, unsigned long storedLen, bool last) {
    sendBits(static_cast<unsigned>((0 << 1) + (last ? 1 : 0)), 3);
    biWindup();
    out_.push_back(static_cast<char>(storedLen & 0xff));
    out_.push_back(static_cast<char>((storedLen >> 8) & 0xff));
    out_.push_back(static_cast<char>(~storedLen & 0xff));
    out_.push_back(static_cast<char>((~storedLen >> 8) & 0xff));
    out_.append(reinterpret_cast<const char*>(buf), storedLen);
  }

  void flushBlock(const uint8_t* buf, unsigned long storedLen, bool last) {
    unsigned long optLenb, staticLenb;
    int maxBlIndex = 0;
    buildTree(&lDesc_);
    buildTree(&dDesc_);
    maxBlIndex = buildBlTree();
    optLenb = (optLen_ + 3 + 7) >> 3;
    staticLenb = (staticLen_ + 3 + 7) >> 3;
    if (staticLenb <= optLenb) optLenb = staticLenb;
    if (storedLen + 4 <= optLenb && buf != nullptr) {
      storedBlock(buf, storedLen, last);
    } else if (staticLenb == optLenb) {
      sendBits(static_cast<unsigned>((1 << 1) + (last ? 1 : 0)), 3);
      compressBlock(tables().ltree, tables().dtree);
    } else {
      sendBits(static_cast<unsigned>((2 << 1) + (last ? 1 : 0)), 3);
      sendAllTrees(lDesc_.maxCode + 1, dDesc_.maxCode + 1, maxBlIndex + 1);
      compressBlock(dynLtree_, dynDtree_);
    }
    initBlock();
    if (last) biWindup();
  }

  void flushBlockOnly(bool last) {
    flushBlock(blockStart_ >= 0 ? &window_[static_cast<size_t>(blockStart_)] : nullptr,
               static_cast<unsigned long>(static_cast<long>(strStart_) - blockStart_), last);
    blockStart_ = static_cast<long>(strStart_);
  }

  bool tallyLit(unsigned c) {
    symBuf_.push_back(0);
    symBuf_.push_back(0);
    symBuf_.push_back(static_cast<uint8_t>(c));
    dynLtree_[c].fc++;
    return symBuf_.size() == static_cast<size_t>(kLitBufSize - 1) * 3;
  }

  bool tallyDist(unsigned distance, unsigned length) {
    const uint16_t dist = static_cast<uint16_t>(distance);
    symBuf_.push_back(static_cast<uint8_t>(dist & 0xff));
    symBuf_.push_back(static_cast<uint8_t>(dist >> 8));
    symBuf_.push_back(static_cast<uint8_t>(length));
    ++matches_;
    const unsigned d = distance - 1;
    dynLtree_[tables().lengthCode[length & 0xff] + kLiterals + 1].fc++;
    dynDtree_[dCode(d)].fc++;
    return symBuf_.size() == static_cast<size_t>(kLitBufSize - 1) * 3;
  }

  // ----- the window -------------------------------------------------------
  void updateHash(unsigned* h, unsigned c) const { *h = ((*h << kHashShift) ^ c) & kHashMask; }

  unsigned insertString(unsigned str) {
    updateHash(&insH_, window_[str + (kMinMatch - 1)]);
    const unsigned matchHead = head_[insH_];
    prev_[str & kWMask] = head_[insH_];
    head_[insH_] = static_cast<uint16_t>(str);
    return matchHead;
  }

  void slideHash() {
    for (auto& m : head_) m = static_cast<uint16_t>(m >= kWSize ? m - kWSize : 0);
    for (auto& m : prev_) m = static_cast<uint16_t>(m >= kWSize ? m - kWSize : 0);
  }

  void fillWindow() {
    const unsigned windowSize = 2 * kWSize;
    do {
      unsigned more = windowSize - lookahead_ - strStart_;
      if (strStart_ >= static_cast<unsigned>(kWSize + kMaxDist)) {
        std::memcpy(window_.data(), window_.data() + kWSize, kWSize - more);
        matchStart_ -= kWSize;
        strStart_ -= kWSize;
        blockStart_ -= kWSize;
        slideHash();
        more += kWSize;
      }
      if (inPos_ >= in_.size()) break;
      const size_t n = std::min<size_t>(more, in_.size() - inPos_);
      std::memcpy(window_.data() + strStart_ + lookahead_, in_.data() + inPos_, n);
      inPos_ += n;
      lookahead_ += static_cast<unsigned>(n);
      if (lookahead_ + insert_ >= kMinMatch) {
        unsigned str = strStart_ - insert_;
        insH_ = window_[str];
        updateHash(&insH_, window_[str + 1]);
        while (insert_) {
          updateHash(&insH_, window_[str + kMinMatch - 1]);
          prev_[str & kWMask] = head_[insH_];
          head_[insH_] = static_cast<uint16_t>(str);
          ++str;
          --insert_;
          if (lookahead_ + insert_ < kMinMatch) break;
        }
      }
    } while (lookahead_ < kMinLookahead && inPos_ < in_.size());
    if (highWater_ < windowSize) {
      const unsigned long curr = strStart_ + lookahead_;
      if (highWater_ < curr) {
        unsigned long init = windowSize - curr;
        if (init > static_cast<unsigned long>(kWinInit)) init = kWinInit;
        std::memset(window_.data() + curr, 0, init);
        highWater_ = curr + init;
      } else if (highWater_ < curr + kWinInit) {
        unsigned long init = curr + kWinInit - highWater_;
        if (init > windowSize - highWater_) init = windowSize - highWater_;
        std::memset(window_.data() + highWater_, 0, init);
        highWater_ += init;
      }
    }
  }

  unsigned longestMatch(unsigned curMatch) {
    unsigned chainLength = static_cast<unsigned>(maxChain_);
    const uint8_t* scan = window_.data() + strStart_;
    int bestLen = static_cast<int>(prevLength_);
    int niceMatch = niceMatch_;
    const unsigned limit = strStart_ > static_cast<unsigned>(kMaxDist) ? strStart_ - kMaxDist : 0;
    uint8_t scanEnd1 = scan[bestLen - 1];
    uint8_t scanEnd = scan[bestLen];
    if (prevLength_ >= static_cast<unsigned>(goodMatch_)) chainLength >>= 2;
    if (static_cast<unsigned>(niceMatch) > lookahead_) niceMatch = static_cast<int>(lookahead_);
    do {
      const uint8_t* match = window_.data() + curMatch;
      if (match[bestLen] != scanEnd || match[bestLen - 1] != scanEnd1 || match[0] != scan[0] ||
          match[1] != scan[1])
        continue;
      // scan[2] == match[2] by the equal hash keys; the rest is compared.
      int len = 3;
      while (len < kMaxMatch && scan[len] == match[len]) ++len;
      if (len > bestLen) {
        matchStart_ = curMatch;
        bestLen = len;
        if (len >= niceMatch) break;
        scanEnd1 = scan[bestLen - 1];
        scanEnd = scan[bestLen];
      }
    } while ((curMatch = prev_[curMatch & kWMask]) > limit && --chainLength != 0);
    if (static_cast<unsigned>(bestLen) <= lookahead_) return static_cast<unsigned>(bestLen);
    return lookahead_;
  }

  void fast() {
    unsigned hashHead;
    bool bflush;
    for (;;) {
      if (lookahead_ < kMinLookahead) {
        fillWindow();
        if (lookahead_ == 0) break;
      }
      hashHead = 0;
      if (lookahead_ >= kMinMatch) hashHead = insertString(strStart_);
      if (hashHead != 0 && strStart_ - hashHead <= static_cast<unsigned>(kMaxDist))
        matchLength_ = longestMatch(hashHead);
      if (matchLength_ >= kMinMatch) {
        bflush = tallyDist(strStart_ - matchStart_, matchLength_ - kMinMatch);
        lookahead_ -= matchLength_;
        if (matchLength_ <= static_cast<unsigned>(maxLazy_) && lookahead_ >= kMinMatch) {
          --matchLength_;
          do {
            ++strStart_;
            insertString(strStart_);
          } while (--matchLength_ != 0);
          ++strStart_;
        } else {
          strStart_ += matchLength_;
          matchLength_ = 0;
          insH_ = window_[strStart_];
          updateHash(&insH_, window_[strStart_ + 1]);
        }
      } else {
        bflush = tallyLit(window_[strStart_]);
        --lookahead_;
        ++strStart_;
      }
      if (bflush) flushBlockOnly(false);
    }
    insert_ = strStart_ < kMinMatch - 1 ? strStart_ : kMinMatch - 1;
    flushBlockOnly(true);
  }

  void slow() {
    unsigned hashHead;
    bool bflush;
    for (;;) {
      if (lookahead_ < kMinLookahead) {
        fillWindow();
        if (lookahead_ == 0) break;
      }
      hashHead = 0;
      if (lookahead_ >= kMinMatch) hashHead = insertString(strStart_);
      prevLength_ = matchLength_;
      prevMatch_ = matchStart_;
      matchLength_ = kMinMatch - 1;
      if (hashHead != 0 && prevLength_ < static_cast<unsigned>(maxLazy_) &&
          strStart_ - hashHead <= static_cast<unsigned>(kMaxDist)) {
        matchLength_ = longestMatch(hashHead);
        if (matchLength_ <= 5 && (matchLength_ == kMinMatch && strStart_ - matchStart_ > kTooFar))
          matchLength_ = kMinMatch - 1;
      }
      if (prevLength_ >= kMinMatch && matchLength_ <= prevLength_) {
        const unsigned maxInsert = strStart_ + lookahead_ - kMinMatch;
        bflush = tallyDist(strStart_ - 1 - prevMatch_, prevLength_ - kMinMatch);
        lookahead_ -= prevLength_ - 1;
        prevLength_ -= 2;
        do {
          if (++strStart_ <= maxInsert) insertString(strStart_);
        } while (--prevLength_ != 0);
        matchAvailable_ = false;
        matchLength_ = kMinMatch - 1;
        ++strStart_;
        if (bflush) flushBlockOnly(false);
      } else if (matchAvailable_) {
        bflush = tallyLit(window_[strStart_ - 1]);
        if (bflush) flushBlockOnly(false);
        ++strStart_;
        --lookahead_;
      } else {
        matchAvailable_ = true;
        ++strStart_;
        --lookahead_;
      }
    }
    if (matchAvailable_) {
      tallyLit(window_[strStart_ - 1]);
      matchAvailable_ = false;
    }
    insert_ = strStart_ < kMinMatch - 1 ? strStart_ : kMinMatch - 1;
    flushBlockOnly(true);
  }

  void stored() {
    size_t pos = 0;
    do {
      const size_t len = std::min<size_t>(65535, in_.size() - pos);
      const bool last = pos + len >= in_.size();
      storedBlock(reinterpret_cast<const uint8_t*>(in_.data()) + pos, len, last);
      pos += len;
    } while (pos < in_.size());
  }
};

}  // namespace

std::string rawDeflate(const std::string& input, int level) {
  if (level < 0 || level > 9) level = 6;
  Deflater d(input, level);
  return d.run();
}

}  // namespace piet::fileio
