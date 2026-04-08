#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Image.h"
#include "ImageArray.hpp"
#include "JitBuilder.hpp"
#include "Parser.h"
#include "VectorProgram.hpp"

namespace
{
   bool almostEqual(double lhs, double rhs, double eps = 1.0 / 255.0 + 1e-9)
   {
      return std::fabs(lhs - rhs) <= eps;
   }

   void fillVectorArgPointers(const std::vector<gnine::Image> &images,
                              const std::vector<gnine::VectorArgBinding> &bindings,
                              std::vector<double *> &dataPtrs,
                              std::vector<int32_t> &inputWidths,
                              std::vector<int32_t> &inputHeights,
                              std::vector<int32_t> &inputStrides)
   {
      dataPtrs.clear();
      dataPtrs.reserve(bindings.size());
      inputWidths.clear();
      inputWidths.reserve(bindings.size());
      inputHeights.clear();
      inputHeights.reserve(bindings.size());
      inputStrides.clear();
      inputStrides.reserve(bindings.size());
      for (size_t idx = 0; idx < bindings.size(); ++idx)
      {
         const gnine::VectorArgBinding &binding = bindings[idx];
         const gnine::Image &image = images[binding.inputIndex];
         int sourceChannel = image.channelCount() == 1 ? 0 : binding.channel;
         dataPtrs.push_back(const_cast<double *>(image.getChannelData(sourceChannel)));
         inputWidths.push_back(image.width());
         inputHeights.push_back(image.height());
         inputStrides.push_back(image.stride());
      }
   }

   std::vector<std::vector<double>> runLoweredProgram(const std::string &source,
                                                      const std::vector<gnine::Image> &inputs)
   {
      gnine::LoweredProgram lowered = gnine::lowerProgram(gnine::cellFromString(source));

      OMR::JitBuilder::TypeDictionary types;
      std::vector<ImageArrayFunctionType *> functions;
      for (size_t idx = 0; idx < lowered.channelPrograms.size(); ++idx)
      {
         ImageArray method(&types);
         method.runByteCodes(lowered.channelPrograms[idx], false);

         void *entry = nullptr;
         int32_t rc = compileMethodBuilder(&method, &entry);
         if (rc != 0)
            throw std::runtime_error("compileMethodBuilder failed with rc=" + std::to_string(rc));
         functions.push_back(reinterpret_cast<ImageArrayFunctionType *>(entry));
      }

      const int width = inputs[0].width();
      const int height = inputs[0].height();
      const int outputChannels = lowered.usesVectorFeatures ? (lowered.outputIsVector ? 3 : 1) : inputs[0].channelCount();

      std::vector<std::vector<double>> outputs(outputChannels, std::vector<double>(width * height, 0.0));
      std::vector<double *> dataPtrs;
      std::vector<int32_t> inputWidths;
      std::vector<int32_t> inputHeights;
      std::vector<int32_t> inputStrides;
      for (int channel = 0; channel < outputChannels; ++channel)
      {
         size_t functionIndex = lowered.usesVectorFeatures ? static_cast<size_t>(channel) : 0;
         if (lowered.usesVectorFeatures)
         {
            fillVectorArgPointers(inputs, lowered.argBindings, dataPtrs, inputWidths, inputHeights, inputStrides);
         }
         else
         {
            dataPtrs.clear();
            inputWidths.clear();
            inputHeights.clear();
            inputStrides.clear();
            for (size_t inputIdx = 0; inputIdx < inputs.size(); ++inputIdx)
            {
               int sourceChannel = inputs[inputIdx].channelCount() == 1 ? 0 : channel;
               dataPtrs.push_back(const_cast<double *>(inputs[inputIdx].getChannelData(sourceChannel)));
               inputWidths.push_back(inputs[inputIdx].width());
               inputHeights.push_back(inputs[inputIdx].height());
               inputStrides.push_back(inputs[inputIdx].stride());
            }
         }
         functions[functionIndex](width,
                                  height,
                                  1,
                                  dataPtrs.data(),
                                  inputWidths.data(),
                                  inputHeights.data(),
                                  inputStrides.data(),
                                  outputs[channel].data());
      }

      return outputs;
   }

   int runRgbRoundTripCase()
   {
      gnine::Image image(2, 1, 2, 3);
      image(0, 0, 0) = 1.0;
      image(0, 0, 1) = 0.5;
      image(0, 0, 2) = 0.0;
      image(0, 1, 0) = 0.25;
      image(0, 1, 1) = 0.75;
      image(0, 1, 2) = 1.0;

      const std::string path = "/tmp/gnine_color_roundtrip.png";
      image.write(path);

      gnine::Image reloaded(path);
      std::remove(path.c_str());

      if (reloaded.width() != 2 || reloaded.height() != 1 || reloaded.channelCount() != 3)
      {
         std::cerr << "rgb_round_trip_io: unexpected image shape after reload\n";
         return 1;
      }

      for (int channel = 0; channel < 3; ++channel)
      {
         for (int col = 0; col < 2; ++col)
         {
            if (!almostEqual(image(0, col, channel), reloaded(0, col, channel)))
            {
               std::cerr << "rgb_round_trip_io: mismatch at channel " << channel
                         << " col " << col << "\n";
               return 1;
            }
         }
      }

      std::cout << "[PASS] rgb_round_trip_io\n";
      return 0;
   }

   int runChannelwiseExecutionCase()
   {
      gnine::Cell code = gnine::cellFromString("((A B) (+ A B))");

      OMR::JitBuilder::TypeDictionary types;
      ImageArray method(&types);
      method.runByteCodes(code, false);

      void *entry = nullptr;
      int32_t rc = compileMethodBuilder(&method, &entry);
      if (rc != 0)
      {
         std::cerr << "rgb_channelwise_broadcast: compileMethodBuilder failed with rc=" << rc << "\n";
         return 1;
      }

      auto *fn = reinterpret_cast<ImageArrayFunctionType *>(entry);
      const int width = 2;
      const int height = 2;

      std::vector<double> rgbPlanes[] = {
          {0.1, 0.2, 0.3, 0.4},
          {0.5, 0.6, 0.7, 0.8},
          {0.9, 0.1, 0.2, 0.3},
      };
      std::vector<double> grayPlane = {0.05, 0.10, 0.15, 0.20};
      std::vector<double> outputs[] = {
          std::vector<double>(width * height, 0.0),
          std::vector<double>(width * height, 0.0),
          std::vector<double>(width * height, 0.0),
      };

      std::vector<double *> dataPtrs(2);
      std::vector<int32_t> inputWidths(2, width);
      std::vector<int32_t> inputHeights(2, height);
      std::vector<int32_t> inputStrides(2, width);
      for (int channel = 0; channel < 3; ++channel)
      {
         dataPtrs[0] = rgbPlanes[channel].data();
         dataPtrs[1] = grayPlane.data();
         fn(width,
            height,
            1,
            dataPtrs.data(),
            inputWidths.data(),
            inputHeights.data(),
            inputStrides.data(),
            outputs[channel].data());
      }

      for (int channel = 0; channel < 3; ++channel)
      {
         for (int idx = 0; idx < width * height; ++idx)
         {
            double expected = rgbPlanes[channel][idx] + grayPlane[idx];
            if (!almostEqual(expected, outputs[channel][idx], 1e-9))
            {
               std::cerr << "rgb_channelwise_broadcast: mismatch at channel "
                         << channel << " index " << idx << "\n";
               return 1;
            }
         }
      }

      std::cout << "[PASS] rgb_channelwise_broadcast\n";
      return 0;
   }

   int runVectorChannelSwapCase()
   {
      gnine::Image image(2, 2, 2, 3);
      std::vector<double> red = {0.1, 0.2, 0.3, 0.4};
      std::vector<double> green = {0.5, 0.6, 0.7, 0.8};
      std::vector<double> blue = {0.9, 0.8, 0.7, 0.6};

      for (int idx = 0; idx < 4; ++idx)
      {
         image.getChannelData(0)[idx] = red[idx];
         image.getChannelData(1)[idx] = green[idx];
         image.getChannelData(2)[idx] = blue[idx];
      }

      std::vector<gnine::Image> inputs;
      inputs.push_back(gnine::Image(image.getData(), image.width(), image.height(), image.stride(), image.channelCount()));

      std::vector<std::vector<double>> outputs = runLoweredProgram("((A) (vec (b (color A)) (g (color A)) (r (color A))))", inputs);
      for (int idx = 0; idx < 4; ++idx)
      {
         if (!almostEqual(outputs[0][idx], blue[idx]) ||
             !almostEqual(outputs[1][idx], green[idx]) ||
             !almostEqual(outputs[2][idx], red[idx]))
         {
            std::cerr << "vector_channel_swap: mismatch at index " << idx << "\n";
            return 1;
         }
      }

      std::cout << "[PASS] vector_channel_swap\n";
      return 0;
   }

   int runVectorDotLumaCase()
   {
      gnine::Image image(2, 1, 2, 3);
      image(0, 0, 0) = 0.2;
      image(0, 0, 1) = 0.4;
      image(0, 0, 2) = 0.6;
      image(0, 1, 0) = 0.8;
      image(0, 1, 1) = 0.2;
      image(0, 1, 2) = 0.4;

      std::vector<gnine::Image> inputs;
      inputs.push_back(gnine::Image(image.getData(), image.width(), image.height(), image.stride(), image.channelCount()));

      std::vector<std::vector<double>> outputs = runLoweredProgram("((A) (dot (color A) (vec 0.25 0.5 0.25)))", inputs);
      if (outputs.size() != 1)
      {
         std::cerr << "vector_dot_luma: expected single-channel output\n";
         return 1;
      }

      std::vector<double> expected = {0.4, 0.4};
      for (int idx = 0; idx < 2; ++idx)
      {
         if (!almostEqual(outputs[0][idx], expected[idx], 1e-9))
         {
            std::cerr << "vector_dot_luma: mismatch at index " << idx << "\n";
            return 1;
         }
      }

      std::cout << "[PASS] vector_dot_luma\n";
      return 0;
   }
}

int main()
{
   std::string jitOptions = "-Xjit:acceptHugeMethods,enableBasicBlockHoisting,omitFramePointer,useILValidator";
   if (!initializeJitWithOptions(const_cast<char *>(jitOptions.c_str())))
   {
      std::cerr << "Failed to initialize JIT\n";
      return 1;
   }

   int failures = 0;
   failures += runRgbRoundTripCase();
   failures += runChannelwiseExecutionCase();
   failures += runVectorChannelSwapCase();
   failures += runVectorDotLumaCase();

   shutdownJit();

   return failures == 0 ? 0 : 1;
}
