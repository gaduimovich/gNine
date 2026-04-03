/*******************************************************************************
 * This originates from Pixslam
 * Original Author is Luke Dodd
 * Geoffrey Duimovich added the OMR implementation
 ********************************************************************************/

#include <iostream>
#include <sstream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <string>
#include <stdexcept>

#include "Image.h"
#include "Parser.h"
#include "ImageArray.hpp"
#include "VectorProgram.hpp"
#include "JitBuilder.hpp"

using namespace gnine;

namespace
{
   size_t programInputCount(const LoweredProgram &program)
   {
      if (program.channelPrograms.empty())
         throw std::runtime_error("Lowered program did not produce any channel programs");

      const Cell &channelProgram = program.channelPrograms[0];
      if (channelProgram.type != Cell::List || channelProgram.list.empty() || channelProgram.list[0].type != Cell::List)
         throw std::runtime_error("Lowered channel program must start with an argument list");

      return channelProgram.list[0].list.size();
   }

   bool isTopLevelIterate(const Cell &cell)
   {
      return cell.type == Cell::List &&
             cell.list.size() == 3 &&
             cell.list[0].type == Cell::Symbol &&
             cell.list[0].val == "iterate" &&
             cell.list[1].type == Cell::Number &&
             cell.list[2].type == Cell::List;
   }

   int parseIterateCount(const Cell &cell)
   {
      std::stringstream ss(cell.list[1].val);
      int value = 0;
      ss >> value;
      char trailing = '\0';
      if (!ss || (ss >> trailing) || value <= 0)
         throw std::runtime_error("iterate count must be a positive integer");
      return value;
   }

   std::string makeChainedFramePath(const std::string &basePath, int iteration)
   {
      size_t slashPos = basePath.find_last_of("/\\");
      size_t dotPos = basePath.find_last_of('.');
      if (dotPos == std::string::npos || (slashPos != std::string::npos && dotPos < slashPos))
         return basePath + "_" + std::to_string(iteration);

      return basePath.substr(0, dotPos) + "_" + std::to_string(iteration) + basePath.substr(dotPos);
   }

   int effectiveChannelCount(const std::vector<Image> &images)
   {
      int channels = 1;
      for (const Image &image : images)
         channels = std::max(channels, image.channelCount());
      return channels;
   }

   bool imagesHaveSameExtent(const std::vector<Image> &images)
   {
      if (images.empty())
         return true;

      const int width = images[0].width();
      const int height = images[0].height();
      for (const Image &image : images)
      {
         if (image.width() != width || image.height() != height)
            return false;
      }
      return true;
   }

   void fillChannelPointers(const std::vector<Image> &images,
                            int channel,
                            std::vector<double *> &channelPtrs)
   {
      channelPtrs.clear();
      channelPtrs.reserve(images.size());
      for (const Image &image : images)
      {
         int sourceChannel = image.channelCount() == 1 ? 0 : channel;
         channelPtrs.push_back(const_cast<double *>(image.getChannelData(sourceChannel)));
      }
   }

   void fillVectorArgPointers(const std::vector<Image> &images,
                              const std::vector<VectorArgBinding> &bindings,
                              std::vector<double *> &dataPtrs)
   {
      dataPtrs.clear();
      dataPtrs.reserve(bindings.size());
      for (size_t idx = 0; idx < bindings.size(); ++idx)
      {
         const VectorArgBinding &binding = bindings[idx];
         const Image &image = images[binding.inputIndex];
         int sourceChannel = image.channelCount() == 1 ? 0 : binding.channel;
         dataPtrs.push_back(const_cast<double *>(image.getChannelData(sourceChannel)));
      }
   }
}

void logCommandLine(int argc, char *argv[], const std::string &filePrefix)
{
#ifdef _WIN32
   std::string fileName = filePrefix + ".bat";
#else
   std::string fileName = filePrefix + ".sh";
#endif
   std::ofstream out(fileName, std::ios::out);

   for (int i = 0; i < argc; ++i)
      out << argv[i] << " ";

   out << std::endl;
}

int main(int argc, char *argsRaw[])
{
   if (argc < 3)
   {
      std::cout << "Usage:\n\n";
      std::cout << "    pixslam <code> [input-images] <output>\n\n";
      std::cout << "Code can either be supplied directly, or as a file path to read in.\n";
      std::cout << "The number of input images read is dependent on the supplied code.\n";
      std::cout << "The output argument is optional, defaults to out.png.\n\n";
      std::cout << "e.g:\n";
      std::cout << "Multiply image by 2 and output to out.png.\n\n";
      std::cout << "    pixslam \"((A) (* A 2))\" image.png\n\n";
      std::cout << "If file mult_by_two.pixslam contains \"(* A 2)\" then the following \n";
      std::cout << "multiplies image.png by 2 and output to image_times_two.png.\n\n";
      std::cout << "    pixslam mult_by_two.pixslam image.png image_times_two.png\n\n";
      std::cout << "Blend two images together equally and output to blend.png.\n";
      std::cout << "    pixslam ((A B) (* 0.5 (+ A B))) image1.png image2.png blend.png\n\n";
      std::cout << "Arguments:\n\n";
      std::cout << "--danger indexing will calcualte it only if you give it indexs in ranger increase performance\n";
      std::cout << "TIMES=N Executes the jited function more then once.\n";
      std::cout << "CHAIN-TIMES=N Executes the jited function as a chained simulation.\n";
      std::cout << "--benchmark prints compile and execution timings.\n";
      std::cout << "--emit-frames=PATH writes chained iterations as PATH with _N suffixes.\n";
      std::cout << "Color images are processed channel-wise and preserve RGB output.\n";
      std::cout << "Top-level form (iterate N ((A) ...)) runs the full transform N times.\n";
      std::cout << "Inside a transform, iter is the 1-based chained iteration counter.\n";
      std::cout << "Top-level form (pipeline ((A ...) ...) ((A ...) ...)) fuses scalar stages into one kernel.\n";
      std::cout << "In later pipeline stages, the first argument names the previous stage output.\n";

      return 1;
   }

   std::vector<std::string> argv;
   std::vector<std::string> options;

   // separate options from file/code arguments
   for (int i = 0; i < argc; ++i)
   {
      char *s = argsRaw[i];
      if (*s != '-')
         argv.emplace_back(s);
      else
         options.emplace_back(s);
   }

   // parse command line arguments
   bool logAsm = false;
   bool logCommand = false;
   bool danger = false;
   bool benchmark = false;
   std::string emitFramesPath;
   int n_times = 1;
   int chain_times = 0;
   for (auto s : options)
   {
      if (s == "--logAsm")
         logAsm = true;
      else if (s == "--danger")
         danger = true;
      else if (s == "--benchmark")
         benchmark = true;
      else if (s.length() > 14 && s.substr(0, 14) == "--emit-frames=")
         emitFramesPath = s.substr(14);
      else if (s == "--logCommand")
         logCommand = true;
      else if (s.length() > 14 and s.substr(0, 14) == "--chain-times=")
      {
         std::stringstream ss(s.substr(14));
         ss >> chain_times;
      }
      else if (s.length() > 8 and s.substr(0, 8) == "--times=")
      {
         std::stringstream ss(s.substr(8));
         ss >> n_times;
      }
      else
      {
         std::cerr << "Unrecognised command line switch: " << s << std::endl;
         return 1;
      }
   }

   if (chain_times > 0)
      n_times = chain_times;

   // See if first arg is a file and read code from it.
   // We infer file by checking that first char is not a '(' (cheeky but it works!)
   // Otherwise we interperate the argument as code directly.
   std::string codeString;
   if (argv[1].size() > 0 && argv[1][0] != '(')
   {
      std::ifstream ifs(argv[1]);
      if (ifs)
      {
         std::stringstream buffer;
         buffer << ifs.rdbuf();
         codeString = buffer.str();
      }
      else
      {
         std::cout << "Could not find file " << argv[1] << std::endl;
         return 1;
      }
   }
   else
   {
      codeString = argv[1];
   }
   //   printf("Code String: %s", codeString.c_str());
   // Generate code.
   Cell code = cellFromString(codeString);
   Cell effectiveCode = code;
   int iterateCount = 0;
   if (isTopLevelIterate(code))
   {
      iterateCount = parseIterateCount(code);
      effectiveCode = code.list[2];
      if (chain_times == 0)
      {
         chain_times = iterateCount;
         n_times = chain_times;
      }
   }

   LoweredProgram loweredProgram = lowerProgram(effectiveCode);
   size_t inputCount = programInputCount(loweredProgram);
   // Read in input images specified by arguments.
   int padding = 0;
   std::vector<Image> inputImages;
   for (size_t i = 0; i < inputCount; ++i)
   {
      Image im(argv[2 + i]);

      if (im.width() * im.height() == 0)
      {
         std::cout << "Failed to load image " << argv[2 + i] << std::endl;
         return 1;
      }

      inputImages.emplace_back(im, padding, padding);
   }

   if (!imagesHaveSameExtent(inputImages))
   {
      std::cerr << "All input images must have the same width and height." << std::endl;
      return 1;
   }

   //   printf("Step 1: initialize JIT\n");
   if (logCommand)
      logCommandLine(argc, argsRaw, "gnine-command");

   std::string jitOptions = "-Xjit:acceptHugeMethods,enableBasicBlockHoisting,omitFramePointer,useILValidator";
   if (logAsm)
      jitOptions += ",traceCG,log=gnine-jit.log";

   bool initialized = initializeJitWithOptions(const_cast<char *>(jitOptions.c_str()));
   if (!initialized)
   {
      fprintf(stderr, "FAIL: could not initialize JIT\n");
      exit(-1);
   }

   //   printf("Step 2: define type dictionary\n");
   OMR::JitBuilder::TypeDictionary types;

   //   printf("Step 3: compile method builder\n");
   auto compileStart = std::chrono::steady_clock::now();
   std::vector<ImageArrayFunctionType *> compiledFunctions;
   compiledFunctions.reserve(loweredProgram.channelPrograms.size());
   for (size_t programIdx = 0; programIdx < loweredProgram.channelPrograms.size(); ++programIdx)
   {
      ImageArray method(&types);
      method.runByteCodes(loweredProgram.channelPrograms[programIdx], danger);

      void *entry = 0;
      int32_t rc = compileMethodBuilder(&method, &entry);
      if (rc != 0)
      {
         fprintf(stderr, "FAIL: compilation error %d\n", rc);
         exit(-2);
      }
      compiledFunctions.push_back(reinterpret_cast<ImageArrayFunctionType *>(entry));
   }
   auto compileEnd = std::chrono::steady_clock::now();

   std::string outputImagePath = "out.png";

   if (argv.size() >= 3 + inputCount)
      outputImagePath = argv[3 + inputCount - 1];

   Image *image = &inputImages[0];
   int outputChannels = loweredProgram.usesVectorFeatures
                            ? (loweredProgram.outputIsVector ? 3 : 1)
                            : effectiveChannelCount(inputImages);

   Image outIm(image->width(), image->height(), image->stride(), outputChannels);

   std::vector<double *> dataPtrs;
   auto executionStart = std::chrono::steady_clock::now();
   if (chain_times > 0)
   {
      if (inputImages.size() != 1)
      {
         std::cerr << "--chain-times only supports single-input programs." << std::endl;
         shutdownJit();
         return 1;
      }

      Image chainInput(image->width(), image->height(), image->stride(), outputChannels);
      Image chainOutput(image->width(), image->height(), image->stride(), outputChannels);
      for (int channel = 0; channel < outputChannels; ++channel)
      {
         int sourceChannel = image->channelCount() == 1 ? 0 : channel;
         std::copy(image->getChannelData(sourceChannel),
                   image->getChannelData(sourceChannel) + image->planeSize(),
                   chainInput.getChannelData(channel));
      }

      for (int i = 0; i < chain_times; i++)
      {
         std::vector<Image> chainImages;
         chainImages.push_back(Image(chainInput.getData(),
                                     image->width(),
                                     image->height(),
                                     image->stride(),
                                     outputChannels));

         for (int channel = 0; channel < outputChannels; ++channel)
         {
            if (loweredProgram.usesVectorFeatures)
               fillVectorArgPointers(chainImages, loweredProgram.argBindings, dataPtrs);
            else
            {
               dataPtrs.resize(1);
               dataPtrs[0] = chainInput.getChannelData(channel);
            }

            size_t functionIndex = loweredProgram.usesVectorFeatures ? static_cast<size_t>(channel) : 0;
            compiledFunctions[functionIndex](image->width(),
                                             image->height(),
                                             i + 1,
                                             dataPtrs.data(),
                                             chainOutput.getChannelData(channel));
         }
         std::swap(chainInput.data, chainOutput.data);
         if (!emitFramesPath.empty())
         {
            Image frame(chainInput.getData(), image->width(), image->height(), image->stride(), outputChannels);
            frame.write(makeChainedFramePath(emitFramesPath, i + 1));
         }
      }

      std::copy(chainInput.getData(),
                chainInput.getData() + outputChannels * image->planeSize(),
                outIm.getData());
   }
   else
   {
      if (!emitFramesPath.empty())
      {
         std::cerr << "--emit-frames requires chained execution via --chain-times or iterate." << std::endl;
         shutdownJit();
         return 1;
      }
      for (int i = 0; i < n_times; i++)
      {
         for (int channel = 0; channel < outputChannels; ++channel)
         {
            if (loweredProgram.usesVectorFeatures)
               fillVectorArgPointers(inputImages, loweredProgram.argBindings, dataPtrs);
            else
               fillChannelPointers(inputImages, channel, dataPtrs);

            size_t functionIndex = loweredProgram.usesVectorFeatures ? static_cast<size_t>(channel) : 0;
            compiledFunctions[functionIndex](image->width(),
                                             image->height(),
                                             1,
                                             dataPtrs.data(),
                                             outIm.getChannelData(channel));
         }
      }
   }
   auto executionEnd = std::chrono::steady_clock::now();

   outIm.write(outputImagePath);
   if (benchmark)
   {
      auto compileMicros = std::chrono::duration_cast<std::chrono::microseconds>(compileEnd - compileStart).count();
      auto executionMicros = std::chrono::duration_cast<std::chrono::microseconds>(executionEnd - executionStart).count();
      double compileMillis = compileMicros / 1000.0;
      double executionMillis = executionMicros / 1000.0;
      double averageMillis = (n_times > 0) ? executionMillis / n_times : 0.0;
      double totalPixels = static_cast<double>(image->width()) * image->height() * outputChannels * n_times;
      double pixelsPerSecond = (executionMicros > 0) ? (totalPixels * 1000000.0) / executionMicros : 0.0;

      std::cout << "benchmark.compile_ms=" << compileMillis << std::endl;
      std::cout << "benchmark.execute_ms=" << executionMillis << std::endl;
      std::cout << "benchmark.iterations=" << n_times << std::endl;
      std::cout << "benchmark.mode=" << (chain_times > 0 ? "chain" : "repeat") << std::endl;
      std::cout << "benchmark.avg_iter_ms=" << averageMillis << std::endl;
      std::cout << "benchmark.pixels_per_second=" << pixelsPerSecond << std::endl;
   }
   shutdownJit();

   inputImages.clear();
   argv.clear();
   options.clear();
   dataPtrs.clear();
   return 0;
}

//((A)(* 1 (= (- i (/ width 2)) (- i (/ height 2)))))
