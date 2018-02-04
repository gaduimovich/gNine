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
#include <dlfcn.h>
#include <errno.h>

#include "Jit.hpp"
#include "ilgen/TypeDictionary.hpp"
#include "ilgen/MethodBuilder.hpp"
#include "ImageArray.hpp"

static void printArray(int32_t numLongs, int64_t *array)
   {
   #define PRINTArray_LINE LINETOSTR(__LINE__)
   printf("printArray (%d) :\n", numLongs);
   for (int32_t i=0;i < numLongs;i++)
      printf("\t%lld\n", array[i]);
   }
static void printArrayDouble(int32_t numLongs, double *array)
{
#define PRINTArrayDouble_LINE LINETOSTR(__LINE__)
   printf("printArrayDouble (%d) :\n", numLongs);
   for (int32_t i=0;i < numLongs;i++)
      printf("\t%f\n", array[i]);
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
   
   return
   bldr->LoadAt(pDouble,
                bldr->   IndexAt(pDouble,
                                 base,
                                 bldr->      Add(
                                                 bldr->         Mul(
                                                                    first,
                                                                    W),
                                                 second)));
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
   
   
//   TR::IlValue *firstAbs, *secondAbs, *fy, *sy;
//   fy = bldr->ShiftR(first, bldr->ConstInt32(31));
//   sy = bldr->ShiftR(second, bldr->ConstInt32(31));
//   firstAbs = bldr->Sub(bldr->Xor(first, fy), fy);
//   secondAbs = bldr->Sub(bldr->Xor(second, sy), sy);
//
   

   
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
   DefineParameter("size", Int32);
   DefineParameter("width", Int32);
   DefineParameter("height", Int32);
   DefineParameter("stride", Int32);
   DefineParameter("data", pDouble);
   DefineParameter("result", pDouble);
   DefineReturnType(NoType);

   DefineFunction((char *)"printArray", 
                  (char *)__FILE__,
                  (char *)PRINTArray_LINE,
                  (void *)&printArray,
                  NoType,
                  2,
                  Int32,
                  pInt64);
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
   bldr->Call("printString", 1,
              bldr->   ConstInt64((int64_t)(char *)s));
}

bool
ImageArray::buildIL()
   {
      //Constants
      TR::IlValue *one = ConstInt32(1);
      TR::IlValue *zero = ConstInt32(0);
      TR::IlValue *neg = ConstInt32(-1);
      
      //size, width, height, stride, data, result
      TR::IlValue *size = Load("size");
      TR::IlValue *width = Sub(Load("width"), one);
      TR::IlValue *height = Sub(Load("height"), one);
      TR::IlValue *stride = Sub(Load("stride"), one);
      TR::IlValue *data = Load("data");
      TR::IlValue *result = Load("result");

      TR::IlValue *A_upper;
      TR::IlValue *A_lower;
      TR::IlValue *A_middle;
      TR::IlValue *A_result;
//      Call("printInt32", 1,
//           MaxAbs(this, ConstInt32(25), ConstInt32(20)));      PrintString(this, "\n");
//      Call("printInt32", 1,
//           MaxAbs(this, ConstInt32(20), ConstInt32(20)));      PrintString(this, "\n");
//      Call("printInt32", 1,
//           MaxAbs(this, ConstInt32(10), ConstInt32(20)));      PrintString(this, "\n");
//      Call("printInt32", 1,
//           MaxAbs(this, ConstInt32(-5), ConstInt32(20)));      PrintString(this, "\n");
//      Call("printInt32", 1,
//           MaxAbs(this, ConstInt32(-20), ConstInt32(20)));      PrintString(this, "\n");

//      Call("printDouble", 1,
//           Load2DAbs(this, data, ConstInt32(-1), ConstInt32(-1), width, height));      PrintString(this, "\n");

      //Load2DAbs(this, data, ConstInt32(-20), ConstInt32(20), width, height)
      //   max-  abs((max - first))
//
//      PrintString(this, "   data is ");
//      Call("printPointer", 1,
//           data);
//      PrintString(this, "\n");
//
//      PrintString(this, "   result is ");
//      Call("printPointer", 1,
//           result);
//      PrintString(this, "\n");
//
//      PrintString(this, "   size, width, height, stride is \n");
//      Call("printInt32", 1,
//           size);      PrintString(this, "\n");
//
//      Call("printInt32", 1,
//           width);      PrintString(this, "\n");
//
//      Call("printInt32", 1,
//           height);      PrintString(this, "\n");
////
//      Call("printInt32", 1,
//           stride);      PrintString(this, "\n");
//
//      PrintString(this, "\n");
//          Call("printInt32", 1,
//               Abs(this, ConstInt32(-20)));      PrintString(this, "\n");
      
      /// |      x
      // y|
      //  |
      //i is x j is y
      TR::IlValue *i, *j;
      TR::IlBuilder *iloop=NULL, *jloop=NULL;
      ForLoopUp("i", &iloop, zero, Add(one, width), one);
      {
         i = iloop->Load("i");
         
         iloop->ForLoopUp("j", &jloop, zero, iloop->Add(one, height), one);
         {
            j = jloop->Load("j");
            
            A_upper =
            jloop->Add(
            jloop->Add(
                       Load2DAbs(jloop, data, jloop->Add(i, neg), jloop->Add(j, neg), width, height),
                       Load2DAbs(jloop, data, jloop->Add(i, zero), jloop->Add(j, neg), width, height)),
                       Load2DAbs(jloop, data, jloop->Add(i, one), jloop->Add(j, neg), width, height));
            A_middle =
            jloop->Add(
            jloop->Add(Load2DAbs(jloop, data, jloop->Add(i, neg), j, width, height),
                       Load2DAbs(jloop, data, i, j, width, height)),
                       Load2DAbs(jloop, data, jloop->Add(i, one), j, width, height));

            A_lower =
            jloop->Add(
            jloop->Add(
                       Load2DAbs(jloop, data, jloop->Add(i, neg), jloop->Add(j, one), width, height),
                       Load2DAbs(jloop, data, i, jloop->Add(j, one), width, height)),
                       Load2DAbs(jloop, data, jloop->Add(i, one), jloop->Add(j, one), width, height));
            
            
            A_result = jloop->Div(jloop->Add(jloop->Add(A_middle, A_lower), A_upper), jloop->ConstDouble(9.0));
            
            Store2D(jloop, result, i, j, stride, A_result);
            
         }
      }

   Return();

   return true;
   }
