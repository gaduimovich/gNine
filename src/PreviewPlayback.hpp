#ifndef GNINE_PREVIEW_PLAYBACK_HPP
#define GNINE_PREVIEW_PLAYBACK_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

#include "RuntimeInputState.hpp"

namespace gnine
{
   enum class PreviewPlaybackScenario
   {
      None,
      Snake,
      Pong,
      Flappy,
      BrickBreaker
   };

   inline bool parsePreviewPlaybackScenario(const std::string &text, PreviewPlaybackScenario *outScenario)
   {
      if (text == "snake")
      {
         *outScenario = PreviewPlaybackScenario::Snake;
         return true;
      }

      if (text == "pong")
      {
         *outScenario = PreviewPlaybackScenario::Pong;
         return true;
      }

      if (text == "flappy")
      {
         *outScenario = PreviewPlaybackScenario::Flappy;
         return true;
      }

      if (text == "brick-breaker")
      {
         *outScenario = PreviewPlaybackScenario::BrickBreaker;
         return true;
      }

      return false;
   }

   inline const char *previewPlaybackScenarioName(PreviewPlaybackScenario scenario)
   {
      switch (scenario)
      {
      case PreviewPlaybackScenario::Snake:
         return "snake";
      case PreviewPlaybackScenario::Pong:
         return "pong";
      case PreviewPlaybackScenario::Flappy:
         return "flappy";
      case PreviewPlaybackScenario::BrickBreaker:
         return "brick-breaker";
      case PreviewPlaybackScenario::None:
      default:
         return "none";
      }
   }

   inline int previewPlaybackFrameBudget(double durationMs, double frameDeltaMs)
   {
      if (durationMs <= 0.0 || frameDeltaMs <= 0.0)
         return 0;
      return static_cast<int>(std::ceil(durationMs / frameDeltaMs));
   }

   inline RuntimeInputState makePreviewPlaybackInput(PreviewPlaybackScenario scenario, int frameIndex)
   {
      RuntimeInputState input;
      input.mouseX = 0.0;
      input.mouseY = 0.0;
      input.mouseLeft = 0.0;
      input.mouseRight = 0.0;
      input.mouseWheelY = 0.0;

      switch (scenario)
      {
      case PreviewPlaybackScenario::Snake:
         if (frameIndex < 32)
            input.keyRight = input.keyD = 1.0;
         else if (frameIndex < 52)
            input.keyUp = input.keyW = 1.0;
         else if (frameIndex < 80)
            input.keyLeft = input.keyA = 1.0;
         else if (frameIndex < 116)
            input.keyDown = input.keyS = 1.0;
         else if (frameIndex < 148)
            input.keyRight = input.keyD = 1.0;
         else if (frameIndex < 188)
            input.keyUp = input.keyW = 1.0;
         else if (frameIndex < 236)
            input.keyLeft = input.keyA = 1.0;
         else
         {
            const int cycleFrame = (frameIndex - 236) % 184;
            if (cycleFrame < 44)
               input.keyDown = input.keyS = 1.0;
            else if (cycleFrame < 92)
               input.keyRight = input.keyD = 1.0;
            else if (cycleFrame < 136)
               input.keyUp = input.keyW = 1.0;
            else
               input.keyLeft = input.keyA = 1.0;
         }
         break;
      case PreviewPlaybackScenario::Pong:
         if ((frameIndex / 45) % 2 == 0)
            input.keyUp = 1.0;
         else
            input.keyDown = 1.0;
         break;
      case PreviewPlaybackScenario::Flappy:
         if (frameIndex == 12 || (frameIndex > 12 && ((frameIndex - 12) % 24) == 0))
            input.keySpace = 1.0;
         break;
      case PreviewPlaybackScenario::BrickBreaker:
         if ((frameIndex / 40) % 2 == 0)
            input.keyRight = input.keyD = 1.0;
         else
            input.keyLeft = input.keyA = 1.0;
         break;
      case PreviewPlaybackScenario::None:
      default:
         break;
      }

      return input;
   }
}

#endif
