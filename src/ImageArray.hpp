/*******************************************************************************
 * Copyright (c) 2016, 2016 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/


#ifndef IMAGEARRAY_INCL
#define IMAGEARRAY_INCL

#define ADD   '+'
#define SUB   '-'
#define MUL   '*'
#define DIV   '/'
#define MIN   'M'
#define MAX   'X'
#define LT   'L'
#define GT   'G'
#define GE   'g'
#define LE   'l'
#define EQ   'E'
#define NOTQ   'N'


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
#include "Jit.hpp"
#include "ilgen/TypeDictionary.hpp"
#include "ilgen/MethodBuilder.hpp"
#include <iostream>


namespace TR { class TypeDictionary; }

//size, width, height, stride, data, result
typedef void (ImageArrayFunctionType)(int32_t, int32_t, int32_t, int32_t, double **, double *);

class ImageArray : public TR::MethodBuilder
   {
   private:
   void Store2D(TR::IlBuilder *bldr,
                TR::IlValue *base,
                TR::IlValue *first,
                TR::IlValue *second,
                TR::IlValue *N,
                TR::IlValue *value);
   TR::IlValue *Load2D(TR::IlBuilder *bldr,
                       TR::IlValue *base,
                       TR::IlValue *first,
                       TR::IlValue *second,
                       TR::IlValue *W, TR::IlValue *H);
   TR::IlValue *Load2DAbs(TR::IlBuilder *bldr,
                       TR::IlValue *base,
                       TR::IlValue *first,
                       TR::IlValue *second,
                          TR::IlValue *W, TR::IlValue *H);
   TR::IlValue *
      Abs(TR::IlBuilder *bldr, TR::IlValue *first);

   TR::IlValue *
      MaxAbs(TR::IlBuilder *bldr, TR::IlValue *first, TR::IlValue *max);
   TR::IlValue *
      function(TR::IlBuilder *bldr, std::vector<TR::IlValue*> &vects, char &function);
      
   void PrintString (TR::IlBuilder *bldr, const char *s);
   TR::IlType *pInt32;
   TR::IlType *pInt64;
   TR::IlType *pDouble;
   TR::IlType *ppDouble;
   TR::IlValue *i, *j;
   gnine::Cell cell_;
   std::map<std::string, TR::IlValue*> symbols;
   std::map<std::string, size_t> argNameToIndex;
   std::vector<TR::IlValue*> argv;
   public:
      void runByteCodes(gnine::Cell);
      
      TR::IlValue* eval(TR::IlBuilder *bldr, gnine::Cell &c);
      TR::IlValue* functionHandler(TR::IlBuilder *, const std::string &functionName,
                                          std::vector<TR::IlValue*> &args);
                                   TR::IlValue* numberHandler(TR::IlBuilder *, const std::string &number);
                                   TR::IlValue* symbolHandler(TR::IlBuilder *, const std::string &name);

      
   ImageArray(TR::TypeDictionary *);
      bool buildIL() override;
   };

#endif // !defined(IMAGEARRAY_INCL)
