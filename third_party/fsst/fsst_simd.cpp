// this software is distributed under the MIT License (http://www.opensource.org/licenses/MIT):
//
// Copyright 2018-2020, CWI, TU Munich, FSU Jena
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files
// (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify,
// merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// - The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
// IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// You can contact the authors via the FSST source repository : https://github.com/cwida/fsst
#include "libfsst.hpp"

namespace libfsst {

size_t compressSIMD(SymbolTable &symbolTable, u8* symbolBase, size_t nlines, const size_t len[], u8* const line[], size_t size, u8* dst, size_t lenOut[], u8* strOut[], int unroll) {
   size_t curLine = 0, inOff = 0, outOff = 0, batchPos = 0, empty = 0, budget = size;
   u8 *lim = dst + size, *codeBase = symbolBase + (1<<18); // 512KB temp space for compressing 512 strings 
   SIMDjob input[512];  // combined offsets of input strings (cur,end), and string #id (pos) and output (dst) pointer
   SIMDjob output[512]; // output are (pos:9,dst:19) end pointers (compute compressed length from this)
   size_t jobLine[512]; // for which line in the input sequence was this job (needed because we may split a line into multiple jobs)

   while (curLine < nlines && outOff <= (1<<19)) {
      size_t prevLine = curLine, chunk, curOff = 0;
 
      // bail out if the output buffer cannot hold the compressed next string fully
      if (((len[curLine]-curOff)*2 + 7) > budget) break; // see below for the +7
      else budget -= (len[curLine]-curOff)*2;

      strOut[curLine] = (u8*) 0; 
      lenOut[curLine] = 0;

      do {
         do {
            chunk = len[curLine] - curOff;
            if (chunk > 511) {
               chunk = 511; // large strings need to be chopped up into segments of 511 bytes
            }
            // create a job in this batch
            SIMDjob job;
            job.cur = inOff;
            job.end = job.cur + chunk;
            job.pos = batchPos;
            job.out = outOff;
   
            // worst case estimate for compressed size (+7 is for the scatter that writes extra 7 zeros)
            outOff += 7 + 2*(size_t)(job.end - job.cur); // note, total size needed is 512*(511*2+7) bytes.
            if (outOff > (1<<19)) break; // simdbuf may get full, stop before this chunk
   
            // register job in this batch
            input[batchPos] = job;
            jobLine[batchPos] = curLine;
   
            if (chunk == 0) {
               empty++; // detect empty chunks -- SIMD code cannot handle empty strings, so they need to be filtered out
            } else {
               // copy string chunk into temp buffer 
               memcpy(symbolBase + inOff, line[curLine] + curOff, chunk);
               inOff += chunk;
               curOff += chunk;
               symbolBase[inOff++] = (u8) symbolTable.terminator; // write an extra char at the end that will not be encoded
            }
            if (++batchPos == 512) break;
         } while(curOff < len[curLine]);
   
         if ((batchPos == 512) || (outOff > (1<<19)) || (++curLine >= nlines) || (((len[curLine])*2 + 7) > budget)) { // cannot accumulate more?
            if (batchPos-empty >= 32) { // if we have enough work, fire off fsst_compressAVX512 (32 is due to max 4x8 unrolling)
               // radix-sort jobs on length (longest string first) 
               // -- this provides best load balancing and allows to skip empty jobs at the end
               u16 sortpos[513]; 
               memset(sortpos, 0, sizeof(sortpos));
   
               // calculate length histo 
               for(size_t i=0; i<batchPos; i++) { 
                  size_t len = input[i].end - input[i].cur; 
                  sortpos[512UL - len]++;
               }
               // calculate running sum
               for(size_t i=1; i<=512; i++) 
                  sortpos[i] += sortpos[i-1]; 
   
               // move jobs to their final destination
               SIMDjob inputOrdered[512];
               for(size_t i=0; i<batchPos; i++) {
                  size_t len = input[i].end - input[i].cur; 
                  size_t pos = sortpos[511UL - len]++;
                  inputOrdered[pos] = input[i]; 
                }
               // finally.. SIMD compress max 256KB of simdbuf into (max) 512KB of simdbuf (but presumably much less..) 
               for(size_t done = fsst_compressAVX512(symbolTable, codeBase, symbolBase, inputOrdered, output, batchPos-empty, unroll);
                   done < batchPos; done++) output[done] = inputOrdered[done]; 
            } else {
               memcpy(output, input, batchPos*sizeof(SIMDjob));
            }
   
            // finish encoding (unfinished strings in process, plus the few last strings not yet processed)
            for(size_t i=0; i<batchPos; i++) {
               SIMDjob job = output[i];
               if (job.cur < job.end) { // finish encoding this string with scalar code
                  u8* cur = symbolBase + job.cur;
                  u8* end = symbolBase + job.end;
                  u8* out = codeBase + job.out;
                  while (cur < end) {
                     u64 word = fsst_unaligned_load(cur);
                     size_t code = symbolTable.shortCodes[word & 0xFFFF];
                     size_t pos = word & 0xFFFFFF;
                     size_t idx = FSST_HASH(pos)&(symbolTable.hashTabSize-1);
                     Symbol s = symbolTable.hashTab[idx];
                     out[1] = (u8) word; // speculatively write out escaped byte
                     word &= (0xFFFFFFFFFFFFFFFF >> (u8) s.icl);
                     if ((s.icl < FSST_ICL_FREE) && s.load_num() == word) {
                        *out++ = (u8) s.code(); cur += s.length();
                     } else {
                        // could be a 2-byte or 1-byte code, or miss
                        // handle everything with predication 
                        *out = (u8) code; 
                        out += 1+((code&FSST_CODE_BASE)>>8);
                        cur += (code>>FSST_LEN_BITS); 
                    }
                  }
                  job.out = out - codeBase;
               } 
               // postprocess job info
               job.cur = 0;
               job.end = job.out - input[job.pos].out; // misuse .end field as compressed size 
               job.out = input[job.pos].out; // reset offset to start of encoded string
               input[job.pos] = job; 
            }
   
            // copy out the result data
            for(size_t i=0; i<batchPos; i++) {
               size_t lineNr = jobLine[i]; // the sort must be order-preserving, as we concatenate results string in order
               size_t sz = input[i].end; // had stored compressed lengths here
               if (!strOut[lineNr]) strOut[lineNr] = dst; // first segment will be the strOut pointer
               lenOut[lineNr] += sz; // add segment (lenOut starts at 0 for this reason)
               memcpy(dst, codeBase+input[i].out, sz);
               dst += sz;
            }
   
            // go for the next batch of 512 chunks
            inOff = outOff = batchPos = empty = 0;
            budget = (size_t) (lim - dst);
         } 
      } while (curLine == prevLine && outOff <= (1<<19));
   }
   return curLine;
}

}  // namespace libfsst
