//
//  JitImageFunction.hpp
//  gnine
//
//  Created by Geoffrey Duimovich on 2018-01-21.
//

#ifndef JitImageFunction_hpp
#define JitImageFunction_hpp
#endif /* JitImageFunction_hpp */

#pragma once

#include <functional>
#include <map>
#include <stdexcept>
#include <algorithm>
#include <numeric>
#include <map>


#include "Image.h"
#include "Parser.h"

namespace gnine{
   
   template <typename EvalReturn> class Visitor{
   public:
      typedef const double * const * Arguments;
      typedef void (*FuncPtrType)(Arguments, size_t, size_t, size_t, double *);
      
      
   public:
      Visitor(){
      }
      
      EvalReturn eval(const Cell &c){
         switch(c.type){
            case Cell::Number:{
               return numberHandler(c.val.c_str());
            }case Cell::List:{
               std::vector<EvalReturn> evalArgs(c.list.size()-1);
               
               // eval each argument
               std::transform(c.list.begin()+1, c.list.end(), evalArgs.begin(),
                              [=](const Cell &c) -> EvalReturn{
                                 return this->eval(c);
                              }
                              );
               
               return functionHandler(c.list[0].val, evalArgs);
            }case Cell::Symbol:{
               return symbolHandler(c.val);
            }
         }
         throw std::runtime_error("Should never get here.");
      }
      
      virtual ~Visitor(){}
      
   protected:
      virtual EvalReturn symbolHandler(const std::string &symbol) = 0;
      virtual EvalReturn functionHandler(const std::string &functionName,
                                         const std::vector<EvalReturn> &args) = 0;
      virtual EvalReturn numberHandler(const std::string &number) = 0;
      
   };
   
   class JitImageFunction : public Visitor<std::string>{
   public:
      JitImageFunction(const Cell &cell, bool stdOutLogging = false);
      
      // Call JIT function.
      void operator()(const std::vector<Image> &images, Image &out) const;
      
      // "Lower level" call for image data from other sources (e.g. opencv)
      void operator()(const std::vector<const double *> &args,
                      int w, int h, int stride,
                      double *out) const;
      
      virtual size_t getNumArgs() const {return argNameToIndex.size();}
      
      virtual ~JitImageFunction();
      
   private:
      FuncPtrType generate(const Cell &c);
      
      virtual std::string functionHandler(const std::string &functionName,
                                          const std::vector<std::string> &args);
      
      virtual std::string numberHandler(const std::string &number);
      
      virtual std::string symbolHandler(const std::string &name);
      
      void SetXmmVar(std::string &c, std::string &v, double d);
      
      void PopulateBuiltInFunctionHandlerMap();
      
   private:
      
      typedef std::function<std::string (const std::vector<std::string> &)>
      BuiltInFunctionHandler;
      std::map<std::string, BuiltInFunctionHandler> functionHandlerMap;
      
      std::map<std::string, size_t> argNameToIndex;
      
      FuncPtrType generatedFunction;
      
      int32_t currentI;
      int32_t currentJ;
      int32_t currentIndex;
      
      int32_t w;
      int32_t h;
      int32_t stride;
      double * out;
      
      int32_t zero;
      int32_t one;
      std::vector<Image *> argv;
      
      std::map<std::string, double *> symbols;
      
      
   };
   
}
