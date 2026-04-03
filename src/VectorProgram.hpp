#pragma once

#ifndef VECTORPROGRAM_INCL
#define VECTORPROGRAM_INCL

#include <vector>

#include "Parser.h"

namespace gnine
{
   struct VectorArgBinding
   {
      size_t inputIndex;
      int channel;
   };

   struct LoweredProgram
   {
      bool usesVectorFeatures;
      bool outputIsVector;
      std::vector<Cell> channelPrograms;
      std::vector<VectorArgBinding> argBindings;
   };

   LoweredProgram lowerProgram(const Cell &program);
}

#endif // VECTORPROGRAM_INCL
