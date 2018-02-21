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


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <functional>
#include <map>
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
#include "ImageArray.hpp"
#include <iostream>

static void printArray(int32_t numLongs, double *array)
   {
   #define PRINTArray_LINE LINETOSTR(__LINE__)
   printf("printArray (%f) :\n", numLongs);
   for (int32_t i=0;i < numLongs;i++)
      printf("\t%lld\n", array[i]);
   }
static void printArrayDouble(int32_t numLongs, double *array)
{
#define PRINTArrayDouble_LINE LINETOSTR(__LINE__)
   printf("printArrayDouble (%d) :\n", numLongs);
   for (int32_t i=0;i < numLongs;i++) {
      if(i % 4 == 0) {
         printf("\n");
      }
      printf("%f ", array[i]);
   }
}

static void printInt64(int64_t num)
   {
   #define PRINTInt64_LINE LINETOSTR(__LINE__)
   printf("%lld\n", num);
   }
static void printString(int64_t ptr)
{
#define PRINTSTRING_LINE LINETOSTR(__LINE__)
   char *str = (char *) ptr;
   printf("%s", str);
}

static void printInt32(int32_t val)
{
#define PRINTINT32_LINE LINETOSTR(__LINE__)
   printf("%d", val);
}

static void printDouble(double val)
{
#define PRINTDOUBLE_LINE LINETOSTR(__LINE__)
   printf("%lf", val);
}

static void printPointer(int64_t val)
{
#define PRINTPOINTER_LINE LINETOSTR(__LINE__)
   printf("%llx", val);
}

void
ImageArray::Store2D(TR::IlBuilder *bldr,
                 TR::IlValue *base,
                 TR::IlValue *first,
                 TR::IlValue *second,
                 TR::IlValue *N,
                 TR::IlValue *value)
{
   bldr->StoreAt(
                 bldr->   IndexAt(pDouble,
                                  base,
                                  bldr->      Add(
                                                  bldr->         Mul(
                                                                     first,
                                                                     N),
                                                  second)),
                 value);
}
TR::IlValue *
ImageArray::Load2D(TR::IlBuilder *bldr,
                   TR::IlValue *base,
                   TR::IlValue *first,
                   TR::IlValue *second,
                   TR::IlValue *W, TR::IlValue *H)
{
   
//   bldr->Call("printInt32", 1,
//        first); PrintString(bldr, " first \n");
//   bldr->Call("printInt32", 1,
//              second); PrintString(bldr, " second \n");
//   bldr->Call("printInt32", 1,
//              bldr->      Add(
//                              bldr->         Mul(
//                                                 first,
//                                                 W),
//                              second)); PrintString(bldr, " arrayindex \n");
   
   TR::IlValue *firstAbs, *secondAbs, *fy, *sy;
   fy = bldr->ShiftR(first, bldr->ConstInt32(31));
   sy = bldr->ShiftR(second, bldr->ConstInt32(31));
   firstAbs = bldr->Sub(bldr->Xor(first, fy), fy);
   secondAbs = bldr->Sub(bldr->Xor(second, sy), sy);
   

   return
   bldr->LoadAt(pDouble,
                bldr->   IndexAt(pDouble,
                                 base,
                                bldr->Add( bldr->      Add(
                                                 bldr->         Mul(
                                                                    firstAbs,
                                                                    W),
                                                 secondAbs), firstAbs)) );
}

TR::IlValue *
ImageArray::MaxAbs(TR::IlBuilder *bldr, TR::IlValue *first, TR::IlValue *max)
{
   TR::IlBuilder *rc3True = NULL, *rc3False = NULL;
   bldr->IfThenElse(&rc3True, &rc3False,
                    bldr->GreaterThan(first, max));
   //   max-  abs((max - first))

   rc3True->Store("rc3",
                  rc3True->Sub(max, Abs(rc3True, rc3True->Sub(max, first))));
   rc3False->Store("rc3",
                  Abs(rc3False, first));
   
   return bldr->Load("rc3");

}



TR::IlValue *
ImageArray::Abs(TR::IlBuilder *bldr, TR::IlValue *first)
{
   TR::IlValue *fy;
   fy = bldr->ShiftR(first, bldr->ConstInt32(31));
   return bldr->Sub(bldr->Xor(first, fy), fy);
}

TR::IlValue *
ImageArray::Load2DAbs(TR::IlBuilder *bldr,
                   TR::IlValue *base,
                   TR::IlValue *first,
                   TR::IlValue *second,
                   TR::IlValue *W, TR::IlValue *H)
{
   
   
   TR::IlValue *firstAbs, *secondAbs, *fy, *sy;
   fy = bldr->ShiftR(first, bldr->ConstInt32(31));
   sy = bldr->ShiftR(second, bldr->ConstInt32(31));
   firstAbs = bldr->Sub(bldr->Xor(first, fy), fy);
   secondAbs = bldr->Sub(bldr->Xor(second, sy), sy);

   

   
   return
   bldr->LoadAt(pDouble,
                bldr->   IndexAt(pDouble,
                                 base,
                                 bldr->      Add(
                                                 bldr->         Mul(
                                                                    MaxAbs(bldr, first, W),
                                                                    W),
                                                 MaxAbs(bldr, second, H))));
}


ImageArray::ImageArray(TR::TypeDictionary *d)
   : MethodBuilder(d)
   {
   DefineLine(LINETOSTR(__LINE__));
   DefineFile(__FILE__);

   DefineName("imagearray");

   //size, width, height, stride, data, result

   pInt32 = d->PointerTo(Int32);
   pInt64 = d->PointerTo(Int64);
   pDouble = d->PointerTo(Double);
   ppDouble = d->PointerTo(pDouble);

   DefineParameter("size", Int32);
   DefineParameter("width", Int32);
   DefineParameter("height", Int32);
   DefineParameter("stride", Int32);
   DefineParameter("data", ppDouble);
   DefineParameter("result", pDouble);
   DefineReturnType(NoType);

   DefineFunction((char *)"printArray", 
                  (char *)__FILE__,
                  (char *)PRINTArray_LINE,
                  (void *)&printArray,
                  NoType,
                  2,
                  Int32,
                  pDouble);
   DefineFunction((char *)"printArrayDouble",
                  (char *)__FILE__,
                  (char *)PRINTArrayDouble_LINE,
                  (void *)&printArrayDouble,
                  NoType,
                  2,
                  Int32,
                  pDouble);
   
   DefineFunction((char *)"printString",
                  (char *)__FILE__,
                  (char *)PRINTSTRING_LINE,
                  (void *)&printString,
                  NoType,
                  1,
                  Int64);
   DefineFunction((char *)"printInt32",
                  (char *)__FILE__,
                  (char *)PRINTINT32_LINE,
                  (void *)&printInt32,
                  NoType,
                  1,
                  Int32);
   DefineFunction((char *)"printInt64",
                  (char *)__FILE__,
                  (char *)PRINTInt64_LINE,
                  (void *)&printInt64,
                  NoType,
                  1,
                  Int32);

   DefineFunction((char *)"printDouble",
                  (char *)__FILE__,
                  (char *)PRINTDOUBLE_LINE,
                  (void *)&printDouble,
                  NoType,
                  1,
                  Double);
   DefineFunction((char *)"printPointer",
                  (char *)__FILE__,
                  (char *)PRINTPOINTER_LINE,
                  (void *)&printPointer,
                  NoType,
                  1,
                  Int64);
   }
void
ImageArray::PrintString(TR::IlBuilder *bldr, const char *s)
{
//   bldr->Call("printString", 1,
//              bldr->   ConstInt64((int64_t)(char *)s));
}
void
ImageArray::runByteCodes(gnine::Cell cell)
{
   cell_ = cell;
   
//   uint8_t* entry;
//   int rc = compileMethodBuilder(this, &entry);
//   if (rc != 0) {
//      std::cout << "Compilation failed" << std::endl;
//      return;
//   }
//
//   void (*compiledFunction)() = reinterpret_cast<decltype(compiledFunction)>(entry);
//   (*compiledFunction)();

}

TR::IlValue* ImageArray::eval(TR::IlBuilder *bldr, gnine::Cell &c){
   switch(c.type){
      case gnine::Cell::Number:{
         return numberHandler(bldr, c.val.c_str());
      }case gnine::Cell::List:{
         std::vector<TR::IlValue*> evalArgs(c.list.size() - 1);
         
         std::transform(c.list.begin()+1, c.list.end(), evalArgs.begin(),
                        [this, &bldr](gnine::Cell &k) -> TR::IlValue * {
                           return eval(bldr, k);
                     
                        });
         
         return functionHandler(bldr, c.list[0].val, evalArgs);
      }case gnine::Cell::Symbol:{

         return symbolHandler(bldr, c.val);
      }
   }
   throw std::runtime_error("Should never get here.");
}



TR::IlValue* ImageArray::functionHandler(TR::IlBuilder *bldr, const std::string &functionName,
                                       std::vector<TR::IlValue*> &args) {
   
   char s = MUL;
   if(functionName == "+") {
      s = ADD;
   } else if(functionName == "*") {
      s = MUL;
   } else if(functionName == "/") {
      s = DIV;
   } else if(functionName == "-") {
      s = SUB;
   } else if(functionName == "min") {
      s = MIN;
   } else if(functionName == "max") {
      s = MAX;
   } else if(functionName == "<") {
      s = LT;
   } else if(functionName == ">") {
      s = GT;
   } else if(functionName == "<=") {
      s = LE;
   } else if(functionName == ">=") {
      s = GE;
   } else if(functionName == "==") {
      s = EQ;
   } else if(functionName == "!=") {
      s = NOTQ;
   } else if(functionName[0] == '@') {
      std::string imageName = std::string(functionName.begin()+1, functionName.end());
//      bldr->Call("printInt32", 1,
//                 i); PrintString(bldr, " i \n");
//     bldr->Call("printInt32", 1,
//                j); PrintString(bldr, " j \n");
//
//      bldr->Call("printInt32", 1,
//                 bldr->ConvertTo(Int32, args[0])); PrintString(bldr, "a0 \n");
//      bldr->Call("printInt32", 1,
//                 bldr->ConvertTo(Int32, args[1])); PrintString(bldr, "a1 \n");

      if(argNameToIndex.find(imageName) == argNameToIndex.end())
         std::runtime_error("Absolute indexing with unknown image " + imageName);
      return Load2D(bldr, argv[argNameToIndex.at(imageName)],
                       bldr->ConvertTo(Int32, args[0]),
                       bldr->ConvertTo(Int32, args[1]),
                       symbols["w"], symbols["h"]);
      

   } else {
//      bldr->Call("printInt32", 1,
//                 bldr->ConvertTo(Int32, args[1]));
//
   return Load2D(bldr, argv[argNameToIndex.at(functionName)],
                       bldr->Add(bldr->ConvertTo(Int32, args[0]), i),
                       bldr->Add(bldr->ConvertTo(Int32, args[1]), j),
                       symbols["w"], symbols["h"]);
      

      


   }

   return function(bldr, args, s);
}

TR::IlValue* ImageArray::numberHandler(TR::IlBuilder *bldr, const std::string &number) {
   double x = std::atof(number.c_str());
   return bldr->ConstDouble(x);
}
TR::IlValue* ImageArray::symbolHandler(TR::IlBuilder *bldr, const std::string &name){
   
   if(argNameToIndex.find(name) != argNameToIndex.end()){
      return Load2D(bldr, argv[argNameToIndex.at(name)],
                       i, j, symbols["w"], symbols["h"]);
   }else if(name == "i" || name == "j"){ // special symbols
      return name == "i" ? bldr->ConvertTo(Double, i) : bldr->ConvertTo(Double, j);
   }else if(name == "width" || name == "height"){
//      bldr->Call("printInt32", 1,
//                 symbols["w"]); PrintString(bldr, " wi \n");
//      bldr->Call("printInt32", 1,
//                 symbols["h"]); PrintString(bldr, " height \n");
//
      return name == "width" ? bldr->ConvertTo(Double, symbols["w"]) : bldr->ConvertTo(Double, symbols["h"]);
   }else {
      throw std::runtime_error("Unable to find symbol: " + name);
   }
   
}


TR::IlValue* ImageArray::function(TR::IlBuilder *bldr, std::vector<TR::IlValue*> &vects, char &function) {
   TR::IlBuilder *rc3True = NULL;
   TR::IlValue *one = bldr->ConstDouble(1.0);
   TR::IlValue *zero = bldr->ConstDouble(0.0);
   bldr->Store("sum", vects[0]);
   
   for (unsigned int l = 1; l < vects.size(); l++) {
      switch (function) {
         case ADD:
            bldr->Store("sum", bldr->Add(vects[l], bldr->Load("sum")));
            break;
         case SUB:
            bldr->Store("sum", bldr->Sub(bldr->Load("sum"), vects[l]));
            break;
         case MUL:
            bldr->Store("sum", bldr->Mul(vects[l], bldr->Load("sum")));
            break;
         case DIV:
            bldr->Store("sum", bldr->Div(bldr->Load("sum"), vects[l]));
            break;
         case MIN:
            rc3True = NULL;
            
            bldr->IfThen(&rc3True,
                         bldr->LessThan(vects[l], bldr->Load("sum")));
            rc3True->Store("sum",
                           vects[l]);
            break;
         case MAX:
            rc3True = NULL;
            bldr->IfThen(&rc3True,
                         bldr->GreaterThan(vects[l], bldr->Load("sum")));
            rc3True->Store("sum",
                           vects[l]);
            break;
         case GT:
            rc3True = NULL;
            bldr->Store("sum", zero);
            bldr->IfThen(&rc3True,
                         bldr->GreaterThan(vects[0], vects[1]));
            rc3True->Store("sum",
                           one);
            return bldr->Load("sum");
         case LT:
            rc3True = NULL;
            bldr->Store("sum", zero);
            bldr->IfThen(&rc3True,
                         bldr->LessThan(vects[0], vects[1]));
            rc3True->Store("sum",
                           one);
            return bldr->Load("sum");
         case GE:
            rc3True = NULL;
            bldr->Store("sum", zero);
            bldr->IfThen(&rc3True,
                         bldr->GreaterOrEqualTo(vects[0], vects[1]));
            rc3True->Store("sum",
                           one);
            return bldr->Load("sum");

         case LE:
            rc3True = NULL;
            bldr->Store("sum", zero);
            bldr->IfThen(&rc3True,
                         bldr->LessOrEqualTo(vects[0], vects[1]));
            rc3True->Store("sum",
                           one);
            return bldr->Load("sum");
         case EQ:
            rc3True = NULL;
            bldr->Store("sum", zero);
            bldr->IfThen(&rc3True,
                         bldr->EqualTo(vects[0], vects[1]));
            rc3True->Store("sum",
                           one);
            return bldr->Load("sum");
         case NOTQ:
            rc3True = NULL;
            bldr->Store("sum", zero);
            bldr->IfThen(&rc3True,
                         bldr->NotEqualTo(vects[0], vects[1]));
            rc3True->Store("sum",
                           one);
            return bldr->Load("sum");

         default:
            break;
      }
      
   }
   return bldr->Load("sum");
}




bool
ImageArray::buildIL()
   {
      //Constants
      TR::IlValue *one = ConstInt32(1);
      TR::IlValue *zero = ConstInt32(0);
      
      //size, width, height, stride, data, result
      TR::IlValue *width = Load("width");
      TR::IlValue *height = Load("height");
      TR::IlValue *result = Load("result");

      PrintString(this, " \n");

//      Call("printArrayDouble", 2, ConstInt32(16),
//                  data); PrintString(this, " data \n");


      
      symbols["w"] = Sub(Load("width"), one);
      symbols["h"] = Sub(Load("height"), one);

      TR::IlBuilder *builder  = this;

      
      gnine::Cell &argsCell = cell_.list[0];
      gnine::Cell &code = cell_.list[1];
      
      // Load arguments
      std::vector<std::string> argNames;
      for(gnine::Cell c : argsCell.list){
         if(c.type == gnine::Cell::Symbol)
            argNames.push_back(c.val);
         else
            throw std::runtime_error(
                                     "Function cell must be of form ((arg1 arg2 ...) (code))");
      }
      
      for(size_t i = 0; i < argNames.size(); ++i)
         argNameToIndex[argNames[i]] = i;
   
      for(size_t i = 0; i < argNames.size(); ++i) {
         argv.push_back(builder->LoadAt(ppDouble, builder->IndexAt(ppDouble, Load("data"), ConstInt32(i))));
      }

         
      TR::IlBuilder *iloop=NULL, *jloop=NULL;
      ForLoopUp("i", &iloop, zero, height, one);
      {
         i = iloop->Load("i");
         
         iloop->ForLoopUp("j", &jloop, zero, width, one);
         {
            j = jloop->Load("j");
            TR::IlValue *ret = eval(jloop, code);
//          jloop->Call("printDouble", 1,
//                       ret); PrintString(jloop, " output \n");
            Store2D(jloop, result, i, j, width,ret );

         }
      }
//      PrintString(this, " \n");

//      Call("printArrayDouble", 2,ConstInt32(16),
//           result); PrintString(this, " result \n");
//

   Return();

   return true;
   }
