/*******************************************************************************
 * Eclipse OMR JitBuilder implementation of Luke Dodd's Pixslam
 * Author Geoffrey Duimovich
 * G9: Just in Time Image Processing with Eclipse OMR
 * COMP4905 – Honours Project
 *******************************************************************************/


#ifndef IMAGEARRAY_INCL
#define IMAGEARRAY_INCL

#include "Image.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <functional>
#include <stdexcept>
#include <algorithm>
#include <numeric>
#include <map>
#include <dlfcn.h>
#include <errno.h>
#include <vector>
#include <Parser.h>
#include <iostream>
#include "JitBuilder.hpp"


//size, width, height, stride, data, result
typedef void (ImageArrayFunctionType)(int32_t, int32_t, double **, double *);

static const char *argsAndTempNames[] = {
   "arg00", "arg01", "arg02", "arg03", "arg04", "arg05", "arg06",
   "arg07", "arg08", "arg09", "arg10", "arg11", "arg12", "arg13",
   "arg14", "arg15", "arg16", "arg17", "arg18", "arg19", "arg20",
   "arg21", "arg22", "arg23", "arg24", "arg25", "arg26", "arg27",
   "arg28", "arg29", "arg30", "arg31", "arg32"};


class ImageArray : public OMR::JitBuilder::MethodBuilder
   {
   private:
   void Store2D(OMR::JitBuilder::IlBuilder *bldr,
                OMR::JitBuilder::IlValue *base,
                OMR::JitBuilder::IlValue *first,
                OMR::JitBuilder::IlValue *second,
                OMR::JitBuilder::IlValue *value);
   OMR::JitBuilder::IlValue *Load2D(OMR::JitBuilder::IlBuilder *bldr,
                       OMR::JitBuilder::IlValue *base,
                       OMR::JitBuilder::IlValue *first,
                       OMR::JitBuilder::IlValue *second);
   
   OMR::JitBuilder::IlValue *
      Abs32(OMR::JitBuilder::IlBuilder *bldr, OMR::JitBuilder::IlValue *first);
   
      void min(OMR::JitBuilder::IlBuilder *bldr, OMR::JitBuilder::IlValue* val1);
      void max(OMR::JitBuilder::IlBuilder *bldr, OMR::JitBuilder::IlValue* val1);
      OMR::JitBuilder::IlValue* cast(OMR::JitBuilder::IlBuilder *bldr, OMR::JitBuilder::IlValue* val1);

   OMR::JitBuilder::IlValue *
   GetIndex(OMR::JitBuilder::IlBuilder *bldr,
                        OMR::JitBuilder::IlValue *j,
            OMR::JitBuilder::IlValue *W);
      
      OMR::JitBuilder::IlValue *
   Fib(OMR::JitBuilder::IlBuilder *bldr, OMR::JitBuilder::IlValue *n);

   void PrintString (OMR::JitBuilder::IlBuilder *bldr, const char *s);
   OMR::JitBuilder::IlType *pInt32;
   OMR::JitBuilder::IlType *pDouble;
   OMR::JitBuilder::IlType *ppDouble;
   OMR::JitBuilder::IlValue *i, *j, *c;
   gnine::Cell cell_;
      bool danger_;
   std::map<std::string, size_t> argNameToIndex;
   std::map<std::string, const char *> symbols_map;
   
   std::vector<OMR::JitBuilder::IlValue*> argv;
   public:
      void runByteCodes(gnine::Cell, bool);
      
      OMR::JitBuilder::IlValue* eval(OMR::JitBuilder::IlBuilder *bldr, gnine::Cell &c);
      OMR::JitBuilder::IlValue* functionHandler(OMR::JitBuilder::IlBuilder *, const std::string &functionName,
                                          std::vector<OMR::JitBuilder::IlValue*> &args);
                                   OMR::JitBuilder::IlValue* numberHandler(OMR::JitBuilder::IlBuilder *, const std::string &number);
                                   OMR::JitBuilder::IlValue* symbolHandler(OMR::JitBuilder::IlBuilder *, const std::string &name);


      
   ImageArray(OMR::JitBuilder::TypeDictionary *);
      bool buildIL() override;
   };

#endif // !defined(IMAGEARRAY_INCL)
