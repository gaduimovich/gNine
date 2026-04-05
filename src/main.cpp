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
#include <cstdlib>
#include <string>
#include <stdexcept>
#include <memory>
#include <utility>

#include "Image.h"
#include "Parser.h"
#include "ImageArray.hpp"
#include "VectorProgram.hpp"
#include "JitBuilder.hpp"
#include "Runtime.hpp"

using namespace gnine;

namespace
{
   int parsePositiveInt(const std::string &text, const std::string &flagName)
   {
      std::stringstream ss(text);
      int value = 0;
      char trailing = '\0';
      ss >> value;
      if (!ss || (ss >> trailing) || value <= 0)
         throw std::runtime_error(flagName + " expects a positive integer");
      return value;
   }

   std::pair<int, int> parseDimensions(const std::string &text, const std::string &flagName)
   {
      size_t xPos = text.find('x');
      if (xPos == std::string::npos)
         xPos = text.find('X');
      if (xPos == std::string::npos)
         throw std::runtime_error(flagName + " expects WIDTHxHEIGHT");

      return std::make_pair(parsePositiveInt(text.substr(0, xPos), flagName),
                            parsePositiveInt(text.substr(xPos + 1), flagName));
   }

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

   bool isTopLevelIterateState(const Cell &cell)
   {
      return cell.type == Cell::List &&
             cell.list.size() == 4 &&
             cell.list[0].type == Cell::Symbol &&
             cell.list[0].val == "iterate-state" &&
             cell.list[1].type == Cell::Number &&
             cell.list[2].type == Cell::List &&
             cell.list[3].type == Cell::List;
   }

   bool isTopLevelIterateUntil(const Cell &cell)
   {
      return cell.type == Cell::List &&
             cell.list.size() == 5 &&
             cell.list[0].type == Cell::Symbol &&
             cell.list[0].val == "iterate-until" &&
             cell.list[1].type == Cell::Number &&
             cell.list[2].type == Cell::List &&
             cell.list[3].type == Cell::List &&
             cell.list[4].type == Cell::List;
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

   std::string makeComparisonPath(const std::string &basePath)
   {
      size_t slashPos = basePath.find_last_of("/\\");
      size_t dotPos = basePath.find_last_of('.');
      if (dotPos == std::string::npos || (slashPos != std::string::npos && dotPos < slashPos))
         return basePath + "_compare";

      return basePath.substr(0, dotPos) + "_compare" + basePath.substr(dotPos);
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

   int displayChannelCount(const Image &lhs, const Image &rhs)
   {
      return std::max(lhs.channelCount(), rhs.channelCount());
   }

   size_t programArgumentCount(const Cell &program)
   {
      if (program.type != Cell::List || program.list.empty() || program.list[0].type != Cell::List)
         throw std::runtime_error("Program must be of form ((A ...) expr)");
      return program.list[0].list.size();
   }

   std::string runtimeArgumentBindingName(const Cell &pattern, size_t index)
   {
      if (pattern.type == Cell::Symbol)
         return pattern.val;
      return "__arg" + std::to_string(index) + "__";
   }

   Image copyImage(const Image &source)
   {
      Image copy(source.width(), source.height(), source.stride(), source.channelCount());
      std::copy(source.getData(),
                source.getData() + source.channelCount() * source.planeSize(),
                copy.getData());
      return copy;
   }

   Image resizeNearest(const Image &source, int targetWidth, int targetHeight)
   {
      if (source.width() == targetWidth && source.height() == targetHeight)
         return copyImage(source);

      Image resized(targetWidth, targetHeight, targetWidth, source.channelCount());
      for (int channel = 0; channel < source.channelCount(); ++channel)
      {
         for (int row = 0; row < targetHeight; ++row)
         {
            int sourceRow = static_cast<int>((static_cast<long long>(row) * source.height()) / targetHeight);
            if (sourceRow >= source.height())
               sourceRow = source.height() - 1;

            for (int col = 0; col < targetWidth; ++col)
            {
               int sourceCol = static_cast<int>((static_cast<long long>(col) * source.width()) / targetWidth);
               if (sourceCol >= source.width())
                  sourceCol = source.width() - 1;
               resized(row, col, channel) = source(sourceRow, sourceCol, channel);
            }
         }
      }
      return resized;
   }

   runtime::Value runRuntimeProgram(runtime::Evaluator &evaluator,
                                   const Cell &program,
                                   const std::map<std::string, runtime::Value> &bindings)
   {
      return evaluator.evaluateProgram(program, bindings);
   }

   std::unique_ptr<Image> tryCopyRuntimeImage(const runtime::Value &value)
   {
      if (value.isObject() && value.object->type == runtime::Object::Image)
      {
         runtime::ImageObject *imageObj = static_cast<runtime::ImageObject *>(value.object);
         return std::unique_ptr<Image>(new Image(copyImage(*imageObj->image)));
      }

      if (value.isObject() && value.object->type == runtime::Object::Tuple)
      {
         runtime::TupleObject *tupleObj = static_cast<runtime::TupleObject *>(value.object);
         if (!tupleObj->values.empty())
            return tryCopyRuntimeImage(tupleObj->values[0]);
      }

      return std::unique_ptr<Image>();
   }

   double sampleDisplayChannel(const Image &image, int row, int col, int channel)
   {
      int sourceChannel = image.channelCount() == 1 ? 0 : channel;
      return image(row, col, sourceChannel);
   }

   void makeComparisonImage(const Image &original, const Image &filtered, Image &comparison)
   {
      const int gutter = 12;
      int channels = comparison.channelCount();

      for (int row = 0; row < original.height(); ++row)
      {
         for (int col = 0; col < original.width(); ++col)
         {
            for (int channel = 0; channel < channels; ++channel)
               comparison(row, col, channel) = sampleDisplayChannel(original, row, col, channel);
         }
      }

      for (int row = 0; row < filtered.height(); ++row)
      {
         for (int col = 0; col < filtered.width(); ++col)
         {
            for (int channel = 0; channel < channels; ++channel)
               comparison(row, original.width() + gutter + col, channel) = sampleDisplayChannel(filtered, row, col, channel);
         }
      }

      for (int row = 0; row < comparison.height(); ++row)
      {
         for (int col = original.width(); col < original.width() + gutter; ++col)
         {
            for (int channel = 0; channel < channels; ++channel)
               comparison(row, col, channel) = 1.0;
         }
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
      std::cout << "--compare[=PATH] writes a side-by-side original/result comparison image.\n";
      std::cout << "--display-scale=N writes output frames enlarged by an integer factor.\n";
      std::cout << "--display-size=WIDTHxHEIGHT writes output frames at an exact size.\n";
      std::cout << "--runtime interprets managed image programs instead of compiling JIT kernels.\n";
      std::cout << "Color images are processed channel-wise and preserve RGB output.\n";
      std::cout << "Top-level form (iterate N ((A) ...)) runs the full transform N times.\n";
      std::cout << "Top-level form (iterate-state N ((A ...) init) ((state) ...)) seeds runtime chaining from an explicit initial state.\n";
      std::cout << "Top-level form (iterate-until N ((A ...) init) ((state) step) ((state) done)) runs until done is nonzero or N steps are reached.\n";
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
   bool runtimeMode = false;
   std::string emitFramesPath;
   std::string comparePath;
   int displayScale = 1;
   int displayWidth = 0;
   int displayHeight = 0;
   int n_times = 1;
   int chain_times = 0;
   for (auto s : options)
   {
      if (s == "--logAsm")
         logAsm = true;
      else if (s == "--runtime")
         runtimeMode = true;
      else if (s == "--danger")
         danger = true;
      else if (s == "--benchmark")
         benchmark = true;
      else if (s == "--compare")
         comparePath = "__AUTO__";
      else if (s.length() > 10 && s.substr(0, 10) == "--compare=")
         comparePath = s.substr(10);
      else if (s.length() > 16 && s.substr(0, 16) == "--display-scale=")
         displayScale = parsePositiveInt(s.substr(16), "--display-scale");
      else if (s.length() > 15 && s.substr(0, 15) == "--display-size=")
      {
         std::pair<int, int> dims = parseDimensions(s.substr(15), "--display-size");
         displayWidth = dims.first;
         displayHeight = dims.second;
      }
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

   if (displayWidth > 0 && displayScale != 1)
      throw std::runtime_error("Use either --display-scale or --display-size, not both");

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
   Cell iterateStateInit;
   Cell iterateUntilDone;
   bool hasIterateState = false;
   bool hasIterateUntil = false;
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
   else if (isTopLevelIterateState(code))
   {
      iterateCount = parseIterateCount(code);
      iterateStateInit = code.list[2];
      effectiveCode = code.list[3];
      hasIterateState = true;
      if (chain_times == 0)
      {
         chain_times = iterateCount;
         n_times = chain_times;
      }
   }
   else if (isTopLevelIterateUntil(code))
   {
      iterateCount = parseIterateCount(code);
      iterateStateInit = code.list[2];
      effectiveCode = code.list[3];
      iterateUntilDone = code.list[4];
      hasIterateState = true;
      hasIterateUntil = true;
      if (chain_times == 0)
      {
         chain_times = iterateCount;
         n_times = chain_times;
      }
   }

   if ((hasIterateState || hasIterateUntil) && !runtimeMode)
      throw std::runtime_error(hasIterateUntil ? "iterate-until currently requires --runtime"
                                               : "iterate-state currently requires --runtime");

   size_t inputCount = runtimeMode ? programArgumentCount(hasIterateState ? iterateStateInit : effectiveCode) : 0;
   LoweredProgram loweredProgram;
   if (!runtimeMode)
   {
      loweredProgram = lowerProgram(effectiveCode);
      inputCount = programInputCount(loweredProgram);
   }

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

   std::string outputImagePath = "out.png";
  if (argv.size() >= 3 + inputCount)
     outputImagePath = argv[3 + inputCount - 1];

   auto writeDisplayImage = [&](const Image &imageToWrite, const std::string &path)
   {
      if (displayWidth > 0)
      {
         Image scaled = resizeNearest(imageToWrite, displayWidth, displayHeight);
         scaled.write(path);
         return;
      }

      if (displayScale != 1)
      {
         Image scaled = resizeNearest(imageToWrite,
                                      imageToWrite.width() * displayScale,
                                      imageToWrite.height() * displayScale);
         scaled.write(path);
         return;
      }

      imageToWrite.write(path);
   };

   auto makeDisplayImage = [&](const Image &imageToWrite) -> Image
   {
      if (displayWidth > 0)
         return resizeNearest(imageToWrite, displayWidth, displayHeight);
      if (displayScale != 1)
         return resizeNearest(imageToWrite,
                              imageToWrite.width() * displayScale,
                              imageToWrite.height() * displayScale);
      return copyImage(imageToWrite);
   };

   if (runtimeMode)
   {
      auto executionStart = std::chrono::steady_clock::now();
      std::unique_ptr<Image> runtimeImageResult;
      bool hasImageResult = false;
      runtime::Evaluator evaluator;
      runtime::Value runtimeState = runtime::Value::nil();
      runtime::Heap::Root runtimeStateRoot(evaluator.heap(), runtimeState);
      bool hasRuntimeState = false;
      bool hasScalarResult = false;
      double runtimeScalarResult = 0.0;
      int runtimeIterationsExecuted = 0;

      if (chain_times > 0)
      {
         if (hasIterateState)
         {
            const Cell &initArgsCell = iterateStateInit.list[0];
            const Cell &stateArgsCell = effectiveCode.list[0];
            if (stateArgsCell.list.size() != 1)
               throw std::runtime_error((hasIterateUntil ? "iterate-until" : "iterate-state") +
                                        std::string(" step program must take exactly one state argument"));
            if (hasIterateUntil)
            {
               const Cell &doneArgsCell = iterateUntilDone.list[0];
               if (doneArgsCell.list.size() != 1)
                  throw std::runtime_error("iterate-until done program must take exactly one state argument");
            }
            if (initArgsCell.list.size() != inputImages.size())
               throw std::runtime_error((hasIterateUntil ? "iterate-until" : "iterate-state") +
                                        std::string(" init input count does not match program arguments"));

            std::map<std::string, runtime::Value> initBindings;
            for (size_t idx = 0; idx < initArgsCell.list.size(); ++idx)
               initBindings[runtimeArgumentBindingName(initArgsCell.list[idx], idx)] = evaluator.imageValue(inputImages[idx]);
            initBindings["iter"] = runtime::Value::numberValue(0.0);
            runtimeState = runRuntimeProgram(evaluator, iterateStateInit, initBindings);
            hasRuntimeState = true;
         }
         else
         {
            if (inputImages.size() != 1)
            {
               std::cerr << "--runtime iterate/chain execution currently supports a single input image unless you use iterate-state." << std::endl;
               return 1;
            }

            runtimeState = evaluator.imageValue(inputImages[0]);
            hasRuntimeState = true;
         }
         for (int iter = 1; iter <= chain_times; ++iter)
         {
            std::map<std::string, runtime::Value> bindings;
            bindings[runtimeArgumentBindingName(effectiveCode.list[0].list[0], 0)] = runtimeState;
            bindings["iter"] = runtime::Value::numberValue(static_cast<double>(iter));
            runtimeState = runRuntimeProgram(evaluator, effectiveCode, bindings);
            hasRuntimeState = true;
            runtimeIterationsExecuted = iter;

            std::unique_ptr<Image> nextImage = tryCopyRuntimeImage(runtimeState);
            if (!nextImage)
               throw std::runtime_error("Runtime chained execution requires the program to return an image or a tuple whose first element is an image");

            runtimeImageResult.reset(new Image(copyImage(*nextImage)));
            hasImageResult = true;

            if (!emitFramesPath.empty())
               writeDisplayImage(*nextImage, makeChainedFramePath(emitFramesPath, iter));

            if (hasIterateUntil)
            {
               std::map<std::string, runtime::Value> doneBindings;
               doneBindings[runtimeArgumentBindingName(iterateUntilDone.list[0].list[0], 0)] = runtimeState;
               doneBindings["iter"] = runtime::Value::numberValue(static_cast<double>(iter));
               runtime::Value doneValue = runRuntimeProgram(evaluator, iterateUntilDone, doneBindings);
               if (!doneValue.isNumber())
                  throw std::runtime_error("iterate-until done program must return a number");
               if (doneValue.number != 0.0)
                  break;
            }
         }
      }
      else
      {
         if (!emitFramesPath.empty())
         {
            std::cerr << "--emit-frames requires chained execution via --chain-times or iterate." << std::endl;
            return 1;
         }

         const Cell &argsCell = effectiveCode.list[0];
         if (argsCell.list.size() != inputImages.size())
            throw std::runtime_error("Runtime input count does not match program arguments");

         std::map<std::string, runtime::Value> bindings;
         for (size_t idx = 0; idx < argsCell.list.size(); ++idx)
            bindings[runtimeArgumentBindingName(argsCell.list[idx], idx)] = evaluator.imageValue(inputImages[idx]);
         bindings["iter"] = runtime::Value::numberValue(1.0);

         runtimeState = runRuntimeProgram(evaluator, effectiveCode, bindings);
         hasRuntimeState = true;

         std::unique_ptr<Image> singleResultImage = tryCopyRuntimeImage(runtimeState);
         if (singleResultImage)
         {
            runtimeImageResult.reset(new Image(copyImage(*singleResultImage)));
            hasImageResult = true;
         }
         else if (runtimeState.isNumber())
         {
            hasScalarResult = true;
            runtimeScalarResult = runtimeState.number;
         }
      }

      auto executionEnd = std::chrono::steady_clock::now();

      if (hasImageResult)
      {
         Image writtenImage = makeDisplayImage(*runtimeImageResult);
         writtenImage.write(outputImagePath);
         if (!comparePath.empty())
         {
            if (inputImages.empty())
            {
               std::cerr << "--compare requires at least one input image." << std::endl;
               return 1;
            }
            if (comparePath == "__AUTO__")
               comparePath = makeComparisonPath(outputImagePath);

            const int gutter = 12;
            int compareChannels = displayChannelCount(inputImages[0], writtenImage);
            Image comparison(inputImages[0].width() + gutter + writtenImage.width(),
                             std::max(inputImages[0].height(), writtenImage.height()),
                             inputImages[0].width() + gutter + writtenImage.width(),
                             compareChannels);
            makeComparisonImage(inputImages[0], writtenImage, comparison);
            comparison.write(comparePath);
         }
      }
      else if (hasScalarResult)
      {
         if (!comparePath.empty())
            throw std::runtime_error("--compare requires the runtime program to return an image");
         if (argv.size() >= 3 + inputCount)
            throw std::runtime_error("Runtime scalar results do not accept an output image path");
         std::cout << runtimeScalarResult << std::endl;
      }
      else
      {
         throw std::runtime_error("Runtime mode requires the program to produce a number, an image, or a tuple whose first element is an image");
      }

      if (benchmark)
      {
         auto executionMicros = std::chrono::duration_cast<std::chrono::microseconds>(executionEnd - executionStart).count();
         double executionMillis = executionMicros / 1000.0;
         std::cout << "benchmark.compile_ms=0" << std::endl;
         std::cout << "benchmark.execute_ms=" << executionMillis << std::endl;
         std::cout << "benchmark.iterations=" << (chain_times > 0 ? runtimeIterationsExecuted : n_times) << std::endl;
         std::cout << "benchmark.mode=" << (hasIterateUntil ? "runtime-until"
                                                            : (chain_times > 0 ? "runtime-chain" : "runtime")) << std::endl;
      }

      inputImages.clear();
      argv.clear();
      options.clear();
      return 0;
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
            writeDisplayImage(frame, makeChainedFramePath(emitFramesPath, i + 1));
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

   Image writtenOutput = makeDisplayImage(outIm);
   writtenOutput.write(outputImagePath);
   if (!comparePath.empty())
   {
      if (comparePath == "__AUTO__")
         comparePath = makeComparisonPath(outputImagePath);

      const int gutter = 12;
      int compareChannels = displayChannelCount(inputImages[0], writtenOutput);
      Image comparison(inputImages[0].width() + gutter + writtenOutput.width(),
                       std::max(inputImages[0].height(), writtenOutput.height()),
                       inputImages[0].width() + gutter + writtenOutput.width(),
                       compareChannels);
      makeComparisonImage(inputImages[0], writtenOutput, comparison);
      comparison.write(comparePath);
   }
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
