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

#include "Image.h"
#include "Parser.h"
#include "ImageArray.hpp"
#include "JitBuilder.hpp"

using namespace gnine;

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
   // Read in input images specified by arguments.
   int padding = 0;
   std::vector<Image> inputImages;
   for (size_t i = 0; i < code.list[0].list.size(); ++i)
   {
      Image im(argv[2 + i]);

      if (im.width() * im.height() == 0)
      {
         std::cout << "Failed to load image " << argv[2 + i] << std::endl;
         return 1;
      }

      inputImages.emplace_back(im, padding, padding);
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
   ImageArray method(&types);
   method.runByteCodes(code, danger);

   void *entry = 0;
   auto compileStart = std::chrono::steady_clock::now();
   int32_t rc = compileMethodBuilder(&method, &entry);
   auto compileEnd = std::chrono::steady_clock::now();
   if (rc != 0)
   {
      fprintf(stderr, "FAIL: compilation error %d\n", rc);
      exit(-2);
   }

   //   printf("Step 4: invoke compiled code and verify results\n");
   ImageArrayFunctionType *test = (ImageArrayFunctionType *)entry;

   std::string outputImagePath = "out.png";

   if (argv.size() >= 3 + code.list[0].list.size())
      outputImagePath = argv[3 + code.list[0].list.size() - 1];

   Image *image = &inputImages[0];

   Image outIm(image->width(), image->height(), image->stride());

   std::vector<double *> dataPtrs;
   for (Image &im : inputImages)
   {
      dataPtrs.push_back(im.getData());
   }
   auto executionStart = std::chrono::steady_clock::now();
   if (chain_times > 0)
   {
      if (inputImages.size() != 1)
      {
         std::cerr << "--chain-times only supports single-input programs." << std::endl;
         shutdownJit();
         return 1;
      }

      Image chainInput(image->width(), image->height(), image->stride());
      Image chainOutput(image->width(), image->height(), image->stride());
      std::copy(image->getData(),
                image->getData() + image->height() * image->stride(),
                chainInput.getData());

      std::vector<double *> chainPtrs(1);
      chainPtrs[0] = chainInput.getData();

      for (int i = 0; i < chain_times; i++)
      {
         test(image->width(), image->height(), chainPtrs.data(), chainOutput.getData());
         std::swap(chainInput.data, chainOutput.data);
         chainPtrs[0] = chainInput.getData();
      }

      std::copy(chainInput.getData(),
                chainInput.getData() + image->height() * image->stride(),
                outIm.getData());
   }
   else
   {
      for (int i = 0; i < n_times; i++)
      {
         test(image->width(), image->height(), dataPtrs.data(), outIm.getData());
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
      double totalPixels = static_cast<double>(image->width()) * image->height() * n_times;
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
