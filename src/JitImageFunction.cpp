//
//  JitImageFunction.cpp
//  gnine
//
//  Created by Geoffrey Duimovich on 2018-01-21.
//

#include "JitImageFunction.hpp"

namespace gnine{
   
   JitImageFunction::JitImageFunction(const Cell &cell, bool stdOutLogging) {
      
      // Check cell is of form ((arg list) (expr))
      if(!(cell.type == Cell::List && cell.list.size() == 2 &&
           cell.list[0].type == Cell::List && cell.list[1].type == Cell::List))
         throw std::runtime_error("Function cell must be of form ((arg1 arg2 ...) (code))");
      
      
      const Cell &argsCell = cell.list[0];
      const Cell &code = cell.list[1];
      
      // Load arguments
      std::vector<std::string> argNames;
      for(Cell c : argsCell.list){
         if(c.type == Cell::Symbol)
            argNames.push_back(c.val);
         else
            throw std::runtime_error(
                                     "Function cell must be of form ((arg1 arg2 ...) (code))");
      }
      
      
      for(size_t i = 0; i < argNames.size(); ++i)
         argNameToIndex[argNames[i]] = i;
      
      
      // Specify how to deal with function calls.
      PopulateBuiltInFunctionHandlerMap();
      // Generate the code!
      generatedFunction = generate(code);
   }
   
   JitImageFunction::FuncPtrType JitImageFunction::generate(const Cell &c){
      //((A) (* 0.5 A))
      for(size_t i = 0; i < argNameToIndex.size(); ++i){
         
      }
      // Bind input array of image pointers to AsmJit vars.
      // Setup some useful constants.
      // Convert above into doubles so they can be bound to symbols.
      // Perpare loop vars
      // for i = 0..h
      // for j = 0..w
      
   }
   
   
   
   void JitImageFunction::operator()(const std::vector<Image> &images, Image &out) const {
      if(images.empty())
         throw std::runtime_error("must have at least one input image.");
      
      // check all images have the same dimension + stride.
      for(size_t i = 1; i < images.size(); ++i)
         if(   images[i-1].width() != images[i].width()
            || images[i-1].height() != images[i].height()
            || images[i-1].stride() != images[i].stride())
            throw std::runtime_error("all input images must have same dimensions.");
      
      if(   images[0].width() != out.width()
         || images[0].height() != out.height()
         || images[0].stride() != out.stride())
         throw std::runtime_error("all input images and output must have same dimensions.");
      
      
      std::vector<const double*> dataPtrs;
      for(const Image &im : images)
         dataPtrs.push_back(im.getData());
      
      generatedFunction(&dataPtrs[0], images[0].width(), images[0].height(), images[0].stride(),
                        out.getData());
   }
   
   // "Lower level" call for image data from other sources (e.g. opencv)
   void JitImageFunction::operator()(const std::vector<const double *> &args,
                                     int w, int h, int stride,
                                     double *out) const {
      generatedFunction(&args[0], w, h, stride, out);
   }
   
   JitImageFunction::~JitImageFunction(){
      
   }
   
   std::string JitImageFunction::functionHandler(const std::string &functionName,
                                                 const std::vector<std::string> &args){
      
      // std::cout << argNameToIndex.size() << std::endl;
      // std::cout << argNameToIndex["A"] << std::endl;
      // std::cout << "functionName: " << functionName << std::endl;
      
      // try builtin function lookup first
      auto it = functionHandlerMap.find(functionName);
      if(it != functionHandlerMap.end()){
         return it->second(args);
      }
      
      // std::cout << "Not a function." << std::endl;
      
      if(functionName[0] == '@'){
         std::string imageName = std::string(functionName.begin()+1, functionName.end());
         
         if(argNameToIndex.find(imageName) == argNameToIndex.end())
            std::runtime_error("Absolute indexing with unknown image " + imageName);
         
         return "Absolute indexing";
      }else{
         std::string imageName = functionName;
         
         if(argNameToIndex.find(imageName) == argNameToIndex.end())
            std::runtime_error("Unknown function (or image) " + imageName);
         
         // Otherwise they must have been doing an image lookup...
         // Convert double to int for indexing
         // TODO: figure out a better way.
         return "Relative Indexing";
      }
      
   }
   
   std::string JitImageFunction::numberHandler(const std::string &number){
      double x = std::atof(number.c_str());
      return "number handler";
   };
   
   std::string JitImageFunction::symbolHandler(const std::string &name){
      
      // Use of an argument as a symbol not a function call
      // is equivanlent to x_i_j
      if(argNameToIndex.find(name) != argNameToIndex.end()){
         //            GpVar pImage = argv[argNameToIndex.at(name)];
         //            XmmVar v(compiler.newXmmVar());
         //            compiler.movsd(v, ptr(pImage, currentIndex, kScale8Times));
         return "arg as symbol not function";
      }else if(name == "i" || name == "j"){ // special symbols
         //            XmmVar v(compiler.newXmmVar());
         //            AsmJit::GpVar index = name == "i" ? currentI : currentJ;
         //            compiler.cvtsi2sd(v, index);
         return "special symbols i and j";
      }else if(name == "width" || name == "height"){
         //            XmmVar v(compiler.newXmmVar());
         //            AsmJit::GpVar index = name == "w" ? w : h;
         //            compiler.cvtsi2sd(v, index);
         return "sepcial symbols width and height";
      }else if(symbols.find(name) != symbols.end()){
         //return symbols[name];
      }
      
      throw std::runtime_error("Unable to find symbol: " + name);
   };
   
   
   void JitImageFunction::PopulateBuiltInFunctionHandlerMap(){
      
      //Subtract Add mulitplciation min max loop through the arguments and add them up
      
      // TODO: This could be cleaner, the pattern is the same up to max,
      // but we're not sharing any code.
      functionHandlerMap["+"] = [&](const std::vector<std::string> &args) -> std::string{
         std::for_each(args.begin()+1, args.end(),  [&](const std::string &a){
            "add each onne";
         });
         
         return "* handler";
      };
      
      functionHandlerMap["min"] = [&](const std::vector<std::string> &args) -> std::string{
         std::for_each(args.begin()+1, args.end(),  [&](const std::string &a){
            "find the smallest each one";
         });
         
         return "min handler";
      };
      
      
      functionHandlerMap["max"] = [&](const std::vector<std::string> &args) -> std::string{
         std::for_each(args.begin()+1, args.end(),  [&](const std::string &a){
            "find the largest";
         });
         
         return "max handler";
      };
      
      
      functionHandlerMap["*"] = [&](const std::vector<std::string> &args) -> std::string{
         std::for_each(args.begin()+1, args.end(),  [&](const std::string &a){
            "multipy each onne";
         });
         
         return "* handler";
      };
      
      
      functionHandlerMap["-"] = [&](const std::vector<std::string> &args) -> std::string{
         std::for_each(args.begin()+1, args.end(),  [&](const std::string &a){
            "subtraction each onne";
         });
         
         return "- subtraction handler";
      };
      
      //The compare ones  look at args args[0] and args[1]
      
      functionHandlerMap["<"] = [&](const std::vector<std::string> &args) -> std::string{
         return "< handler";
         
      };
      
      functionHandlerMap[">"] = [&](const std::vector<std::string> &args) -> std::string{
         return "> handler";
      };
      
      functionHandlerMap["<="] = [&](const std::vector<std::string> &args) -> std::string{
         return "<= handler";
      };
      
      functionHandlerMap[">="] = [&](const std::vector<std::string> &args) -> std::string{
         return ">= handler";
         
      };
      
      functionHandlerMap["=="] = [&](const std::vector<std::string> &args) -> std::string{
         return "== handler";
         
      };
      
      functionHandlerMap["!="] = [&](const std::vector<std::string> &args) -> std::string{
         return "!= handler";
         
      };
   }
   
   
   
}
