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
#define IF   '3'
#define MIN   'M'
#define MAX   'X'
#define ABS   'A'
#define LT   'L'
#define GT   'G'
#define GE   'g'
#define LE   'l'
#define EQ   'E'
#define NOTQ   'N'
#define INT 'I'
#define POW2 'P'
#define FIB 'F'
#define AND '5'

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
typedef void (ImageArrayFunctionType)(int32_t, int32_t, double **, double *);

static const char *argsAndTempNames[] = {
   "arg00", "arg01", "arg02", "arg03", "arg04", "arg05", "arg06",
   "arg07", "arg08", "arg09", "arg10", "arg11", "arg12", "arg13",
   "arg14", "arg15", "arg16", "arg17", "arg18", "arg19", "arg20",
   "arg21", "arg22", "arg23", "arg24", "arg25", "arg26", "arg27",
   "arg28", "arg29", "arg30", "arg31", "arg32"};


class ImageArray : public TR::MethodBuilder
   {
   private:
   void Store2D(TR::IlBuilder *bldr,
                TR::IlValue *base,
                TR::IlValue *first,
                TR::IlValue *second,
                TR::IlValue *value);
   TR::IlValue *Load2D(TR::IlBuilder *bldr,
                       TR::IlValue *base,
                       TR::IlValue *first,
                       TR::IlValue *second);
   
   TR::IlValue *
      Abs32(TR::IlBuilder *bldr, TR::IlValue *first);
   
      void min(TR::IlBuilder *bldr, TR::IlValue* val1);
      void max(TR::IlBuilder *bldr, TR::IlValue* val1);
      TR::IlValue* cast(TR::IlBuilder *bldr, TR::IlValue* val1);

   TR::IlValue *
   GetIndex(TR::IlBuilder *bldr,
                        TR::IlValue *j,
            TR::IlValue *W);
      
      TR::IlValue *
   Fib(TR::IlBuilder *bldr, TR::IlValue *n);

   void PrintString (TR::IlBuilder *bldr, const char *s);
   TR::IlType *pInt32;
   TR::IlType *pDouble;
   TR::IlType *ppDouble;
   TR::IlValue *i, *j, *c;
   gnine::Cell cell_;
      bool danger_;
   std::map<std::string, TR::IlValue*> symbols;
   std::map<std::string, size_t> argNameToIndex;
   std::map<std::string, const char *> symbols_map;
   
   std::vector<TR::IlValue*> argv;
   public:
      void runByteCodes(gnine::Cell, bool);
      
      TR::IlValue* eval(TR::IlBuilder *bldr, gnine::Cell &c);
      TR::IlValue* functionHandler(TR::IlBuilder *, const std::string &functionName,
                                          std::vector<TR::IlValue*> &args);
                                   TR::IlValue* numberHandler(TR::IlBuilder *, const std::string &number);
                                   TR::IlValue* symbolHandler(TR::IlBuilder *, const std::string &name);


      
   ImageArray(TR::TypeDictionary *);
      bool buildIL() override;
   };

#endif // !defined(IMAGEARRAY_INCL)
