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

#include "ImageArray.hpp"

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

void
ImageArray::Store2D(TR::IlBuilder *bldr,
                 TR::IlValue *base,
                 TR::IlValue *first,
                 TR::IlValue *second,
                 TR::IlValue *value)
{
   bldr->StoreAt(
                 bldr->   IndexAt(pDouble,
                                  base,
                                  bldr->      Add(
                                                  bldr->         Mul(
                                                                     first,
                                                                     bldr->Load("width")),
                                                  second)),
                 value);
   
   }

TR::IlValue *
ImageArray::GetIndex(TR::IlBuilder *bldr,
                   TR::IlValue *j,
                     TR::IlValue *W) {
   
   bldr->Store("abs", Abs32(bldr, j));
   

   return   bldr->Add(

                      bldr->Mul(bldr->GreaterThan(j, W),

                                bldr->Sub(bldr->Add(bldr->ConstInt32(1), bldr->Add(W, W)), bldr->Load("abs"))),

                   bldr->Mul(bldr->LessOrEqualTo(j, W), bldr->Load("abs")));
}






TR::IlValue *
ImageArray::Load2D(TR::IlBuilder *bldr,
                   TR::IlValue *base,
                   TR::IlValue *i,
                   TR::IlValue *j)

{
   
   TR::IlValue *reti = NULL;
   TR::IlValue *retj = NULL;
   if (danger_){
      reti = i;
      retj = j;
   } else {
      reti = GetIndex(bldr, i, bldr->Load("h"));
      retj = GetIndex(bldr, j, bldr->Load("w"));
   }
   
   return
   bldr->LoadAt(pDouble,
                bldr-> IndexAt(pDouble, base, bldr->Add(
                                                        bldr->         Mul(
                                                                           reti,
                                                                           bldr->Load("width")),
                                                        retj)));

   
}

TR::IlValue *
ImageArray::Abs32(TR::IlBuilder *bldr, TR::IlValue *first)
{
   
   return bldr->Mul(bldr->Sub(bldr->ShiftL(bldr->GreaterThan(first, bldr->ConstInt32(0)), bldr->ConstInt32(1)), bldr->ConstInt32(1)),
             first);
   
}

TR::IlValue *
ImageArray::Fib(TR::IlBuilder *bldr, TR::IlValue *n) {
   
   TR::IlBuilder *returnN = NULL;
   TR::IlBuilder *elseN = NULL;
   
   bldr->Store("n", n);
   
   bldr->IfThenElse(&returnN, &elseN,
                    bldr->LessThan(
                             bldr->Load("n"),
                             bldr->ConstDouble(2)));
   
   returnN->Store("Sum", bldr->Load("n"));
   
   
   elseN->Store("LastSum",
                elseN->ConstDouble(0));
   
   elseN->Store("Sum",
                elseN->ConstDouble(1));
   
   TR::IlBuilder *mloop = NULL;
   elseN->ForLoopUp("kk", &mloop,
                    elseN->ConstInt32(1),
                    elseN->ConvertTo(Int32, elseN->Load("n")),
                    elseN->ConstInt32(1));
   
   mloop->Store("tempSum",
                mloop->   Add(
                              mloop->      Load("Sum"),
                              mloop->      Load("LastSum")));
   mloop->Store("LastSum",
                mloop->   Load("Sum"));
   mloop->Store("Sum",
                mloop->   Load("tempSum"));
   
   return bldr->Load("Sum");
   
}



ImageArray::ImageArray(TR::TypeDictionary *d)
   : MethodBuilder(d)
   {
   DefineLine(LINETOSTR(__LINE__));
   DefineFile(__FILE__);

   DefineName("imagearray");

   //size, width, height, data, result

   pInt32 = d->PointerTo(Int32);
   pDouble = d->PointerTo(Double);
   ppDouble = d->PointerTo(pDouble);

   DefineParameter("width", Int32);
   DefineParameter("height", Int32);
   DefineParameter("data", ppDouble);
   DefineParameter("result", pDouble);
   DefineReturnType(NoType);
   
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
   DefineFunction((char *)"printDouble",
                  (char *)__FILE__,
                  (char *)PRINTDOUBLE_LINE,
                  (void *)&printDouble,
                  NoType,
                  1,
                  Double);
   }
void
ImageArray::PrintString(TR::IlBuilder *bldr, const char *s)
{
   bldr->Call("printString", 1,
              bldr->   ConstInt64((int64_t)(char *)s));
}
void
ImageArray::runByteCodes(gnine::Cell cell, bool danger)
{
   cell_ = cell;
   danger_ = danger;
   
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
   
   bldr->Store("sum", args[0]);

   if(functionName == "if") {
      TR::IlBuilder *rc3True = OrphanBuilder();
      TR::IlBuilder *rc3False = OrphanBuilder();
      
      bldr->IfThenElse(&rc3True, &rc3False,
                       bldr->EqualTo(args[0], bldr->ConstDouble(1)));
      
      rc3True->Store("sum", args[1]);
      rc3False->Store("sum", args[2]);
      

   } else if(functionName == "+") {
      for (unsigned int l = 1; l < args.size(); l++) {
               bldr->Store("sum", bldr->Add(args[l], bldr->Load("sum")));
      }
   } else if(functionName == "*") {
      for (unsigned int l = 1; l < args.size(); l++) {
         bldr->Store("sum", bldr->Mul(args[l], bldr->Load("sum")));
      }
      return bldr->Load("sum");
   } else if(functionName == "/") {
      for (unsigned int l = 1; l < args.size(); l++) {
         bldr->Store("sum", bldr->Div(bldr->Load("sum"), args[l]));
      }
   } else if(functionName == "-") {
      for (unsigned int l = 1; l < args.size(); l++) {
         bldr->Store("sum", bldr->Sub(bldr->Load("sum"), args[l]));
      }
   } else if(functionName == "min") {
      for (unsigned int l = 1; l < args.size(); l++) {
         min(bldr, args[l]);
      }
   } else if(functionName == "max") {
      for (unsigned int l = 1; l < args.size(); l++) {
         max(bldr, args[l]);
      }
   } else if(functionName == "int") {
      return cast(bldr, args[0]);
   } else if(functionName == "<") {
      return bldr->ConvertTo(Double, bldr->LessThan(args[0], args[1]));
   } else if(functionName == ">") {
      return bldr->ConvertTo(Double, bldr->GreaterThan(args[0], args[1]));
   } else if(functionName == "<=") {
      return bldr->ConvertTo(Double, bldr->LessOrEqualTo(args[0], args[1]));
   } else if(functionName == ">=") {
      return bldr->ConvertTo(Double, bldr->GreaterOrEqualTo(args[0], args[1]));
   } else if(functionName == "==") {
      return bldr->ConvertTo(Double, bldr->EqualTo(args[0], args[1]));
   } else if(functionName == "!=") {
      return bldr->ConvertTo(Double, bldr->NotEqualTo(args[0], args[1]));
   } else if(functionName == "fib") {
      return Fib(bldr, args[0]);
   } else if(functionName[0] == '@') {
      std::string imageName = std::string(functionName.begin()+1, functionName.end());
      if(argNameToIndex.find(imageName) == argNameToIndex.end())
         std::runtime_error("Absolute indexing with unknown image " + imageName);
      return Load2D(bldr, argv[argNameToIndex.at(imageName)],
                       bldr->ConvertTo(Int32, args[0]),
                       bldr->ConvertTo(Int32, args[1]));
   } else {
      return Load2D(bldr, argv[argNameToIndex.at(functionName)],
                    bldr->Add(bldr->ConvertTo(Int32, args[0]), i),
                    bldr->Add(bldr->ConvertTo(Int32, args[1]), j));
   }
   
   return bldr->Load("sum");
}

TR::IlValue* ImageArray::numberHandler(TR::IlBuilder *bldr, const std::string &number) {
   double x = std::atof(number.c_str());
   return bldr->ConstDouble(x);
}

TR::IlValue* ImageArray::symbolHandler(TR::IlBuilder *bldr, const std::string &name){
   
   if(argNameToIndex.find(name) != argNameToIndex.end()){
      return Load2D(bldr, argv[argNameToIndex.at(name)], i, j);
   }else if(name == "i" || name == "j"){ // special symbols
      return name == "i" ? bldr->ConvertTo(Double, i) : bldr->ConvertTo(Double, j);
   } else if(name == "c") {
      return bldr->ConvertTo(Double, c);
   }else if(name == "width" || name == "height"){
      return name == "width" ? bldr->ConvertTo(Double, bldr->Load("w")) : bldr->ConvertTo(Double, bldr->Load("h"));
   } else if(symbols.find(name) != symbols.end()) {
      return symbols[name];
   } else {
      throw std::runtime_error("Unable to find symbol: " + name);
   }
   
}
void ImageArray::min(TR::IlBuilder *bldr, TR::IlValue* val1) {
   TR::IlBuilder * rc3True = OrphanBuilder();
   
   bldr->IfThen(&rc3True,
                bldr->LessThan(val1, bldr->Load("sum")));
   rc3True->Store("sum",
                  val1);
}

void ImageArray::max(TR::IlBuilder *bldr, TR::IlValue* val1) {
   TR::IlBuilder * rc3True = OrphanBuilder();
   
   bldr->IfThen(&rc3True,
                bldr->GreaterThan(val1, bldr->Load("sum")));
   
   rc3True->Store("sum",
                  val1);
}

TR::IlValue* ImageArray::cast(TR::IlBuilder *bldr, TR::IlValue* val1) {
   return bldr->ConvertTo(Double, bldr->ConvertTo(Int32, val1));
}




bool
ImageArray::buildIL()
   {
      //Constants
      TR::IlValue *one = ConstInt32(1);
      TR::IlValue *zero = ConstInt32(0);
      
      //size, width, height, data, result
      TR::IlValue *width = Load("width");
      TR::IlValue *height = Load("height");
      TR::IlValue *result = Load("result");
      
      Store("w", Sub(Load("width"), ConstInt32(1)));
      Store("h", Sub(Load("height"), ConstInt32(1)));
      
      
      TR::IlBuilder *builder  = this;
      
      gnine::Cell &argsCell = cell_.list[0];
      
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
      
      argNameToIndex["output"] = argNames.size();
      argv.push_back(result);
      
      cell_.list.erase(cell_.list.begin());

         
      TR::IlBuilder *iloop=NULL, *jloop=NULL;
      ForLoopUp("i", &iloop, zero, height, one);
      {
         i = iloop->Load("i");
         
         iloop->ForLoopUp("j", &jloop, zero, width, one);
         {
            j = jloop->Load("j");
            c = jloop->Add(jloop->Mul(i, width), j);
            for(gnine::Cell c : cell_.list) {
               if (c.type == gnine::Cell::List and c.list[0].val == "define") {
                     symbols[c.list[1].val] = eval(jloop, c.list[2]);
               } else {
                  gnine::Cell &code = c;
                  TR::IlValue *ret = eval(jloop, code);
                  Store2D(jloop, result, i, j, ret);
               }
            }

         }
      }
      
   Return();

   return true;
   }
